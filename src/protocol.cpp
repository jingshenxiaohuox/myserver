#include "protocol.h"
#include <iostream>
#include <arpa/inet.h>

bool isValidHeader(const MsgHeader& header) {
    if (ntohs(header.magic) != MAGIC_NUMBER || 
        header.version != 1 || 
        ntohl(header.length) >= MAX_PAYLOAD_SIZE) {
        std::cerr << "这个包头有问题！！！\n";
        return false;
    }
    return true;
}