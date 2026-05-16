#pragma once
#include <cstdint>

#pragma pack(push, 1)//防止内存对齐,由于编译器处理机制不同,无强制对齐可能导致消息错误解译
//协议规定
struct MsgHeader {
    uint16_t magic;//用来识别垃圾包
    uint8_t  version;//增加版本号,用来识别不同的协议版本
    uint16_t type;//发送来的数据包类型:心跳包,内容包
    uint32_t length;//包的长度
    uint16_t id;//发送设备id
    uint16_t organization;//消息的发送者的组织
 };
#pragma pack(pop)