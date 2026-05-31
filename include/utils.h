#pragma once
#include <fcntl.h>


inline void setNonBlocking(int fd) { //定义一个函数用于设置文件描述符为非阻塞模式
    int flags = fcntl(fd, F_GETFL, 0); //获取文件描述符的当前标志
    fcntl(fd, F_SETFL, flags | O_NONBLOCK); //将非阻塞标志添加到当前标志中
}