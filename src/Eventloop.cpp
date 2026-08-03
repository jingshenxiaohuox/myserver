#include <EventLoop.h>
#include <unistd.h>   // close
#include <sys/epoll.h> // epoll_create1, epoll_ctl, epoll_wait
#include <cerrno>
#include <system_error>
#include <cstring>
#include <iostream>
#include <string>

EventLoop::EventLoop()
    : epoll_fd_(epoll_create1(EPOLL_CLOEXEC))
    {
    //防止epoll创建失败继续执行
        if (epoll_fd_ == -1)
        {
            const int error = errno;
            throw std::system_error(
                error,
                std::generic_category(),
                "epoll_create1"
            );
        }
    }

EventLoop::~EventLoop() {
    close(epoll_fd_);
}

bool EventLoop::addFd(int fd, uint32_t events) {
    struct epoll_event ev{};
    ev.events = events;
    ev.data.fd = fd;

    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) == -1)
    {
        const int error = errno;
        std::cerr << "epoll_ctl(ADD) failed: fd=" << fd
                << ", errno=" << error
                << " (" << std::strerror(error) << ")\n";
        return false;
    }

    return true;
}

bool EventLoop::modFd(int fd, uint32_t events) {
    struct epoll_event ev{};
    ev.events = events;
    ev.data.fd = fd;

    if (epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev) == -1)
    {
        const int error = errno;
        std::cerr << "epoll_ctl(MOD) failed: fd=" << fd
                << ", errno=" << error
                << " (" << std::strerror(error) << ")\n";
        return false;
    }

    return true;
}

void EventLoop::delFd(int fd) {
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr) == -1)
    {
        const int error = errno;
        std::cerr << "epoll_ctl(DEL) failed: fd=" << fd
                << ", errno=" << error
                << " (" << std::strerror(error) << ")\n";
    }
}

void EventLoop::setHandler(int fd, std::function<void(uint32_t)> handler) {
    handlers_[fd] = std::move(handler);//移动语义
}

void EventLoop::removeHandler(int fd) {
    handlers_.erase(fd);
}

void EventLoop::pushTimer(int fd, int timeout_sec) {
    timer_heap_.push({time(nullptr) + timeout_sec, fd});
}

void EventLoop::setTimeoutCallback(std::function<void(int)> cb) {
    on_timeout_ = std::move(cb);
}

void EventLoop::run() {
    while (true) {
        //epollwait进入一个for循环
        int ready_num = epoll_wait(epoll_fd_, events_, MAX_EVENTS, 5000);
        if (ready_num == -1)
        {
            const int error = errno;

            if (error == EINTR)
            {
                checkTimers();
                continue;
            }

            const std::string context =
                "epoll_wait: epoll_fd=" + std::to_string(epoll_fd_)
                + ", errno=" + std::to_string(error);

            throw std::system_error(
                error,
                std::generic_category(),
                context
            );
        }
        
        //检查定时器(不管有没有事件都要检查)
        checkTimers();

        //遍历就绪事件
        for (int i = 0; i < ready_num; i++) {
            auto it = handlers_.find(events_[i].data.fd);
            if (it != handlers_.end()) {
                auto handler = it->second;
                handler(events_[i].events);
            }
            
        }
    }
        
}

void EventLoop::checkTimers() {
    time_t now = time(nullptr);
    while (!timer_heap_.empty()) {
        TimerEntry time_top = timer_heap_.top();
        if (now < time_top.expire) break;
        timer_heap_.pop();
        if (on_timeout_) on_timeout_(time_top.fd);
    }
}