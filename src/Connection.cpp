#include "Connection.h"
#include "protocol.h"      // MsgHeader, isValidHeader
#include <sys/epoll.h>     // epoll_ctl
#include <sys/socket.h>    // recv
#include <unistd.h>        // close, write
#include <cstring>         // memset
#include <cerrno>          // errno, EAGAIN
#include <arpa/inet.h>     // ntohl, ntohs
#include <iostream>        // cerr, cout
#include <vector>          // vector


static constexpr size_t HIGH_WATER_MARK = 1 * 1024 * 1024;

Connection::Connection(int fd, int epoll_fd)
    : fd_(fd)
    , epoll_fd_(epoll_fd)
    , last_active_time_(time(nullptr))
    , recv_buffer_(128 * 1024)
    , send_buffer_(4 * 1024 * 1024) {

}

Connection::~Connection() {
    //把fd从epoll监听树上面摘掉
    epoll_ctl(epoll_fd_, EPOLL_CTL_DEL,  fd_, nullptr);
    //关闭fd
    close(fd_);
   
}

void Connection::updateEpollout() {
    struct epoll_event event{};
    if (send_buffer_.empty()) {
        event.events = EPOLLIN | EPOLLET;
    }
    else {
        event.events = EPOLLIN | EPOLLET | EPOLLOUT;
    }
    event.data.fd = fd_;
    epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd_, &event);
}

void Connection::send(const char* data, size_t len) {
    if (!send_buffer_.empty()) {
        bool append_status = send_buffer_.append(data, len);
        if (!append_status || send_buffer_.readableBytes() > HIGH_WATER_MARK) {
            std::cerr << fd_ << ":慢客户端,踢掉\n";
            on_close_(this);
            return;
        }
        updateEpollout();
        return;
    }
    ssize_t sent = write(fd_, data, len);
    if (sent >= 0 && static_cast<size_t>(sent) == len) {
        //数据都发完了，万事大吉！！！
        updateEpollout();
    }
    else if (sent >= 0 && static_cast<size_t>(sent) < len) {
        //没发完,先查看有没有成功把剩余数据添加到缓冲区,然后在判断是不是慢客户端
        bool append_status = send_buffer_.append(data + sent, len - sent);
        if (!append_status) {
            std::cerr << fd_ << ":这个客户端是一个慢客户端，先踢掉了\n";
            on_close_(this);
            return;
        }
        else {
            if (send_buffer_.readableBytes() > HIGH_WATER_MARK) {
                std::cerr << fd_ << ":这个客户端是一个慢客户端，先踢掉了\n";
                on_close_(this);
                return;
            }
            else {
                updateEpollout();
            }
        }
    }
    else {
        if (errno == EAGAIN) {
            //内核缓冲区满了,先把数据放到缓冲区,如果触发水位线,就踢掉,否则就更新epollout监听
            bool append_status = send_buffer_.append(data, len);
            if (!append_status) {
                std::cerr << fd_ << ":这个客户端是一个慢客户端，先踢掉了\n";
                on_close_(this);
                return;
            }
            else {
                if (send_buffer_.readableBytes() > HIGH_WATER_MARK) {
                    std::cerr << fd_ << ":\n这个客户端是一个慢客户端，先踢掉了\n";
                    on_close_(this);
                    return;
                }
                else {
                    updateEpollout();
                }
            }


        }
        else {
            //连接异常，发送失败
            on_close_(this);
            return;
        }
    }
}

void Connection::handleWrite() {
    /*
    1. peek 数据到临时缓冲区
    2. 直接 write()
    3. 如果发完了 → retrieve 全部 → update_epollout
    4. 如果发了一部分 → retrieve 已发送的部分 → 保持 EPOLLOUT
    5. 如果出错 → close_client
    */
    size_t wait_send = send_buffer_.readableBytes();
    std::vector<char> data(wait_send);
    send_buffer_.peek(data.data(), wait_send);
    ssize_t sent = write(fd_, data.data(), wait_send);
    if (sent >= 0 && static_cast<size_t>(sent) == wait_send) {
        send_buffer_.retrieve(wait_send);
        updateEpollout();
    }
    else if (sent >= 0 && static_cast<size_t>(sent) < wait_send) {
        send_buffer_.retrieve(sent);
    }
    else {
        if (errno == EAGAIN) {
            //内核缓冲区满了，发不了
            
        }
        else {
            //出问题了，关闭连接
            on_close_(this);
        }
    }
}

void Connection::handleRead() {
    //因为是边缘触发模式，必须要用一个while循环一次读完
    char buffer[64 * 1024];//把从内核传过来的数据暂时放到缓冲区当中
    bool connection_closed = false;
    while (true) {
        memset(buffer, 0, sizeof(buffer));//把缓冲区内的数据初始化为0，防止读到垃圾数据
        ssize_t bytes_read = recv(fd_, buffer, sizeof(buffer) - 1, 0);
        if (bytes_read > 0) {
            RingBuffer& rb = recv_buffer_;
            last_active_time_ = time(nullptr);
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
                if (!isValidHeader(header)) {
                    on_close_(this);
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

                std::vector<char> full_packet(total_packet_size);
                rb.peek(full_packet.data(), total_packet_size);
                rb.retrieve(total_packet_size);

                // std::cout << "成功解包! 文件描述符: " << fd_
                //           << " 业务类型: " << ntohs(header.type)
                //           << " 包体长度: " << body_length << "字节\n";
                
                //业务分发使用on_message_回调
                on_message_(this, full_packet.data(), total_packet_size);
                // std::cout << "卡死在348行了";
                
            }
        }
        if (connection_closed) break;
        if (bytes_read == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                //读干净了
                break;
            }
            else {//进入这个分支通常代表客户端已经关闭,或者tcp已经出现异常,留着连接不放只会无谓的浪费系统的资源
                std::cerr <<  fd_ << "没成功从内核读到数据" << "\n";
                on_close_(this);  
                break;
            }
        }
        else if (bytes_read == 0) {
            //客户端完成四次挥手正常关闭连接
            std::cout << "客户端:" << fd_ << "正常关闭连接\n";
            on_close_(this);
            break; 
        }
        else {
            // std::cout << "收到数据: " << bytes_read << "字节" << "数据来自" << fd_ << "\n";
        }
        // std::cout<< "卡死在373行了";
    }
}