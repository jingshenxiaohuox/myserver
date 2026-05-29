#pragma once
#include <vector>
#include <cstddef>


class RingBuffer {
    //1.零拷贝消费
    //2.支持偷看,协议解析时,先查看完整帧再消费
    //3.自动处理回绕,使用线性数组模拟环
    //4.利用多分配的一个字节来判断当前是环满还是环空的状态
    //5.适合链接级上下文,每个链接分配一个ring buffer
public:
    //默认给每个链接分配8KB的缓冲区
    explicit RingBuffer(size_t capacity = 8192);

    //可读的剩余空间(有这么多数据等着被解析)
    size_t readableBytes() const;

    //可写的剩余空间
    size_t writeableBytes() const;

    //把epoll recv到的网络数据放进环形缓冲区
    bool append(const char* data, size_t len) ;

    //检查用于检查包头,不移动read_idx
    void peek(char* dest, size_t len) const;
    
    //提取数据并移动读指针
    void retrieve(size_t len);

    //判断当前的缓冲区是否是空的
    bool empty() const;

private:
    std::vector<char> buffer_;
    size_t capacity_;//物理的总容量
    size_t read_idx_;//读指针
    size_t write_idx_;//写指针
};