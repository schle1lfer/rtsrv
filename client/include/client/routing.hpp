/**
 * @file client/include/client/routing.hpp
 * @brief Linux kernel routing-table and network-interface manager.
 *
 * RoutingManager interacts directly with the kernel via a NETLINK_ROUTE socket
 * to enumerate and modify network interfaces and the IP routing table.  It
 * operates independently of the srmd gRPC daemon and reflects actual kernel
 * state.
 *
 * Capabilities provided:
 *  - Enumerate all network interfaces (name, index, flags, MTU, MAC, IPs).
 *  - Look up a single interface by name.
 *  - Test whether an interface is administratively up.
 *  - Dump the kernel routing table with optional address-family / table
 *    filters.
 *  - Install, remove, and replace routes via RTM_NEWROUTE / RTM_DELROUTE.
 *
 * All public methods return @c std::expected<T,std::string> so callers can
 * handle errors without exceptions.  The class is thread-safe: a single
 * instance may be shared between threads; internal state is protected by a
 * mutex.
 *
 * @note Mutating operations (addRoute, removeRoute, replaceRoute) require
 *       the @c CAP_NET_ADMIN capability (or effective UID 0).
 *
 * Typical usage:
 * @code
 *   sra::RoutingManager rm;
 *
 *   // List all interfaces
 *   auto ifaces = rm.listInterfaces();
 *   if (ifaces) {
 *       for (const auto& iface : *ifaces)
 *           std::println("{:3}: {:10} {} MTU {}",
 *               iface.index, iface.name,
 *               iface.isUp() ? "UP" : "down", iface.mtu);
 *   }
 *
 *   // Add a static route
 *   sra::RouteParams p;
 *   p.destination   = "10.20.30.0/24";
 *   p.gateway       = "192.168.1.1";
 *   p.interfaceName = "eth0";
 *   if (auto r = rm.addRoute(p); !r)
 *       std::println(stderr, "addRoute failed: {}", r.error());
 * @endcode
 *
 * @version 1.0
 */

#pragma once

#include <linux/rtnetlink.h>
#include <net/if.h>

#include <cstdint>
#include <expected>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace sra
{

// ---------------------------------------------------------------------------
// InterfaceAddress
// ---------------------------------------------------------------------------

/**
 * @brief A single IP address assigned to a network interface.
 */
struct InterfaceAddress
{
    std::string address;  ///< Address in text notation (e.g. "192.168.1.1").
    uint8_t prefixLen{0}; ///< Prefix length (e.g. 24 for /24).
    int family{AF_INET};  ///< Address family: @c AF_INET or @c AF_INET6.
};

// ---------------------------------------------------------------------------
// NetworkInterface
// ---------------------------------------------------------------------------

/**
 * @brief Describes a network interface as reported by the kernel.
 *
 * Populated by RoutingManager::listInterfaces() from RTM_NEWLINK and
 * RTM_NEWADDR netlink messages.
 */
struct NetworkInterface
{
    std::string name;                        ///< Interface name (e.g. "eth0").
    uint32_t index{0};                       ///< Kernel interface index.
    uint32_t flags{0};                       ///< Interface flags (IFF_UP, …).
    std::string hwAddress;                   ///< MAC in "aa:bb:cc:dd:ee:ff" or "".
    uint32_t mtu{0};                         ///< MTU in bytes.
    std::vector<InterfaceAddress> addresses; ///< Assigned IP addresses.

    /**
     * @brief Returns @c true when the @c IFF_UP flag is set.
     */
    [[nodiscard]] bool isUp() const noexcept
    {
        return (flags & static_cast<uint32_t>(IFF_UP)) != 0;
    }

    /**
     * @brief Returns @c true when the @c IFF_LOOPBACK flag is set.
     */
    [[nodiscard]] bool isLoopback() const noexcept
    {
        return (flags & static_cast<uint32_t>(IFF_LOOPBACK)) != 0;
    }

    /**
     * @brief Returns @c true when the @c IFF_RUNNING flag is set.
     *
     * IFF_RUNNING indicates that the interface has a carrier (link is up
     * at the physical layer), which differs from IFF_UP (administrative
     * state).
     */
    [[nodiscard]] bool isRunning() const noexcept
    {
        return (flags & static_cast<uint32_t>(IFF_RUNNING)) != 0;
    }
};

// ---------------------------------------------------------------------------
// KernelRoute
// ---------------------------------------------------------------------------

/**
 * @brief A single entry in the kernel IP routing table.
 *
 * Populated by RoutingManager::listRoutes() from RTM_NEWROUTE netlink
 * messages.  Field values mirror the corresponding @c struct rtmsg and
 * @c rtattr payload fields.
 */
struct KernelRoute
{
    std::string destination; ///< Destination prefix in CIDR (e.g. "10.0.0.0/8").
    std::string gateway;     ///< Next-hop gateway (empty when on-link/direct).
    std::string interfaceName; ///< Outgoing interface name (e.g. "eth0").
    uint32_t interfaceIndex{0}; ///< Outgoing interface index (0 if unknown).
    uint32_t metric{0};      ///< Route priority; lower value wins.
    uint8_t prefixLen{0};    ///< Prefix length (duplicates the /N in destination).
    uint8_t protocol{0};     ///< Origin protocol (RTPROT_STATIC, RTPROT_ZEBRA, …).
    uint8_t type{RTN_UNICAST}; ///< Route type (RTN_UNICAST, RTN_BLACKHOLE, …).
    uint32_t table{RT_TABLE_MAIN}; ///< Routing table ID (254 = main).
    uint8_t scope{RT_SCOPE_UNIVERSE}; ///< Route scope.
    int family{AF_INET};     ///< Address family: @c AF_INET or @c AF_INET6.
};

// ---------------------------------------------------------------------------
// RouteParams
// ---------------------------------------------------------------------------

/**
 * @brief Parameters for adding, removing, or replacing a kernel route.
 *
 * Set @p destination (mandatory) and any of the optional fields that
 * apply to the operation.  For removeRoute(), only @p destination,
 * @p gateway, @p interfaceName, and @p table are used to match the
 * existing route; the other fields are ignored by the kernel.
 *
 * Example — add a default route via 192.168.1.1 on eth0:
 * @code
 *   sra::RouteParams p;
 *   p.destination   = "0.0.0.0/0";
 *   p.gateway       = "192.168.1.1";
 *   p.interfaceName = "eth0";
 *   rm.addRoute(p);
 * @endcode
 *
 * Example — remove the same route:
 * @code
 *   sra::RouteParams p;
 *   p.destination   = "0.0.0.0/0";
 *   p.gateway       = "192.168.1.1";
 *   p.interfaceName = "eth0";
 *   rm.removeRoute(p);
 * @endcode
 */
struct RouteParams
{
    /** @brief Destination in CIDR notation (e.g. "192.168.2.0/24",
     *         "0.0.0.0/0", "2001:db8::/32").  Mandatory. */
    std::string destination;

    /** @brief Next-hop gateway address.  Empty means on-link (direct). */
    std::string gateway;

    /** @brief Outgoing interface name.  Empty lets the kernel decide. */
    std::string interfaceName;

    /** @brief Route metric/priority.  0 lets the kernel apply its default. */
    uint32_t metric{0};

    /** @brief Origin protocol tag written into the kernel route entry.
     *
     *  Common values: @c RTPROT_STATIC, @c RTPROT_BGP, @c RTPROT_ZEBRA.
     *  Defaults to @c RTPROT_STATIC.
     */
    uint8_t protocol{RTPROT_STATIC};

    /** @brief Route type.  Defaults to @c RTN_UNICAST (normal forwarding). */
    uint8_t type{RTN_UNICAST};

    /** @brief Routing table ID.  Defaults to @c RT_TABLE_MAIN (254). */
    uint32_t table{RT_TABLE_MAIN};

    /** @brief Address family: @c AF_INET (default) or @c AF_INET6. */
    int family{AF_INET};

    /** @brief Route scope.  Defaults to @c RT_SCOPE_UNIVERSE.
     *
     *  When the gateway is empty and an interface is specified the scope is
     *  automatically promoted to @c RT_SCOPE_LINK unless overridden here.
     */
    uint8_t scope{RT_SCOPE_UNIVERSE};
};

// ---------------------------------------------------------------------------
// RoutingManager
// ---------------------------------------------------------------------------

/**
 * @brief Manages network interfaces and the kernel routing table via netlink.
 *
 * Opens a single @c AF_NETLINK / @c NETLINK_ROUTE socket on construction and
 * keeps it open for the lifetime of the object.  All operations are
 * serialised with an internal mutex so the same instance can be used safely
 * from multiple threads.
 *
 * The constructor throws @c std::system_error if the socket cannot be created
 * or bound.  All other errors are returned via @c std::expected.
 *
 * @note The class is non-copyable and non-movable to prevent accidental
 *       sharing of the underlying file descriptor.
 */
class RoutingManager
{
public:
    /**
     * @brief Opens and binds the NETLINK_ROUTE socket.
     *
     * @throws std::system_error when @c socket(2) or @c bind(2) fails.
     */
    RoutingManager();

    /**
     * @brief Closes the NETLINK_ROUTE socket.
     */
    ~RoutingManager();

    RoutingManager(const RoutingManager&) = delete;
    RoutingManager& operator=(const RoutingManager&) = delete;
    RoutingManager(RoutingManager&&) = delete;
    RoutingManager& operator=(RoutingManager&&) = delete;

    // -----------------------------------------------------------------------
    // Interface queries
    // -----------------------------------------------------------------------

    /**
     * @brief Enumerates all network interfaces and their IP addresses.
     *
     * Issues RTM_GETLINK and RTM_GETADDR dump requests to the kernel and
     * returns one NetworkInterface per interface found.
     *
     * @return Vector of NetworkInterface descriptors on success, or an error
     *         string when the netlink exchange fails.
     */
    [[nodiscard]] std::expected<std::vector<NetworkInterface>, std::string>
    listInterfaces() const;

    /**
     * @brief Returns the descriptor for a single interface by name.
     *
     * Calls listInterfaces() and filters for @p name.
     *
     * @param name  Interface name (e.g. "eth0", "lo").
     * @return The matching NetworkInterface, or an error string when the
     *         interface does not exist or the query fails.
     */
    [[nodiscard]] std::expected<NetworkInterface, std::string>
    getInterface(const std::string& name) const;

    /**
     * @brief Tests whether the named interface has the IFF_UP flag set.
     *
     * @param name  Interface name.
     * @return @c true if up, @c false if down, or an error string when the
     *         interface is not found.
     */
    [[nodiscard]] std::expected<bool, std::string>
    isInterfaceUp(const std::string& name) const;

    // -----------------------------------------------------------------------
    // Route queries
    // -----------------------------------------------------------------------

    /**
     * @brief Dumps the kernel routing table.
     *
     * Issues an RTM_GETROUTE dump and returns all matching entries.
     *
     * @param family  Address family filter: @c AF_INET, @c AF_INET6, or
     *                @c AF_UNSPEC (default) for both.
     * @param table   Routing table filter: @c RT_TABLE_UNSPEC (default) for
     *                all tables, or a specific ID (e.g. @c RT_TABLE_MAIN).
     * @return Vector of KernelRoute entries on success, or an error string.
     */
    [[nodiscard]] std::expected<std::vector<KernelRoute>, std::string>
    listRoutes(int family = AF_UNSPEC,
               uint32_t table = RT_TABLE_UNSPEC) const;

    // -----------------------------------------------------------------------
    // Route mutations
    // -----------------------------------------------------------------------

    /**
     * @brief Installs a new route in the kernel routing table.
     *
     * Sends @c RTM_NEWROUTE with @c NLM_F_CREATE | @c NLM_F_EXCL.  Fails
     * with an error (EEXIST) if an identical route already exists.
     *
     * @param params  Route parameters (destination is mandatory).
     * @return @c void on success, or an error string on failure.
     *
     * @note Requires @c CAP_NET_ADMIN.
     */
    [[nodiscard]] std::expected<void, std::string>
    addRoute(const RouteParams& params);

    /**
     * @brief Removes a matching route from the kernel routing table.
     *
     * Sends @c RTM_DELROUTE.  The kernel matches on (destination, table,
     * gateway, oif); unset optional fields widen the match.
     *
     * @param params  Parameters identifying the route to delete.
     * @return @c void on success, or an error string on failure
     *         (e.g. ESRCH when the route does not exist).
     *
     * @note Requires @c CAP_NET_ADMIN.
     */
    [[nodiscard]] std::expected<void, std::string>
    removeRoute(const RouteParams& params);

    /**
     * @brief Creates or replaces a route in the kernel routing table.
     *
     * Sends @c RTM_NEWROUTE with @c NLM_F_CREATE | @c NLM_F_REPLACE.  If a
     * route with the same key (destination, table) already exists it is
     * updated in-place; otherwise a new route is created.
     *
     * Equivalent to @c "ip route replace …".
     *
     * @param params  New or replacement route parameters.
     * @return @c void on success, or an error string on failure.
     *
     * @note Requires @c CAP_NET_ADMIN.
     */
    [[nodiscard]] std::expected<void, std::string>
    replaceRoute(const RouteParams& params);

private:
    int fd_{-1};               ///< NETLINK_ROUTE socket file descriptor.
    mutable uint32_t seq_{1};  ///< Monotonically increasing request sequence.
    mutable std::mutex mtx_;   ///< Serialises all socket operations.

    /**
     * @brief Reads all netlink response messages for one request.
     *
     * Loops until @c NLMSG_DONE or @c NLMSG_ERROR, ignoring messages whose
     * @c nlmsg_seq does not match @p seq.  Invokes @p handler for every
     * data message.  Returns an error when a negative @c NLMSG_ERROR is
     * received.
     *
     * @param seq     Expected sequence number.
     * @param handler Called once per non-control message in the response.
     * @return @c void on success, or an error string on failure.
     */
    [[nodiscard]] std::expected<void, std::string>
    recvAll(uint32_t seq,
            const std::function<void(nlmsghdr*)>& handler) const;

    /**
     * @brief Issues RTM_GETLINK dump and returns raw interface descriptors.
     *
     * Must be called with @p mtx_ held.
     *
     * @return Vector of NetworkInterface (without address lists), or error.
     */
    [[nodiscard]] std::expected<std::vector<NetworkInterface>, std::string>
    dumpLinks() const;

    /**
     * @brief Issues RTM_GETADDR dump and fills address lists.
     *
     * Must be called with @p mtx_ held.  Matches each address to the
     * interface in @p ifaces by index.
     *
     * @param ifaces  Interface list to populate; must already contain the
     *                link descriptors from dumpLinks().
     * @return @c void on success, or an error string.
     */
    [[nodiscard]] std::expected<void, std::string>
    dumpAddrs(std::vector<NetworkInterface>& ifaces) const;

    /**
     * @brief Builds and sends a route request, then waits for the ACK.
     *
     * @param params  Route parameters.
     * @param nlType  @c RTM_NEWROUTE or @c RTM_DELROUTE.
     * @param flags   Extra @c NLM_F_* flags (e.g. @c NLM_F_CREATE,
     *                @c NLM_F_REPLACE, @c NLM_F_EXCL).
     * @return @c void on success, or an error string.
     */
    [[nodiscard]] std::expected<void, std::string>
    sendRouteRequest(const RouteParams& params,
                     uint16_t nlType,
                     uint16_t flags);
};

} // namespace sra
