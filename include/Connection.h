#pragma once
#include <functional>
#include <ctime>
#include "ringbuffer.h"


class Connection {
public:
    enum class Role {Unknow, Collector, Monitor};

    Connection(int fd, int epoll_fd);
    ~Connection();

    void handleRead();
    void handleWrite();
    void send(const char* data, size_t len);

    std::function<void(Connection*, const char*, size_t)> on_message_;
    std::function<void(Connection*)> on_close_;

    int fd_;
    int epoll_fd_;
    Role role_ = Role::Unknow;
    uint16_t device_id_ = 0;
    time_t last_active_time_;

private:
    RingBuffer recv_buffer_;
    RingBuffer send_buffer_;
    void updateEpollout();
};