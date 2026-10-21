//
// Created by ljq on 2026/8/3.
//
#include "modbus/tcp/ModbusTcpCodec.h"

namespace modbus
{
    bool encodeModbusTcpFrame(                           //将结构化的字段编码成最终给socket的二进制
        const ModbusTcpFrame& frame,
        std::vector<std::uint8_t>& adu
    )
    {
        adu.clear(); //清空缓冲区中旧数据

        if (frame.pdu.empty())
        {
            return false;
        }
        if (frame.pdu.size() > 253) //modbustcp的pdu最大长度是253
        {
            return false;
        }

        const std::uint16_t mbap_length = static_cast<std::uint16_t>(1 + frame.pdu.size());
        adu.reserve(7 + frame.pdu.size());
        //填充transaction_id字段
        adu.push_back(
            static_cast<std::uint8_t>(
                (frame.transaction_id >> 8) & 0xFF
            )
        );
        adu.push_back(
            static_cast<std::uint8_t>(
                frame.transaction_id & 0xFF
            )
        );
        //填充protocol id字段
        adu.push_back(0x00);
        adu.push_back(0x00);
        //填入length字段
        adu.push_back(
            static_cast<std::uint8_t>(
                (mbap_length >> 8) & 0xFF
            )
        );
        adu.push_back(
            static_cast<std::uint8_t>(
                mbap_length & 0xFF
            )
        );
        //填入unit_id字段
        adu.push_back(
            frame.unit_id
        );
        //填入完整的pdu
        adu.insert(
            adu.end(),
            frame.pdu.begin(),
            frame.pdu.end()
        );

        return true;
    }

}
