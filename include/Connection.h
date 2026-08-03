#pragma once
#include <functional>
#include <ctime>
#include "ringbuffer.h"
#include <cstdint>

class EventLoop;
class Connection {
public:
    enum class Role {Unknow, Collector, Monitor};

    Connection(int fd, EventLoop* loop);
    ~Connection();

    void handleRead();
    void handleWrite();
    void send(const char* data, size_t len);
    void requestClose();
    bool isClosing() const { return closing_;  }
    void handleError();

    std::function<void(Connection*, const char*, size_t)> on_message_;
    std::function<void(Connection*)> on_close_;

    int fd_;
    EventLoop* loop_;
    Role role_ = Role::Unknow;
    uint16_t device_id_ = 0;
    time_t last_active_time_;

private:
    RingBuffer recv_buffer_;
    RingBuffer send_buffer_;
    bool closing_ = false;
    void updateEpollout();
};
