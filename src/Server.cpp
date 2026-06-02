#include "Server.h"
#include <cstdint>
#include "utils.h"
#include <csignal>
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

    //注册server_fd的handler
    loop_->setHandler(server_fd_, [this](uint32_t) {
        acceptNewConnection();
    });
    //注册超时回调
    loop_->setTimeoutCallback([this](int fd) {
        // 第一步：fd 还在不在
        if (connections_.find(fd) == connections_.end()) return;
        
        // 第二步：心跳有没有刷新
        time_t now = time(nullptr);
        if (connections_[fd]->last_active_time_ + 30 > now) return;
        
        // 走到这里说明真的超时了，踢掉
        onClose(connections_[fd].get());
    });

    //设置监听套接字为非阻塞监听
    setNonBlocking(server_fd_);

    //将监听套接字添加到epoll实例中
    loop_->addFd(server_fd_, EPOLLIN | EPOLLET);

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
        connections_[client_fd] = std::make_unique<Connection>(client_fd, loop_);//怎么创建一个独占指着对象来着?


        //将新的设备挂在到epoll_fd的监听红黑树上面，并设置监听模式为非阻塞式监听
        setNonBlocking(client_fd);
        connections_[client_fd]->on_message_ = [this](Connection* conn, const char* data, size_t len) {
            onMessage(conn, data, len);
        };
        connections_[client_fd]->on_close_ = [this](Connection* conn) {
            onClose(conn);
        };
        loop_->pushTimer(client_fd, 30);
        Connection* conn_ptr = connections_[client_fd].get();
        loop_->setHandler(client_fd, [conn_ptr](uint32_t events) {
            if (events & EPOLLIN)  conn_ptr->handleRead();
            if (events & EPOLLOUT) conn_ptr->handleWrite();
        });
        loop_->addFd(client_fd, EPOLLIN | EPOLLET);
        
        // std::cout << "卡死在265行了";
    }
}

void Server::onClose(Connection* conn) {
    //从映射表中抹除掉(只有当客户端是采集端的时候才需要从映射表中抹除)
    if (conn->role_ == Connection::Role::Collector) {
        //当前的客户端是采集端
        device_id_to_fd_.erase(conn->device_id_);
    }
    //从订阅者中抹除掉
    if (conn->role_ == Connection::Role::Collector) {
        //当前客户端是采集端
        //拿到自己的转发列表,知道都有谁订阅了自己
        for (auto i : subscribers_[conn->fd_]) {
            //把自己从订阅端的订阅列表中的移除
            std::vector<int>& vec = subscribed_to_[i];
            vec.erase(std::remove(vec.begin(), vec.end(), conn->fd_), vec.end());
        }
        subscribers_.erase(conn->fd_);
    }
    //从向谁订阅列表中抹除掉
    if (conn->role_ == Connection::Role::Monitor) {
        //当前客户端是订阅端
        //拿到自己的订阅列表,知道自己都订阅了谁
        for (auto i : subscribed_to_[conn->fd_]) {
            //把自己从采集端的转发列表中的移除
            std::vector<int>& vec = subscribers_[i];
            vec.erase(std::remove(vec.begin(), vec.end(), conn->fd_), vec.end());
        }
        subscribed_to_.erase(conn->fd_);
    }
    //从连接列表中抹除掉
    loop_->removeHandler(conn->fd_);
    connections_.erase(conn->fd_);
    


}

void Server::onMessage(Connection* conn, const char* data, size_t len) {
    MsgHeader header;
    memcpy(&header, data, sizeof(MsgHeader));
    if (ntohs(header.type) == static_cast<uint16_t>(MsgType::HeartBeat)) {
        conn->send(reinterpret_cast<char*>(&header),sizeof(header));
    }
    else if (ntohs(header.type) == static_cast<uint16_t>(MsgType::RegisterCollector)) {
        conn->role_ = Connection::Role::Collector;
        conn->device_id_ = ntohs(header.id);
        std::cout << "id: " << conn->device_id_ << "成功注册为采集端\n";
        device_id_to_fd_.emplace(conn->device_id_, conn->fd_);
    }
    else if (ntohs(header.type) == static_cast<uint16_t>(MsgType::RegisterMonitor)) {
        conn->role_ = Connection::Role::Monitor;
        conn->device_id_ = ntohs(header.id);
        std::cout << "id: " << conn->device_id_ << "成功注册为监控端\n";
        if (device_id_to_fd_.find(conn->device_id_) == device_id_to_fd_.end()) {
            std::cout << "试图订阅的采集端不存在!!!\n";//这个信息其实应该发给订阅端,不知道怎么直接发送一串字符串
            return;
        }
        subscribed_to_[conn->fd_].push_back(device_id_to_fd_[conn->device_id_]);
        subscribers_[device_id_to_fd_[conn->device_id_]].push_back(conn->fd_);
    }
    else if (ntohs(header.type) == static_cast<uint16_t>(MsgType::VideoFrame)) {
        for (int sub_fd : subscribers_[conn->fd_]) {
            connections_[sub_fd]->send(data, len);
        }
    }
}