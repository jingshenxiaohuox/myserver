//
// Created by ljq on 2026/8/3.
//
#pragma once

#include <cstdint>
#include <vector>

namespace modbus
{
    enum class FunctionCode : std::uint8_t
    {
        ReadHoldingRegisters = 0x03
    };

    enum class ExceptionCode : std::uint8_t
    {
        IllegalFunction = 0x01,
        IllegalDataAddress = 0x02,
        IllegalDataValue = 0x03,
        ServerDeviceFailure = 0x04
    };

    struct ReadHoldingRegistersRequest
    {
        std::uint16_t start_address = 0;
        std::uint16_t quantity = 0;
    };

    struct ReadHoldingRegistersResponse
    {
        std::vector<std::uint16_t> registers;
    };
}
