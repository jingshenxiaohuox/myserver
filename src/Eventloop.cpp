#include <EventLoop.h>
#include <unistd.h>   // close
#include <sys/epoll.h> // epoll_create1, epoll_ctl, epoll_wait

EventLoop::EventLoop() {
    epoll_fd_ = epoll_create1(0);
}

EventLoop::~EventLoop() {
    close(epoll_fd_);
}

void EventLoop::addFd(int fd, uint32_t events) {
    struct epoll_event ev{};
    ev.events = events;
    ev.data.fd = fd;
    epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev);
}

void EventLoop::modFd(int fd, uint32_t events) {
    struct epoll_event ev{};
    ev.events = events;
    ev.data.fd = fd;
    epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev);
}

void EventLoop::delFd(int fd) {
    epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
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

        
        //检查定时器(不管有没有事件都要检查)
        checkTimers();

        //遍历就绪事件
        for (int i = 0; i < ready_num; i++) {
            auto it = handlers_.find(events_[i].data.fd);
            if (it != handlers_.end()) {
                it->second(events_[i].events);
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