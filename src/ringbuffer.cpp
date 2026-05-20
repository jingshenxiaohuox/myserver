#include "ringbuffer.h"
#include <cstring>
#include <algorithm>
#include <iostream>

//默认给每个链接分配8KB的缓冲区
RingBuffer::RingBuffer(size_t capacity) //explicit可以禁止编译器进行隐式的类型转换
    : buffer_(capacity + 1), capacity_(capacity + 1), read_idx_(0), write_idx_(0) {//成员初始化列表
    // 多分配一字节用来区分缓冲区是满还是空

}

//可读的剩余空间(有这么多数据等着被解析)
size_t RingBuffer::readableBytes() const {
    if (write_idx_ >= read_idx_) {//证明现在写指针在读指针前面,有东西可读
        return write_idx_ - read_idx_;
    }
    return capacity_ - read_idx_ + write_idx_;//发生回绕,写指针跑到读指针的前面了,可读部分就变成了从读指针到缓冲区尾和从缓冲区头到写指针
}

//可写的剩余空间
size_t RingBuffer::writeableBytes() const {
    return capacity_ - 1 - readableBytes();
}

//把epoll recv到的网络数据放进环形缓冲区
void RingBuffer::append(const char* data, size_t len) {//data是要写入的数据,len是要写入的长度
    if (len > writeableBytes()) {
        //缓冲区满就记录日志或者断开连接
        std::cerr << "缓冲区溢出,抛弃数据\n";
        return;
    }

    size_t first_chunk_len = std::min(len, capacity_ - write_idx_);
    std::memcpy(buffer_.data() + write_idx_, data, first_chunk_len);

    if(first_chunk_len < len) {
        std::memcpy(buffer_.data(), data + first_chunk_len, len - first_chunk_len);
    }

    write_idx_ = (write_idx_ + len) % capacity_;

    //如果数据在末尾写不下,折返到头部继续写

}

//检查用于检查包头,不移动read_idx
void RingBuffer::peek(char* dest, size_t len) const {
    if (len > readableBytes()) return;

    size_t first_chunk_len = std::min(len, capacity_ - read_idx_);
    std::memcpy(dest, buffer_.data() + read_idx_, first_chunk_len);

    if (first_chunk_len < len) {
        std::memcpy(dest + first_chunk_len, buffer_.data(), len - first_chunk_len);
    }
}

//提取数据并移动读指针
void RingBuffer::retrieve(size_t len) {
    if (len > readableBytes()) {
        read_idx_ = write_idx_;
    }
    else {
        read_idx_ = (read_idx_ + len) % capacity_;
    }
}

bool RingBuffer::empty() const{
    return read_idx_ == write_idx_;
}