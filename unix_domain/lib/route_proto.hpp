/**
 * @file route_proto.hpp
 * @brief High-level routing protocol carried inside udproto data fields.
 *
 * This layer is encapsulated in the @c data field of a udproto::Packet.
 * It defines message types, a message structure, and encode/decode helpers.
 *
 * ## Message layout (big-endian)
 * | Field    | Type      | Size | Notes                         |
 * |----------|-----------|------|-------------------------------|
 * | msg_type | uint8_t   | 1    | See MsgType enum              |
 * | flags    | uint8_t   | 1    | Application-defined flags     |
 * | msg_id   | uint16_t  | 2    | Request/response correlation  |
 * | payload  | uint8_t[] | var  | Type-specific data            |
 *
 * 
 * @date    2026
 */

#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <system_error>
#include <vector>

namespace routeproto
{

// ---------------------------------------------------------------------------
// Message types
// ---------------------------------------------------------------------------

/**
 * @brief High-level message type identifiers.
 */
enum class MsgType : std::uint8_t
{
    PING = 0x01,     ///< Connectivity check request
    PONG = 0x02,     ///< Connectivity check response
    DATA = 0x10,     ///< Payload data from client
    DATA_ACK = 0x11, ///< Server acknowledgement for DATA
    ERROR = 0xFF,    ///< Error notification
};

// ---------------------------------------------------------------------------
// Flags
// ---------------------------------------------------------------------------

/// @brief Message flag bits (bitfield values for the @c flags byte).
enum Flags : std::uint8_t
{
    FLAG_NONE = 0x00,
    FLAG_MORE_FRAGS = 0x01, ///< More fragments follow (fragmentation support)
    FLAG_COMPRESSED = 0x02, ///< Payload is compressed (reserved for future use)
    FLAG_ENCRYPTED = 0x04,  ///< Payload is encrypted  (reserved for future use)
};

// ---------------------------------------------------------------------------
// Error codes
// ---------------------------------------------------------------------------

/// @brief Errors specific to the routing protocol layer.
enum class RouteError : int
{
    InvalidMessage = 1, ///< Buffer too small or malformed header
    UnknownType,        ///< Unrecognised MsgType value
};

/// @brief Returns the error category for RouteError.
const std::error_category& route_error_category() noexcept;

/// @brief Creates a std::error_code from a RouteError.
std::error_code make_error_code(RouteError e) noexcept;

// ---------------------------------------------------------------------------
// Exchange-data error codes
// ---------------------------------------------------------------------------

/// @brief Errors specific to the exchange-data layer.
enum class ExchangeError : int
{
    InvalidData = 1, ///< Buffer too small or malformed
    LengthMismatch,  ///< data_length field does not match actual buffer size
    CrcMismatch,     ///< CRC-16 check failed
    CountMismatch,   ///< num_commands does not match parsed command count
};

/// @brief Returns the error category for ExchangeError.
const std::error_category& exchange_error_category() noexcept;

/// @brief Creates a std::error_code from an ExchangeError.
std::error_code make_error_code(ExchangeError e) noexcept;

} // namespace routeproto

template <>
struct std::is_error_code_enum<routeproto::RouteError> : std::true_type
{};

template <>
struct std::is_error_code_enum<routeproto::ExchangeError> : std::true_type
{};

namespace routeproto
{

// ---------------------------------------------------------------------------
// Exchange-data structure
// ---------------------------------------------------------------------------

/**
 * @brief Decoded exchange-data block carried in the payload of a DATA message.
 *
 * ## Wire layout (big-endian)
 * | Field        | Type      | Size | Notes                              |
 * |--------------|-----------|------|------------------------------------|
 * | num_commands | uint16_t  | 2    | Total number of commands           |
 * | data_length  | uint16_t  | 2    | Total byte length of this block    |
 * | commands     | uint8_t[] | N    | One byte per command               |
 * | crc          | uint16_t  | 2    | CRC-16/IBM over all preceding bytes|
 *
 * @note  data_length must equal 2 + 2 + N + 2 = 6 + N, where N == num_commands.
 */
struct ExchangeData
{
    std::uint16_t num_commands{};       ///< Number of entries in @c commands
    std::vector<std::uint8_t> commands; ///< Routing command bytes
};

/// @brief Minimum encoded size of an ExchangeData block (no commands).
inline constexpr std::size_t EXCHANGE_MIN_SIZE =
    6; // num_commands(2)+data_length(2)+crc(2)

/**
 * @brief Serialises an ExchangeData block into raw bytes.
 * @param ed  Exchange data to encode.
 * @return Encoded byte vector on success, or an error code.
 */
[[nodiscard]] std::expected<std::vector<std::uint8_t>, std::error_code>
encode_exchange(const ExchangeData& ed);

/**
 * @brief Deserialises an ExchangeData block from raw bytes.
 *
 * Performs length-field verification and CRC-16 validation before parsing
 * the command array.
 *
 * @param raw  Raw bytes of the exchange-data block.
 * @return Decoded ExchangeData on success, or an error code.
 */
[[nodiscard]] std::expected<ExchangeData, std::error_code>
decode_exchange(std::span<const std::uint8_t> raw);

// ---------------------------------------------------------------------------
// Message structure
// ---------------------------------------------------------------------------

/**
 * @brief Decoded high-level protocol message.
 */
struct Message
{
    MsgType msg_type{MsgType::PING};
    std::uint8_t flags{FLAG_NONE};
    std::uint16_t msg_id{};
    std::vector<std::uint8_t> payload; ///< Type-specific content
};

/// @brief Minimum encoded message size (header without payload).
inline constexpr std::size_t MSG_HEADER_SIZE = 4; // type(1)+flags(1)+id(2)

// ---------------------------------------------------------------------------
// Convenience constructors
// ---------------------------------------------------------------------------

/// @brief Creates a PING message.
Message make_ping(std::uint16_t msg_id);

/// @brief Creates a PONG message (response to a PING).
Message make_pong(std::uint16_t msg_id);

/**
 * @brief Creates a DATA message.
 * @param msg_id  Correlation identifier.
 * @param payload Application data bytes.
 */
Message make_data(std::uint16_t msg_id, std::span<const std::uint8_t> payload);

/**
 * @brief Creates a DATA_ACK message.
 * @param msg_id    Matches the corresponding DATA message id.
 * @param ok        true for success, false for error.
 */
Message make_data_ack(std::uint16_t msg_id, bool ok = true);

/**
 * @brief Creates an ERROR message carrying a UTF-8 description.
 * @param msg_id  Correlation identifier.
 * @param text    Human-readable error string.
 */
Message make_error(std::uint16_t msg_id, const std::string& text);

// ---------------------------------------------------------------------------
// Encode / decode
// ---------------------------------------------------------------------------

/**
 * @brief Serialises a Message into raw bytes suitable for the udproto data
 * field.
 * @param msg  Message to encode.
 * @return Encoded byte vector, or an error code.
 */
[[nodiscard]] std::expected<std::vector<std::uint8_t>, std::error_code>
encode_message(const Message& msg);

/**
 * @brief Deserialises a Message from the raw bytes of a udproto data field.
 * @param raw  Bytes from the udproto data field.
 * @return Decoded Message on success, or an error code.
 */
[[nodiscard]] std::expected<Message, std::error_code>
decode_message(std::span<const std::uint8_t> raw);

} // namespace routeproto
