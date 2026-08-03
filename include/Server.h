#pragma once
#include <cstdint>
#include <unordered_map>
#include <memory> //智能指针
#include "EventLoop.h"
#include <cstddef>
#include <unordered_set>
#include "protocol.h"


class Connection;

class Server {
public:
    Server(int port, EventLoop* loop);
    ~Server();
    void start();
    void acceptNewConnection();
    void onMessage(Connection*, const char*, size_t);
    void onClose(Connection*);
    void publish(
        uint16_t device_id,
        MsgType type,
        const char* payload,
        size_t payload_len
    );

private:
    int port_;
    EventLoop* loop_;
    int server_fd_;
    std::unordered_map<int, std::shared_ptr<Connection>> connections_;
    std::unordered_map<uint16_t, int> device_id_to_fd_;
    std::unordered_map<uint16_t, std::unordered_set<int>> device_subscribers_;
    std::unordered_map<int, std::unordered_set<uint16_t>> monitor_subscriptions_;

};
