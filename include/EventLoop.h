#pragma once
#include <functional>
#include <unordered_map>
#include <queue>
#include <vector>
#include <ctime>
#include <sys/epoll.h>

class EventLoop {
public:
    static constexpr int MAX_EVENTS = 1024;

    EventLoop();
    ~EventLoop();

    // ── epoll 封装 ──────────────────────────────────
    void addFd(int fd, uint32_t events);
    void modFd(int fd, uint32_t events);
    void delFd(int fd);

    // ── 事件分发表 ──────────────────────────────────
    void setHandler(int fd, std::function<void(uint32_t)> handler);
    void removeHandler(int fd);

    // ── 定时器 ──────────────────────────────────────
    void pushTimer(int fd, int timeout_sec);
    void setTimeoutCallback(std::function<void(int)> cb);

    // ── 主循环 ──────────────────────────────────────
    void run();

    int getEpollFd() const { return epoll_fd_; }

private:
    int epoll_fd_;
    struct epoll_event events_[MAX_EVENTS];

    std::unordered_map<int, std::function<void(uint32_t events)>> handlers_;

    struct TimerEntry {
        time_t expire;
        int fd;
        bool operator>(const TimerEntry& o) const { return expire > o.expire; }
    };
    std::priority_queue
        <TimerEntry,
        std::vector<TimerEntry>,
        std::greater<TimerEntry>> timer_heap_;

    std::function<void(int)> on_timeout_;

    void checkTimers();
};