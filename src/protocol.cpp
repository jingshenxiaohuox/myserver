#include "protocol.h"
#include <iostream>

bool isValidHeader(const MsgHeader& header) {
    if (header.magic != MAGIC_NUMBER || header.version != 1 || header.length >= MAX_PAYLOAD_SIZE) {
        std::cerr << "这个包头有问题！！！\n";
        return false;
    }
    return true;
}