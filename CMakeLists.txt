cmake_minimum_required(VERSION 3.10)
project(MyTcpServer)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 开启所有常见警告，培养良好的编码习惯
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -g -O0")

# 我们的主程序叫 gateway_server
add_executable(gateway_server
    src/main.cpp
    src/ringbuffer.cpp
    src/protocol.cpp
)
add_executable(bench_client
    test/bench_client.cpp
    src/ringbuffer.cpp
    src/protocol.cpp
)

target_include_directories(bench_client PRIVATE include)
target_include_directories(gateway_server PRIVATE include)


set(CMAKE_EXPORT_COMPILE_COMMANDS ON)