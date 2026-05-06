/**
 * @file client/src/udp_table_server_example.cpp
 * @brief Standalone example for UdpTableServer on ports 9001 / 9002 / 9003.
 *
 * This program demonstrates the full lifecycle of UdpTableServer by:
 *
 *  1. Starting the server on the three default ports:
 *       9001 → ARP/NDP neighbor table
 *       9002 → nexthop object table
 *       9003 → IPv4 /32 routing table
 *
 *  2. Populating each table with synthetic NetLink-style data that mirrors
 *     the format produced by the real sra monitor commands.
 *
 *  3. Simulating periodic table updates (new entries, removals) so the push
 *     mechanism can be verified with a subscriber.
 *
 * Build
 * ─────
 * @code
 *   cmake --build build --target udp_table_server_example
 *   ./build/client/udp_table_server_example
 * @endcode
 *
 * Querying from another terminal
 * ──────────────────────────────
 * @code
 *   # One-shot query (any datagram triggers a reply):
 *   echo "" | nc -u -w1 127.0.0.1 9001    # neighbors
 *   echo "" | nc -u -w1 127.0.0.1 9002    # nexthops
 *   echo "" | nc -u -w1 127.0.0.1 9003    # routes
 *
 *   # Subscribe to push updates (receive a reply on every table change):
 *   echo "SUBSCRIBE" | nc -u -l 15001 &   # listen for pushed data
 *   echo "SUBSCRIBE" | nc -u 127.0.0.1 9001  # register with the server
 * @endcode
 *
 * @version 1.0
 */

#include "client/udp_table_server.hpp"

#include <arpa/inet.h>
#include <net/if.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <format>
#include <iostream>
#include <print>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// Stop flag (set by Ctrl-C)
// ---------------------------------------------------------------------------

static std::atomic<bool> g_stop{false};

static void sigHandler(int)
{
    g_stop = true;
}

// ---------------------------------------------------------------------------
// Simulated in-memory tables
// ---------------------------------------------------------------------------

/**
 * @brief Minimal neighbor entry for the example.
 */
struct ExNeighbor
{
    std::string family; ///< "inet" or "inet6"
    std::string dst;    ///< IP address
    std::string mac;    ///< MAC address
    std::string iface;  ///< Interface name
    std::string state;  ///< NUD state label
};

/**
 * @brief One member of an ECMP nexthop group.
 */
struct ExNhGroupMember
{
    uint32_t id{0};
    uint8_t weight{0}; ///< raw kernel weight; actual = weight + 1
};

/**
 * @brief Nexthop object entry matching the real netlink_nexthop_t data.
 *
 * Simple nexthops carry gateway / oif; group nexthops set is_group=true
 * and populate group_members instead.
 */
struct ExNexthop
{
    uint32_t id{0};
    std::string family;   ///< "inet" / "inet6" / "-"
    std::string gateway;  ///< next-hop IP or "-"
    std::string oif;      ///< outgoing interface name or "-"
    std::string protocol; ///< "ospf", "zebra", "static", …
    std::string scope;    ///< "global", "link", …
    std::string flags;    ///< RTNH_F_* label or "0x0"
    bool is_group{false};
    std::vector<ExNhGroupMember> group_members;
    std::string group_type; ///< "mpath" / "resilient" / "-"
    uint16_t encap_type{0};
    bool blackhole{false};
    bool fdb{false};
    std::string master; ///< VRF/bridge master name or "-"
};

/**
 * @brief One resolved nexthop within a route (gateway + outgoing interface).
 */
struct ExNhInfo
{
    std::string gateway; ///< next-hop IP or "(none)"
    std::string dev;     ///< outgoing interface name or "(none)"
    uint32_t ifindex{0};
    uint8_t weight{1};
};

/**
 * @brief Route entry matching the WatchRoute structure.
 *
 * A single dest may have multiple ExNhInfo entries (ECMP).
 */
struct ExRoute
{
    std::string dest; ///< e.g. "10.0.0.1/32"
    uint32_t nhid{0}; ///< nexthop object ID (0 when inline)
    uint32_t metric{0};
    uint32_t table{254}; ///< RT_TABLE_MAIN
    std::string protocol;
    std::string family;
    uint8_t dst_len{32};
    uint8_t tos{0};
    std::string scope;
    uint8_t type{1}; ///< RTN_UNICAST
    uint32_t flags{0};
    std::string srmd_id; ///< server-assigned route ID or "(pending)"
    std::vector<ExNhInfo> nexthops;
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::string formatGroupIds(const std::vector<ExNhGroupMember>& members)
{
    if (members.empty())
        return "-";
    std::string s;
    for (const auto& m : members)
    {
        if (!s.empty())
            s += ',';
        s += std::to_string(m.id);
    }
    return s;
}

// ---------------------------------------------------------------------------
// Serializers
// ---------------------------------------------------------------------------

/**
 * @brief Serializes the neighbor table to a text snapshot for UDP delivery.
 *
 * Format:
 * @code
 *   NEIGHBORS <count>
 *   family=inet dst=<ip> mac=<mac> iface=<if> state=<state>
 *   …
 * @endcode
 */
static std::string serializeNeighbors(const std::vector<ExNeighbor>& table)
{
    std::ostringstream os;
    os << "NEIGHBORS " << table.size() << '\n';
    for (const auto& n : table)
    {
        os << std::format("family={} dst={} mac={} iface={} state={}\n",
                          n.family,
                          n.dst,
                          n.mac,
                          n.iface,
                          n.state);
    }
    return os.str();
}

/**
 * @brief Serializes the nexthop table to a text snapshot for UDP delivery.
 *
 * Format:
 * @code
 *   NEXTHOPS <count>
 *   id=<n> family=<f> scope=<s> proto=<p> flags=<fl> oif=<if> gw=<gw>
 *   is_group=yes/no group_ids=<id>[,<id>...] group_type=mpath|resilient|-
 *   encap_type=<n> bh=yes/no fdb=yes/no master=<m>
 *   …
 * @endcode
 */
static std::string serializeNexthops(const std::vector<ExNexthop>& table)
{
    std::ostringstream os;
    os << "NEXTHOPS " << table.size() << '\n';
    for (const auto& n : table)
    {
        const std::string gtp =
            n.is_group ? (n.group_type.empty() ? "mpath" : n.group_type) : "-";
        os << std::format(
            "id={} family={} scope={} proto={} flags={} oif={} gw={}"
            " is_group={} group_ids={} group_type={} encap_type={}"
            " bh={} fdb={} master={}\n",
            n.id,
            n.family.empty() ? "-" : n.family,
            n.scope.empty() ? "-" : n.scope,
            n.protocol.empty() ? "-" : n.protocol,
            n.flags.empty() ? "0x0" : n.flags,
            n.oif.empty() ? "-" : n.oif,
            n.gateway.empty() ? "-" : n.gateway,
            n.is_group ? "yes" : "no",
            formatGroupIds(n.group_members),
            gtp,
            n.encap_type,
            n.blackhole ? "yes" : "no",
            n.fdb ? "yes" : "no",
            n.master.empty() ? "-" : n.master);
    }
    return os.str();
}

/**
 * @brief Serializes the routing table to a text snapshot for UDP delivery.
 *
 * Format:
 * @code
 *   ROUTES <count>
 *   dest=<prefix> nhid=<n> metric=<m> table=<t> proto=<p>
 *   family=<f> dst_len=<d> tos=<t> scope=<s> type=<t> flags=<fl> id=<srmd-id>
 *     nexthop gw=<gw> dev=<dev> ifidx=<n> weight=<w>
 *     …
 * @endcode
 */
static std::string serializeRoutes(const std::vector<ExRoute>& table)
{
    std::ostringstream os;
    os << "ROUTES " << table.size() << '\n';
    for (const auto& r : table)
    {
        os << std::format(
            "dest={} nhid={} metric={} table={} proto={}"
            " family={} dst_len={} tos={} scope={} type={} flags=0x{:08x}"
            " id={}\n",
            r.dest,
            r.nhid,
            r.metric,
            r.table,
            r.protocol.empty() ? "-" : r.protocol,
            r.family.empty() ? "inet" : r.family,
            static_cast<unsigned>(r.dst_len),
            static_cast<unsigned>(r.tos),
            r.scope.empty() ? "global" : r.scope,
            static_cast<unsigned>(r.type),
            r.flags,
            r.srmd_id.empty() ? "(pending)" : r.srmd_id);
        for (const auto& nh : r.nexthops)
        {
            os << std::format("  nexthop gw={} dev={} ifidx={} weight={}\n",
                              nh.gateway.empty() ? "(none)" : nh.gateway,
                              nh.dev.empty() ? "(none)" : nh.dev,
                              nh.ifindex,
                              static_cast<unsigned>(nh.weight));
        }
    }
    return os.str();
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main()
{
    /* Install Ctrl-C handler. */
    struct sigaction sa
    {};
    sa.sa_handler = sigHandler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    std::println("udp_table_server_example – UdpTableServer demo");
    std::println("  Port {} → ARP/NDP neighbor table", sra::UDP_PORT_NEIGHBORS);
    std::println("  Port {} → nexthop object table", sra::UDP_PORT_NEXTHOPS);
    std::println("  Port {} → IPv4 /32 routing table", sra::UDP_PORT_ROUTES);
    std::println("");

    // ── Create and start the server ──────────────────────────────────────

    sra::UdpTableServer udp(
        sra::UDP_PORT_NEIGHBORS, sra::UDP_PORT_NEXTHOPS, sra::UDP_PORT_ROUTES);

    if (!udp.start())
    {
        std::println(std::cerr, "Error: failed to start UdpTableServer");
        return 1;
    }

    // ── Initial table state ───────────────────────────────────────────────

    // ── Port 9001: ARP neighbors ─────────────────────────────────────────
    std::vector<ExNeighbor> neighbors = {
        {"inet", "192.168.0.2", "aa:bb:cc:dd:ee:01", "Ethernet46", "reachable"},
        {"inet", "192.168.0.6", "aa:bb:cc:dd:ee:02", "Ethernet47", "reachable"},
        {"inet6", "fe80::1", "aa:bb:cc:dd:ee:03", "Ethernet46", "stale"},
    };
    udp.setNeighborData(serializeNeighbors(neighbors));

    // ── Port 9002: nexthop objects ────────────────────────────────────────
    //
    // Two simple nexthops (354, 364) plus one ECMP group (363) that
    // references them — mirrors the "id 363 group 354/364 proto zebra"
    // scenario from FRR zebra.
    std::vector<ExNexthop> nexthops = {
        {
            .id = 354,
            .family = "inet",
            .gateway = "192.168.0.2",
            .oif = "Ethernet46",
            .protocol = "zebra",
            .scope = "global",
            .flags = "0x0",
        },
        {
            .id = 364,
            .family = "inet",
            .gateway = "192.168.0.6",
            .oif = "Ethernet47",
            .protocol = "zebra",
            .scope = "global",
            .flags = "0x0",
        },
        {
            .id = 363,
            .family = "-",
            .protocol = "zebra",
            .scope = "-",
            .flags = "0x0",
            .is_group = true,
            .group_members = {{354, 0}, {364, 0}}, // weight 0 → actual 1
            .group_type = "mpath",
        },
    };
    udp.setNexthopData(serializeNexthops(nexthops));

    // ── Port 9003: IPv4 /32 routing table ─────────────────────────────────
    //
    // ECMP route for 2.2.2.2/32 via nhid 363 (group of 354 + 364).
    // Inline simple route for 3.3.3.3/32 (no nexthop object).
    std::vector<ExRoute> routes = {
        {
            .dest = "2.2.2.2/32",
            .nhid = 363,
            .metric = 20,
            .table = 254,
            .protocol = "ospf",
            .family = "inet",
            .dst_len = 32,
            .scope = "global",
            .srmd_id = "(pending)",
            .nexthops =
                {
                    {"192.168.0.2", "Ethernet46", 46, 1},
                    {"192.168.0.6", "Ethernet47", 47, 1},
                },
        },
        {
            .dest = "3.3.3.3/32",
            .nhid = 0,
            .metric = 20,
            .table = 254,
            .protocol = "ospf",
            .family = "inet",
            .dst_len = 32,
            .scope = "global",
            .srmd_id = "(pending)",
            .nexthops =
                {
                    {"10.0.0.1", "eth0", 2, 1},
                },
        },
    };
    udp.setRouteData(serializeRoutes(routes));

    std::println("[example] Initial tables published.  Ctrl-C to stop.");
    std::println("[example] Query with: echo \"\" | nc -u -w1 127.0.0.1 9001");
    std::println(
        "[example] Subscribe : echo \"SUBSCRIBE\" | nc -u 127.0.0.1 9001");
    std::println("");

    // ── Simulate periodic NetLink events ─────────────────────────────────
    //
    // Every 5 seconds a new synthetic event is injected so subscribers on
    // each port receive a push update.

    int tick = 0;
    while (!g_stop)
    {
        std::this_thread::sleep_for(100ms);
        ++tick;

        if (tick % 50 != 0) // every 5 s (50 × 100 ms)
            continue;

        const int cycle = (tick / 50) % 3;

        if (cycle == 0)
        {
            // ── Simulate NETLINK_NEIGH_ADDED on port 9001 ────────────────
            std::println("[event] NEIGH ADDED 192.168.1.10 on Ethernet48 "
                         "(pushed to port {} subscribers)",
                         sra::UDP_PORT_NEIGHBORS);
            neighbors.push_back({"inet",
                                 "192.168.1.10",
                                 "de:ad:be:ef:00:01",
                                 "Ethernet48",
                                 "reachable"});
            udp.setNeighborData(serializeNeighbors(neighbors));
        }
        else if (cycle == 1)
        {
            // ── Simulate NETLINK_NEXTHOP_ADDED on port 9002 ──────────────
            // Add a new simple nexthop and a group referencing it + 354.
            std::println("[event] NEXTHOP ADDED id=370 via 192.168.1.10 + "
                         "group id=371 (pushed to port {} subscribers)",
                         sra::UDP_PORT_NEXTHOPS);
            nexthops.push_back({
                .id = 370,
                .family = "inet",
                .gateway = "192.168.1.10",
                .oif = "Ethernet48",
                .protocol = "zebra",
                .scope = "global",
                .flags = "0x0",
            });
            nexthops.push_back({
                .id = 371,
                .family = "-",
                .protocol = "zebra",
                .scope = "-",
                .flags = "0x0",
                .is_group = true,
                .group_members = {{354, 0}, {370, 0}},
                .group_type = "mpath",
            });
            udp.setNexthopData(serializeNexthops(nexthops));
        }
        else
        {
            // ── Simulate NETLINK_ROUTE_ADDED on port 9003 ────────────────
            std::println("[event] ROUTE ADDED 4.4.4.4/32 nhid=371 "
                         "(pushed to port {} subscribers)",
                         sra::UDP_PORT_ROUTES);
            routes.push_back({
                .dest = "4.4.4.4/32",
                .nhid = 371,
                .metric = 20,
                .table = 254,
                .protocol = "ospf",
                .family = "inet",
                .dst_len = 32,
                .scope = "global",
                .srmd_id = "(pending)",
                .nexthops =
                    {
                        {"192.168.0.2", "Ethernet46", 46, 1},
                        {"192.168.1.10", "Ethernet48", 48, 1},
                    },
            });
            udp.setRouteData(serializeRoutes(routes));
        }
    }

    std::println("\n[example] Stopping…");
    udp.stop();
    std::println("[example] Done.");
    return 0;
}
