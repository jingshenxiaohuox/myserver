#pragma once
#include <cstdint>

static constexpr uint16_t MAGIC_NUMBER = 520;
static constexpr uint32_t MAX_PAYLOAD_SIZE = 4 * 1024 * 1024; //4MB

enum class MsgType : uint16_t {
    HeartBeat = 0x0001,
    VideoFrame = 0x0002,
    RegisterCollector = 0x0003,//告诉网关自己是采集端
    RegisterMonitor = 0x0004,//告诉网关自己是监控端
};


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

bool isValidHeader(const MsgHeader& header);