#include "Server.h"
#include <cstdint>
#include "utils.h"

Server::Server(int port, int epoll_fd)
    : port_(port)
    , epoll_fd_(epoll_fd)
    , server_fd_(-1) {

}

void Server::start() {
    signal(SIGPIPE, SIG_IGN);
    //创建一个TCP套接字
    server_fd_ = socket(AF_INET, SOCK_STREAM, 0); //使用socket()函数创建一个TCP套接字，返回一个文件描述符
    //检查分配文件描述符是否成功
    if (server_fd_ == -1) {
        std::cerr << "文件描述符分配失败:" << strerror(errno) << std::endl;
        return;
    }
    //设置端口复用，防止重启网关时address already in use错误
    int opt = 1;
    if (setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
        std::cerr << "设置端口复用失败:" << strerror(errno) << std::endl;
        return;
    }

    //绑定套接字到指定端口
    struct sockaddr_in server_addr{}; //定义一个sockaddr_in结构体变量来存储服务器地址信息
    server_addr.sin_family = AF_INET;//设置套接字使用的协议
    server_addr.sin_addr.s_addr = INADDR_ANY;//设置要监听的目标IP地址
    server_addr.sin_port = htons(port_);//设置要监听的端口

    //绑定IP地址
    if (bind(server_fd_, (struct sockaddr*)&server_addr, sizeof(server_addr)) == -1) {
        std::cerr << "绑定IP失败\n";
        return;
    }

    //监听套接字
    if (listen(server_fd_, SOMAXCONN) == -1) {
        std::cerr << "监听失败";
        return;
    }
    //设置监听套接字为非阻塞监听
    setNonBlocking(server_fd_);

    //将监听套接字添加到epoll实例中
    struct epoll_event event{};
    event.events = EPOLLIN | EPOLLET;//监听读事件和边缘触发式监听
    event.data.fd = server_fd_;//监听指定文件描述符下的事件
    epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, server_fd_, &event);//往该epoll实例管理的监听红黑树中添加一个监听事件

}

void Server::acceptNewConnection() {
    while(true) {
        struct sockaddr_in client_addr{};
        socklen_t client_addr_len = sizeof(client_addr);
        int client_fd = accept(server_fd_, (struct sockaddr*)&client_addr, &client_addr_len);//需要把sockaddr_in手动转换成sockaddr
        //给客户端分配文件描述符失败
        if (client_fd == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;//这个就绪事件已经处理完了，break这个死循环去处理下一个就绪事件
            }
            else {
                std::cerr << "给客户端分配文件描述符失败\n";
                return;
            }
        }
        //给客户端分配文件描述符成功
        std::cout << "有新的客户端连进来了！！！新成员的文件描述符是：" << client_fd << "\n";
        connections_[client_fd] = std::make_unique<Connection>(client_fd, epoll_fd_);//怎么创建一个独占指着对象来着?


        //将新的设备挂在到epoll_fd的监听红黑树上面，并设置监听模式为非阻塞式监听
        setNonBlocking(client_fd);
        connections_[client_fd]->on_message_ = [this](Connection* conn, const char* data, size_t len) {
            onMessage(conn, data, len);
        }
        connections_[client_fd]->on_close_ = [this](Connection* conn) {
            onClose(conn);
        }
        // timer_heap.push({time(nullptr) + 30, client_fd});//这个应该是后面Eventloop负责的吧?
        struct epoll_event client_event{};
        client_event.events = EPOLLIN | EPOLLET;
        client_event.data.fd = client_fd;
        epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, client_fd, &client_event);
        
        // std::cout << "卡死在265行了";
    }
}

void Server::onClose(Connection* conn) {
    //关闭文件描述符
    //从连接列表中抹除掉
    //从映射表中抹除掉
    //从订阅者中抹除掉
    //从向谁订阅列表中抹除掉

}