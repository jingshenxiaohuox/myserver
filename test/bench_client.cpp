#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include "protocol.h"

const char* SERVER_IP = "127.0.0.1";
const int SERVER_PORT = 8081;

int connect_to_server() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(SERVER_PORT);
    inet_pton(AF_INET, SERVER_IP, &addr.sin_addr);
    connect(fd, (struct sockaddr*)&addr, sizeof(addr));
    return fd;

}

int main() {
    return 0;
}