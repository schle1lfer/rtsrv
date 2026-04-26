/**
 * @file ud_proto.cpp
 * @brief Implementation of the UNIX-domain communication protocol.
 *
 * Compiled into both lib/ud_proto.so (shared) and lib/ud_proto.ar (static).
 *
 * 
 * @date    2026
 */

#include "ud_proto.hpp"

#include "logger.hpp"

#include <algorithm>
#include <bit>
#include <format>
#include <stdexcept>

// Canonical constexpr-equivalent constant: maximum data bytes per batch packet
// (4 KB).
static_assert(udproto::MAX_DATA_PER_PACKET == 4096,
              "MAX_DATA_PER_PACKET must be 4096 bytes (4 KB)");

namespace udproto
{

// ---------------------------------------------------------------------------
// Error category
// ---------------------------------------------------------------------------

namespace
{

/**
 * @brief Custom std::error_category for udproto::ProtoError codes.
 *
 * Registered as a singleton via proto_error_category().
 */
class ProtoErrorCategory final : public std::error_category
{
public:
    /**
     * @brief Returns the category name.
     * @return The string "udproto".
     */
    [[nodiscard]] const char* name() const noexcept override
    {
        return "udproto";
    }

    /**
     * @brief Maps a ProtoError integer value to a human-readable message.
     * @param ev Integer representation of a ProtoError enumerator.
     * @return Descriptive error string.
     */
    [[nodiscard]] std::string message(int ev) const override
    {
        switch (static_cast<ProtoError>(ev))
        {
        case ProtoError::InvalidFrame:
            return "invalid frame (bad delimiter)";
        case ProtoError::BadLength:
            return "payload length mismatch";
        case ProtoError::CrcMismatch:
            return "CRC-16 mismatch";
        case ProtoError::BufferTooSmall:
            return "buffer too small";
        case ProtoError::IncompleteFrame:
            return "incomplete frame";
        default:
            return "unknown protocol error";
        }
    }
};

} // anonymous namespace

/**
 * @brief Returns the singleton ProtoErrorCategory instance.
 * @return Reference to the global udproto error category.
 */
const std::error_category& proto_error_category() noexcept
{
    static const ProtoErrorCategory instance;
    return instance;
}

/**
 * @brief Creates a std::error_code from a ProtoError enumerator.
 * @param e The ProtoError value to wrap.
 * @return Corresponding std::error_code in the proto_error_category.
 */
std::error_code make_error_code(ProtoError e) noexcept
{
    return {static_cast<int>(e), proto_error_category()};
}

// ---------------------------------------------------------------------------
// CRC-16/IBM  (poly=0x8005, init=0x0000, refin=true, refout=true,
// xorout=0x0000)
// ---------------------------------------------------------------------------

/**
 * @brief Computes CRC-16/IBM over @p data.
 *
 * Parameters: poly=0x8005, init=0x0000, refin=true, refout=true, xorout=0x0000.
 *
 * @param data Input byte span to checksum.
 * @return 16-bit CRC value.
 */
std::uint16_t crc16(std::span<const std::uint8_t> data) noexcept
{
    constexpr std::uint16_t POLY = 0x8005;
    std::uint16_t crc = 0x0000;
    for (auto byte : data)
    {
        crc ^= static_cast<std::uint16_t>(byte);
        for (int i = 0; i < 8; ++i)
        {
            if (crc & 0x0001)
            {
                crc = static_cast<std::uint16_t>((crc >> 1) ^ POLY);
            }
            else
            {
                crc >>= 1;
            }
        }
    }
    return crc;
}

// ---------------------------------------------------------------------------
// Big-endian serialisation helpers
// ---------------------------------------------------------------------------

namespace
{

/// @brief Writes a uint32 in big-endian order into @p out.
void write_be32(std::vector<std::uint8_t>& out, std::uint32_t v)
{
    out.push_back(static_cast<std::uint8_t>(v >> 24));
    out.push_back(static_cast<std::uint8_t>(v >> 16));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v));
}

/// @brief Writes a uint16 in big-endian order into @p out.
void write_be16(std::vector<std::uint8_t>& out, std::uint16_t v)
{
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v));
}

/// @brief Reads a big-endian uint32 from @p buf at @p offset.
std::uint32_t read_be32(std::span<const std::uint8_t> buf, std::size_t offset)
{
    return (static_cast<std::uint32_t>(buf[offset]) << 24) |
           (static_cast<std::uint32_t>(buf[offset + 1]) << 16) |
           (static_cast<std::uint32_t>(buf[offset + 2]) << 8) |
           static_cast<std::uint32_t>(buf[offset + 3]);
}

/// @brief Reads a big-endian uint16 from @p buf at @p offset.
std::uint16_t read_be16(std::span<const std::uint8_t> buf, std::size_t offset)
{
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(buf[offset]) << 8) |
        static_cast<std::uint16_t>(buf[offset + 1]));
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Byte stuffing / de-stuffing
// ---------------------------------------------------------------------------

/**
 * @brief Applies HDLC-style byte stuffing to @p raw.
 *
 * Replaces every FRAME_DELIM (0x7E) with {ESCAPE_BYTE, ESC_DELIM} and every
 * ESCAPE_BYTE (0x7D) with {ESCAPE_BYTE, ESC_ESCAPE}.  The caller must then
 * surround the result with FRAME_DELIM bytes to form a complete frame.
 *
 * @param raw Input bytes to stuff.
 * @return Stuffed byte sequence (no framing delimiters included).
 */
std::vector<std::uint8_t> stuff(std::span<const std::uint8_t> raw)
{
    std::vector<std::uint8_t> out;
    out.reserve(raw.size() + 4); // slight over-estimate
    for (auto b : raw)
    {
        if (b == FRAME_DELIM)
        {
            out.push_back(ESCAPE_BYTE);
            out.push_back(ESC_DELIM);
        }
        else if (b == ESCAPE_BYTE)
        {
            out.push_back(ESCAPE_BYTE);
            out.push_back(ESC_ESCAPE);
        }
        else
        {
            out.push_back(b);
        }
    }
    return out;
}

/**
 * @brief Reverses byte stuffing applied by stuff().
 *
 * Processes escape sequences and rejects malformed input.
 *
 * @param stuffed Stuffed payload (without framing delimiters).
 * @return Original byte sequence on success, or ProtoError::InvalidFrame if
 *         an unexpected escape sequence is encountered.
 */
std::expected<std::vector<std::uint8_t>, std::error_code>
destuff(std::span<const std::uint8_t> stuffed)
{
    std::vector<std::uint8_t> out;
    out.reserve(stuffed.size());
    bool escaped = false;
    for (auto b : stuffed)
    {
        if (escaped)
        {
            if (b == ESC_DELIM)
            {
                out.push_back(FRAME_DELIM);
            }
            else if (b == ESC_ESCAPE)
            {
                out.push_back(ESCAPE_BYTE);
            }
            else
            {
                return std::unexpected(
                    make_error_code(ProtoError::InvalidFrame));
            }
            escaped = false;
        }
        else if (b == ESCAPE_BYTE)
        {
            escaped = true;
        }
        else
        {
            out.push_back(b);
        }
    }
    if (escaped)
    {
        return std::unexpected(make_error_code(ProtoError::InvalidFrame));
    }
    return out;
}

// ---------------------------------------------------------------------------
// Encoding helpers
// ---------------------------------------------------------------------------

namespace
{

/**
 * @brief Builds the raw (pre-stuffing) payload for a request packet.
 *        The CRC covers everything from the first byte of @p payload up to
 *        (but not including) the CRC field itself.
 */
std::vector<std::uint8_t>
build_request_payload(std::uint32_t length,
                      std::uint16_t pkt_num,
                      std::uint16_t total_pkts,
                      std::uint16_t ctrl,
                      std::span<const std::uint8_t> data)
{
    std::vector<std::uint8_t> payload;
    payload.reserve(HEADER_SIZE + data.size() + CRC_SIZE);

    write_be32(payload, length);
    write_be16(payload, pkt_num);
    write_be16(payload, total_pkts);
    write_be16(payload, ctrl);
    payload.insert(payload.end(), data.begin(), data.end());

    const auto checksum = crc16(payload);
    write_be16(payload, checksum);
    return payload;
}

/// @brief Wraps @p raw payload in byte stuffing and frame delimiters.
std::vector<std::uint8_t> frame(std::span<const std::uint8_t> raw)
{
    auto stuffed = stuff(raw);
    std::vector<std::uint8_t> frame;
    frame.reserve(stuffed.size() + 2);
    frame.push_back(FRAME_DELIM);
    frame.insert(frame.end(), stuffed.begin(), stuffed.end());
    frame.push_back(FRAME_DELIM);
    return frame;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Public encode API
// ---------------------------------------------------------------------------

std::expected<std::vector<std::uint8_t>, std::error_code>
encode_packet(const Packet& pkt)
{
    // Compute the total payload size (header + data + CRC) and place it in
    // the length field.  If the caller left length==0 we fill it in.
    const std::uint32_t total_len =
        static_cast<std::uint32_t>(HEADER_SIZE + pkt.data.size() + CRC_SIZE);

    const std::uint32_t length_field =
        (pkt.length != 0) ? pkt.length : total_len;

    auto raw = build_request_payload(
        length_field, pkt.pkt_num, pkt.total_pkts, pkt.ctrl, pkt.data);

    auto result = frame(raw);
    logger::log(logger::DEBUG,
                "udproto",
                std::format("encode pkt: pkt_num={} total_pkts={} "
                            "ctrl={:#06x} data_len={} frame_len={}",
                            pkt.pkt_num,
                            pkt.total_pkts,
                            pkt.ctrl,
                            pkt.data.size(),
                            result.size()));
    return result;
}

std::expected<std::vector<std::uint8_t>, std::error_code>
encode_response(const Response& rsp)
{
    // Response data is a single status byte.
    const std::uint32_t total_len =
        static_cast<std::uint32_t>(HEADER_SIZE + 1 + CRC_SIZE);
    const std::uint32_t length_field =
        (rsp.length != 0) ? rsp.length : total_len;

    const std::uint8_t status_byte = rsp.status;
    auto raw =
        build_request_payload(length_field,
                              rsp.pkt_num,
                              rsp.total_pkts,
                              rsp.ctrl,
                              std::span<const std::uint8_t>(&status_byte, 1));

    auto result = frame(raw);
    logger::log(logger::DEBUG,
                "udproto",
                std::format("encode rsp: pkt_num={} ctrl={:#06x} "
                            "status={:#04x} frame_len={}",
                            rsp.pkt_num,
                            rsp.ctrl,
                            rsp.status,
                            result.size()));
    return result;
}

// ---------------------------------------------------------------------------
// Decoding helpers
// ---------------------------------------------------------------------------

namespace
{

/**
 * @brief Validates and de-stuffs a complete frame, returning the raw payload.
 *
 * Expects @p frame to start and end with FRAME_DELIM.
 */
std::expected<std::vector<std::uint8_t>, std::error_code>
extract_payload(std::span<const std::uint8_t> frm)
{
    if (frm.size() < 2 || frm.front() != FRAME_DELIM ||
        frm.back() != FRAME_DELIM)
    {
        return std::unexpected(make_error_code(ProtoError::InvalidFrame));
    }

    // Strip the two delimiters.
    auto inner = frm.subspan(1, frm.size() - 2);
    auto result = destuff(inner);
    if (!result)
        return result;

    auto& raw = *result;
    if (raw.size() < MIN_PAYLOAD)
    {
        return std::unexpected(make_error_code(ProtoError::BadLength));
    }

    // Verify CRC (covers everything except the last 2 bytes).
    const auto crc_expected = read_be16(raw, raw.size() - CRC_SIZE);
    const auto crc_computed =
        crc16(std::span{raw}.subspan(0, raw.size() - CRC_SIZE));
    if (crc_expected != crc_computed)
    {
        return std::unexpected(make_error_code(ProtoError::CrcMismatch));
    }

    // Verify length field.
    const auto reported_len = read_be32(raw, 0);
    if (reported_len != static_cast<std::uint32_t>(raw.size()))
    {
        return std::unexpected(make_error_code(ProtoError::BadLength));
    }

    return raw;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Public decode API
// ---------------------------------------------------------------------------

std::expected<Packet, std::error_code>
decode_packet(std::span<const std::uint8_t> frm)
{
    auto payload_result = extract_payload(frm);
    if (!payload_result)
    {
        logger::log(logger::ERR,
                    "udproto",
                    std::format("decode_packet failed: {}",
                                payload_result.error().message()));
        return std::unexpected(payload_result.error());
    }
    auto& raw = *payload_result;

    Packet pkt;
    pkt.length = read_be32(raw, 0);
    pkt.pkt_num = read_be16(raw, 4);
    pkt.total_pkts = read_be16(raw, 6);
    pkt.ctrl = read_be16(raw, 8);

    // data occupies raw[10 .. raw.size()-2] (exclude CRC at tail).
    const std::size_t data_start = HEADER_SIZE;
    const std::size_t data_end = raw.size() - CRC_SIZE;
    pkt.data.assign(raw.begin() + data_start, raw.begin() + data_end);

    logger::log(logger::DEBUG,
                "udproto",
                std::format("decode pkt: pkt_num={} total_pkts={} "
                            "ctrl={:#06x} data_len={}",
                            pkt.pkt_num,
                            pkt.total_pkts,
                            pkt.ctrl,
                            pkt.data.size()));
    return pkt;
}

std::expected<Response, std::error_code>
decode_response(std::span<const std::uint8_t> frm)
{
    auto payload_result = extract_payload(frm);
    if (!payload_result)
    {
        logger::log(logger::ERR,
                    "udproto",
                    std::format("decode_response failed: {}",
                                payload_result.error().message()));
        return std::unexpected(payload_result.error());
    }
    auto& raw = *payload_result;

    // Response data must be exactly 1 byte.
    const std::size_t data_len = raw.size() - MIN_PAYLOAD;
    if (data_len != 1)
    {
        logger::log(logger::ERR,
                    "udproto",
                    std::format("decode_response: bad data_len={}", data_len));
        return std::unexpected(make_error_code(ProtoError::BadLength));
    }

    Response rsp;
    rsp.length = read_be32(raw, 0);
    rsp.pkt_num = read_be16(raw, 4);
    rsp.total_pkts = read_be16(raw, 6);
    rsp.ctrl = read_be16(raw, 8);
    rsp.status = raw[HEADER_SIZE];

    logger::log(
        logger::DEBUG,
        "udproto",
        std::format("decode rsp: pkt_num={} ctrl={:#06x} status={:#04x}",
                    rsp.pkt_num,
                    rsp.ctrl,
                    rsp.status));
    return rsp;
}

// ---------------------------------------------------------------------------
// Stream scanner
// ---------------------------------------------------------------------------

std::expected<std::span<const std::uint8_t>, std::error_code>
find_frame(std::span<const std::uint8_t> stream, std::size_t& end)
{
    if (stream.empty())
    {
        return std::unexpected(make_error_code(ProtoError::IncompleteFrame));
    }

    // Find the first delimiter.
    std::size_t start = 0;
    while (start < stream.size() && stream[start] != FRAME_DELIM)
    {
        ++start;
    }
    if (start >= stream.size())
    {
        return std::unexpected(make_error_code(ProtoError::IncompleteFrame));
    }

    // Find the closing delimiter (must be at least 1 byte after start).
    std::size_t close = start + 1;
    while (close < stream.size() && stream[close] != FRAME_DELIM)
    {
        ++close;
    }
    if (close >= stream.size())
    {
        return std::unexpected(make_error_code(ProtoError::IncompleteFrame));
    }

    end = close + 1; // index after the closing delimiter
    const auto found = stream.subspan(start, end - start);
    logger::log(
        logger::DEBUG,
        "udproto",
        std::format("frame found: offset={} len={}", start, found.size()));
    return found;
}

} // namespace udproto
