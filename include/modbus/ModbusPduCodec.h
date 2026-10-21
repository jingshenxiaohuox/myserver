#pragma once

#include "modbus/ModbusTypes.h"

#include <cstdint>
#include <vector>

namespace modbus
{
    enum class DecodeStatus
    {
        Success,
        ExceptionResponse,
        MalformedPdu
    };

    struct ReadHoldingRegistersDecodeResult
    {
        DecodeStatus status = DecodeStatus::MalformedPdu;
        ReadHoldingRegistersResponse response;
        std::uint8_t exception_code = 0;
    };

    bool encodeReadHoldingRegistersRequest(
        const ReadHoldingRegistersRequest& request,
        std::vector<std::uint8_t>& pdu
    );

    ReadHoldingRegistersDecodeResult
    decodeReadHoldingRegistersResponse(
        const std::vector<std::uint8_t>& pdu,
        std::uint16_t expected_quantity
    );
} // namespace modbus
