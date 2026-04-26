/**
 * @file cmd_proto.hpp
 * @brief Command protocol layer carried inside routeproto exchange-data.
 *
 * This layer is encapsulated in the @c commands field of a
 * routeproto::ExchangeData block and defines structured commands for IPv4 route
 * management.
 *
 * ## Command wire format (big-endian)
 * Commands in the command array are laid out sequentially.  Each command
 * starts with a 3-byte header followed by its field data:
 *
 * | Field    | Type     | Size | Notes                                   |
 * |----------|----------|------|-----------------------------------------|
 * | cmd_id   | uint8_t  | 1    | Command identifier (see CmdId enum)     |
 * | data_len | uint16_t | 2    | Total byte length of all field entries  |
 * | fields   | Field[]  | var  | Zero or more field entries              |
 *
 * Each field entry inside @c data_len bytes:
 *
 * | Sub-field  | Type    | Size | Notes                               |
 * |------------|---------|------|-------------------------------------|
 * | field_id   | uint8_t | 1    | Field identifier (see FieldId enum) |
 * | field_len  | uint8_t | 1    | Length of @c field_data in bytes    |
 * | field_data | uint8_t | N    | Field-specific payload              |
 *
 * ## Supported commands
 * | CmdId      | Value | Description                         |
 * |------------|-------|-------------------------------------|
 * | ROUTE_ADD  | 0x01  | Install an IPv4 network route       |
 * | ROUTE_DEL  | 0x02  | Remove an IPv4 network route        |
 * | ROUTE_LIST | 0x03  | Retrieve the IPv4 routing table     |
 *
 * ## Field IDs used by route commands
 * | FieldId    | Value | Size    | Description                         |
 * |------------|-------|---------|-------------------------------------|
 * | DST_ADDR   | 0x01  | 4 bytes | Destination IPv4 (network order)    |
 * | PREFIX_LEN | 0x02  | 1 byte  | CIDR prefix length (0–32)           |
 * | GATEWAY    | 0x03  | 4 bytes | Next-hop gateway IPv4 (net order)   |
 * | IF_NAME    | 0x04  | var     | Egress interface name (UTF-8)       |
 * | METRIC     | 0x05  | 2 bytes | Route metric (uint16_t big-endian)  |
 *
 * @author  Generated
 * @date    2026
 */

#pragma once

#include "routing.hpp"

#include <array>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <system_error>
#include <vector>

namespace cmdproto
{

// ---------------------------------------------------------------------------
// Command identifiers
// ---------------------------------------------------------------------------

/// @brief Command identifiers carried in the @c cmd_id byte.
enum class CmdId : std::uint8_t
{
    ROUTE_ADD = 0x01,  ///< Install an IPv4 network route in the kernel
    ROUTE_DEL = 0x02,  ///< Remove an IPv4 network route from the kernel
    ROUTE_LIST = 0x03, ///< Retrieve the list of IPv4 routes
};

// ---------------------------------------------------------------------------
// Field identifiers
// ---------------------------------------------------------------------------

/// @brief Field identifiers shared across route commands.
enum class FieldId : std::uint8_t
{
    DST_ADDR = 0x01, ///< Destination IPv4 address (4 bytes, network byte order)
    PREFIX_LEN = 0x02, ///< CIDR prefix length (1 byte, 0–32)
    GATEWAY =
        0x03, ///< Next-hop gateway IPv4 address (4 bytes, network byte order)
    IF_NAME = 0x04, ///< Egress interface name (variable-length UTF-8 string)
    METRIC = 0x05,  ///< Route metric (2 bytes, uint16_t big-endian)
    VRF_NAME = 0x06, ///< VRF name (variable-length UTF-8 string)
};

// ---------------------------------------------------------------------------
// Error codes
// ---------------------------------------------------------------------------

/// @brief Errors specific to the command protocol layer.
enum class CmdError : int
{
    InvalidCommand = 1, ///< Buffer too small or malformed command header
    UnknownCommand,     ///< Unrecognised CmdId value
    MissingField,       ///< Required field is absent from the command
    InvalidField,       ///< Field length or value is semantically invalid
};

/// @brief Returns the error category for CmdError.
const std::error_category& cmd_error_category() noexcept;

/// @brief Creates a std::error_code from a CmdError.
std::error_code make_error_code(CmdError e) noexcept;

} // namespace cmdproto

template <>
struct std::is_error_code_enum<cmdproto::CmdError> : std::true_type
{};

namespace cmdproto
{

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

/// @brief IPv4 address type: 4 raw bytes in network byte order.
using Ipv4Addr = std::array<std::uint8_t, 4>;

/// @brief Byte size of the per-command header (cmd_id + data_len).
inline constexpr std::size_t CMD_HEADER_SIZE = 3; // cmd_id(1) + data_len(2)

/// @brief Byte size of the per-field header (field_id + field_len).
inline constexpr std::size_t FIELD_HEADER_SIZE =
    2; // field_id(1) + field_len(1)

// ---------------------------------------------------------------------------
// Generic field and command structures
// ---------------------------------------------------------------------------

/// @brief A single decoded field within a command.
struct Field
{
    FieldId field_id{};             ///< Field identifier
    std::vector<std::uint8_t> data; ///< Raw field data bytes
};

/// @brief A fully decoded command with its list of fields.
struct Command
{
    CmdId cmd_id{};                        ///< Command identifier
    std::vector<Field> fields;             ///< TLV fields (legacy format)
    std::vector<std::uint8_t> raw_payload; ///< Raw binary payload (new format)
};

// ---------------------------------------------------------------------------
// Typed parameter structures
// ---------------------------------------------------------------------------

/**
 * @brief Parameters for a ROUTE_ADD command.
 *
 * Mandatory fields: dst_addr, prefix_len, gateway, if_name.
 * Optional fields: metric (0 means "use kernel default").
 */
struct RouteAddParams
{
    Ipv4Addr dst_addr{};       ///< Destination network address
    std::uint8_t prefix_len{}; ///< CIDR prefix length (0–32)
    Ipv4Addr gateway{};        ///< Next-hop gateway address
    std::string if_name;       ///< Egress network interface name
    std::uint16_t metric{};    ///< Route metric (0 = kernel default)
};

/**
 * @brief Parameters for a ROUTE_DEL command.
 *
 * Mandatory fields: dst_addr, prefix_len.
 * Optional fields: gateway (all-zero means "match any gateway").
 */
struct RouteDelParams
{
    Ipv4Addr dst_addr{};       ///< Destination network address
    std::uint8_t prefix_len{}; ///< CIDR prefix length (0–32)
    Ipv4Addr gateway{};        ///< Gateway to match (all-zero = any)
    std::string vrfs_name;     ///< VRF name (empty = default VRF)
};

// ---------------------------------------------------------------------------
// Route-list response entry
// ---------------------------------------------------------------------------

/**
 * @brief A single IPv4 route entry returned by ROUTE_LIST.
 *
 * ## Wire encoding inside a ROUTE_LIST response payload
 * Entries are concatenated without padding.  Each entry layout:
 *
 * | Field       | Type    | Size | Notes                             |
 * |-------------|---------|------|-----------------------------------|
 * | dst_addr    | uint8_t | 4    | Destination address (network ord) |
 * | prefix_len  | uint8_t | 1    | CIDR prefix length                |
 * | gateway     | uint8_t | 4    | Gateway address (network order)   |
 * | metric      | uint16_t| 2    | Route metric (big-endian)         |
 * | if_name_len | uint8_t | 1    | Length of if_name in bytes        |
 * | if_name     | uint8_t | N    | Interface name (UTF-8, no NUL)    |
 */
struct RouteEntry
{
    Ipv4Addr dst_addr{};       ///< Destination network address
    std::uint8_t prefix_len{}; ///< CIDR prefix length (0–32)
    Ipv4Addr gateway{};        ///< Next-hop gateway address
    std::uint16_t metric{};    ///< Route metric
    std::string if_name;       ///< Interface name
};

/// @brief Minimum wire size of one RouteEntry (empty if_name): 4+1+4+2+1 = 12
inline constexpr std::size_t ROUTE_ENTRY_MIN_SIZE = 12;

// ---------------------------------------------------------------------------
// Binary ROUTE_ADD payload — request structures
// ---------------------------------------------------------------------------

/// @brief Payload type discriminator carried as the first byte of a binary
///        ROUTE_ADD payload.
enum class RouteAddPayloadType : std::uint8_t
{
    SINGLE_ROUTE = 1, ///< Interface route-add: type + iface_count + interface[]
};

/// @brief Wire size in bytes of one prefix_ipv4 entry (4-byte address +
///        1-byte mask length).
inline constexpr std::size_t PREFIX_IPV4_WIRE_SIZE = 5;

/// @brief One IPv4 prefix entry carried inside an Interface.
struct PrefixIpv4
{
    Ipv4Addr addr{};         ///< Prefix network address (network byte order)
    std::uint8_t mask_len{}; ///< Prefix length in bits (0–32)
};

/// @brief Fixed-length interface name field: 32 bytes, null-terminated.
using IfaceName = std::array<char, 32>;

/// @brief Wire size of the interface name field.
inline constexpr std::size_t IFACE_NAME_SIZE = 32;

/// @brief Fixed-size portion of one interface wire entry:
///        iface_name(32) + nexthop_addr(4) + nexthop_id(4) + prefix_count(2).
inline constexpr std::size_t INTERFACE_FIXED_SIZE = 42;

/**
 * @brief One interface entry in a SingleRouteRequest.
 *
 * ## Wire layout (big-endian, no padding)
 * | Field             | Size    | Notes                              |
 * |-------------------|---------|------------------------------------|
 * | iface_name        | 32      | NUL-terminated interface name      |
 * | nexthop_addr_ipv4 | 4       | Next-hop gateway (network order)   |
 * | nexthop_id_ipv4   | 4       | Next-hop identifier (big-endian)   |
 * | prefix_count      | 2       | Number of following prefix entries |
 * | prefix_ipv4[]     | N × 5   | N prefix entries (addr+mask each)  |
 */
struct Interface
{
    IfaceName iface_name{};          ///< Interface name (32 bytes, null-padded)
    Ipv4Addr nexthop_addr_ipv4{};    ///< Next-hop gateway address
    std::uint32_t nexthop_id_ipv4{}; ///< Next-hop identifier
    std::vector<PrefixIpv4> prefixes; ///< Prefix list for this interface
};

/**
 * @brief Decoded ROUTE_ADD binary request payload (type == SINGLE_ROUTE).
 *
 * ## Wire layout (big-endian, no padding)
 * | Field             | Size    | Notes                                   |
 * |-------------------|---------|-----------------------------------------|
 * | type              | 1 byte  | RouteAddPayloadType::SINGLE_ROUTE       |
 * | vrfs_name_len     | 1 byte  | Length of vrfs_name in bytes            |
 * | vrfs_name         | N bytes | VRF name (UTF-8, no NUL)                |
 * | iface_count       | 2 bytes | Number of interface entries (big-endian)|
 * | interface[]       | M × var | M interface entries                     |
 *
 * Each interface entry:
 * | iface_name        | 32      | NUL-terminated interface name           |
 * | nexthop_addr_ipv4 | 4       | Next-hop gateway (network order)        |
 * | nexthop_id_ipv4   | 4       | Next-hop identifier (big-endian)        |
 * | prefix_count      | 2       | Number of prefix entries (big-endian)   |
 * | prefix_ipv4[]     | K × 5   | K prefix entries (addr+mask each)       |
 */
struct SingleRouteRequest
{
    std::string vrfs_name;             ///< VRF name (empty = default VRF)
    std::vector<Interface> interfaces; ///< Interface list (may be empty)
};

// ---------------------------------------------------------------------------
// Binary ROUTE_ADD payload — response structure
// ---------------------------------------------------------------------------

/**
 * @brief Decoded ROUTE_ADD binary response payload.
 *
 * ## Wire layout
 * | Field         | Size        | Notes                               |
 * |---------------|-------------|-------------------------------------|
 * | status_code   | 1 byte      | 0x00 = all prefixes installed ok    |
 * | prefix_status | ⌈N/8⌉ bytes | Packed bits, one per prefix         |
 *
 * Bit packing: bit 0 of the first status byte corresponds to prefix[0],
 * bit 1 to prefix[1], etc.  A set bit (1) indicates success for that
 * prefix.  Padding bits in the last byte (when N is not a multiple of 8)
 * are zero.  The total wire size is always an integer number of bytes.
 */
struct RouteAddBinaryResponse
{
    std::uint8_t status_code{};      ///< Overall status (0x00 = all ok)
    std::vector<bool> prefix_status; ///< Per-prefix result (true = installed)
};

// ---------------------------------------------------------------------------
// Encode / decode — generic Command
// ---------------------------------------------------------------------------

/**
 * @brief Serialises a single Command into raw bytes.
 * @param cmd  Command to encode.
 * @return Encoded bytes on success, or an error code.
 */
[[nodiscard]] std::expected<std::vector<std::uint8_t>, std::error_code>
encode_command(const Command& cmd);

/**
 * @brief Deserialises one Command from the front of @p raw.
 *
 * Only consumes as many bytes as the command's data_len field specifies.
 * Use decode_commands() to parse a concatenated sequence of commands.
 *
 * @param raw  Byte span starting at the cmd_id byte.
 * @return Decoded Command on success, or an error code.
 */
[[nodiscard]] std::expected<Command, std::error_code>
decode_command(std::span<const std::uint8_t> raw);

/**
 * @brief Deserialises all Commands from a flat byte buffer.
 *
 * Commands are expected to be concatenated sequentially with no padding.
 *
 * @param raw  Buffer containing zero or more encoded commands.
 * @return Vector of decoded Commands on success, or an error code.
 */
[[nodiscard]] std::expected<std::vector<Command>, std::error_code>
decode_commands(std::span<const std::uint8_t> raw);

// ---------------------------------------------------------------------------
// Command builders
// ---------------------------------------------------------------------------

/// @brief Builds a ROUTE_ADD Command from the given parameters (TLV format).
[[nodiscard]] Command make_route_add(const RouteAddParams& p);

/// @brief Builds a ROUTE_ADD Command using the new binary payload format.
[[nodiscard]] Command make_route_add_binary(const SingleRouteRequest& req);

/// @brief Builds a ROUTE_DEL Command from the given parameters.
[[nodiscard]] Command make_route_del(const RouteDelParams& p);

/// @brief Builds a ROUTE_LIST Command with an optional VRF name.
[[nodiscard]] Command make_route_list(const std::string& vrfs_name = {});

// ---------------------------------------------------------------------------
// Typed-parameter extractors
// ---------------------------------------------------------------------------

/**
 * @brief Extracts RouteAddParams from a decoded ROUTE_ADD Command.
 * @param cmd  Decoded command; must have cmd_id == CmdId::ROUTE_ADD.
 * @return Populated RouteAddParams or an error code.
 */
[[nodiscard]] std::expected<RouteAddParams, std::error_code>
parse_route_add(const Command& cmd);

/**
 * @brief Extracts RouteDelParams from a decoded ROUTE_DEL Command.
 * @param cmd  Decoded command; must have cmd_id == CmdId::ROUTE_DEL.
 * @return Populated RouteDelParams or an error code.
 */
[[nodiscard]] std::expected<RouteDelParams, std::error_code>
parse_route_del(const Command& cmd);

// ---------------------------------------------------------------------------
// Route-list response encode / decode
// ---------------------------------------------------------------------------

/**
 * @brief Serialises a list of RouteEntry values into a response payload.
 * @param routes  Entries to encode.
 * @return Encoded byte vector on success, or an error code.
 */
[[nodiscard]] std::expected<std::vector<std::uint8_t>, std::error_code>
encode_route_list_response(const std::vector<RouteEntry>& routes);

/**
 * @brief Deserialises a ROUTE_LIST response payload into RouteEntry values.
 * @param raw  Encoded response payload.
 * @return Decoded route list on success, or an error code.
 */
[[nodiscard]] std::expected<std::vector<RouteEntry>, std::error_code>
decode_route_list_response(std::span<const std::uint8_t> raw);

// ---------------------------------------------------------------------------
// Callback mechanism
// ---------------------------------------------------------------------------

/**
 * @brief Map of handler callbacks for route operations.
 *
 * Assign a callable to each field to override the default netlink
 * implementation.  A null (default-constructed) function means the handler
 * falls back to the direct netlink call.  Data is transferred to each
 * callback via std::shared_ptr.
 *
 * Example — wire the default netlink back-end:
 * @code
 *   cmdproto::HandlerCallbacks cbs;
 *   cbs.route_add  = [](std::shared_ptr<netlink::RouteAddParams> p)
 *       { return netlink::add_route(*p); };
 *   cbs.route_del  = [](std::shared_ptr<netlink::RouteDelParams> p)
 *       { return netlink::delete_route(*p); };
 *   cbs.route_list = [](std::shared_ptr<netlink::RouteListParams> p)
 *       { return netlink::list_routes(p->table); };
 *   cmdproto::init_callbacks(std::move(cbs));
 * @endcode
 */
struct HandlerCallbacks
{
    /// Signature for route-add callbacks.
    using AddRouteFn = std::function<std::expected<void, std::error_code>(
        std::shared_ptr<netlink::RouteAddParams>)>;

    /// Signature for route-delete callbacks.
    using DelRouteFn = std::function<std::expected<void, std::error_code>(
        std::shared_ptr<netlink::RouteDelParams>)>;

    /// Signature for route-list callbacks.
    using ListRoutesFn = std::function<
        std::expected<std::vector<netlink::RouteEntry>, std::error_code>(
            std::shared_ptr<netlink::RouteListParams>)>;

    AddRouteFn route_add;    ///< Called by handle_route_add()
    DelRouteFn route_del;    ///< Called by handle_route_del()
    ListRoutesFn route_list; ///< Called by handle_route_list()
};

/**
 * @brief Registers the handler callbacks used by handle_route_add(),
 *        handle_route_del(), and handle_route_list().
 *
 * Must be called before any handler function is invoked.  Each null entry
 * in @p callbacks causes the corresponding handler to fall back to the
 * direct netlink call.
 *
 * @param callbacks  Callback map to install.
 */
void init_callbacks(HandlerCallbacks callbacks);

// ---------------------------------------------------------------------------
// Stub command handlers
// ---------------------------------------------------------------------------

/**
 * @brief Stub handler: install an IPv4 network route.
 *
 * In a real implementation this would invoke @c ip_route_add() or the
 * equivalent netlink/RTNL call.  This stub prints the route parameters
 * and returns success.
 *
 * @param p  Route parameters parsed from a ROUTE_ADD command.
 * @return void on success, or an error code.
 */
[[nodiscard]] std::expected<void, std::error_code>
handle_route_add(const RouteAddParams& p);

/**
 * @brief Stub handler: remove an IPv4 network route.
 *
 * In a real implementation this would invoke @c ip_route_del() or the
 * equivalent netlink/RTNL call.  This stub prints the route parameters
 * and returns success.
 *
 * @param p  Route parameters parsed from a ROUTE_DEL command.
 * @return void on success, or an error code.
 */
[[nodiscard]] std::expected<void, std::error_code>
handle_route_del(const RouteDelParams& p);

/**
 * @brief Stub handler: retrieve the IPv4 routing table.
 *
 * In a real implementation this would read @c /proc/net/route or issue an
 * RTNL dump request.  This stub returns a small fixed set of sample routes.
 *
 * @return Populated route list on success, or an error code.
 */
[[nodiscard]] std::expected<std::vector<RouteEntry>, std::error_code>
handle_route_list(const std::string& vrfs_name = {});

// ---------------------------------------------------------------------------
// Binary ROUTE_ADD payload — encode / decode / handle
// ---------------------------------------------------------------------------

/**
 * @brief Serialises a SingleRouteRequest to its binary wire bytes.
 *
 * Produces: type(1) + vrfs_name_len(1) + vrfs_name(N) + iface_count(2) + M × interface entries.
 *
 * @param req  Request to encode.
 * @return Encoded bytes on success, or an error code.
 */
[[nodiscard]] std::expected<std::vector<std::uint8_t>, std::error_code>
encode_route_add_payload(const SingleRouteRequest& req);

/**
 * @brief Deserialises a SingleRouteRequest from raw binary payload bytes.
 *
 * Validates the type byte (must be SINGLE_ROUTE = 1), iface_count, and each
 * interface entry including mask_len values (0–32).
 *
 * @param raw  Payload bytes starting at the type byte.
 * @return Decoded SingleRouteRequest on success, or an error code.
 */
[[nodiscard]] std::expected<SingleRouteRequest, std::error_code>
decode_route_add_payload(std::span<const std::uint8_t> raw);

/**
 * @brief Serialises a RouteAddBinaryResponse to its wire bytes.
 *
 * Produces: status_code(1) + ⌈N/8⌉ bytes of packed prefix_status bits.
 *
 * @param resp  Response to encode.
 * @return Encoded bytes on success, or an error code.
 */
[[nodiscard]] std::expected<std::vector<std::uint8_t>, std::error_code>
encode_route_add_binary_response(const RouteAddBinaryResponse& resp);

/**
 * @brief Deserialises a RouteAddBinaryResponse from its wire bytes.
 *
 * The number of prefix_status bits equals (raw.size() - 1) × 8.
 * Callers should truncate to the expected prefix count if needed.
 *
 * @param raw  Response bytes starting at status_code.
 * @return Decoded RouteAddBinaryResponse on success, or an error code.
 */
[[nodiscard]] std::expected<RouteAddBinaryResponse, std::error_code>
decode_route_add_binary_response(std::span<const std::uint8_t> raw);

/**
 * @brief Handles a binary ROUTE_ADD request.
 *
 * Installs each prefix in @p req via netlink and returns a
 * RouteAddBinaryResponse with per-prefix success bits.
 *
 * @param req  Decoded binary request payload.
 * @return RouteAddBinaryResponse on success, or an error code.
 */
[[nodiscard]] std::expected<RouteAddBinaryResponse, std::error_code>
handle_route_add_payload(const SingleRouteRequest& req);

} // namespace cmdproto
