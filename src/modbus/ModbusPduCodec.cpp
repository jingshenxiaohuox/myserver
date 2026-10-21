#include "modbus/ModbusPduCodec.h"

#include <cstddef>

namespace modbus
{
    bool encodeReadHoldingRegistersRequest(
        const ReadHoldingRegistersRequest& request,
        std::vector<std::uint8_t>& pdu
    )
    {
        pdu.clear();

        constexpr std::uint16_t kMinQuantity = 1;
        constexpr std::uint16_t kMaxQuantity = 125;

        if (request.quantity < kMinQuantity
            || request.quantity > kMaxQuantity)
        {
            return false;
        }

        pdu.reserve(5);

        pdu.push_back(
            static_cast<std::uint8_t>(
                FunctionCode::ReadHoldingRegisters
            )
        );

        pdu.push_back(
            static_cast<std::uint8_t>(
                (request.start_address >> 8) & 0xFF
            )
        );
        pdu.push_back(
            static_cast<std::uint8_t>(
                request.start_address & 0xFF
            )
        );

        pdu.push_back(
            static_cast<std::uint8_t>(
                (request.quantity >> 8) & 0xFF
            )
        );
        pdu.push_back(
            static_cast<std::uint8_t>(
                request.quantity & 0xFF
            )
        );

        return true;
    }

    ReadHoldingRegistersDecodeResult
    decodeReadHoldingRegistersResponse(
        const std::vector<std::uint8_t>& pdu,
        std::uint16_t expected_quantity
    )
    {
        ReadHoldingRegistersDecodeResult result{};

        constexpr std::uint16_t kMinQuantity = 1;
        constexpr std::uint16_t kMaxQuantity = 125;

        if (expected_quantity < kMinQuantity
            || expected_quantity > kMaxQuantity)
        {
            return result;
        }

        if (pdu.empty())
        {
            return result;
        }

        const std::uint8_t expected_function_code =
            static_cast<std::uint8_t>(
                FunctionCode::ReadHoldingRegisters
            );

        const std::uint8_t exception_function_code =
            static_cast<std::uint8_t>(
                expected_function_code | 0x80
            );

        if (pdu[0] == exception_function_code)
        {
            if (pdu.size() != 2)
            {
                return result;
            }

            result.status = DecodeStatus::ExceptionResponse;
            result.exception_code = pdu[1];
            return result;
        }

        if (pdu[0] != expected_function_code)
        {
            return result;
        }

        if (pdu.size() < 2)
        {
            return result;
        }

        const std::uint8_t byte_count = pdu[1];

        if (byte_count % 2 != 0)
        {
            return result;
        }

        const std::size_t expected_pdu_size =
            2 + static_cast<std::size_t>(byte_count);

        if (pdu.size() != expected_pdu_size)
        {
            return result;
        }

        const std::uint16_t expected_byte_count =
            static_cast<std::uint16_t>(
                expected_quantity * 2
            );

        if (byte_count != expected_byte_count)
        {
            return result;
        }

        result.response.registers.reserve(expected_quantity);

        for (std::size_t index = 2;
             index < pdu.size();
             index += 2)
        {
            const std::uint16_t register_value =
                static_cast<std::uint16_t>(
                    static_cast<std::uint16_t>(pdu[index]) << 8
                )
                | static_cast<std::uint16_t>(pdu[index + 1]);

            result.response.registers.push_back(register_value);
        }

        result.status = DecodeStatus::Success;
        return result;
    }
} // namespace modbus
