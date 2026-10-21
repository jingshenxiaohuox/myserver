//
// Created by ljq on 2026/8/3.
//

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>


namespace modbus
{
    enum class TcpDecodeStatus
    {
        Success,
        NeedMoreData,
        MalformedAdu   //格式错误
    };

    struct ModbusTcpFrame
    {
        std::uint16_t transaction_id = 0;
        std::uint8_t unit_id = 0;       //用来标识不同的从设备
        std::vector<std::uint8_t> pdu;  //这个是modbus的消息体
    };

    struct ModbusTcpDecodeResult
    {
        TcpDecodeStatus status = TcpDecodeStatus::MalformedAdu;   //这个是解码的结果状态类型,用的枚举类型
        ModbusTcpFrame frame;                                     //解析后的结构化字段
        std::size_t consumed_bytes = 0;                           //
    };

    bool encodeModbusTcpFrame(                                    //将结构化的字段编码成最终给socket的二进制
        const ModbusTcpFrame& frame,
        std::vector<std::uint8_t>& adu
        );

    ModbusTcpDecodeResult decodeModbusTcpFrame(                   //从二进制中解析出来结构化字段
        const std::vector<std::uint8_t>& bytes
        );
}
