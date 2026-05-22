#include <iostream> //这个头文件包含了输入输出流的定义，可以使用std::cout和std::cin等对象进行输入输出操作。
#include <sys/socket.h>//这个头文件包含了套接字编程的定义，可以使用socket()、bind()、listen()、accept()等函数进行网络通信。
#include <netinet/in.h>//这个头文件包含了Internet地址族的定义，可以使用sockaddr_in结构体来表示IPv4地址和端口号。
#include <arpa/inet.h>//这个头文件包含了IP地址转换的定义，可以使用inet_addr()、inet_ntoa()等函数进行IP地址转换。
#include <sys/epoll.h>//这个头文件包含了epoll机制的定义，可以使用epoll_create()、epoll_ctl()、epoll_wait()等函数进行高效的I/O多路复用。
#include <fcntl.h>//这个头文件包含了文件控制的定义，可以使用fcntl()函数进行文件描述符的操作。
#include <cstring>//这个头文件包含了字符串处理的定义，可以使用strlen()、strcpy()、strcat()等函数进行字符串操作。
#include <cerrno>//这个头文件包含了错误代码的定义，可以使用errno变量获取系统调用的错误信息。
#include <unistd.h>
#include "protocol.h"
#include "ringbuffer.h"
#include <unordered_map>
#include <vector>
#include <queue>
#include <csignal>


const int MAX_EVENTS = 1024; //定义最大事件数为1024
const int PORT = 8081; //定义服务器监听的端口号为8081
static constexpr size_t HIGH_WATER_MARK = 6144;


void setNonBlocking(int fd) { //定义一个函数用于设置文件描述符为非阻塞模式
    int flags = fcntl(fd, F_GETFL, 0); //获取文件描述符的当前标志
    fcntl(fd, F_SETFL, flags | O_NONBLOCK); //将非阻塞标志添加到当前标志中
}

struct ClientContext {
    RingBuffer recv_buffer; //接收缓冲区
    RingBuffer send_buffer; //发送缓冲区
    int client_fd;
    time_t last_active_time; //上一次活跃时间

    ClientContext() : recv_buffer(8192), send_buffer(8192), client_fd(-1), last_active_time(time(nullptr)) {}
};

std::unordered_map<int, ClientContext> clients;
struct TimerEntry {
    time_t expire; //绝对超时时刻
    int fd;        //对应的连接
};
struct CmpExpire {
    bool operator()(const TimerEntry& a, const TimerEntry& b) {
        return a.expire > b.expire; //大的排在后面，小的在堆顶
    }
};
std::priority_queue<TimerEntry, std::vector<TimerEntry>, CmpExpire> timer_heap;


void update_epollout(int epoll_fd, struct ClientContext* ctx) {
    struct epoll_event event{};
    if (ctx->send_buffer.empty()) {
        event.events = EPOLLIN | EPOLLET;
    }
    else {
        event.events = EPOLLIN | EPOLLET | EPOLLOUT;
    }
    event.data.fd = ctx->client_fd;
    epoll_ctl(epoll_fd, EPOLL_CTL_MOD, ctx->client_fd, &event);
}

void close_client(int epoll_fd, struct ClientContext* ctx) {
    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, ctx->client_fd, nullptr);
    close(ctx->client_fd);
    clients.erase(ctx->client_fd);
}

void send_data(int epoll_fd, struct ClientContext* ctx, const char* data, size_t len) {
    ssize_t sent = write(ctx->client_fd, data, len);
    if (sent >= 0 && static_cast<size_t>(sent) == len) {
        //数据都发完了，万事大吉！！！
        update_epollout(epoll_fd, ctx);
    }
    else if (sent >= 0 && static_cast<size_t>(sent) < len) {
        //没发完
        ctx->send_buffer.append(data + sent, len - sent);
        if (ctx->send_buffer.readableBytes() > HIGH_WATER_MARK) {
            std::cerr << ctx->client_fd << ":\n这个客户端是一个慢客户端，先踢掉了\n";
            close_client(epoll_fd, ctx);
            return;
        }
        update_epollout(epoll_fd, ctx);
    }
    else {
        if (errno == EAGAIN) {
            //内核缓冲区满了
            ctx->send_buffer.append(data, len);
            if (ctx->send_buffer.readableBytes() > HIGH_WATER_MARK) {
            std::cerr << ctx->client_fd << ":\n这个客户端是一个慢客户端，先踢掉了\n";
            close_client(epoll_fd, ctx);
            return;
        }
            update_epollout(epoll_fd, ctx);
        }
        else {
            //连接异常，发送失败
            close_client(epoll_fd, ctx);
            return;
        }
    }
}

void handle_write(int epoll_fd, struct ClientContext* ctx) {
    /*
    1. peek 数据到临时缓冲区
    2. 直接 write()
    3. 如果发完了 → retrieve 全部 → update_epollout
    4. 如果发了一部分 → retrieve 已发送的部分 → 保持 EPOLLOUT
    5. 如果出错 → close_client
    */
    size_t wait_send = ctx->send_buffer.readableBytes();
    std::vector<char> data(wait_send);
    ctx->send_buffer.peek(data.data(), wait_send);
    ssize_t sent = write(ctx->client_fd, data.data(), wait_send);
    if (sent >= 0 && static_cast<size_t>(sent) == wait_send) {
        ctx->send_buffer.retrieve(wait_send);
        update_epollout(epoll_fd, ctx);
    }
    else if (sent >= 0 && static_cast<size_t>(sent) < wait_send) {
        ctx->send_buffer.retrieve(sent);
    }
    else {
        if (errno == EAGAIN) {
            //内核缓冲区满了，发不了
            
        }
        else {
            //出问题了，关闭连接
            close_client(epoll_fd, ctx);
        }
    }

}



int main() {
    signal(SIGPIPE, SIG_IGN);
    //创建一个TCP套接字
    int server_fd = socket(AF_INET, SOCK_STREAM, 0); //使用socket()函数创建一个TCP套接字，返回一个文件描述符
    //检查分配文件描述符是否成功
    if (server_fd == -1) {
        std::cerr << "文件描述符分配失败:" << strerror(errno) << std::endl;
        return -1;
    }
    //设置端口复用，防止重启网关时address already in use错误
    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
        std::cerr << "设置端口复用失败:" << strerror(errno) << std::endl;
        return -1;
    }

    //绑定套接字到指定端口
    struct sockaddr_in server_addr{}; //定义一个sockaddr_in结构体变量来存储服务器地址信息
    server_addr.sin_family = AF_INET;//设置套接字使用的协议
    server_addr.sin_addr.s_addr = INADDR_ANY;//设置要监听的目标IP地址
    server_addr.sin_port = htons(PORT);//设置要监听的端口

    //绑定IP地址
    if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) == -1) {
        std::cerr << "绑定IP失败\n";
        return -1;
    }

    //监听套接字
    if (listen(server_fd, SOMAXCONN) == -1) {
        std::cerr << "监听失败";
        return -1;
    }
    //设置监听套接字为非阻塞监听
    setNonBlocking(server_fd);
    //创建一个epoll实例
    int epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) {
        std::cerr << "创建epoll实例失败";
        return -1;
    }
    //将监听套接字添加到epoll实例中
    struct epoll_event event{};
    event.events = EPOLLIN | EPOLLET;//监听读事件和边缘触发式监听
    event.data.fd = server_fd;//监听指定文件描述符下的事件
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &event);//往该epoll实例管理的监听红黑树中添加一个监听事件
    //创建监听事件列表
    struct epoll_event events[MAX_EVENTS];
    std::cout << "开始监听端口：" << PORT << "传来的消息......\n";

    std::vector<char> full_packet;
    full_packet.reserve(8192); // 预留空间

    //进入事件主循环
    while (true) {
        
        //获取目前就绪的事件列表
        int ready_num = epoll_wait(epoll_fd, events, MAX_EVENTS, 5000);

        //时间轮心跳保活
        time_t now = time(nullptr);
        while (!timer_heap.empty()) {
            TimerEntry time_top = timer_heap.top();
            if (now < time_top.expire) break;
            timer_heap.pop();
            if (clients.find(time_top.fd) == clients.end()) continue;
            if (clients[time_top.fd].last_active_time + 30 > now) continue;
            close_client(epoll_fd, &clients[time_top.fd]);
                
        }
        
        //开始在循环处理这次的就绪事件
        for (int i = 0; i < ready_num; i++) {
            //区分事件类型，如果监听到的就绪文件描述符是server，证明有新的连接来了
            if (events[i].data.fd == server_fd) {
                //因为是边缘模式的监听，必须数据一来就用一个while读完，不然就会漏数据
                while (true) {
                    struct sockaddr_in client_addr{};
                    socklen_t client_addr_len = sizeof(client_addr);
                    int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_addr_len);//需要把sockaddr_in手动转换成sockaddr
                    //给客户端分配文件描述符失败
                    if (client_fd == -1) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break;//这个就绪事件已经处理完了，break这个死循环去处理下一个就绪事件
                        }
                        else {
                            std::cerr << "给客户端分配文件描述符失败\n";
                            break;
                        }
                    }
                    //给客户端分配文件描述符成功
                    std::cout << "有新的客户端连进来了！！！新成员的文件描述符是：" << client_fd << "\n";

                    //将新的设备挂在到epoll_fd的监听红黑树上面，并设置监听模式为非阻塞式监听
                    setNonBlocking(client_fd);
                    clients.emplace(client_fd, ClientContext{});//为新连接上的客户端分配缓冲区,默认缓冲区大小是8kb
                    clients[client_fd].client_fd = client_fd;
                    timer_heap.push({time(nullptr) + 30, client_fd});
                    struct epoll_event client_event{};
                    client_event.events = EPOLLIN | EPOLLET;
                    client_event.data.fd = client_fd;
                    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &client_event);
                    

                }
            }
            else if (events[i].events & EPOLLIN) {//epollin是读事件，有数据发进来了
                int client_fd = events[i].data.fd;
                struct ClientContext* ctx = &clients[client_fd];
                char buffer[1024];//把从内核传过来的数据暂时放到缓冲区当中
                bool connection_closed = false;

                //因为是边缘触发模式，必须要用一个while循环一次读完
                while (true) {
                    memset(buffer, 0, sizeof(buffer));//把缓冲区内的数据初始化为0，防止读到垃圾数据
                    ssize_t bytes_read = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
                    if (bytes_read > 0) {
                        RingBuffer& rb = clients[client_fd].recv_buffer;
                        clients[client_fd].last_active_time = time(nullptr);
                        timer_heap.push({time(nullptr) + 30, client_fd});
                        rb.append(buffer, bytes_read);
                        while (true) {
                            //状态A:解析包头

                            if (rb.readableBytes() < sizeof(MsgHeader)) {
                                //数据不够一个包头,说明是半包,退出当前的循环,等待下一次的epoll唤醒
                                break;
                            }

                            MsgHeader header;
                            rb.peek(reinterpret_cast<char*>(&header), sizeof(MsgHeader));
                            //先判断是不是正确的包,魔法数对不上就丢弃
                            if (ntohs(header.magic) != MAGIC_NUMBER) {
                                std::cerr << "文件描述符:" << client_fd << "收到非魔法数,可能是一个垃圾包,断开连接\n";
                                close_client(epoll_fd, &(clients[client_fd]));
                                connection_closed = true;
                                break;
                            }
                            //状态B:提取包体

                            uint32_t body_length = ntohl(header.length);
                            size_t total_packet_size = sizeof(MsgHeader) + body_length;//算上包头的完整长度
                            //检查recv到缓冲区的数据够不够当前完整的包
                            if (rb.readableBytes() < total_packet_size) {
                                //走到这里就说明包头没问题,但是缓冲区中的数据长度不够包体的长度,说明有数据还没到缓冲区中
                                break;
                            }

                            //状态C:提取完整包并滑动窗口

                            full_packet.resize(total_packet_size);
                            rb.peek(full_packet.data(), total_packet_size);
                            rb.retrieve(total_packet_size);

                            std::cout << "成功解包! 文件描述符: " << client_fd
                                      << " 业务类型: " << ntohs(header.type)
                                      << " 包体长度: " << body_length << "字节\n";
                            
                            //判断是否是心跳包
                            if (ntohs(header.type) == static_cast<uint16_t>(MsgType::HeartBeat)) {
                                send_data(epoll_fd, ctx, reinterpret_cast<char*>(&header),sizeof(header));
                            }
                            
                        }
                    }
                    if (connection_closed) break;
                    if (bytes_read == -1) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            //读干净了
                            break;
                        }
                        else {//进入这个分支通常代表客户端已经关闭,或者tcp已经出现异常,留着连接不放只会无谓的浪费系统的资源
                            std::cerr <<  client_fd << "没成功从内核读到数据" << "\n";
                            close_client(epoll_fd, &(clients[client_fd]));  
                            break;
                        }
                    }
                    else if (bytes_read == 0) {
                        //客户端完成四次挥手正常关闭连接
                        std::cout << "客户端:" << client_fd << "正常关闭连接\n";
                        close_client(epoll_fd, &(clients[client_fd]));
                        break; 
                    }
                    else {
                        std::cout << "收到数据: " << bytes_read << "字节" << "数据来自" << client_fd << " 数据内容:" << buffer << "\n";
                    }
                    
                }
                
            }
            else if (events[i].events & EPOLLOUT) {
                int client_fd = events[i].data.fd;
                if (clients.find(client_fd) == clients.end()) continue;
                struct ClientContext* ctx = &clients[client_fd];
                handle_write(epoll_fd, ctx);
            }
            
            //如果监听到的就绪文件描述符不是server的，那就肯定是客户端的，证明有新的数据过来了
        }
    }
    close(server_fd);
    close(epoll_fd);
    return 0;

}
