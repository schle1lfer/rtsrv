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
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <format>
#include <iostream>
#include <net/if.h>
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

static void sigHandler(int) { g_stop = true; }

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
 * @brief Minimal nexthop entry for the example.
 */
struct ExNexthop
{
    uint32_t    id{0};
    std::string family;
    std::string gateway;
    std::string oif;
    std::string protocol;
    std::string scope;
};

/**
 * @brief Minimal route entry for the example.
 */
struct ExRoute
{
    std::string dest;     ///< e.g. "10.0.0.1/32"
    std::string gateway;
    std::string iface;
    uint32_t    metric{0};
    std::string protocol;
};

// ---------------------------------------------------------------------------
// Serializers
// ---------------------------------------------------------------------------

/**
 * @brief Serializes the neighbor table to a text snapshot for UDP delivery.
 *
 * Format (one entry per line after the header):
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
                          n.family, n.dst, n.mac, n.iface, n.state);
    }
    return os.str();
}

/**
 * @brief Serializes the nexthop table to a text snapshot for UDP delivery.
 *
 * Format:
 * @code
 *   NEXTHOPS <count>
 *   id=<n> family=<f> gateway=<gw> oif=<if> protocol=<p> scope=<s>
 *   …
 * @endcode
 */
static std::string serializeNexthops(const std::vector<ExNexthop>& table)
{
    std::ostringstream os;
    os << "NEXTHOPS " << table.size() << '\n';
    for (const auto& n : table)
    {
        os << std::format(
            "id={} family={} gateway={} oif={} protocol={} scope={}\n",
            n.id, n.family, n.gateway, n.oif, n.protocol, n.scope);
    }
    return os.str();
}

/**
 * @brief Serializes the routing table to a text snapshot for UDP delivery.
 *
 * Format:
 * @code
 *   ROUTES <count>
 *   dest=<prefix> gw=<gw> iface=<if> metric=<m> protocol=<p>
 *   …
 * @endcode
 */
static std::string serializeRoutes(const std::vector<ExRoute>& table)
{
    std::ostringstream os;
    os << "ROUTES " << table.size() << '\n';
    for (const auto& r : table)
    {
        os << std::format(
            "dest={} gw={} iface={} metric={} protocol={}\n",
            r.dest,
            r.gateway.empty() ? "(none)" : r.gateway,
            r.iface,
            r.metric,
            r.protocol);
    }
    return os.str();
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main()
{
    /* Install Ctrl-C handler. */
    struct sigaction sa{};
    sa.sa_handler = sigHandler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    std::println("udp_table_server_example – UdpTableServer demo");
    std::println("  Port {} → ARP/NDP neighbor table", sra::UDP_PORT_NEIGHBORS);
    std::println("  Port {} → nexthop object table",   sra::UDP_PORT_NEXTHOPS);
    std::println("  Port {} → IPv4 /32 routing table", sra::UDP_PORT_ROUTES);
    std::println("");

    // ── Create and start the server ──────────────────────────────────────

    sra::UdpTableServer udp(sra::UDP_PORT_NEIGHBORS,
                            sra::UDP_PORT_NEXTHOPS,
                            sra::UDP_PORT_ROUTES);

    if (!udp.start())
    {
        std::println(std::cerr, "Error: failed to start UdpTableServer");
        return 1;
    }

    // ── Initial table state ───────────────────────────────────────────────
    //
    // These populate the snapshots that any client querying the ports will
    // receive immediately.

    // ── Port 9001: ARP neighbors ─────────────────────────────────────────
    std::vector<ExNeighbor> neighbors = {
        {"inet",  "192.168.1.1", "aa:bb:cc:dd:ee:01", "eth0", "reachable"},
        {"inet",  "192.168.1.2", "aa:bb:cc:dd:ee:02", "eth0", "stale"},
        {"inet6", "fe80::1",     "aa:bb:cc:dd:ee:03", "eth1", "reachable"},
    };
    udp.setNeighborData(serializeNeighbors(neighbors));

    // ── Port 9002: nexthop objects ────────────────────────────────────────
    std::vector<ExNexthop> nexthops = {
        {1, "inet",  "10.0.0.1",  "eth0", "ospf",   "global"},
        {2, "inet",  "10.0.0.2",  "eth0", "ospf",   "global"},
        {3, "inet6", "fe80::1",   "eth1", "static", "link"},
    };
    udp.setNexthopData(serializeNexthops(nexthops));

    // ── Port 9003: IPv4 /32 routing table ─────────────────────────────────
    std::vector<ExRoute> routes = {
        {"10.1.0.1/32", "10.0.0.1", "eth0", 100, "ospf"},
        {"10.1.0.2/32", "10.0.0.2", "eth0", 100, "ospf"},
    };
    udp.setRouteData(serializeRoutes(routes));

    std::println("[example] Initial tables published.  Ctrl-C to stop.");
    std::println("[example] Query with: echo \"\" | nc -u -w1 127.0.0.1 9001");
    std::println("[example] Subscribe : echo \"SUBSCRIBE\" | nc -u 127.0.0.1 9001");
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

        if (tick % 50 != 0)   // every 5 s (50 × 100 ms)
            continue;

        const int cycle = (tick / 50) % 3;

        if (cycle == 0)
        {
            // ── Simulate NETLINK_NEIGH_ADDED on port 9001 ────────────────
            std::println("[event] NEIGH ADDED 192.168.2.10 on eth2 "
                         "(pushed to port {} subscribers)", sra::UDP_PORT_NEIGHBORS);
            neighbors.push_back(
                {"inet", "192.168.2.10", "de:ad:be:ef:00:01", "eth2", "reachable"});
            udp.setNeighborData(serializeNeighbors(neighbors));
        }
        else if (cycle == 1)
        {
            // ── Simulate NETLINK_NEXTHOP_ADDED on port 9002 ──────────────
            std::println("[event] NEXTHOP ADDED id=4 via 10.0.0.3 "
                         "(pushed to port {} subscribers)", sra::UDP_PORT_NEXTHOPS);
            nexthops.push_back({4, "inet", "10.0.0.3", "eth0", "ospf", "global"});
            udp.setNexthopData(serializeNexthops(nexthops));
        }
        else
        {
            // ── Simulate NETLINK_ROUTE_ADDED on port 9003 ────────────────
            std::println("[event] ROUTE ADDED 10.1.0.3/32 via 10.0.0.3 "
                         "(pushed to port {} subscribers)", sra::UDP_PORT_ROUTES);
            routes.push_back({"10.1.0.3/32", "10.0.0.3", "eth0", 100, "ospf"});
            udp.setRouteData(serializeRoutes(routes));
        }
    }

    std::println("\n[example] Stopping…");
    udp.stop();
    std::println("[example] Done.");
    return 0;
}
