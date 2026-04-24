/**
 * @file ud_proto.hpp
 * @brief UNIX-domain communication protocol with byte stuffing and CRC-16.
 *
 * ## Frame format (on the wire)
 * @code
 *   0x7E  <stuffed-payload>  0x7E
 * @endcode
 *
 * ## Payload layout (big-endian, before stuffing)
 * | Field          | Type      | Size  | Notes                           |
 * |----------------|-----------|-------|---------------------------------|
 * | length         | uint32_t  | 4     | Total payload bytes (incl. CRC) |
 * | pkt_num        | uint16_t  | 2     | Sequence number of this packet  |
 * | total_pkts     | uint16_t  | 2     | Total packets in the message    |
 * | ctrl           | uint16_t  | 2     | Control / flags word            |
 * | data           | uint8_t[] | var   | Application data                |
 * | crc            | uint16_t  | 2     | CRC-16/IBM over all prior bytes |
 *
 * ## Response payload
 * Same as request, except @c data is exactly 1 byte: reception status
 * (0 = success, non-zero = error code).
 *
 * ## Byte stuffing
 * Applied to the raw payload bytes before framing:
 *  - 0x7E in payload  → 0x7D 0x5E
 *  - 0x7D in payload  → 0x7D 0x5D
 *
 * @author  Generated
 * @date    2026
 */

#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <system_error>
#include <vector>

namespace udproto
{

/// @brief Frame delimiter byte.
inline constexpr std::uint8_t FRAME_DELIM = 0x7E;

/// @brief Escape byte used during byte stuffing.
inline constexpr std::uint8_t ESCAPE_BYTE = 0x7D;

/// @brief Escaped representation of FRAME_DELIM (XOR mask = 0x20).
inline constexpr std::uint8_t ESC_DELIM = 0x5E;

/// @brief Escaped representation of ESCAPE_BYTE.
inline constexpr std::uint8_t ESC_ESCAPE = 0x5D;

/// @brief Minimum raw payload size (header + CRC, no application data).
inline constexpr std::size_t HEADER_SIZE = 10; ///< 4+2+2+2 bytes header
inline constexpr std::size_t CRC_SIZE = 2;
inline constexpr std::size_t MIN_PAYLOAD = HEADER_SIZE + CRC_SIZE;

/// @brief Maximum application-data bytes per packet for batch transfers (4 KB).
/// Canonical constexpr constant — value is asserted in ud_proto.cpp.
inline constexpr std::size_t MAX_DATA_PER_PACKET = 4096;

// ---------------------------------------------------------------------------
// Error codes
// ---------------------------------------------------------------------------

/// @brief Error codes for protocol operations.
enum class ProtoError : int
{
    InvalidFrame = 1, ///< Missing or misplaced delimiter
    BadLength,        ///< Length field does not match actual data
    CrcMismatch,      ///< CRC-16 check failed
    BufferTooSmall,   ///< Supplied buffer is too small
    IncompleteFrame,  ///< Not enough bytes for a complete frame
};

/// @brief Returns the error category for ProtoError.
const std::error_category& proto_error_category() noexcept;

/// @brief Creates a std::error_code from a ProtoError.
std::error_code make_error_code(ProtoError e) noexcept;

} // namespace udproto

template <>
struct std::is_error_code_enum<udproto::ProtoError> : std::true_type
{};

namespace udproto
{

// ---------------------------------------------------------------------------
// Packet structures
// ---------------------------------------------------------------------------

/**
 * @brief Decoded representation of a protocol packet.
 */
struct Packet
{
    std::uint32_t length{};     ///< Total payload length (header + data + CRC)
    std::uint16_t pkt_num{};    ///< Sequence number of this fragment
    std::uint16_t total_pkts{}; ///< Total number of fragments
    std::uint16_t ctrl{};       ///< Control / flags word
    std::vector<std::uint8_t> data; ///< Application-level payload
};

/**
 * @brief Decoded representation of a protocol response.
 */
struct Response
{
    std::uint32_t length{};     ///< Total payload length
    std::uint16_t pkt_num{};    ///< Matches the request pkt_num
    std::uint16_t total_pkts{}; ///< Matches the request total_pkts
    std::uint16_t ctrl{};       ///< Control / flags word
    std::uint8_t status{};      ///< 0 = success
};

// ---------------------------------------------------------------------------
// CRC-16/IBM
// ---------------------------------------------------------------------------

/**
 * @brief Computes CRC-16/IBM (poly 0x8005, init 0x0000, reflect in/out)
 *        over the given byte span.
 * @param data Input bytes.
 * @return 16-bit checksum.
 */
[[nodiscard]] std::uint16_t crc16(std::span<const std::uint8_t> data) noexcept;

// ---------------------------------------------------------------------------
// Byte stuffing / de-stuffing
// ---------------------------------------------------------------------------

/**
 * @brief Applies byte stuffing to @p raw payload bytes.
 * @param raw Input bytes (un-stuffed payload).
 * @return Stuffed bytes (ready to be wrapped with delimiters).
 */
[[nodiscard]] std::vector<std::uint8_t>
stuff(std::span<const std::uint8_t> raw);

/**
 * @brief Reverses byte stuffing on @p stuffed bytes.
 * @param stuffed Input bytes (already stripped of frame delimiters).
 * @return De-stuffed raw payload on success, or a ProtoError.
 */
[[nodiscard]] std::expected<std::vector<std::uint8_t>, std::error_code>
destuff(std::span<const std::uint8_t> stuffed);

// ---------------------------------------------------------------------------
// Encoding (Packet → wire bytes)
// ---------------------------------------------------------------------------

/**
 * @brief Serialises a Packet into a complete framed wire representation.
 *
 * Steps:
 *  1. Build raw payload (header + data).
 *  2. Compute and append CRC-16.
 *  3. Apply byte stuffing.
 *  4. Prepend and append 0x7E delimiters.
 *
 * @param pkt The packet to encode.
 * @return Wire-format bytes on success, or an error code.
 */
[[nodiscard]] std::expected<std::vector<std::uint8_t>, std::error_code>
encode_packet(const Packet& pkt);

/**
 * @brief Serialises a Response into a complete framed wire representation.
 *
 * @param rsp The response to encode.
 * @return Wire-format bytes on success, or an error code.
 */
[[nodiscard]] std::expected<std::vector<std::uint8_t>, std::error_code>
encode_response(const Response& rsp);

// ---------------------------------------------------------------------------
// Decoding (wire bytes → Packet / Response)
// ---------------------------------------------------------------------------

/**
 * @brief Parses a framed wire buffer into a Packet.
 *
 * The buffer must start and end with 0x7E.
 *
 * @param frame Complete frame bytes including delimiters.
 * @return Decoded Packet on success, or an error code.
 */
[[nodiscard]] std::expected<Packet, std::error_code>
decode_packet(std::span<const std::uint8_t> frame);

/**
 * @brief Parses a framed wire buffer into a Response.
 *
 * @param frame Complete frame bytes including delimiters.
 * @return Decoded Response on success, or an error code.
 */
[[nodiscard]] std::expected<Response, std::error_code>
decode_response(std::span<const std::uint8_t> frame);

// ---------------------------------------------------------------------------
// Stream scanning
// ---------------------------------------------------------------------------

/**
 * @brief Searches @p stream for the next complete frame.
 *
 * Looks for a pair of 0x7E delimiters.  Returns the byte span
 * [first_delim, second_delim] inclusive.
 *
 * @param stream     Byte buffer that may contain one or more frames.
 * @param[out] end   Set to the index *after* the closing delimiter on success.
 * @return Span covering the complete frame, or an error code.
 */
[[nodiscard]] std::expected<std::span<const std::uint8_t>, std::error_code>
find_frame(std::span<const std::uint8_t> stream, std::size_t& end);

} // namespace udproto
