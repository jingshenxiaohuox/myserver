#pragma once
#include <cstdint>
#include <unordered_map>
#include <vector> 
#include <memory> //智能指针
#include "EventLoop.h"

class Connection;

class Server {
public:
    Server(int port, EventLoop* loop);
    ~Server();
    void start();
    void acceptNewConnection();
    void onMessage(Connection*, const char*, size_t);
    void onClose(Connection*);
private:
    int port_;
    EventLoop* loop_;
    int server_fd_;
    std::unordered_map<int, std::unique_ptr<Connection>> connections_;
    std::unordered_map<uint16_t, int> device_id_to_fd_;
    std::unordered_map<int, std::vector<int>> subscribers_;
    std::unordered_map<int, std::vector<int>> subscribed_to_;
};