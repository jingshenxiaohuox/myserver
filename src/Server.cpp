#include "Server.h"
#include <cstdint>
#include <csignal>
#include <memory>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <iostream>
#include <cstring>
#include <cerrno>
#include <algorithm>
#include <arpa/inet.h>
#include "Connection.h"
#include <protocol.h>
#include <stdexcept>
#include <unistd.h>
#include <string>
#include <system_error>
#include <vector>

namespace {
    constexpr int kConnectionTimeoutSeconds = 4 * 60 * 60;
}


Server::Server(int port, EventLoop* loop)
    : port_(port)
    , loop_(loop)
    , server_fd_(-1) {

}

Server::~Server() {
}

void Server::start() {
    signal(SIGPIPE, SIG_IGN);
    //创建一个TCP套接字
    server_fd_ = socket(
        AF_INET,
        SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
        0
        ); //使用socket()函数创建一个TCP套接字，返回一个文件描述符

    //检查分配文件描述符是否成功
    if (server_fd_ == -1) {
        const int error = errno;
        const std::string context =
            "socket: errno=" + std::to_string(error);

        throw std::system_error(
            error,
            std::generic_category(),
            context
        );
    }
    //设置端口复用，防止重启网关时address already in use错误
    int opt = 1;
    if (setsockopt(
        server_fd_,
        SOL_SOCKET,
        SO_REUSEADDR,
        &opt,
        sizeof(opt)
        ) == -1) {
        const int error = errno;
        const int fd = server_fd_;

        ::close(server_fd_);
        server_fd_ = -1;

        const std::string context =
            "setsockopt(SO_REUSEADDR): fd=" + std::to_string(fd)
            + ", errno=" + std::to_string(error);

        throw std::system_error(
            error,
            std::generic_category(),
            context
        );
    }

    //绑定套接字到指定端口
    struct sockaddr_in server_addr{}; //定义一个sockaddr_in结构体变量来存储服务器地址信息
    server_addr.sin_family = AF_INET;//设置套接字使用的协议
    server_addr.sin_addr.s_addr = INADDR_ANY;//设置要监听的目标IP地址
    server_addr.sin_port = htons(port_);//设置要监听的端口

    //绑定IP地址
    if (::bind(
            server_fd_,
            reinterpret_cast<struct sockaddr*>(&server_addr),
            sizeof(server_addr)
        ) == -1) {
        const int error = errno;
        const int fd = server_fd_;

        ::close(server_fd_);
        server_fd_ = -1;

        const std::string context =
            "bind: fd=" + std::to_string(fd)
            + ", port=" + std::to_string(port_)
            + ", errno=" + std::to_string(error);

        throw std::system_error(
            error,
            std::generic_category(),
            context
        );
    }


    //监听套接字
    if (::listen(server_fd_, SOMAXCONN) == -1) {
        const int error = errno;
        const int fd = server_fd_;

        ::close(server_fd_);
        server_fd_ = -1;

        const std::string context =
            "listen: fd=" + std::to_string(fd)
            + ", errno=" + std::to_string(error);

        throw std::system_error(
            error,
            std::generic_category(),
            context
        );
    }


    //注册server_fd的handler
    loop_->setHandler(server_fd_, [this](uint32_t) {
        acceptNewConnection();
    });
    //注册超时回调
    loop_->setTimeoutCallback([this](int fd) {
        auto it = connections_.find(fd);
        if (it == connections_.end()) {
            return;
        }

        auto conn = it->second;
        const time_t now = time(nullptr);
        const time_t expire =
            conn->last_active_time_ + kConnectionTimeoutSeconds;

        if (expire > now) {
            const int remaining =
                static_cast<int>(expire - now);

            loop_->pushTimer(fd, remaining);
            return;
        }

        conn->requestClose();
    });


    //将监听套接字添加到epoll实例中
    if (!loop_->addFd(server_fd_, EPOLLIN | EPOLLET))
    {
        loop_->removeHandler(server_fd_);
        ::close(server_fd_);
        server_fd_ = -1;
        throw std::runtime_error(
            "failed to add listening socket to epoll"
        );
    }

}

void Server::acceptNewConnection() {
    while(true) {
        struct sockaddr_in client_addr{};
        socklen_t client_addr_len = sizeof(client_addr);
        int client_fd = ::accept4(
            server_fd_,
            reinterpret_cast<struct sockaddr*>(&client_addr),
            &client_addr_len,
            SOCK_NONBLOCK | SOCK_CLOEXEC
        );        //给客户端分配文件描述符失败
        if (client_fd == -1) {
            const int error = errno;

            if (error == EAGAIN || error == EWOULDBLOCK) {
                return;
            }

            if (error == EINTR) {
                continue;
            }

            std::cerr << "accept4 failed: listen_fd=" << server_fd_
                      << ", errno=" << error
                      << " (" << std::strerror(error) << ")\n";
            return;
        }

        //给客户端分配文件描述符成功
        std::cout << "有新的客户端连进来了！！！新成员的文件描述符是：" << client_fd << "\n";

        //将新的设备挂在到epoll_fd的监听红黑树上面
        if (!loop_->addFd(client_fd, EPOLLIN | EPOLLRDHUP | EPOLLET))
        {
            ::close(client_fd);
            continue;
        }

        auto conn = std::make_shared<Connection>(client_fd, loop_);
        connections_[client_fd] = conn;

        conn->on_message_ = [this](Connection* conn, const char* data, size_t len) {
            onMessage(conn, data, len);
        };

        conn->on_close_ = [this](Connection* conn) {
            onClose(conn);
        };

        loop_->pushTimer(
            client_fd,
            kConnectionTimeoutSeconds
        );

        std::weak_ptr<Connection> weak_conn = conn;

        loop_->setHandler(client_fd, [weak_conn](uint32_t events) {
            auto conn = weak_conn.lock();
            if (!conn) {
                return;
            }

            if (events & EPOLLERR) {
                conn->handleError();
                return;
            }


            if (events & EPOLLIN) {
                conn->handleRead();

                if (conn->isClosing()) {
                    return;
                }
            }

            if (events & (EPOLLHUP | EPOLLRDHUP)) {
                std::cerr << "connection hangup: fd=" << conn->fd_
                  << ", EPOLLHUP="
                  << ((events & EPOLLHUP) != 0)
                  << ", EPOLLRDHUP="
                  << ((events & EPOLLRDHUP) != 0)
                  << '\n';

                conn->requestClose();
                return;
            }


            if (events & EPOLLOUT) {
                conn->handleWrite();
            }

        });

        // std::cout << "卡死在265行了";
    }
}

void Server::onClose(Connection* conn) {
    //如果当前的客户端已经被关闭了,不用重复关闭
    if (conn == nullptr)
    {
        return;
    }
    auto it = connections_.find(conn->fd_);
    if (it == connections_.end() || it->second.get() != conn)
    {//防止fd复用的时候删错连接
        return;
    }
    auto closing_conn = it->second;
    const int fd = closing_conn->fd_;
    //从映射表中抹除掉(只有当客户端是采集端的时候才需要从映射表中抹除)
    if (closing_conn->role_ == Connection::Role::Collector) {
        auto device_it = device_id_to_fd_.find(closing_conn->device_id_);

        if (device_it != device_id_to_fd_.end() && device_it->second == fd) {
            device_id_to_fd_.erase(device_it);
        }
    }

    if (closing_conn->role_ == Connection::Role::Monitor) {
        auto subscriptions_it =
            monitor_subscriptions_.find(fd);

        if (subscriptions_it != monitor_subscriptions_.end()) {
            for (uint16_t device_id : subscriptions_it->second) {
                auto subscribers_it =
                    device_subscribers_.find(device_id);

                if (subscribers_it == device_subscribers_.end()) {
                    continue;
                }

                subscribers_it->second.erase(fd);

                if (subscribers_it->second.empty()) {
                    device_subscribers_.erase(subscribers_it);
                }
            }

            monitor_subscriptions_.erase(subscriptions_it);
        }
    }

    //从连接列表中抹除掉
    loop_->removeHandler(fd);
    connections_.erase(it);

}

void Server::publish(
    uint16_t device_id,
    MsgType type,
    const char* payload,
    size_t payload_len
) {
    if (payload_len > MAX_PAYLOAD_SIZE) {
        std::cerr << "publish payload too large: device_id="
                  << device_id
                  << ", payload_len=" << payload_len
                  << '\n';
        return;
    }

    if (payload_len > 0 && payload == nullptr) {
        std::cerr << "publish received null payload: device_id="
                  << device_id << '\n';
        return;
    }

    MsgHeader header{};
    header.magic = htons(MAGIC_NUMBER);
    header.version = 1;
    header.type = htons(static_cast<uint16_t>(type));
    header.length =
        htonl(static_cast<uint32_t>(payload_len));
    header.id = htons(device_id);
    header.organization = htons(0);

    std::vector<char> message(
        sizeof(MsgHeader) + payload_len
    );

    std::memcpy(
        message.data(),
        &header,
        sizeof(MsgHeader)
    );

    if (payload_len > 0) {
        std::memcpy(
            message.data() + sizeof(MsgHeader),
            payload,
            payload_len
        );
    }

    auto subscribers_it =
        device_subscribers_.find(device_id);

    if (subscribers_it == device_subscribers_.end()) {
        return;
    }

    const auto subscriber_fds = subscribers_it->second;

    for (int subscriber_fd : subscriber_fds) {
        auto connection_it =
            connections_.find(subscriber_fd);

        if (connection_it == connections_.end()) {
            continue;
        }

        auto subscriber = connection_it->second;

        if (subscriber->role_ != Connection::Role::Monitor || subscriber->isClosing()) {
            continue;
        }

        subscriber->send(
            message.data(),
            message.size()
        );
    }
}


void Server::onMessage(Connection* conn, const char* data, size_t len) {
    MsgHeader header;
    memcpy(&header, data, sizeof(MsgHeader));
    if (ntohs(header.type) == static_cast<uint16_t>(MsgType::HeartBeat)) {
        conn->send(reinterpret_cast<char*>(&header),sizeof(header));
    }
    else if (ntohs(header.type) == static_cast<uint16_t>(MsgType::RegisterCollector)) {
        const uint16_t device_id = ntohs(header.id);

        if (conn->role_ == Connection::Role::Monitor) {
            std::cerr << "Monitor cannot register as Collector: fd="
                      << conn->fd_ << '\n';

            conn->requestClose();
            return;
        }

        if (conn->role_ == Connection::Role::Collector
            && conn->device_id_ != device_id) {
            std::cerr << "Collector cannot change device_id: fd="
                      << conn->fd_
                      << ", old_device_id=" << conn->device_id_
                      << ", new_device_id=" << device_id
                      << '\n';

            conn->requestClose();
            return;
        }

        auto device_it = device_id_to_fd_.find(device_id);

        if (device_it != device_id_to_fd_.end()
            && device_it->second != conn->fd_) {
            std::cerr << "duplicate Collector registration: device_id="
                      << device_id
                      << ", existing_fd=" << device_it->second
                      << ", rejected_fd=" << conn->fd_
                      << '\n';

            conn->requestClose();
            return;
        }

        conn->role_ = Connection::Role::Collector;
        conn->device_id_ = device_id;
        device_id_to_fd_[device_id] = conn->fd_;

        std::cout << "Collector fd=" << conn->fd_
                  << " registered device_id="
                  << device_id << '\n';

    }
    else if (ntohs(header.type) == static_cast<uint16_t>(MsgType::RegisterMonitor)) {
        //目前即使是离线的设备也能够被订阅了

        if (conn->role_ == Connection::Role::Collector) {
            std::cerr << "Collector cannot register as Monitor: fd="
                      << conn->fd_ << '\n';

            conn->requestClose();
            return;
        }

        conn->role_ = Connection::Role::Monitor;

        const uint16_t device_id = ntohs(header.id);

        device_subscribers_[device_id].insert(conn->fd_);
        monitor_subscriptions_[conn->fd_].insert(device_id);

        std::cout << "Monitor fd=" << conn->fd_
                  << " subscribed to device_id="
                  << device_id << '\n';

    }
    else if (ntohs(header.type) == static_cast<uint16_t>(MsgType::VideoFrame)) {
        const uint16_t device_id = ntohs(header.id);
        const size_t payload_len = ntohl(header.length);

        if (conn->role_ != Connection::Role::Collector) {
            std::cerr << "VideoFrame from non-Collector: fd="
                      << conn->fd_ << '\n';

            conn->requestClose();
            return;
        }

        auto device_it = device_id_to_fd_.find(device_id);

        if (conn->device_id_ != device_id
            || device_it == device_id_to_fd_.end()
            || device_it->second != conn->fd_) {
            std::cerr << "Collector identity mismatch: fd="
                      << conn->fd_
                      << ", registered_device_id="
                      << conn->device_id_
                      << ", message_device_id="
                      << device_id << '\n';

            conn->requestClose();
            return;
        }

        if (len != sizeof(MsgHeader) + payload_len) {
            std::cerr << "VideoFrame size mismatch: fd="
                      << conn->fd_
                      << ", packet_len=" << len
                      << ", payload_len=" << payload_len
                      << '\n';

            conn->requestClose();
            return;
        }

        publish(
            device_id,
            MsgType::VideoFrame,
            data + sizeof(MsgHeader),
            payload_len
        );
    }
}
