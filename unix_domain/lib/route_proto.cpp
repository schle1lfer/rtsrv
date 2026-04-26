/**
 * @file route_proto.cpp
 * @brief Implementation of the high-level routing protocol.
 *
 * Compiled into both lib/route_proto.so (shared) and lib/route_proto.ar
 * (static).
 *
 * 
 * @date    2026
 */

#include "route_proto.hpp"

#include "logger.hpp"
#include "ud_proto.hpp"

#include <algorithm>
#include <cstring>
#include <format>

namespace routeproto
{

// ---------------------------------------------------------------------------
// Error category
// ---------------------------------------------------------------------------

namespace
{

/**
 * @brief Custom std::error_category for routeproto::RouteError codes.
 *
 * Registered as a singleton via route_error_category().
 */
class RouteErrorCategory final : public std::error_category
{
public:
    /**
     * @brief Returns the category name.
     * @return The string "routeproto".
     */
    [[nodiscard]] const char* name() const noexcept override
    {
        return "routeproto";
    }

    /**
     * @brief Maps a RouteError integer value to a human-readable message.
     * @param ev Integer representation of a RouteError enumerator.
     * @return Descriptive error string.
     */
    [[nodiscard]] std::string message(int ev) const override
    {
        switch (static_cast<RouteError>(ev))
        {
        case RouteError::InvalidMessage:
            return "invalid or truncated message";
        case RouteError::UnknownType:
            return "unknown message type";
        default:
            return "unknown route error";
        }
    }
};

/**
 * @brief Custom std::error_category for routeproto::ExchangeError codes.
 *
 * Registered as a singleton via exchange_error_category().
 */
class ExchangeErrorCategory final : public std::error_category
{
public:
    /**
     * @brief Returns the category name.
     * @return The string "routeproto.exchange".
     */
    [[nodiscard]] const char* name() const noexcept override
    {
        return "routeproto.exchange";
    }

    /**
     * @brief Maps an ExchangeError integer value to a human-readable message.
     * @param ev Integer representation of an ExchangeError enumerator.
     * @return Descriptive error string.
     */
    [[nodiscard]] std::string message(int ev) const override
    {
        switch (static_cast<ExchangeError>(ev))
        {
        case ExchangeError::InvalidData:
            return "invalid or truncated exchange data";
        case ExchangeError::LengthMismatch:
            return "exchange data_length field mismatch";
        case ExchangeError::CrcMismatch:
            return "exchange CRC-16 mismatch";
        case ExchangeError::CountMismatch:
            return "num_commands does not match command array size";
        default:
            return "unknown exchange error";
        }
    }
};

} // anonymous namespace

/**
 * @brief Returns the singleton RouteErrorCategory instance.
 * @return Reference to the global routeproto error category.
 */
const std::error_category& route_error_category() noexcept
{
    static const RouteErrorCategory instance;
    return instance;
}

/**
 * @brief Creates a std::error_code from a RouteError enumerator.
 * @param e The RouteError value to wrap.
 * @return Corresponding std::error_code in the route_error_category.
 */
std::error_code make_error_code(RouteError e) noexcept
{
    return {static_cast<int>(e), route_error_category()};
}

/**
 * @brief Returns the singleton ExchangeErrorCategory instance.
 * @return Reference to the global routeproto exchange error category.
 */
const std::error_category& exchange_error_category() noexcept
{
    static const ExchangeErrorCategory instance;
    return instance;
}

/**
 * @brief Creates a std::error_code from an ExchangeError enumerator.
 * @param e The ExchangeError value to wrap.
 * @return Corresponding std::error_code in the exchange_error_category.
 */
std::error_code make_error_code(ExchangeError e) noexcept
{
    return {static_cast<int>(e), exchange_error_category()};
}

// ---------------------------------------------------------------------------
// Big-endian helpers
// ---------------------------------------------------------------------------

namespace
{

/**
 * @brief Appends @p v as two big-endian bytes to @p out.
 * @param out Destination byte vector.
 * @param v   Value to serialise.
 */
void write_be16(std::vector<std::uint8_t>& out, std::uint16_t v)
{
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v));
}

/**
 * @brief Reads a big-endian uint16 from @p buf at @p offset.
 * @param buf    Source byte span.
 * @param offset Byte offset of the high byte.
 * @return Deserialized 16-bit value.
 */
std::uint16_t read_be16(std::span<const std::uint8_t> buf, std::size_t offset)
{
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(buf[offset]) << 8) |
        static_cast<std::uint16_t>(buf[offset + 1]));
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Convenience constructors
// ---------------------------------------------------------------------------

/// @brief @copybrief routeproto::make_ping
Message make_ping(std::uint16_t msg_id)
{
    return Message{
        .msg_type = MsgType::PING,
        .flags = FLAG_NONE,
        .msg_id = msg_id,
        .payload = {},
    };
}

/// @brief @copybrief routeproto::make_pong
Message make_pong(std::uint16_t msg_id)
{
    return Message{
        .msg_type = MsgType::PONG,
        .flags = FLAG_NONE,
        .msg_id = msg_id,
        .payload = {},
    };
}

/// @brief @copybrief routeproto::make_data
Message make_data(std::uint16_t msg_id, std::span<const std::uint8_t> payload)
{
    return Message{
        .msg_type = MsgType::DATA,
        .flags = FLAG_NONE,
        .msg_id = msg_id,
        .payload = {payload.begin(), payload.end()},
    };
}

/// @brief @copybrief routeproto::make_data_ack
Message make_data_ack(std::uint16_t msg_id, bool ok)
{
    const std::uint8_t status = ok ? 0x00 : 0x01;
    return Message{
        .msg_type = MsgType::DATA_ACK,
        .flags = FLAG_NONE,
        .msg_id = msg_id,
        .payload = {status},
    };
}

/// @brief @copybrief routeproto::make_error
Message make_error(std::uint16_t msg_id, const std::string& text)
{
    std::vector<std::uint8_t> pl(text.begin(), text.end());
    return Message{
        .msg_type = MsgType::ERROR,
        .flags = FLAG_NONE,
        .msg_id = msg_id,
        .payload = std::move(pl),
    };
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace
{

/**
 * @brief Maps a MsgType enumerator to its human-readable name.
 * @param t Message type.
 * @return String view of the type name (e.g. "DATA_ACK").
 */
std::string_view msg_type_name(MsgType t) noexcept
{
    switch (t)
    {
    case MsgType::PING:
        return "PING";
    case MsgType::PONG:
        return "PONG";
    case MsgType::DATA:
        return "DATA";
    case MsgType::DATA_ACK:
        return "DATA_ACK";
    case MsgType::ERROR:
        return "ERROR";
    default:
        return "UNKNOWN";
    }
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Encode
// ---------------------------------------------------------------------------

/// @brief @copybrief routeproto::encode_message
std::expected<std::vector<std::uint8_t>, std::error_code>
encode_message(const Message& msg)
{
    std::vector<std::uint8_t> out;
    out.reserve(MSG_HEADER_SIZE + msg.payload.size());

    out.push_back(static_cast<std::uint8_t>(msg.msg_type));
    out.push_back(msg.flags);
    write_be16(out, msg.msg_id);
    out.insert(out.end(), msg.payload.begin(), msg.payload.end());

    logger::log(logger::INFO,
                "routeproto",
                std::format("encode msg: type={} flags={:#04x} id={} "
                            "payload_bytes={} -> total_bytes={}",
                            msg_type_name(msg.msg_type),
                            msg.flags,
                            msg.msg_id,
                            msg.payload.size(),
                            out.size()));
    return out;
}

// ---------------------------------------------------------------------------
// Decode
// ---------------------------------------------------------------------------

/// @brief @copybrief routeproto::decode_message
std::expected<Message, std::error_code>
decode_message(std::span<const std::uint8_t> raw)
{
    if (raw.size() < MSG_HEADER_SIZE)
    {
        return std::unexpected(make_error_code(RouteError::InvalidMessage));
    }

    const auto type_byte = raw[0];
    // Validate known types.
    switch (static_cast<MsgType>(type_byte))
    {
    case MsgType::PING:
    case MsgType::PONG:
    case MsgType::DATA:
    case MsgType::DATA_ACK:
    case MsgType::ERROR:
        break;
    default:
        return std::unexpected(make_error_code(RouteError::UnknownType));
    }

    Message msg;
    msg.msg_type = static_cast<MsgType>(type_byte);
    msg.flags = raw[1];
    msg.msg_id = read_be16(raw, 2);
    msg.payload.assign(raw.begin() + MSG_HEADER_SIZE, raw.end());

    logger::log(logger::INFO,
                "routeproto",
                std::format("decode msg: type={} flags={:#04x} id={} "
                            "payload_bytes={}",
                            msg_type_name(msg.msg_type),
                            msg.flags,
                            msg.msg_id,
                            msg.payload.size()));
    return msg;
}

// ---------------------------------------------------------------------------
// Exchange-data encode / decode
// ---------------------------------------------------------------------------

/// @brief @copybrief routeproto::encode_exchange
std::expected<std::vector<std::uint8_t>, std::error_code>
encode_exchange(const ExchangeData& ed)
{
    // data_length covers: num_commands(2) + data_length(2) + commands(N) +
    // crc(2)
    const auto num_cmds = static_cast<std::uint16_t>(ed.commands.size());
    const auto data_length =
        static_cast<std::uint16_t>(EXCHANGE_MIN_SIZE + ed.commands.size());

    std::vector<std::uint8_t> out;
    out.reserve(data_length);

    write_be16(out, num_cmds);
    write_be16(out, data_length);
    out.insert(out.end(), ed.commands.begin(), ed.commands.end());

    // CRC covers everything written so far.
    const auto checksum = udproto::crc16(std::span<const std::uint8_t>{out});
    write_be16(out, checksum);

    logger::log(logger::INFO,
                "routeproto",
                std::format("encode exchange: num_commands={} data_length={}",
                            num_cmds,
                            data_length));
    return out;
}

/// @brief @copybrief routeproto::decode_exchange
std::expected<ExchangeData, std::error_code>
decode_exchange(std::span<const std::uint8_t> raw)
{
    if (raw.size() < EXCHANGE_MIN_SIZE)
    {
        return std::unexpected(make_error_code(ExchangeError::InvalidData));
    }

    // Field: data_length — must equal the total buffer size.
    const auto data_length = read_be16(raw, 2);
    if (static_cast<std::size_t>(data_length) != raw.size())
    {
        return std::unexpected(make_error_code(ExchangeError::LengthMismatch));
    }

    // CRC validation: covers all bytes except the final 2.
    const auto crc_stored = read_be16(raw, raw.size() - 2);
    const auto crc_computed = udproto::crc16(raw.subspan(0, raw.size() - 2));
    if (crc_stored != crc_computed)
    {
        return std::unexpected(make_error_code(ExchangeError::CrcMismatch));
    }

    // Field: num_commands — must match the number of command bytes present.
    const auto num_commands = read_be16(raw, 0);
    const std::size_t commands_len =
        raw.size() - EXCHANGE_MIN_SIZE; // bytes between header and crc
    if (static_cast<std::size_t>(num_commands) != commands_len)
    {
        return std::unexpected(make_error_code(ExchangeError::CountMismatch));
    }

    ExchangeData ed;
    ed.num_commands = num_commands;
    ed.commands.assign(raw.begin() + 4, raw.begin() + 4 + commands_len);

    logger::log(logger::INFO,
                "routeproto",
                std::format("decode exchange: num_commands={} data_length={}",
                            ed.num_commands,
                            data_length));
    return ed;
}

} // namespace routeproto
