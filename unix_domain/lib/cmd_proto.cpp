/**
 * @file cmd_proto.cpp
 * @brief Implementation of the command protocol layer.
 *
 * Compiled into both lib/cmd_proto.so (shared) and lib/cmd_proto.ar (static).
 *
 * @author  Generated
 * @date    2026
 */

#include "cmd_proto.hpp"

#include "logger.hpp"
#include "routing.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <format>

namespace cmdproto
{

// ---------------------------------------------------------------------------
// Error category
// ---------------------------------------------------------------------------

namespace
{

/**
 * @brief Custom std::error_category for cmdproto::CmdError codes.
 *
 * Registered as a singleton via cmd_error_category().
 */
class CmdErrorCategory final : public std::error_category
{
public:
    /**
     * @brief Returns the category name.
     * @return The string "cmdproto".
     */
    [[nodiscard]] const char* name() const noexcept override
    {
        return "cmdproto";
    }

    /**
     * @brief Maps a CmdError integer value to a human-readable message.
     * @param ev Integer representation of a CmdError enumerator.
     * @return Descriptive error string.
     */
    [[nodiscard]] std::string message(int ev) const override
    {
        switch (static_cast<CmdError>(ev))
        {
        case CmdError::InvalidCommand:
            return "invalid or truncated command";
        case CmdError::UnknownCommand:
            return "unknown command identifier";
        case CmdError::MissingField:
            return "required field absent from command";
        case CmdError::InvalidField:
            return "field length or value is invalid";
        default:
            return "unknown command error";
        }
    }
};

} // anonymous namespace

/**
 * @brief Returns the singleton CmdErrorCategory instance.
 * @return Reference to the global cmdproto error category.
 */
const std::error_category& cmd_error_category() noexcept
{
    static const CmdErrorCategory instance;
    return instance;
}

/**
 * @brief Creates a std::error_code from a CmdError enumerator.
 * @param e The CmdError value to wrap.
 * @return Corresponding std::error_code in the cmd_error_category.
 */
std::error_code make_error_code(CmdError e) noexcept
{
    return {static_cast<int>(e), cmd_error_category()};
}

// ---------------------------------------------------------------------------
// Callback storage
// ---------------------------------------------------------------------------

namespace
{

HandlerCallbacks g_callbacks;

} // anonymous namespace

/**
 * @brief @copybrief cmdproto::init_callbacks
 */
void init_callbacks(HandlerCallbacks callbacks)
{
    g_callbacks = std::move(callbacks);
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

/**
 * @brief Appends @p v as four big-endian bytes to @p out.
 * @param out Destination byte vector.
 * @param v   Value to serialise.
 */
void write_be32(std::vector<std::uint8_t>& out, std::uint32_t v)
{
    out.push_back(static_cast<std::uint8_t>(v >> 24));
    out.push_back(static_cast<std::uint8_t>(v >> 16));
    out.push_back(static_cast<std::uint8_t>(v >>  8));
    out.push_back(static_cast<std::uint8_t>(v));
}

/**
 * @brief Reads a big-endian uint32 from @p buf at @p offset.
 * @param buf    Source byte span.
 * @param offset Byte offset of the highest byte.
 * @return Deserialized 32-bit value.
 */
std::uint32_t read_be32(std::span<const std::uint8_t> buf, std::size_t offset)
{
    return (static_cast<std::uint32_t>(buf[offset])     << 24) |
           (static_cast<std::uint32_t>(buf[offset + 1]) << 16) |
           (static_cast<std::uint32_t>(buf[offset + 2]) <<  8) |
            static_cast<std::uint32_t>(buf[offset + 3]);
}

/**
 * @brief Formats an IPv4 address as dotted-decimal notation "A.B.C.D".
 * @param a IPv4 address bytes in network byte order.
 * @return Formatted address string.
 */
std::string fmt_ipv4(const Ipv4Addr& a)
{
    char buf[16];
    std::snprintf(buf,
                  sizeof(buf),
                  "%u.%u.%u.%u",
                  static_cast<unsigned>(a[0]),
                  static_cast<unsigned>(a[1]),
                  static_cast<unsigned>(a[2]),
                  static_cast<unsigned>(a[3]));
    return buf;
}

/**
 * @brief Finds the first field with the given @p id in a command's field list.
 * @param cmd Command to search.
 * @param id  Field identifier to find.
 * @return Pointer to the matching Field, or nullptr if not found.
 */
const Field* find_field(const Command& cmd, FieldId id)
{
    for (const auto& f : cmd.fields)
    {
        if (f.field_id == id)
            return &f;
    }
    return nullptr;
}

/**
 * @brief Maps a CmdId enumerator to its human-readable name.
 * @param id Command identifier.
 * @return String view of the command name (e.g. "ROUTE_ADD").
 */
std::string_view cmd_id_name(CmdId id) noexcept
{
    switch (id)
    {
    case CmdId::ROUTE_ADD:
        return "ROUTE_ADD";
    case CmdId::ROUTE_DEL:
        return "ROUTE_DEL";
    case CmdId::ROUTE_LIST:
        return "ROUTE_LIST";
    default:
        return "UNKNOWN";
    }
}

/**
 * @brief Maps a FieldId enumerator to its human-readable name.
 */
std::string_view field_id_name(FieldId id) noexcept
{
    switch (id)
    {
    case FieldId::DST_ADDR:   return "DST_ADDR";
    case FieldId::PREFIX_LEN: return "PREFIX_LEN";
    case FieldId::GATEWAY:    return "GATEWAY";
    case FieldId::IF_NAME:    return "IF_NAME";
    case FieldId::METRIC:     return "METRIC";
    default:                  return "UNKNOWN";
    }
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Encode / decode — generic Command
// ---------------------------------------------------------------------------

/// @brief @copybrief cmdproto::encode_command
std::expected<std::vector<std::uint8_t>, std::error_code>
encode_command(const Command& cmd)
{
    std::vector<std::uint8_t> out;

    // New binary format: raw_payload takes precedence over TLV fields.
    if (!cmd.raw_payload.empty())
    {
        out.reserve(CMD_HEADER_SIZE + cmd.raw_payload.size());
        out.push_back(static_cast<std::uint8_t>(cmd.cmd_id));
        write_be16(out, static_cast<std::uint16_t>(cmd.raw_payload.size()));
        out.insert(out.end(), cmd.raw_payload.begin(), cmd.raw_payload.end());

        logger::log(logger::DEBUG, "cmdproto",
                    std::format("encode cmd (binary): cmd_id=0x{:02x}({}) data_len={}  total={}",
                                static_cast<unsigned>(cmd.cmd_id),
                                cmd_id_name(cmd.cmd_id),
                                cmd.raw_payload.size(),
                                out.size()));
        return out;
    }

    // Legacy TLV format.
    std::size_t fields_size = 0;
    for (const auto& f : cmd.fields)
        fields_size += FIELD_HEADER_SIZE + f.data.size();

    out.reserve(CMD_HEADER_SIZE + fields_size);

    // Command header: cmd_id (1) + data_len (2).
    out.push_back(static_cast<std::uint8_t>(cmd.cmd_id));
    write_be16(out, static_cast<std::uint16_t>(fields_size));

    logger::log(logger::DEBUG, "cmdproto",
                std::format("encode cmd (tlv) hdr: cmd_id=0x{:02x}({}) data_len={}",
                            static_cast<unsigned>(cmd.cmd_id),
                            cmd_id_name(cmd.cmd_id),
                            fields_size));

    // Field entries.
    std::size_t fi = 0;
    for (const auto& f : cmd.fields)
    {
        const std::size_t field_offset = out.size();
        out.push_back(static_cast<std::uint8_t>(f.field_id));
        out.push_back(static_cast<std::uint8_t>(f.data.size()));
        out.insert(out.end(), f.data.begin(), f.data.end());
        logger::log(logger::DEBUG, "cmdproto",
                    std::format("  field[{}] @{}: id=0x{:02x}({}) len={}",
                                fi++, field_offset,
                                static_cast<unsigned>(f.field_id),
                                field_id_name(f.field_id),
                                f.data.size()));
    }

    logger::log(logger::INFO,
                "cmdproto",
                std::format("encode cmd (tlv): {} fields={} bytes={}",
                            cmd_id_name(cmd.cmd_id),
                            cmd.fields.size(),
                            out.size()));
    return out;
}

/// @brief @copybrief cmdproto::decode_command
std::expected<Command, std::error_code>
decode_command(std::span<const std::uint8_t> raw)
{
    if (raw.size() < CMD_HEADER_SIZE)
        return std::unexpected(make_error_code(CmdError::InvalidCommand));

    const auto cmd_id_byte = raw[0];

    // Validate known command IDs.
    switch (static_cast<CmdId>(cmd_id_byte))
    {
    case CmdId::ROUTE_ADD:
    case CmdId::ROUTE_DEL:
    case CmdId::ROUTE_LIST:
        break;
    default:
        return std::unexpected(make_error_code(CmdError::UnknownCommand));
    }

    const auto data_len = read_be16(raw, 1);

    // Ensure the buffer holds the declared payload.
    if (raw.size() < CMD_HEADER_SIZE + data_len)
        return std::unexpected(make_error_code(CmdError::InvalidCommand));

    Command cmd;
    cmd.cmd_id = static_cast<CmdId>(cmd_id_byte);

    logger::log(logger::DEBUG, "cmdproto",
                std::format("decode cmd hdr @0: cmd_id=0x{:02x}({}) data_len={}",
                            cmd_id_byte,
                            cmd_id_name(cmd.cmd_id),
                            data_len));

    // Always capture the raw payload bytes so callers can choose between
    // binary-format and TLV-format decoding.
    cmd.raw_payload.assign(raw.begin() + CMD_HEADER_SIZE,
                           raw.begin() + CMD_HEADER_SIZE + data_len);

    // Attempt TLV field parsing (best-effort: stops on any structural error
    // rather than failing the whole decode, because the payload may be in
    // binary format rather than TLV).
    std::size_t pos = CMD_HEADER_SIZE;
    const std::size_t end = CMD_HEADER_SIZE + data_len;
    std::size_t fi = 0;

    while (pos < end)
    {
        if (end - pos < FIELD_HEADER_SIZE)
            break; // truncated field header — stop gracefully

        const auto field_id  = raw[pos];
        const auto field_len = raw[pos + 1];
        pos += FIELD_HEADER_SIZE;

        if (end - pos < field_len)
            break; // field data overruns end — stop gracefully

        Field f;
        f.field_id = static_cast<FieldId>(field_id);
        f.data.assign(raw.begin() + static_cast<std::ptrdiff_t>(pos),
                      raw.begin() +
                          static_cast<std::ptrdiff_t>(pos + field_len));
        logger::log(logger::DEBUG, "cmdproto",
                    std::format("  field[{}] @{}: id=0x{:02x}({}) len={}",
                                fi++, pos - FIELD_HEADER_SIZE,
                                field_id,
                                field_id_name(static_cast<FieldId>(field_id)),
                                field_len));
        pos += field_len;

        cmd.fields.push_back(std::move(f));
    }

    logger::log(logger::INFO,
                "cmdproto",
                std::format("decode cmd: {} fields={} raw_bytes={}",
                            cmd_id_name(cmd.cmd_id),
                            cmd.fields.size(),
                            cmd.raw_payload.size()));
    return cmd;
}

/// @brief @copybrief cmdproto::decode_commands
std::expected<std::vector<Command>, std::error_code>
decode_commands(std::span<const std::uint8_t> raw)
{
    std::vector<Command> result;
    std::size_t pos = 0;

    while (pos < raw.size())
    {
        auto sub = raw.subspan(pos);
        auto res = decode_command(sub);
        if (!res)
            return std::unexpected(res.error());

        // Advance by the exact byte size this command consumed.
        const std::size_t consumed =
            CMD_HEADER_SIZE + read_be16(raw, pos + 1); // re-read data_len

        pos += consumed;
        result.push_back(std::move(*res));
    }

    logger::log(logger::INFO,
                "cmdproto",
                std::format("decode commands: count={}", result.size()));
    return result;
}

// ---------------------------------------------------------------------------
// Command builders
// ---------------------------------------------------------------------------

/// @brief @copybrief cmdproto::make_route_add
Command make_route_add(const RouteAddParams& p)
{
    Command cmd;
    cmd.cmd_id = CmdId::ROUTE_ADD;

    // DST_ADDR: 4 bytes, network byte order.
    {
        Field f;
        f.field_id = FieldId::DST_ADDR;
        f.data.assign(p.dst_addr.begin(), p.dst_addr.end());
        cmd.fields.push_back(std::move(f));
    }

    // PREFIX_LEN: 1 byte.
    {
        Field f;
        f.field_id = FieldId::PREFIX_LEN;
        f.data = {p.prefix_len};
        cmd.fields.push_back(std::move(f));
    }

    // GATEWAY: 4 bytes, network byte order.
    {
        Field f;
        f.field_id = FieldId::GATEWAY;
        f.data.assign(p.gateway.begin(), p.gateway.end());
        cmd.fields.push_back(std::move(f));
    }

    // IF_NAME: variable-length UTF-8.
    {
        Field f;
        f.field_id = FieldId::IF_NAME;
        f.data.assign(p.if_name.begin(), p.if_name.end());
        cmd.fields.push_back(std::move(f));
    }

    // METRIC: 2 bytes BE (omit when zero — caller uses kernel default).
    if (p.metric != 0)
    {
        Field f;
        f.field_id = FieldId::METRIC;
        f.data.resize(2);
        f.data[0] = static_cast<std::uint8_t>(p.metric >> 8);
        f.data[1] = static_cast<std::uint8_t>(p.metric);
        cmd.fields.push_back(std::move(f));
    }

    return cmd;
}

/// @brief @copybrief cmdproto::make_route_del
Command make_route_del(const RouteDelParams& p)
{
    Command cmd;
    cmd.cmd_id = CmdId::ROUTE_DEL;

    // DST_ADDR: 4 bytes.
    {
        Field f;
        f.field_id = FieldId::DST_ADDR;
        f.data.assign(p.dst_addr.begin(), p.dst_addr.end());
        cmd.fields.push_back(std::move(f));
    }

    // PREFIX_LEN: 1 byte.
    {
        Field f;
        f.field_id = FieldId::PREFIX_LEN;
        f.data = {p.prefix_len};
        cmd.fields.push_back(std::move(f));
    }

    // GATEWAY: omit if all-zero (match any gateway).
    {
        const Ipv4Addr zero{};
        if (p.gateway != zero)
        {
            Field f;
            f.field_id = FieldId::GATEWAY;
            f.data.assign(p.gateway.begin(), p.gateway.end());
            cmd.fields.push_back(std::move(f));
        }
    }

    return cmd;
}

/// @brief @copybrief cmdproto::make_route_list
Command make_route_list()
{
    Command cmd;
    cmd.cmd_id = CmdId::ROUTE_LIST;
    // No fields: server returns the full routing table.
    return cmd;
}

// ---------------------------------------------------------------------------
// Typed-parameter extractors
// ---------------------------------------------------------------------------

/// @brief @copybrief cmdproto::parse_route_add
std::expected<RouteAddParams, std::error_code>
parse_route_add(const Command& cmd)
{
    RouteAddParams p;

    // DST_ADDR — mandatory, must be exactly 4 bytes.
    const Field* dst = find_field(cmd, FieldId::DST_ADDR);
    if (!dst)
        return std::unexpected(make_error_code(CmdError::MissingField));
    if (dst->data.size() != 4)
        return std::unexpected(make_error_code(CmdError::InvalidField));
    std::copy(dst->data.begin(), dst->data.end(), p.dst_addr.begin());

    // PREFIX_LEN — mandatory, 1 byte, value 0–32.
    const Field* pfx = find_field(cmd, FieldId::PREFIX_LEN);
    if (!pfx)
        return std::unexpected(make_error_code(CmdError::MissingField));
    if (pfx->data.size() != 1 || pfx->data[0] > 32)
        return std::unexpected(make_error_code(CmdError::InvalidField));
    p.prefix_len = pfx->data[0];

    // GATEWAY — mandatory, must be exactly 4 bytes.
    const Field* gw = find_field(cmd, FieldId::GATEWAY);
    if (!gw)
        return std::unexpected(make_error_code(CmdError::MissingField));
    if (gw->data.size() != 4)
        return std::unexpected(make_error_code(CmdError::InvalidField));
    std::copy(gw->data.begin(), gw->data.end(), p.gateway.begin());

    // IF_NAME — mandatory, at least 1 byte.
    const Field* ifn = find_field(cmd, FieldId::IF_NAME);
    if (!ifn)
        return std::unexpected(make_error_code(CmdError::MissingField));
    if (ifn->data.empty())
        return std::unexpected(make_error_code(CmdError::InvalidField));
    p.if_name.assign(ifn->data.begin(), ifn->data.end());

    // METRIC — optional, must be exactly 2 bytes when present.
    const Field* met = find_field(cmd, FieldId::METRIC);
    if (met)
    {
        if (met->data.size() != 2)
            return std::unexpected(make_error_code(CmdError::InvalidField));
        p.metric = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(met->data[0]) << 8) |
            static_cast<std::uint16_t>(met->data[1]));
    }

    return p;
}

/// @brief @copybrief cmdproto::parse_route_del
std::expected<RouteDelParams, std::error_code>
parse_route_del(const Command& cmd)
{
    RouteDelParams p;

    // DST_ADDR — mandatory, 4 bytes.
    const Field* dst = find_field(cmd, FieldId::DST_ADDR);
    if (!dst)
        return std::unexpected(make_error_code(CmdError::MissingField));
    if (dst->data.size() != 4)
        return std::unexpected(make_error_code(CmdError::InvalidField));
    std::copy(dst->data.begin(), dst->data.end(), p.dst_addr.begin());

    // PREFIX_LEN — mandatory, 1 byte, value 0–32.
    const Field* pfx = find_field(cmd, FieldId::PREFIX_LEN);
    if (!pfx)
        return std::unexpected(make_error_code(CmdError::MissingField));
    if (pfx->data.size() != 1 || pfx->data[0] > 32)
        return std::unexpected(make_error_code(CmdError::InvalidField));
    p.prefix_len = pfx->data[0];

    // GATEWAY — optional, must be exactly 4 bytes when present.
    const Field* gw = find_field(cmd, FieldId::GATEWAY);
    if (gw)
    {
        if (gw->data.size() != 4)
            return std::unexpected(make_error_code(CmdError::InvalidField));
        std::copy(gw->data.begin(), gw->data.end(), p.gateway.begin());
    }
    // When absent, p.gateway remains all-zero ("match any").

    return p;
}

// ---------------------------------------------------------------------------
// Route-list response encode / decode
// ---------------------------------------------------------------------------

/// @brief @copybrief cmdproto::encode_route_list_response
std::expected<std::vector<std::uint8_t>, std::error_code>
encode_route_list_response(const std::vector<RouteEntry>& routes)
{
    std::vector<std::uint8_t> out;

    for (const auto& r : routes)
    {
        // dst_addr (4), prefix_len (1), gateway (4), metric (2), if_name_len
        // (1), if_name (N)
        out.insert(out.end(), r.dst_addr.begin(), r.dst_addr.end());
        out.push_back(r.prefix_len);
        out.insert(out.end(), r.gateway.begin(), r.gateway.end());
        write_be16(out, r.metric);
        out.push_back(static_cast<std::uint8_t>(r.if_name.size()));
        out.insert(out.end(), r.if_name.begin(), r.if_name.end());
    }

    return out;
}

/// @brief @copybrief cmdproto::decode_route_list_response
std::expected<std::vector<RouteEntry>, std::error_code>
decode_route_list_response(std::span<const std::uint8_t> raw)
{
    std::vector<RouteEntry> routes;
    std::size_t pos = 0;

    while (pos < raw.size())
    {
        if (raw.size() - pos < ROUTE_ENTRY_MIN_SIZE)
        {
            return std::unexpected(make_error_code(CmdError::InvalidCommand));
        }

        RouteEntry r;

        // dst_addr: 4 bytes.
        std::copy(raw.begin() + static_cast<std::ptrdiff_t>(pos),
                  raw.begin() + static_cast<std::ptrdiff_t>(pos + 4),
                  r.dst_addr.begin());
        pos += 4;

        // prefix_len: 1 byte.
        r.prefix_len = raw[pos++];

        // gateway: 4 bytes.
        std::copy(raw.begin() + static_cast<std::ptrdiff_t>(pos),
                  raw.begin() + static_cast<std::ptrdiff_t>(pos + 4),
                  r.gateway.begin());
        pos += 4;

        // metric: 2 bytes BE.
        r.metric = read_be16(raw, pos);
        pos += 2;

        // if_name_len: 1 byte.
        const auto if_name_len = static_cast<std::size_t>(raw[pos++]);

        if (raw.size() - pos < if_name_len)
        {
            return std::unexpected(make_error_code(CmdError::InvalidCommand));
        }

        r.if_name.assign(raw.begin() + static_cast<std::ptrdiff_t>(pos),
                         raw.begin() +
                             static_cast<std::ptrdiff_t>(pos + if_name_len));
        pos += if_name_len;

        routes.push_back(std::move(r));
    }

    return routes;
}

// ---------------------------------------------------------------------------
// Stub command handlers
// ---------------------------------------------------------------------------
//std::expected<RouteAddBinaryResponse, std::error_code>
//handle_route_add_payload(const SingleRouteRequest& req)
/// @brief @copybrief cmdproto::handle_route_add
std::expected<void, std::error_code> handle_route_add(const RouteAddParams& p)
{
    std::printf("[cmd_proto] ROUTE_ADD  %s/%u via %s dev %s metric %u\n",
                fmt_ipv4(p.dst_addr).c_str(),
                static_cast<unsigned>(p.prefix_len),
                fmt_ipv4(p.gateway).c_str(),
                p.if_name.c_str(),
                static_cast<unsigned>(p.metric));

    netlink::RouteAddParams np;
    np.dst = p.dst_addr;
    np.prefix_len = p.prefix_len;
    np.if_name = p.if_name;
    np.metric = p.metric;
    np.protocol = netlink::RouteProtocol::Static;
    np.type = netlink::RouteType::Unicast;
    np.table = netlink::RouteTable::Main;

    // When the gateway field is all-zero the caller means "link-scope, no
    // next-hop" — install as a directly-reachable (link-scope) route.
    const Ipv4Addr zero{};
    if (p.gateway != zero)
    {
        np.gateway = p.gateway;
        np.has_gateway = true;
        np.scope = netlink::RouteScope::Universe;
    }
    else
    {
        np.scope = netlink::RouteScope::Link;
    }

    if (g_callbacks.route_add)
    {
        // hw ASIC
        return g_callbacks.route_add(std::make_shared<netlink::RouteAddParams>(np));
    }
    else
    {
        // virt ASIC
        return netlink::add_route(np);
    }
}

/// @brief @copybrief cmdproto::handle_route_del
std::expected<void, std::error_code> handle_route_del(const RouteDelParams& p)
{
    const Ipv4Addr zero{};
    if (p.gateway != zero)
    {
        std::printf("[cmd_proto] ROUTE_DEL  %s/%u via %s\n",
                    fmt_ipv4(p.dst_addr).c_str(),
                    static_cast<unsigned>(p.prefix_len),
                    fmt_ipv4(p.gateway).c_str());
    }
    else
    {
        std::printf("[cmd_proto] ROUTE_DEL  %s/%u (any gateway)\n",
                    fmt_ipv4(p.dst_addr).c_str(),
                    static_cast<unsigned>(p.prefix_len));
    }

    netlink::RouteDelParams np;
    np.dst = p.dst_addr;
    np.prefix_len = p.prefix_len;
    np.table = netlink::RouteTable::Main;

    if (p.gateway != zero)
    {
        np.gateway = p.gateway;
        np.has_gateway = true;
    }

    if (g_callbacks.route_del)
    {
        // hw ASIC
        return g_callbacks.route_del(std::make_shared<netlink::RouteDelParams>(np));
    }
    else
    {
        // virt ASIC
        return netlink::delete_route(np);
    }
}

/// @brief @copybrief cmdproto::handle_route_list
std::expected<std::vector<RouteEntry>, std::error_code> handle_route_list()
{
    std::expected<std::vector<netlink::RouteEntry>, std::error_code> nl_routes;

    if (g_callbacks.route_list)
    {
        // hw ASIC
        auto table = std::make_shared<netlink::RouteTable>(netlink::RouteTable::Main);
        nl_routes = g_callbacks.route_list(std::move(table));
    }
    else
    {
        // virt ASIC
        nl_routes = netlink::list_routes(netlink::RouteTable::Main);
    }

    if (!nl_routes)
        return std::unexpected(nl_routes.error());
    
    std::vector<RouteEntry> routes;
    routes.reserve(nl_routes->size());

    for (const auto& r : *nl_routes)
    {
        RouteEntry e;
        e.dst_addr = r.dst;
        e.prefix_len = r.prefix_len;
        e.gateway = r.has_gateway ? r.gateway : Ipv4Addr{};
        e.metric = static_cast<std::uint16_t>(r.metric);
        e.if_name = r.if_name;
        routes.push_back(std::move(e));
    }

    std::printf("[cmd_proto] ROUTE_LIST returning %zu route(s)\n",
                routes.size());
    return routes;
}

// ---------------------------------------------------------------------------
// Binary ROUTE_ADD payload — command builder
// ---------------------------------------------------------------------------

/// @brief @copybrief cmdproto::make_route_add_binary
Command make_route_add_binary(const SingleRouteRequest& req)
{
    Command cmd;
    cmd.cmd_id = CmdId::ROUTE_ADD;
    if (auto bytes = encode_route_add_payload(req))
        cmd.raw_payload = std::move(*bytes);
    return cmd;
}

// ---------------------------------------------------------------------------
// Binary ROUTE_ADD payload — encode / decode / handle
// ---------------------------------------------------------------------------

/// @brief @copybrief cmdproto::encode_route_add_payload
std::expected<std::vector<std::uint8_t>, std::error_code>
encode_route_add_payload(const SingleRouteRequest& req)
{
    if (req.interfaces.size() > 0xFFFFu)
        return std::unexpected(make_error_code(CmdError::InvalidField));

    std::vector<std::uint8_t> out;
    out.reserve(3 + req.interfaces.size() * INTERFACE_FIXED_SIZE);

    // type (1 byte)
    out.push_back(static_cast<std::uint8_t>(RouteAddPayloadType::SINGLE_ROUTE));

    // iface_count (2 bytes big-endian)
    write_be16(out, static_cast<std::uint16_t>(req.interfaces.size()));

    for (const auto& iface : req.interfaces)
    {
        if (iface.prefixes.size() > 0xFFFFu)
            return std::unexpected(make_error_code(CmdError::InvalidField));

        // iface_name: 32 bytes, null-padded
        for (std::size_t i = 0; i < IFACE_NAME_SIZE; ++i)
            out.push_back(static_cast<std::uint8_t>(iface.iface_name[i]));

        // nexthop_addr_ipv4: 4 bytes network order
        out.insert(out.end(),
                   iface.nexthop_addr_ipv4.begin(),
                   iface.nexthop_addr_ipv4.end());

        // nexthop_id_ipv4: 4 bytes big-endian
        write_be32(out, iface.nexthop_id_ipv4);

        // prefix_count: 2 bytes big-endian
        write_be16(out, static_cast<std::uint16_t>(iface.prefixes.size()));

        // prefix_ipv4[]: N × 5 bytes (addr + mask)
        for (const auto& p : iface.prefixes)
        {
            if (p.mask_len > 32)
                return std::unexpected(make_error_code(CmdError::InvalidField));
            out.insert(out.end(), p.addr.begin(), p.addr.end());
            out.push_back(p.mask_len);
        }
    }

    return out;
}

/// @brief @copybrief cmdproto::decode_route_add_payload
std::expected<SingleRouteRequest, std::error_code>
decode_route_add_payload(std::span<const std::uint8_t> raw)
{
    // Minimum: type(1) + iface_count(2) = 3 bytes.
    if (raw.size() < 3)
        return std::unexpected(make_error_code(CmdError::InvalidCommand));

    if (raw[0] != static_cast<std::uint8_t>(RouteAddPayloadType::SINGLE_ROUTE))
        return std::unexpected(make_error_code(CmdError::InvalidCommand));

    const auto iface_count = static_cast<std::size_t>(read_be16(raw, 1));

    SingleRouteRequest req;
    req.interfaces.reserve(iface_count);
    std::size_t pos = 3;

    for (std::size_t i = 0; i < iface_count; ++i)
    {
        if (raw.size() - pos < INTERFACE_FIXED_SIZE)
            return std::unexpected(make_error_code(CmdError::InvalidCommand));

        Interface iface;

        // iface_name: 32 bytes
        for (std::size_t j = 0; j < IFACE_NAME_SIZE; ++j)
            iface.iface_name[j] = static_cast<char>(raw[pos + j]);
        pos += IFACE_NAME_SIZE;

        // nexthop_addr_ipv4: 4 bytes
        std::copy(raw.begin() + static_cast<std::ptrdiff_t>(pos),
                  raw.begin() + static_cast<std::ptrdiff_t>(pos + 4),
                  iface.nexthop_addr_ipv4.begin());
        pos += 4;

        // nexthop_id_ipv4: 4 bytes big-endian
        iface.nexthop_id_ipv4 = read_be32(raw, pos);
        pos += 4;

        // prefix_count: 2 bytes big-endian
        const auto prefix_count = static_cast<std::size_t>(read_be16(raw, pos));
        pos += 2;

        const std::size_t prefix_bytes = prefix_count * PREFIX_IPV4_WIRE_SIZE;
        if (raw.size() - pos < prefix_bytes)
            return std::unexpected(make_error_code(CmdError::InvalidCommand));

        iface.prefixes.reserve(prefix_count);
        for (std::size_t k = 0; k < prefix_count; ++k)
        {
            PrefixIpv4 p;
            std::copy(raw.begin() + static_cast<std::ptrdiff_t>(pos),
                      raw.begin() + static_cast<std::ptrdiff_t>(pos + 4),
                      p.addr.begin());
            p.mask_len = raw[pos + 4];
            if (p.mask_len > 32)
                return std::unexpected(make_error_code(CmdError::InvalidField));
            pos += PREFIX_IPV4_WIRE_SIZE;
            iface.prefixes.push_back(p);
        }

        logger::log(logger::INFO, "cmdproto",
                    std::format("  iface='{}' nexthop={} id={} prefixes={}",
                                iface.iface_name.data(),
                                fmt_ipv4(iface.nexthop_addr_ipv4),
                                iface.nexthop_id_ipv4,
                                iface.prefixes.size()));

        req.interfaces.push_back(std::move(iface));
    }

    logger::log(logger::INFO, "cmdproto",
                std::format("decode_route_add_payload: type=SINGLE_ROUTE"
                            "  interfaces={}",
                            req.interfaces.size()));
    return req;
}

/// @brief @copybrief cmdproto::encode_route_add_binary_response
std::expected<std::vector<std::uint8_t>, std::error_code>
encode_route_add_binary_response(const RouteAddBinaryResponse& resp)
{
    const std::size_t n = resp.prefix_status.size();
    const std::size_t status_bytes = (n + 7) / 8; // ceil(n / 8)

    std::vector<std::uint8_t> out;
    out.reserve(1 + status_bytes);
    out.push_back(resp.status_code);

    for (std::size_t byte_idx = 0; byte_idx < status_bytes; ++byte_idx)
    {
        std::uint8_t b = 0;
        for (std::size_t bit = 0; bit < 8; ++bit)
        {
            const std::size_t idx = byte_idx * 8 + bit;
            if (idx < n && resp.prefix_status[idx])
                b |= static_cast<std::uint8_t>(1u << bit);
        }
        out.push_back(b);
    }

    // Log the encoded response.
    std::string bits_hex;
    for (std::size_t i = 1; i < out.size(); ++i)
    {
        if (i > 1)
            bits_hex += ' ';
        bits_hex += std::format("{:02x}", out[i]);
    }
    logger::log(logger::INFO, "cmdproto",
                std::format("encode_route_add_binary_response:"
                            "  status=0x{:02x}  prefix_bytes={}  [{}]",
                            resp.status_code, status_bytes,
                            bits_hex.empty() ? "(none)" : bits_hex));

    return out;
}

/// @brief @copybrief cmdproto::decode_route_add_binary_response
std::expected<RouteAddBinaryResponse, std::error_code>
decode_route_add_binary_response(std::span<const std::uint8_t> raw)
{
    if (raw.empty())
        return std::unexpected(make_error_code(CmdError::InvalidCommand));

    RouteAddBinaryResponse resp;
    resp.status_code = raw[0];

    // Unpack one bit per prefix from remaining bytes (LSB-first within byte).
    for (std::size_t byte_idx = 1; byte_idx < raw.size(); ++byte_idx)
        for (std::size_t bit = 0; bit < 8; ++bit)
            resp.prefix_status.push_back(((raw[byte_idx] >> bit) & 1u) != 0);

    return resp;
}

/// @brief @copybrief cmdproto::handle_route_add_payload
std::expected<RouteAddBinaryResponse, std::error_code>
handle_route_add_payload(const SingleRouteRequest& req)
{
    std::size_t total_prefixes = 0;
    for (const auto& iface : req.interfaces)
        total_prefixes += iface.prefixes.size();

    logger::log(logger::INFO, "cmdproto",
                std::format("handle_route_add_payload: {} interface(s)"
                            " {} total prefix(es)",
                            req.interfaces.size(), total_prefixes));

    RouteAddBinaryResponse resp;
    resp.status_code = 0x00;
    resp.prefix_status.reserve(total_prefixes);

    for (const auto& iface : req.interfaces)
    {
        logger::log(logger::INFO, "cmdproto",
                    std::format("  iface='{}' nexthop={} id={} prefixes={}",
                                iface.iface_name.data(),
                                fmt_ipv4(iface.nexthop_addr_ipv4),
                                iface.nexthop_id_ipv4,
                                iface.prefixes.size()));

        for (std::size_t i = 0; i < iface.prefixes.size(); ++i)
        {
            const auto& pfx = iface.prefixes[i];

            netlink::RouteAddParams np;
            np.dst         = pfx.addr;
            np.prefix_len  = pfx.mask_len;
            np.gateway     = iface.nexthop_addr_ipv4;
            np.has_gateway = true;
            np.if_name     = iface.iface_name.data();
            np.scope       = netlink::RouteScope::Universe;
            np.protocol    = netlink::RouteProtocol::Static;
            np.type        = netlink::RouteType::Unicast;
            np.table       = netlink::RouteTable::Main;

            //auto result = netlink::add_route(np);
            if (g_callbacks.route_add) {
                g_callbacks.route_add(std::make_shared<netlink::RouteAddParams>(np));
            }
            const bool ok = true; //result.has_value();
            resp.prefix_status.push_back(ok);
            if (!ok)
                resp.status_code = 0x01;

            logger::log(logger::INFO, "cmdproto",
                        std::format("    prefix[{:02}]  {}/{}  via {}  dev {}"
                                    "  ->  {}{}",
                                    i,
                                    fmt_ipv4(pfx.addr),
                                    static_cast<unsigned>(pfx.mask_len),
                                    fmt_ipv4(iface.nexthop_addr_ipv4),
                                    iface.iface_name.data(),
                                    ok ? "OK" : "FAIL",
                                    ok ? "" : std::format("  ({})",
                                        "CB TEST"/*result.error().message()*/)));
        }
    }

    const int ok_count =
        static_cast<int>(std::count(resp.prefix_status.begin(),
                                    resp.prefix_status.end(), true));
    logger::log(logger::INFO, "cmdproto",
                std::format("handle_route_add_payload: {}/{} prefix(es)"
                            " installed  status=0x{:02x}",
                            ok_count,
                            static_cast<int>(total_prefixes),
                            resp.status_code));

    return resp;
}

} // namespace cmdproto
