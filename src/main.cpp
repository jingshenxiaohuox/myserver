#include "EventLoop.h"
#include "Server.h"

int main() {
    EventLoop loop;
    Server server(8081, &loop);
    server.start();
    loop.run();
}