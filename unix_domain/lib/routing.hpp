/**
 * @file routing.hpp
 * @brief Linux netlink-based route and network interface management API.
 *
 * Provides a C++23 interface for inspecting and modifying the kernel IPv4
 * routing table and for querying network interface state, using AF_NETLINK /
 * NETLINK_ROUTE sockets directly.
 *
 * ## Capabilities
 * Read-only operations (list_routes, list_interfaces, list_interface_addresses,
 * get_interface, get_interface_index) require no special privileges.
 * Write operations (add_route, replace_route, delete_route) require
 * @c CAP_NET_ADMIN.
 *
 * ## Error handling
 * All functions return @c std::expected<T, std::error_code>.  Errors from the
 * kernel are propagated as @c std::system_category() error codes (errno
 * values).  Module-specific failures use the @c netlink error category
 * (NetlinkError).
 *
 * ## Example — add a static route
 * @code
 * auto dst = netlink::parse_ipv4("10.0.0.0");
 * auto gw  = netlink::parse_ipv4("192.168.1.1");
 * if (dst && gw) {
 *     netlink::RouteAddParams p;
 *     p.dst         = *dst;
 *     p.prefix_len  = 8;
 *     p.gateway     = *gw;
 *     p.has_gateway = true;
 *     p.if_name     = "eth0";
 *     if (auto r = netlink::add_route(p); !r) {
 *         // handle r.error()
 *     }
 * }
 * @endcode
 *
 * @author  Generated
 * @date    2026
 */

#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <string>
#include <system_error>
#include <vector>

namespace netlink
{

// ---------------------------------------------------------------------------
// Error codes
// ---------------------------------------------------------------------------

/// @brief Error codes specific to the netlink routing module.
enum class NetlinkError : int
{
    SocketCreateFailed = 1, ///< Failed to create an AF_NETLINK socket
    BindFailed,             ///< Failed to bind the netlink socket
    SendFailed,             ///< Failed to send a netlink request message
    RecvFailed,             ///< Failed to receive a netlink response message
    KernelError,       ///< Kernel returned an NLMSG_ERROR with a non-zero errno
    ParseError,        ///< Received a malformed or truncated netlink message
    NotFound,          ///< Requested object was not found
    AddressError,      ///< IP address string could not be parsed
    InterfaceNotFound, ///< Named network interface does not exist
};

/// @brief Returns the error category singleton for NetlinkError.
const std::error_category& netlink_error_category() noexcept;

/// @brief Creates a @c std::error_code from a NetlinkError value.
std::error_code make_error_code(NetlinkError e) noexcept;

} // namespace netlink

// Register NetlinkError with the standard error_code infrastructure.
template <>
struct std::is_error_code_enum<netlink::NetlinkError> : std::true_type
{};

namespace netlink
{

// ---------------------------------------------------------------------------
// Address types
// ---------------------------------------------------------------------------

/// @brief IPv4 address as 4 raw bytes in network byte order.
using Ipv4Addr = std::array<std::uint8_t, 4>;

// ---------------------------------------------------------------------------
// Routing constants
// ---------------------------------------------------------------------------

/**
 * @brief Linux routing table identifiers (mirrors @c RT_TABLE_* defines).
 *
 * Custom table IDs (1–252) may be used with the underlying @c uint32_t value.
 */
enum class RouteTable : std::uint32_t
{
    Unspec = 0,    ///< RT_TABLE_UNSPEC — unspecified / match all (for dumps)
    Compat = 252,  ///< RT_TABLE_COMPAT — kernel backwards-compatibility table
    Default = 253, ///< RT_TABLE_DEFAULT — default routing table
    Main = 254, ///< RT_TABLE_MAIN   — main table (most user-installed routes)
    Local = 255, ///< RT_TABLE_LOCAL  — local and broadcast routes
};

/**
 * @brief Route originator identifiers (mirrors @c RTPROT_* defines).
 *
 * Identifies the entity that installed the route.
 */
enum class RouteProtocol : std::uint8_t
{
    Unspec = 0,   ///< RTPROT_UNSPEC   — unspecified
    Redirect = 1, ///< RTPROT_REDIRECT — installed by an ICMP redirect
    Kernel = 2,   ///< RTPROT_KERNEL   — installed by the kernel itself
    Boot = 3,     ///< RTPROT_BOOT     — installed during system boot
    Static = 4,   ///< RTPROT_STATIC   — installed by an administrator
    Ra = 9,    ///< RTPROT_RA       — installed by IPv6 router advertisement
    Dhcp = 16, ///< RTPROT_DHCP     — installed by DHCP
};

/**
 * @brief Route scope identifiers (mirrors @c RT_SCOPE_* defines).
 *
 * Describes the distance to the destination.
 */
enum class RouteScope : std::uint8_t
{
    Universe = 0, ///< RT_SCOPE_UNIVERSE — remote endpoint reachable via gateway
    Site = 200,   ///< RT_SCOPE_SITE     — within a local routing domain
    Link = 253, ///< RT_SCOPE_LINK     — directly reachable on the attached link
    Host = 254,    ///< RT_SCOPE_HOST     — local to this host
    Nowhere = 255, ///< RT_SCOPE_NOWHERE  — destination does not exist
};

/**
 * @brief Route type identifiers (mirrors @c RTN_* defines).
 *
 * Determines how matching packets are handled.
 */
enum class RouteType : std::uint8_t
{
    Unspec = 0,      ///< RTN_UNSPEC      — unspecified
    Unicast = 1,     ///< RTN_UNICAST     — regular forwarding route
    Local = 2,       ///< RTN_LOCAL       — local interface route
    Broadcast = 3,   ///< RTN_BROADCAST   — local broadcast route
    Anycast = 4,     ///< RTN_ANYCAST     — anycast route
    Multicast = 5,   ///< RTN_MULTICAST   — multicast route
    Blackhole = 6,   ///< RTN_BLACKHOLE   — silently discard matching packets
    Unreachable = 7, ///< RTN_UNREACHABLE — return ICMP "host unreachable"
    Prohibit = 8,    ///< RTN_PROHIBIT    — return ICMP "admin prohibited"
    Throw = 9,       ///< RTN_THROW       — continue lookup in another table
};

// ---------------------------------------------------------------------------
// Interface flags
// ---------------------------------------------------------------------------

/// @brief Bitmask type for network interface flags (mirrors @c IFF_* defines).
using IfFlags = std::uint32_t;

inline constexpr IfFlags IF_UP =
    0x0001U; ///< IFF_UP          — interface is administratively up
inline constexpr IfFlags IF_BROADCAST =
    0x0002U; ///< IFF_BROADCAST   — broadcast address supported
inline constexpr IfFlags IF_DEBUG =
    0x0004U; ///< IFF_DEBUG       — internal debugging flag
inline constexpr IfFlags IF_LOOPBACK =
    0x0008U; ///< IFF_LOOPBACK    — this is a loopback interface
inline constexpr IfFlags IF_POINTOPOINT =
    0x0010U; ///< IFF_POINTOPOINT — point-to-point link
inline constexpr IfFlags IF_RUNNING =
    0x0040U; ///< IFF_RUNNING     — link is operationally up
inline constexpr IfFlags IF_NOARP =
    0x0080U; ///< IFF_NOARP       — no ARP protocol
inline constexpr IfFlags IF_PROMISC =
    0x0100U; ///< IFF_PROMISC     — promiscuous mode enabled
inline constexpr IfFlags IF_MULTICAST =
    0x1000U; ///< IFF_MULTICAST   — multicast supported

// ---------------------------------------------------------------------------
// Data structures — routes
// ---------------------------------------------------------------------------

/**
 * @brief A single IPv4 route entry as reported by the kernel.
 *
 * Populated by list_routes().  All addresses are in network byte order.
 */
struct RouteEntry
{
    Ipv4Addr dst{}; ///< Destination network address (all-zero for default)
    std::uint8_t prefix_len{}; ///< CIDR prefix length (0–32)
    Ipv4Addr gateway{};        ///< Next-hop gateway address
    bool has_gateway{};        ///< True when a gateway attribute was present
    std::string if_name;       ///< Egress interface name (empty if unknown)
    int if_index{};            ///< Egress interface kernel index (0 if unknown)
    std::uint32_t metric{};    ///< Route metric / priority (lower = preferred)
    RouteTable table{RouteTable::Main}; ///< Routing table this entry belongs to
    RouteProtocol protocol{RouteProtocol::Static}; ///< Route originator
    RouteScope scope{RouteScope::Universe};        ///< Route scope
    RouteType type{RouteType::Unicast};            ///< Route handling type
};

/// @brief One IPv4 prefix carried inside a RouteInterface.
struct RoutePrefix
{
    Ipv4Addr addr{};         ///< Prefix network address (network byte order)
    std::uint8_t mask_len{}; ///< Prefix length in bits (0–32)
};

/// @brief One interface entry bundled inside a bulk RouteAddParams.
struct RouteInterface
{
    std::string iface_name;            ///< Egress interface name
    Ipv4Addr nexthop_addr{};           ///< Next-hop gateway address
    std::uint32_t nexthop_id{};        ///< Next-hop identifier
    std::vector<RoutePrefix> prefixes; ///< Prefix list for this interface
};

/// @brief Parameters for a route-list operation, including optional VRF context.
struct RouteListParams
{
    RouteTable table{RouteTable::Main}; ///< Routing table to query
    std::string vrfs_name;              ///< VRF name (empty = default VRF)
};

/**
 * @brief Parameters for adding or replacing an IPv4 route.
 *
 * Mandatory fields: @c dst, @c prefix_len.
 * At least one of @c gateway (with @c has_gateway = true) or @c if_name must
 * normally be provided so the kernel knows how to forward matching packets.
 */
struct RouteAddParams
{
    Ipv4Addr dst{};            ///< Destination network address
    std::uint8_t prefix_len{}; ///< CIDR prefix length (0–32)
    Ipv4Addr gateway{};        ///< Next-hop gateway address
    bool has_gateway{};        ///< Set to true when @c gateway is valid
    std::string if_name; ///< Egress interface name (empty = let kernel choose)
    std::uint32_t metric{};             ///< Route metric (0 = kernel default)
    RouteTable table{RouteTable::Main}; ///< Destination routing table
    RouteProtocol protocol{
        RouteProtocol::Static};             ///< Route originator to record
    RouteScope scope{RouteScope::Universe}; ///< Route scope
    RouteType type{RouteType::Unicast};     ///< Route type
    std::string vrfs_name;                  ///< VRF name (empty = default VRF)
    std::vector<RouteInterface> interfaces; ///< Interface list for bulk-add
};

/**
 * @brief Parameters for deleting an IPv4 route.
 *
 * Mandatory fields: @c dst, @c prefix_len.
 * The optional @c gateway and @c if_name fields narrow the match: if omitted,
 * the first matching destination/prefix entry is removed.
 */
struct RouteDelParams
{
    Ipv4Addr dst{};            ///< Destination network address to match
    std::uint8_t prefix_len{}; ///< CIDR prefix length to match (0–32)
    Ipv4Addr gateway{};        ///< Gateway to match
    bool has_gateway{};        ///< True when @c gateway is a match criterion
    std::string if_name;       ///< Egress interface to match (empty = any)
    RouteTable table{RouteTable::Main}; ///< Routing table to modify
    std::string vrfs_name;     ///< VRF name (empty = default VRF)
};

// ---------------------------------------------------------------------------
// Data structures — interfaces and addresses
// ---------------------------------------------------------------------------

/**
 * @brief Information about a network interface as reported by the kernel.
 *
 * Populated by list_interfaces(), get_interface(), and
 * get_interface_by_index().
 */
struct InterfaceInfo
{
    int index{};         ///< Kernel interface index (ifindex)
    std::string name;    ///< Interface name (e.g. "eth0", "lo")
    IfFlags flags{};     ///< Interface flags (bitwise OR of IF_* constants)
    std::uint32_t mtu{}; ///< Maximum transmission unit in bytes
    std::array<std::uint8_t, 6> hwaddr{}; ///< Hardware (MAC) address
    bool has_hwaddr{};                    ///< True when @c hwaddr is valid
    std::uint32_t link_type{}; ///< ARPHRD_* link-layer type identifier
};

/**
 * @brief An IPv4 address assigned to a network interface.
 *
 * Populated by list_interface_addresses().  Addresses are in network byte
 * order.
 */
struct IfAddrEntry
{
    Ipv4Addr addr{};           ///< Interface IPv4 address
    std::uint8_t prefix_len{}; ///< Prefix (subnet mask) length in bits
    Ipv4Addr broadcast{};      ///< Broadcast address (all-zero if absent)
    bool has_broadcast{};      ///< True when a broadcast address was reported
    std::string if_name;       ///< Interface name
    int if_index{};            ///< Interface kernel index
};

// ---------------------------------------------------------------------------
// Route management
// ---------------------------------------------------------------------------

/**
 * @brief Installs a new IPv4 route via RTM_NEWROUTE + NLM_F_CREATE.
 *
 * Fails with @c EEXIST (system category) if an identical route already exists.
 * Requires @c CAP_NET_ADMIN.
 *
 * @param params  Parameters describing the route to install.
 * @return void on success, or a @c std::error_code on failure.
 */
[[nodiscard]] std::expected<void, std::error_code>
add_route(const RouteAddParams& params);

/**
 * @brief Replaces an existing IPv4 route or creates one if absent.
 *
 * Uses RTM_NEWROUTE with NLM_F_CREATE | NLM_F_REPLACE, so the operation
 * succeeds whether or not a matching route already exists.
 * Requires @c CAP_NET_ADMIN.
 *
 * @param params  Parameters describing the route to install.
 * @return void on success, or a @c std::error_code on failure.
 */
[[nodiscard]] std::expected<void, std::error_code>
replace_route(const RouteAddParams& params);

/**
 * @brief Removes an IPv4 route via RTM_DELROUTE.
 *
 * Requires @c CAP_NET_ADMIN.
 *
 * @param params  Match criteria for the route to remove.
 * @return void on success, or a @c std::error_code on failure.
 *         Returns @c ESRCH (system category) when no matching route exists.
 */
[[nodiscard]] std::expected<void, std::error_code>
delete_route(const RouteDelParams& params);

/**
 * @brief Retrieves IPv4 routes from the kernel via RTM_GETROUTE + NLM_F_DUMP.
 *
 * @param table  Routing table to query.  Pass RouteTable::Unspec to retrieve
 *               routes from all tables (default: RouteTable::Main).
 * @return Vector of RouteEntry on success, or a @c std::error_code on failure.
 */
[[nodiscard]] std::expected<std::vector<RouteEntry>, std::error_code>
list_routes(RouteTable table = RouteTable::Main);

// ---------------------------------------------------------------------------
// Interface management
// ---------------------------------------------------------------------------

/**
 * @brief Retrieves all network interfaces via RTM_GETLINK + NLM_F_DUMP.
 *
 * @return Vector of InterfaceInfo on success, or a @c std::error_code.
 */
[[nodiscard]] std::expected<std::vector<InterfaceInfo>, std::error_code>
list_interfaces();

/**
 * @brief Retrieves information about a named network interface.
 *
 * @param name  Interface name (e.g. "eth0").
 * @return InterfaceInfo on success, or NetlinkError::InterfaceNotFound when
 *         no interface with that name exists.
 */
[[nodiscard]] std::expected<InterfaceInfo, std::error_code>
get_interface(const std::string& name);

/**
 * @brief Retrieves information about a network interface by kernel index.
 *
 * @param index  Kernel interface index (ifindex).
 * @return InterfaceInfo on success, or NetlinkError::InterfaceNotFound when
 *         no interface with that index exists.
 */
[[nodiscard]] std::expected<InterfaceInfo, std::error_code>
get_interface_by_index(int index);

/**
 * @brief Resolves a network interface name to its kernel index.
 *
 * Uses @c if_nametoindex(3) internally.
 *
 * @param name  Interface name (e.g. "eth0").
 * @return Kernel interface index on success, or
 * NetlinkError::InterfaceNotFound.
 */
[[nodiscard]] std::expected<int, std::error_code>
get_interface_index(const std::string& name);

/**
 * @brief Retrieves IPv4 addresses assigned to interfaces via RTM_GETADDR.
 *
 * @param if_name  When non-empty, only entries for this interface are returned.
 *                 An empty string (default) returns addresses for all
 * interfaces.
 * @return Vector of IfAddrEntry on success, or a @c std::error_code on failure.
 */
[[nodiscard]] std::expected<std::vector<IfAddrEntry>, std::error_code>
list_interface_addresses(const std::string& if_name = {});

// ---------------------------------------------------------------------------
// Utility / formatting helpers
// ---------------------------------------------------------------------------

/**
 * @brief Formats an IPv4 address as a dotted-decimal string (e.g.
 * "192.168.1.1").
 *
 * @param addr  IPv4 address bytes in network byte order.
 * @return Formatted string.
 */
[[nodiscard]] std::string format_ipv4(const Ipv4Addr& addr);

/**
 * @brief Parses a dotted-decimal IPv4 address string into an Ipv4Addr.
 *
 * Uses @c inet_pton(3) internally.
 *
 * @param s  IPv4 address string (e.g. "10.0.0.0").
 * @return Ipv4Addr on success, or NetlinkError::AddressError when the string
 *         is not a valid IPv4 dotted-decimal address.
 */
[[nodiscard]] std::expected<Ipv4Addr, std::error_code>
parse_ipv4(const std::string& s);

/**
 * @brief Formats an IPv4 address with a prefix length in CIDR notation.
 *
 * @param addr       IPv4 address.
 * @param prefix_len Prefix length (0–32).
 * @return String of the form "A.B.C.D/N" (e.g. "192.168.1.0/24").
 */
[[nodiscard]] std::string format_cidr(const Ipv4Addr& addr,
                                      std::uint8_t prefix_len);

} // namespace netlink
