


class Eventloop {
public:
private:
    int epoll_fd_;
    epoll_event events_[MAX_EVENTS];
    unordered_map<int, functional<void()>>;
}