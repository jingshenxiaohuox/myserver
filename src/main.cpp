#include "EventLoop.h"
#include "Server.h"
#include <iostream>
#include <exception>

int main() {
    try
    {
        EventLoop loop;
        Server server(8081, &loop);
        server.start();
        loop.run();
    }
    catch (const std::exception& error)
    {
        std::cerr << "网关启动失败: " << error.what() << std::endl;
        return 1;
    }

    return 0;
}
