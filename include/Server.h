#pragma once
#include <cstdint>
#include <unordered_map>
#include <vector> 
#include <memory> //智能指针

class Connection;

class Server {
public:
    Server(int port, int epoll_fd);
    ~Server();
    void start();
    void acceptNewConnection();
    void onMessage(Connection*, const char*, size_t);
    void onClose(Connection*);
private:
    int port_;
    int epoll_fd_;
    int server_fd_;
    std::unordered_map<int, unique_ptr<Connection>> connections_;
    std::unordered_map<uint16_t, int> device_id_to_fd_;
    std::unordered_map<int, vector<int>> subscribers_;
    std::unordered_map<int, vector<int>> subscribed_to_;
};