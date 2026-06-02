cmake_minimum_required(VERSION 3.10)
project(MyTcpServer)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -g -O0")

add_executable(gateway_server
    src/main.cpp
    src/ringbuffer.cpp
    src/protocol.cpp
    src/Connection.cpp
    src/Server.cpp
    src/Eventloop.cpp
)

add_executable(bench_client
    test/bench_client.cpp
    src/ringbuffer.cpp
    src/protocol.cpp
)

target_include_directories(gateway_server PRIVATE include)
target_include_directories(bench_client PRIVATE include)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)