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
#include "EventLoop.h"


static constexpr size_t HIGH_WATER_MARK = 1 * 1024 * 1024;
static constexpr size_t RECV_CHUNK_SIZE = 64 * 1024;

static constexpr size_t RECV_BUFFER_CAPACITY =
    sizeof(MsgHeader)
    + MAX_PAYLOAD_SIZE
    + RECV_CHUNK_SIZE;

Connection::Connection(int fd, EventLoop* loop)
    : fd_(fd)
    , loop_(loop)
    , last_active_time_(time(nullptr))
    , recv_buffer_(RECV_BUFFER_CAPACITY)
    , send_buffer_(4 * 1024 * 1024) {

}

Connection::~Connection() {
    //把fd从epoll监听树上面摘掉
    loop_->delFd(fd_);
    //关闭fd
    close(fd_);
   
}

void Connection::updateEpollout() {
    struct epoll_event event{};
    if (send_buffer_.empty()) {
        event.events = EPOLLIN | EPOLLRDHUP | EPOLLET;
    }
    else {
        event.events = EPOLLIN | EPOLLRDHUP | EPOLLET | EPOLLOUT;
    }

    event.data.fd = fd_;

    if (!loop_->modFd(fd_, event.events))
    {
        requestClose();
        return;
    }
}

void Connection::send(const char* data, size_t len) {
    if (!send_buffer_.empty()) {
        bool append_status = send_buffer_.append(data, len);
        if (!append_status || send_buffer_.readableBytes() > HIGH_WATER_MARK) {
            std::cerr << fd_ << ":慢客户端,踢掉\n";
            requestClose();
            return;
        }
        updateEpollout();
        return;
    }
    //尝试第一次发送
    ssize_t sent = -1;

    do {
        sent = ::write(fd_, data, len);
    }
    while (sent < 0 && errno == EINTR);

    if (sent >= 0 && static_cast<size_t>(sent) == len) {
        //数据都发完了，万事大吉！！！
        updateEpollout();
    }
    else if (sent >= 0 && static_cast<size_t>(sent) < len) {
        //没发完,先查看有没有成功把剩余数据添加到缓冲区,然后在判断是不是慢客户端
        bool append_status = send_buffer_.append(data + sent, len - sent);
        if (!append_status) {
            std::cerr << fd_ << ":这个客户端是一个慢客户端，先踢掉了\n";
            requestClose();
            return;
        }
        else {
            if (send_buffer_.readableBytes() > HIGH_WATER_MARK) {
                std::cerr << fd_ << ":这个客户端是一个慢客户端，先踢掉了\n";
                requestClose();
                return;
            }
            else {
                updateEpollout();
            }
        }
    }
    else {
        const int error = errno;

        if (error == EAGAIN || error == EWOULDBLOCK) {
            //内核缓冲区满了,先把数据放到缓冲区,如果触发水位线,就踢掉,否则就更新epollout监听
            bool append_status = send_buffer_.append(data, len);
            if (!append_status) {
                std::cerr << fd_ << ":这个客户端是一个慢客户端，先踢掉了\n";
                requestClose();
                return;
            }
            else {
                if (send_buffer_.readableBytes() > HIGH_WATER_MARK) {
                    std::cerr << fd_ << ":\n这个客户端是一个慢客户端，先踢掉了\n";
                    requestClose();
                    return;
                }
                else {
                    updateEpollout();
                }
            }


        }
        else {
            std::cerr << "write failed: fd=" << fd_
                      << ", errno=" << error
                      << " (" << std::strerror(error) << ")\n";

            requestClose();
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
    ssize_t sent = -1;

    do {
        sent = ::write(fd_, data.data(), wait_send);
    }
    while (sent < 0 && errno == EINTR);

    if (sent >= 0 && static_cast<size_t>(sent) == wait_send) {
        send_buffer_.retrieve(wait_send);
        updateEpollout();
    }
    else if (sent >= 0 && static_cast<size_t>(sent) < wait_send) {
        send_buffer_.retrieve(sent);
    }
    else {
        const int error = errno;

        if (error == EAGAIN || error == EWOULDBLOCK) {
            // 数据仍留在 send_buffer_ 中，继续等待 EPOLLOUT。
            return;
        }

        std::cerr << "write failed: fd=" << fd_
                  << ", errno=" << error
                  << " (" << std::strerror(error) << ")\n";

        requestClose();
        return;
    }

}

void Connection::handleRead() {
    //因为是边缘触发模式，必须要用一个while循环一次读完
    char buffer[RECV_CHUNK_SIZE];//把从内核传过来的数据暂时放到缓冲区当中
    bool connection_closed = false;
    while (true) {
        memset(buffer, 0, sizeof(buffer));//把缓冲区内的数据初始化为0，防止读到垃圾数据
        ssize_t bytes_read = recv(fd_, buffer, sizeof(buffer), 0);
        if (bytes_read > 0) {
            RingBuffer& rb = recv_buffer_;

            const size_t readable_before = rb.readableBytes();
            const size_t writeable_before = rb.writeableBytes();

            if (!rb.append(
                    buffer,
                    static_cast<size_t>(bytes_read)
                )) {
                std::cerr << "receive buffer overflow: fd=" << fd_
                          << ", readable=" << readable_before
                          << ", writeable=" << writeable_before
                          << ", recv_bytes=" << bytes_read
                          << '\n';

                requestClose();
                return;
            }

            last_active_time_ = time(nullptr);

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
                    requestClose();
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
        if (bytes_read < 0) {
            const int error = errno;

            if (error == EINTR) {
                continue;
            }

            if (error == EAGAIN || error == EWOULDBLOCK) {
                // ET 模式下已经读空，返回 EventLoop。
                break;
            }

            std::cerr << "recv failed: fd=" << fd_
                      << ", errno=" << error
                      << " (" << std::strerror(error) << ")\n";

            requestClose();
            break;
        }

        else if (bytes_read == 0) {
            //客户端完成四次挥手正常关闭连接
            std::cout << "客户端:" << fd_ << "正常关闭连接\n";
            requestClose();
            break; 
        }
        else {
            // std::cout << "收到数据: " << bytes_read << "字节" << "数据来自" << fd_ << "\n";
        }
        // std::cout<< "卡死在373行了";
    }
}


void Connection::requestClose() {
    if (closing_) {
        return;
    }

    closing_ = true;

    if (on_close_) {
        on_close_(this);
    }
}

void Connection::handleError() {
    int socket_error = 0;
    socklen_t error_length = sizeof(socket_error);

    if (::getsockopt(
            fd_,
            SOL_SOCKET,
            SO_ERROR,
            &socket_error,
            &error_length
        ) == -1) {
        const int error = errno;

        std::cerr << "getsockopt(SO_ERROR) failed: fd=" << fd_
                  << ", errno=" << error
                  << " (" << std::strerror(error) << ")\n";
    }
    else if (socket_error != 0) {
        std::cerr << "socket error: fd=" << fd_
                  << ", errno=" << socket_error
                  << " (" << std::strerror(socket_error) << ")\n";
    }
    else {
        std::cerr << "EPOLLERR: fd=" << fd_
                  << ", SO_ERROR=0\n";
    }

    requestClose();
}
