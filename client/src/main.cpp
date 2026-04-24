/**
 * @file client/src/main.cpp
 * @brief Entry point for sra – the Switch Route Application.
 *
 * sra is a command-line client for the srmd Switch Route Manager daemon.
 * It connects to one or more running srmd instances via gRPC and exposes
 * route management commands.
 *
 * Usage:
 * @code
 *   sra [options] <command> [command-args]
 *
 *   Global options:
 *     -c, --config   <path>   Path to client config.json [default: config/config.json]
 *     -s, --server   <addr>   Single srmd address  (overrides config file)
 *     -w, --switches <path>   Path to switch_config.json (multi-server mode)
 *         --sot      <path>   Path to Source-of-Truth JSON file
 *         --node-ip  <ip>     Management IP of the node to look up in the SOT
 *                             (required for single-server sync mode)
 *     -t, --timeout  <sec>    RPC deadline (overrides config file)
 *         --tls               Use TLS channel (overrides config file)
 *         --ca-cert  <path>   CA certificate for TLS verification
 *         --logstream <dest>  unix_domain layer log destination: "stderr"
 *                             (default), "stdout", or an absolute file path.
 *                             Pass "" to silence all protocol-layer logging.
 *         --loglevel  <n>     Minimum log level (name or number):
 *                             DEBUG|1 (raw socket bytes + hex dumps),
 *                             INFO|2 (decoded summaries), NOTICE|3,
 *                             WARNING|4, ERR|5  [default: 1]
 *     -v, --version           Print version and exit
 *     -h, --help              Print this help and exit
 *
 *   Commands:
 *     test                    Run a full Echo + CRUD round-trip test sequence
 *                             (single server, or all servers when --switches)
 *     sync                    Parse SOT (--sot required), then push all
 *                             prefix routes to matching servers via gRPC.
 *                             Use --switches for multi-server mode, or
 *                             --server + --node-ip for single-node mode.
 *     echo  <message>         Send an Echo RPC and print the response
 *     add   <dest> [gw] [iface] [metric]   Add a route
 *     remove <id>             Remove a route by ID
 *     get    <id>             Retrieve a route by ID
 *     list  [--active]        List all routes (--active filters to active only)
 *     watch                   Subscribe to kernel netlink events for IPv4 /32
 *                             routes and forward each ADDED/CHANGED/REMOVED
 *                             event to srmd via AddRoute / RemoveRoute.
 *                             Runs until Ctrl-C.
 *     neighbors               Dump and watch kernel neighbor (ARP/NDP) table.
 *                             No gRPC connection required.  Runs until Ctrl-C.
 *     nexthops                Dump and watch kernel nexthop objects (Linux 5.3+).
 *                             No gRPC connection required.  Runs until Ctrl-C.
 *     set-loopback <address>  Store a loopback address on the server
 *     get-loopback            Retrieve the stored loopback address
 *     get-loopbacks <loopback>  Query SOT interface list for a loopback (IPv4 or IPv6)
 *     vrf-route-add [socket]  Query srmd via gRPC, build a SingleRouteRequest from
 *                             nni routes, and deliver it to ud_server once (one-shot).
 *                             Default socket: /tmp/ud_server.sock
 *     run [socket]            Full SRA daemon mode — runs until SIGINT/SIGTERM:
 *                               1. RequestLoopback → SOT auth check via srmd
 *                               2. GetLoopbacks / GetAllRoutes via srmd
 *                               3. Build SingleRouteRequest (nni interfaces only)
 *                               4. Deliver to ud_server via Unix-domain socket
 *                               5. Monitor kernel nexthop events continuously
 *                             Default socket: /tmp/ud_server.sock
 *     grpc-proc-demo          Run async GrpcProc background thread demo
 * @endcode
 *
 * @version 1.0
 */

#include "build_info.hpp"
#include "client/grpc_proc.hpp"
#include "client/route_client.hpp"
#include "client/sot_config.hpp"
#include "client/switch_config.hpp"
#include "client/routing.hpp"
#include "client/udp_table_server.hpp"
#include "client/vrf_table.hpp"
#include "client/vrf_udp_client.hpp"
#include "common/config.hpp"
#include "server/netlink.h"
#include "client/netlink_neigh.h"
#include "client/netlink_nexthop.h"

#include "lib/cmd_proto.hpp"
#include "lib/logger.hpp"

#include <arpa/inet.h>
#include <linux/neighbour.h>
#include <netinet/ip_icmp.h>
#include <poll.h>
#include <sys/socket.h>
#include <boost/program_options.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <format>
#include <iostream>
#include <linux/rtnetlink.h>
#include <map>
#include <mutex>
#include <print>
#include <shared_mutex>
#include <queue>
#include <signal.h>
#include <sstream>
#include <unistd.h>
#include <string>
#include <thread>
#include <unordered_map>
#include <variant>
#include <vector>

namespace po = boost::program_options;

// ---------------------------------------------------------------------------
// Output helpers
// ---------------------------------------------------------------------------

/**
 * @brief Pretty-prints a single Route protobuf message to stdout.
 *
 * @param route  The route to display.
 * @param indent Number of leading spaces.
 */
static void printRoute(const srmd::v1::Route& route, int indent = 2)
{
    const std::string pad(static_cast<std::size_t>(indent), ' ');
    std::println("{}ID          : {}", pad, route.id());
    std::println("{}Destination : {}", pad, route.destination());
    std::println("{}Gateway     : {}",
                 pad,
                 route.gateway().empty() ? "(none)" : route.gateway());
    std::println("{}Interface   : {}",
                 pad,
                 route.interface_name().empty() ? "(none)"
                                                : route.interface_name());
    std::println("{}Metric      : {}", pad, route.metric());
    std::println("{}Protocol    : {}",
                 pad,
                 srmd::v1::RouteProtocol_Name(route.protocol()));
    std::println("{}Active      : {}", pad, route.active() ? "yes" : "no");
    std::println("{}Family      : {}",
                 pad,
                 srmd::v1::AddressFamily_Name(route.address_family()));
}

// ---------------------------------------------------------------------------
// Output helpers (continued)
// ---------------------------------------------------------------------------

/**
 * @brief Prints a GetAllRoutesResponse to stdout.
 *
 * Lists the matched hostname, loopback, and every VRF → interface → prefix
 * entry in a structured format so the operator can verify the SOT data.
 *
 * @param resp  Populated GetAllRoutesResponse from the server.
 */
static void printAllRoutes(const srmd::v1::GetAllRoutesResponse& resp)
{
    std::println("GetAllRoutes: {} (loopback={})",
                 resp.hostname(),
                 resp.loopback_ipv4());
    std::println("  {} VRF interface(s):", resp.routes_size());
    for (const auto& r : resp.routes())
    {
        std::println("  ── vrf='{}' iface='{}' type={} local={} nexthop={}"
                     " weight={}",
                     r.vrf_name(),
                     r.interface_name(),
                     r.interface_type(),
                     r.local_address(),
                     r.nexthop().empty() ? "(none)" : r.nexthop(),
                     r.weight());
        for (const auto& p : r.prefixes())
        {
            std::println("       prefix={} weight={} role={} desc={}",
                         p.prefix(), p.weight(), p.role(), p.description());
        }
    }
}

// ---------------------------------------------------------------------------
// Command implementations
// ---------------------------------------------------------------------------

/**
 * @brief Runs the full test sequence against a single server.
 *
 * Steps performed:
 *  1. Echo
 *  2. Heartbeat
 *  3. AddRoute (default via 192.168.1.1/eth0)
 *  4. AddRoute (10.0.0.0/8 via 10.0.0.1/eth1)
 *  5. ListRoutes
 *  6. GetRoute (first added)
 *  7. RemoveRoute (first added)
 *  8. ListRoutes (verify removal)
 *
 * @param client  Connected RouteClient.
 * @return EXIT_SUCCESS if all steps pass; EXIT_FAILURE otherwise.
 */
static int cmdTest(sra::RouteClient& client)
{
    bool allOk = true;

    // ------------------------------------------------------------------
    // Step 1: Echo
    // ------------------------------------------------------------------
    std::println("\n─── Step 1: Echo ─────────────────────────────────────");
    const std::string echoMsg =
        "Hello from sra! Switch Route Application test.";
    const auto t0 = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();

    auto echoResult = client.echo(echoMsg);
    if (!echoResult)
    {
        std::println(std::cerr, "  [FAIL] Echo: {}", echoResult.error());
        return EXIT_FAILURE;
    }

    const auto t1 = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();

    std::println("  [OK]   Echo received back  : '{}'", echoResult->message());
    std::println("  [OK]   Server ID           : {}", echoResult->server_id());
    std::println("  [OK]   Server version      : {}",
                 echoResult->server_version());
    std::println("  [OK]   Round-trip latency  : {} µs", t1 - t0);

    // ------------------------------------------------------------------
    // Step 2: Heartbeat
    // ------------------------------------------------------------------
    std::println("\n─── Step 2: Heartbeat ────────────────────────────────");
    auto hbResult = client.heartbeat(1);
    if (!hbResult)
    {
        std::println(std::cerr, "  [FAIL] Heartbeat: {}", hbResult.error());
        allOk = false;
    }
    else
    {
        std::println("  [OK]   Sequence: {}  server_ts: {} µs",
                     hbResult->sequence(),
                     hbResult->server_ts_us());
    }

    // ------------------------------------------------------------------
    // Step 3: AddRoute (IPv4 default)
    // ------------------------------------------------------------------
    std::println("\n─── Step 3: AddRoute (default via 192.168.1.1) ──────────");
    auto addResult = client.addRoute("default", "192.168.1.1", "eth0", 100);
    if (!addResult)
    {
        std::println(std::cerr, "  [FAIL] AddRoute: {}", addResult.error());
        allOk = false;
    }
    else
    {
        std::println("  [OK]   Route created:");
        printRoute(*addResult, 8);
    }

    // ------------------------------------------------------------------
    // Step 4: AddRoute (10.0.0.0/8)
    // ------------------------------------------------------------------
    std::println("\n─── Step 4: AddRoute (10.0.0.0/8 via 10.0.0.1) ─────");
    auto addResult2 = client.addRoute("10.0.0.0/8", "10.0.0.1", "eth1", 50);
    if (!addResult2)
    {
        std::println(std::cerr, "  [FAIL] AddRoute2: {}", addResult2.error());
        allOk = false;
    }
    else
    {
        std::println("  [OK]   Route created:");
        printRoute(*addResult2, 8);
    }

    // ------------------------------------------------------------------
    // Step 5: ListRoutes
    // ------------------------------------------------------------------
    std::println("\n─── Step 5: ListRoutes ───────────────────────────────");
    auto listResult = client.listRoutes();
    if (!listResult)
    {
        std::println(std::cerr, "  [FAIL] ListRoutes: {}", listResult.error());
        allOk = false;
    }
    else
    {
        std::println("  [OK]   {} route(s) in table:", listResult->size());
        for (const auto& r : *listResult)
        {
            std::println("  ----");
            printRoute(r, 6);
        }
    }

    // ------------------------------------------------------------------
    // Step 6: GetRoute by ID
    // ------------------------------------------------------------------
    if (addResult)
    {
        std::println("\n─── Step 6: GetRoute ({}) ────────────────────────────",
                     addResult->id().substr(0, 8) + "…");
        auto getResult = client.getRoute(addResult->id());
        if (!getResult)
        {
            std::println(std::cerr, "  [FAIL] GetRoute: {}", getResult.error());
            allOk = false;
        }
        else
        {
            std::println("  [OK]   Retrieved:");
            printRoute(*getResult, 8);
        }
    }

    // ------------------------------------------------------------------
    // Step 7: RemoveRoute (first route)
    // ------------------------------------------------------------------
    if (addResult)
    {
        std::println("\n─── Step 7: RemoveRoute ({}) ─────────────────────────",
                     addResult->id().substr(0, 8) + "…");
        auto removeResult = client.removeRoute(addResult->id());
        if (!removeResult)
        {
            std::println(
                std::cerr, "  [FAIL] RemoveRoute: {}", removeResult.error());
            allOk = false;
        }
        else
        {
            std::println("  [OK]   Route removed");
        }
    }

    // ------------------------------------------------------------------
    // Step 8: ListRoutes again (should be 1 remaining)
    // ------------------------------------------------------------------
    std::println("\n─── Step 8: ListRoutes (after remove) ───────────────");
    auto listResult2 = client.listRoutes();
    if (!listResult2)
    {
        std::println(
            std::cerr, "  [FAIL] ListRoutes2: {}", listResult2.error());
        allOk = false;
    }
    else
    {
        std::println("  [OK]   {} route(s) remaining:", listResult2->size());
        for (const auto& r : *listResult2)
        {
            std::println("  ----");
            printRoute(r, 6);
        }
    }

    // ------------------------------------------------------------------
    // Summary
    // ------------------------------------------------------------------
    std::println("\n─── Test result: {} ──────────────────────────────────",
                 allOk ? "ALL PASSED" : "SOME FAILURES");
    return allOk ? EXIT_SUCCESS : EXIT_FAILURE;
}

// ---------------------------------------------------------------------------
// SOT sync helpers
// ---------------------------------------------------------------------------

/**
 * @brief Prints a parsed @c SotConfig summary to stdout.
 *
 * Lists every node with its hostname, management IP, loopback addresses,
 * and a count of VRFs, interfaces, and prefixes.  Called immediately after
 * successful SOT parsing so the operator can verify the loaded data.
 *
 * @param cfg  Parsed SOT configuration.
 */
static void printSotSummary(const sra::SotConfig& cfg)
{
    std::println("SOT parsed: {} node(s), {} prefix(es) total",
                 cfg.nodes.size(),
                 cfg.totalPrefixCount());
    std::println("{}", std::string(60, '-'));

    for (const auto& node : cfg.nodes)
    {
        std::size_t ifaceCount = 0;
        std::size_t prefixCount = 0;
        for (const auto& vrf : node.vrfs)
        {
            ifaceCount += vrf.ipv4.interfaces.size();
            for (const auto& iface : vrf.ipv4.interfaces)
            {
                prefixCount += iface.prefixes.size();
            }
        }

        std::println("  {} ({})  lo4={}  vrfs={}  ifaces={}  prefixes={}",
                     node.hostname,
                     node.management_ip,
                     node.loopbacks.ipv4,
                     node.vrfs.size(),
                     ifaceCount,
                     prefixCount);
    }
    std::println("{}", std::string(60, '-'));
}

/**
 * @brief Pushes all prefix routes from a single SOT node to an srmd server.
 *
 * Iterates every VRF → interface → prefix of @p node and calls
 * @c RouteClient::addRoute() for each prefix.  The interface's nexthop and
 * name are forwarded as gateway and interface_name respectively.
 *
 * @param client  Connected @c RouteClient targeting the matching srmd.
 * @param node    SOT node whose routes are to be pushed.
 * @return Number of routes successfully added.  Failures are printed to
 *         stderr but do not abort the remaining prefixes.
 */
static std::size_t pushNodeRoutes(sra::RouteClient& client,
                                  const sra::SotNode& node)
{
    std::size_t pushed = 0;

    for (const auto& vrf : node.vrfs)
    {
        for (const auto& iface : vrf.ipv4.interfaces)
        {
            for (const auto& pfx : iface.prefixes)
            {
                auto result = client.addRoute(
                    pfx.prefix, iface.nexthop, iface.name, pfx.weight);
                if (!result)
                {
                    std::println(std::cerr,
                                 "  [FAIL] AddRoute {}/{} via {} ({}): {}",
                                 pfx.prefix,
                                 iface.name,
                                 iface.nexthop,
                                 vrf.name,
                                 result.error());
                }
                else
                {
                    std::println("  [OK]   {} via {} iface={} metric={}  "
                                 "id={}",
                                 pfx.prefix,
                                 iface.nexthop,
                                 iface.name,
                                 pfx.weight,
                                 result->id().substr(0, 8) + "…");
                    ++pushed;
                }
            }
        }
    }

    return pushed;
}

/**
 * @brief Synchronises SOT routes to a single server.
 *
 * Looks up @p managementIp in the SOT, then calls @c pushNodeRoutes().
 * If the management IP is not found in the SOT the server is skipped with
 * a warning.
 *
 * @param client        Connected @c RouteClient.
 * @param managementIp  Management IP used to correlate switch with SOT node.
 * @param label         Display label for log output.
 * @param sot           Parsed SOT configuration.
 * @return EXIT_SUCCESS if the node was found and all routes pushed;
 *         EXIT_FAILURE if the node was missing or any push failed.
 */
static int syncOneServer(sra::RouteClient& client,
                         const std::string& managementIp,
                         const std::string& label,
                         const sra::SotConfig& sot)
{
    const sra::SotNode* node = sot.findByManagementIp(managementIp);
    if (!node)
    {
        std::println(std::cerr,
                     "  [WARN] {} ({}): no SOT entry found – skipping",
                     label,
                     managementIp);
        return EXIT_FAILURE;
    }

    std::println("  SOT node  : {} ({})", node->hostname, node->management_ip);
    std::println("  Loopback  : ipv4={} ipv6={}",
                 node->loopbacks.ipv4,
                 node->loopbacks.ipv6);

    const std::size_t pushed = pushNodeRoutes(client, *node);
    std::println("  => {} route(s) pushed", pushed);

    return EXIT_SUCCESS;
}

/**
 * @brief Synchronises SOT routes to all servers listed in @p switches.
 *
 * The SOT file is parsed @em before any gRPC connections are opened (the
 * function receives an already-loaded @c SotConfig).  For each switch entry
 * the management IP (@c SwitchEntry::ipv4) is looked up in the SOT; matching
 * entries have their prefix routes pushed via @c RouteClient::addRoute().
 *
 * @param sot       Pre-parsed SOT configuration.
 * @param switches  List of switch entries from @c switch_config.json.
 * @param useTls    Whether to use TLS gRPC channels.
 * @param caCert    Path to PEM CA certificate (empty = system roots).
 * @param timeout   Per-RPC deadline in seconds.
 * @return EXIT_SUCCESS if every matched server succeeded; EXIT_FAILURE if any
 *         server was missing from the SOT or experienced push failures.
 */
static int cmdSyncMulti(const sra::SotConfig& sot,
                        const std::vector<sra::SwitchEntry>& switches,
                        bool useTls,
                        const std::string& caCert,
                        int timeout)
{
    struct Result
    {
        std::string label;
        int exitCode;
    };
    std::vector<Result> results;
    results.reserve(switches.size());

    for (const auto& sw : switches)
    {
        std::println("\n{}", std::string(60, '='));
        std::println("Switch : {}  ({})", sw.label(), sw.target());
        std::println("{}", std::string(60, '='));

        sra::RouteClient client(
            sw.target(), useTls, caCert, timeout, sw.login, sw.password);

        const int rc = syncOneServer(client, sw.ipv4, sw.label(), sot);
        results.push_back({sw.label(), rc});
    }

    // Aggregate summary
    std::println("\n{}", std::string(60, '='));
    std::println("SOT sync summary  ({} server(s))", switches.size());
    std::println("{}", std::string(60, '-'));
    bool anyFailed = false;
    for (const auto& r : results)
    {
        const bool ok = (r.exitCode == EXIT_SUCCESS);
        if (!ok)
        {
            anyFailed = true;
        }
        std::println("  {}  {}", ok ? "[OK  ]" : "[FAIL]", r.label);
    }
    std::println("{}", std::string(60, '='));
    return anyFailed ? EXIT_FAILURE : EXIT_SUCCESS;
}

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------

/**
 * @brief Runs @c cmdTest() against every server listed in @p switches.
 *
 * For each @c SwitchEntry a new @c RouteClient is constructed using that
 * entry's IPv4/port and credentials.  Results are aggregated and a final
 * per-server summary is printed.
 *
 * @param switches     Parsed switch configuration entries.
 * @param useTls       Whether to use a TLS gRPC channel.
 * @param caCert       Path to PEM CA certificate (empty = system roots).
 * @param timeout      Per-RPC deadline in seconds.
 * @return EXIT_SUCCESS if every server's test passed; EXIT_FAILURE otherwise.
 */
static int cmdMultiTest(const std::vector<sra::SwitchEntry>& switches,
                        bool useTls,
                        const std::string& caCert,
                        int timeout)
{
    struct Result
    {
        std::string label;
        int exitCode;
    };
    std::vector<Result> results;
    results.reserve(switches.size());

    for (const auto& sw : switches)
    {
        const std::string header(60, '=');
        std::println("\n{}", header);
        std::println("Switch : {}  ({})", sw.label(), sw.target());
        std::println("Login  : {}", sw.login);
        std::println("{}", header);

        sra::RouteClient client(
            sw.target(), useTls, caCert, timeout, sw.login, sw.password);

        const int rc = cmdTest(client);
        results.push_back({sw.label(), rc});
    }

    // Print aggregate summary
    std::println("\n{}", std::string(60, '='));
    std::println("Multi-server test summary  ({} server(s))", switches.size());
    std::println("{}", std::string(60, '-'));

    bool anyFailed = false;
    for (const auto& r : results)
    {
        const bool passed = (r.exitCode == EXIT_SUCCESS);
        if (!passed)
        {
            anyFailed = true;
        }
        std::println("  {}  {}", passed ? "[PASS]" : "[FAIL]", r.label);
    }
    std::println("{}", std::string(60, '='));

    return anyFailed ? EXIT_FAILURE : EXIT_SUCCESS;
}

// ---------------------------------------------------------------------------
// Netlink watch command
// ---------------------------------------------------------------------------

/**
 * @brief Maps a Linux rtnetlink protocol byte to the closest srmd RouteProtocol.
 *
 * FRR zebra redistributes routes with protocol codes that predate the srmd
 * enumeration (RTPROT_ZEBRA, RTPROT_OSPF, etc.).  This helper maps them to
 * the srmd values so they are stored with a meaningful protocol tag.
 *
 * @param rtProto  The rtm_protocol byte from the kernel route message.
 * @return         Corresponding srmd::v1::RouteProtocol value.
 */
static srmd::v1::RouteProtocol mapRtProtocol(uint8_t rtProto)
{
    switch (rtProto)
    {
    case RTPROT_OSPF:   return srmd::v1::ROUTE_PROTOCOL_OSPF;
    case RTPROT_BGP:    return srmd::v1::ROUTE_PROTOCOL_BGP;
    case RTPROT_RIP:    return srmd::v1::ROUTE_PROTOCOL_RIP;
    case RTPROT_KERNEL: return srmd::v1::ROUTE_PROTOCOL_CONNECTED;
    case RTPROT_BOOT:   return srmd::v1::ROUTE_PROTOCOL_CONNECTED;
    default:            return srmd::v1::ROUTE_PROTOCOL_STATIC;
    }
}

/**
 * @brief A single entry in the dynamic /32 route table.
 *
 * All fields mirror the corresponding @c netlink_route32_t members so the
 * table carries the complete netlink message payload — both the struct rtmsg
 * header fields and every extracted RTA_* attribute.
 */
struct WatchRoute
{
    /* ── RTA_* attributes ──────────────────────────────────────────────── */
    std::string dest;        ///< Destination prefix (e.g. "10.0.0.1/32").
    std::string gateway;     ///< Next-hop gateway (empty if none / 0.0.0.0).
    std::string iface;       ///< Outgoing interface name (ifname).
    uint32_t    ifindex{0};  ///< RTA_OIF: outgoing interface index.
    uint32_t    metric{0};   ///< RTA_PRIORITY: route metric / preference.
    uint32_t    table{0};    ///< RTA_TABLE / rtm_table: routing table ID.
    /* ── From struct rtmsg ─────────────────────────────────────────────── */
    uint8_t     protocol{0}; ///< rtm_protocol: RTPROT_OSPF, RTPROT_ZEBRA, …
    /* ── gRPC tracking ─────────────────────────────────────────────────── */
    std::string srmdId;      ///< Server-assigned ID (empty when push failed).
    /* ── Remaining struct rtmsg fields ──────────────────────────────────── */
    uint8_t     family{0};   ///< rtm_family: AF_INET = 2.
    uint8_t     dst_len{0};  ///< rtm_dst_len: prefix length (32 for host routes).
    uint8_t     tos{0};      ///< rtm_tos: type-of-service filter value.
    uint8_t     scope{0};    ///< rtm_scope: RT_SCOPE_* (universe/link/host/…).
    uint8_t     type{0};     ///< rtm_type: RTN_UNICAST, RTN_BLACKHOLE, …
    uint32_t    flags{0};    ///< rtm_flags: RTM_F_* bitmask.
};

/**
 * @brief Maps a Linux rtnetlink protocol byte to a short display string.
 *
 * @param proto  rtm_protocol byte from the kernel route message.
 * @return       Short human-readable label (e.g. "ospf", "zebra", "static").
 */
static std::string protoLabel(uint8_t proto)
{
    switch (proto)
    {
    case RTPROT_KERNEL:  return "kernel";
    case RTPROT_BOOT:    return "boot";
    case RTPROT_STATIC:  return "static";
    case RTPROT_OSPF:    return "ospf";
    case RTPROT_BGP:     return "bgp";
    case RTPROT_RIP:     return "rip";
    case RTPROT_ZEBRA:   return "zebra";
    default:             return std::to_string(static_cast<int>(proto));
    }
}

/* Forward declarations for helpers defined later in this file. */
static std::string familyLabel(uint8_t family);
static std::string scopeLabel(uint8_t scope);

/**
 * @brief Pretty-prints the dynamic /32 OSPF route table using box-drawing lines.
 *
 * Columns mirror every field of @c netlink_route32_t so the operator can
 * inspect the complete netlink payload at a glance.  Column widths are fixed
 * so each redraw is perfectly aligned.  The table is keyed by the compound
 * key dest|gateway|iface so ECMP routes to the same destination appear as
 * separate rows.
 *
 * Columns displayed:
 *  Destination · Gateway · Interface · IfIdx · Metric · Table · Protocol · Server ID
 *
 * @param routes  Ordered map of compound-key → WatchRoute entries.
 */
static void printRouteTable(const std::map<std::string, WatchRoute>& routes)
{
    // ── Column content widths (characters, not including padding spaces) ──
    // Destination : "255.255.255.255/32" = 18 chars  → 20
    // Gateway     : "255.255.255.255"    = 15 chars  → 17
    // Interface   : IFNAMSIZ-1           = 15 chars  → 15
    // IfIdx       : up to 5 digits       →  5 (right-aligned)
    // Metric      : up to 10 digits      →  7 (right-aligned)
    // Table       : up to 5 digits       →  5 (right-aligned)
    // Protocol    : "static" = 6 chars   →  8 (left-aligned)
    // Server ID   : "abcdefgh…" = 9 ch   →  9 (left-aligned)
    constexpr int cD = 20, cG = 17, cI = 15, cX = 5, cM = 7, cT = 5,
                  cP = 8,  cS = 9;

    // ── UTF-8 box-drawing helpers ─────────────────────────────────────────
    auto hbar = [](int n) {
        std::string s;
        s.reserve(static_cast<std::size_t>(n) * 3); // "─" = 3 UTF-8 bytes
        for (int i = 0; i < n; ++i) s += "─";
        return s;
    };

    // Build top / mid / bottom border rows with a cell for every column.
    auto mkBorder = [&](const char* l, const char* j, const char* r) {
        return std::string(l)
             + hbar(cD+2) + j + hbar(cG+2) + j + hbar(cI+2) + j
             + hbar(cX+2) + j + hbar(cM+2) + j + hbar(cT+2) + j
             + hbar(cP+2) + j + hbar(cS+2) + r;
    };

    const std::string top    = mkBorder("┌", "┬", "┐");
    const std::string mid    = mkBorder("├", "┼", "┤");
    const std::string bottom = mkBorder("└", "┴", "┘");

    // ── Helper: one table row ─────────────────────────────────────────────
    auto printRow = [&](const std::string& dst,
                        const std::string& gw,
                        const std::string& ifc,
                        const std::string& ifx,
                        const std::string& met,
                        const std::string& tbl,
                        const std::string& prot,
                        const std::string& sid)
    {
        std::println("│ {:<{}} │ {:<{}} │ {:<{}} │ {:>{}} │ {:>{}} │ {:>{}} │ {:<{}} │ {:<{}} │",
                     dst,  cD,
                     gw,   cG,
                     ifc,  cI,
                     ifx,  cX,
                     met,  cM,
                     tbl,  cT,
                     prot, cP,
                     sid,  cS);
    };

    // ── Title ─────────────────────────────────────────────────────────────
    std::println("\n OSPF /32 Route Table  ({} route(s))", routes.size());
    std::println("{}", top);
    printRow("Destination", "Gateway", "Interface", "IfIdx",
             "Metric",      "Table",   "Protocol",  "Server ID");

    if (routes.empty())
    {
        std::println("{}", bottom);
        return;
    }

    std::println("{}", mid);

    for (const auto& [key, r] : routes)
    {
        const std::string gw   = r.gateway.empty() ? "(none)" : r.gateway;
        const std::string ifc  = r.iface.empty()   ? "(none)" : r.iface;
        const std::string ifx  = r.ifindex == 0
                                    ? "-"
                                    : std::to_string(r.ifindex);
        const std::string met  = std::to_string(r.metric);
        const std::string tbl  = r.table == 0
                                    ? "-"
                                    : std::to_string(r.table);
        const std::string prot = protoLabel(r.protocol);
        const std::string sid  = r.srmdId.empty()
                                    ? "(pending)"
                                    : r.srmdId.substr(0, 8) + "…";

        printRow(r.dest, gw, ifc, ifx, met, tbl, prot, sid);
    }

    std::println("{}", bottom);
}

/**
 * @brief Serializes the route table to a text snapshot for UDP delivery.
 *
 * Format (one entry per line after the header):
 * @code
 *   ROUTES <count>
 *   dest=<prefix> gw=<gw> iface=<if> ifidx=<n> metric=<m> table=<t> proto=<p> id=<srmd-id>
 *   …
 * @endcode
 */
static std::string serializeRouteTable(const std::map<std::string, WatchRoute>& routes)
{
    std::ostringstream os;
    os << "ROUTES " << routes.size() << '\n';
    for (const auto& [key, r] : routes)
    {
        os << std::format(
            "dest={} gw={} iface={} ifidx={} metric={} table={} proto={}"
            " family={} dst_len={} tos={} scope={} type={} flags=0x{:08x}"
            " id={}\n",
            r.dest,
            r.gateway.empty() ? "(none)" : r.gateway,
            r.iface.empty()   ? "(none)" : r.iface,
            r.ifindex,
            r.metric,
            r.table,
            protoLabel(r.protocol),
            familyLabel(r.family),
            static_cast<unsigned>(r.dst_len),
            static_cast<unsigned>(r.tos),
            scopeLabel(r.scope),
            static_cast<unsigned>(r.type),
            r.flags,
            r.srmdId.empty() ? "(pending)" : r.srmdId);
    }
    return os.str();
}

/**
 * @brief Context forwarded via the @c user_data pointer to the netlink callback.
 *
 * Carries the live gRPC client and the dynamic route table that tracks every
 * /32 OSPF route event together with its srmd server ID.
 */
struct WatchCtx
{
    sra::RouteClient*                 client;
    std::map<std::string, WatchRoute> routes;     ///< "dest|gw|iface" → WatchRoute
    std::string                       loopback;   ///< Node's own loopback IP for GetLoopbacks
    sra::UdpTableServer*              udpServer;  ///< UDP publisher (port 9003); may be nullptr
};

/**
 * @brief Netlink callback: translates each /32 route event into a gRPC call.
 *
 * Called by @c netlink_run() for every IPv4 /32 unicast route event that
 * passes the kernel filter.  Behaviour per event type:
 *
 * - NETLINK_ROUTE_ADDED   → AddRoute RPC; stores the returned server ID.
 * - NETLINK_ROUTE_CHANGED → RemoveRoute (if we hold an ID for that dest) then
 *                           AddRoute with the new attributes; updates stored ID.
 * - NETLINK_ROUTE_REMOVED → RemoveRoute for the stored ID (if any); erases
 *                           the entry from the local map.
 *
 * All outcomes (success and failure) are printed to stdout as a timestamped
 * log line so the operator can trace every kernel→srmd interaction.
 *
 * @param event      Route event type from the netlink module.
 * @param route      Parsed /32 route descriptor; valid for this call only.
 * @param user_data  Pointer to a @c WatchCtx.
 */
static void nlWatchCb(netlink_event_t          event,
                      const netlink_route32_t* route,
                      void*                    user_data)
{
    auto* ctx = static_cast<WatchCtx*>(user_data);

    /* UTC timestamp */
    const auto now = std::chrono::system_clock::now();
    const auto us  = std::chrono::duration_cast<std::chrono::microseconds>(
                         now.time_since_epoch())
                         .count();
    const std::time_t sec = static_cast<std::time_t>(us / 1'000'000);
    const int         ms  = static_cast<int>((us % 1'000'000) / 1000);
    std::tm           tm_utc{};
    gmtime_r(&sec, &tm_utc);
    const std::string ts = std::format("{:02d}:{:02d}:{:02d}.{:03d}",
                                       tm_utc.tm_hour,
                                       tm_utc.tm_min,
                                       tm_utc.tm_sec,
                                       ms);

    /* Destination string "x.x.x.x/<len>" */
    char dst_buf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &route->dst, dst_buf, sizeof(dst_buf));
    const std::string dest = std::string(dst_buf) + "/"
                           + std::to_string(static_cast<unsigned>(route->dst_len));

    /* Gateway string (empty when nexthop is 0.0.0.0) */
    char gw_buf[INET_ADDRSTRLEN] = {};
    if (route->gateway.s_addr != 0)
    {
        inet_ntop(AF_INET, &route->gateway, gw_buf, sizeof(gw_buf));
    }
    const std::string gw    = gw_buf;
    const std::string iface = route->ifname;

    const srmd::v1::RouteProtocol proto = mapRtProtocol(route->protocol);

    // only for OSPF now
    if (proto == srmd::v1::ROUTE_PROTOCOL_OSPF)
    {
        /* Compound key: dest|gateway|iface – allows multiple routes to the
         * same destination (ECMP) to coexist as separate table rows. */
        const std::string key = dest + "|" + gw + "|" + iface;

        /* ---- ADDED ---------------------------------------------------------- */
        if (event == NETLINK_ROUTE_ADDED)
        {
            auto result = ctx->client->addRoute(
                dest, gw, iface, route->metric,
                srmd::v1::ADDRESS_FAMILY_IPV4, proto);

            if (result)
            {
                ctx->routes[key] = WatchRoute{dest, gw, iface,
                                              route->ifindex,
                                              route->metric, route->table,
                                              route->protocol, result->id(),
                                              route->family, route->dst_len,
                                              route->tos, route->scope,
                                              route->type, route->flags};
                std::println("{} [ADDED]   {} via {} dev {} metric {} → id={}",
                            ts, dest, gw, iface, route->metric,
                            result->id().substr(0, 8) + "…");

                /* If we know our own loopback, ask the server for the full
                 * interface+prefix list and print the entry matching this
                 * nexthop (gateway). */
                if (!ctx->loopback.empty() && !gw.empty())
                {
                    auto lbResult = ctx->client->getLoopbacks(ctx->loopback);
                    if (lbResult)
                    {
                        bool found = false;
                        for (const auto& intf : lbResult->interfaces())
                        {
                            if (intf.nexthop() != gw)
                            {
                                continue;
                            }
                            found = true;
                            std::println("{}   SOT nexthop {} → iface \"{}\" "
                                         "(type={} local={} weight={} desc={})",
                                         ts, gw,
                                         intf.name(), intf.type(),
                                         intf.local_address(), intf.weight(),
                                         intf.description());
                            for (const auto& pfx : intf.prefixes())
                            {
                                std::println("{}     prefix {} weight={} "
                                             "role={} desc={}",
                                             ts,
                                             pfx.prefix(), pfx.weight(),
                                             pfx.role(), pfx.description());
                            }
                        }
                        if (!found)
                        {
                            std::println("{} [ADDED]   SOT: no interface with "
                                         "nexthop {} found for loopback {}",
                                         ts, gw, ctx->loopback);
                        }
                    }
                    else
                    {
                        std::println("{} [ADDED]   GetLoopbacks FAILED: {}",
                                    ts, lbResult.error());
                    }
                }
            }
            else
            {
                /* Track with empty srmdId so REMOVED events still clean up. */
                ctx->routes[key] = WatchRoute{dest, gw, iface,
                                              route->ifindex,
                                              route->metric, route->table,
                                              route->protocol, {},
                                              route->family, route->dst_len,
                                              route->tos, route->scope,
                                              route->type, route->flags};
                std::println("{} [ADDED]   {} → gRPC FAILED: {}",
                            ts, dest, result.error());
            }
        }
        /* ---- CHANGED -------------------------------------------------------- */
        else if (event == NETLINK_ROUTE_CHANGED)
        {
            /* Remove ALL existing entries for this destination (the kernel
             * replaces the route, so old nexthops are superseded regardless
             * of gateway/iface).  Each entry is removed from srmd first. */
            for (auto it = ctx->routes.begin(); it != ctx->routes.end(); )
            {
                if (it->second.dest == dest)
                {
                    if (!it->second.srmdId.empty())
                    {
                        auto rmResult =
                            ctx->client->removeRoute(it->second.srmdId);
                        if (!rmResult)
                        {
                            std::println(
                                "{} [CHANGED] {} stale-remove failed: {}",
                                ts, dest, rmResult.error());
                        }
                    }
                    it = ctx->routes.erase(it);
                }
                else
                {
                    ++it;
                }
            }

            auto result = ctx->client->addRoute(
                dest, gw, iface, route->metric,
                srmd::v1::ADDRESS_FAMILY_IPV4, proto);

            if (result)
            {
                ctx->routes[key] = WatchRoute{dest, gw, iface,
                                              route->ifindex,
                                              route->metric, route->table,
                                              route->protocol, result->id(),
                                              route->family, route->dst_len,
                                              route->tos, route->scope,
                                              route->type, route->flags};
                std::println("{} [CHANGED] {} via {} dev {} metric {} → id={}",
                            ts, dest, gw, iface, route->metric,
                            result->id().substr(0, 8) + "…");
            }
            else
            {
                ctx->routes[key] = WatchRoute{dest, gw, iface,
                                              route->ifindex,
                                              route->metric, route->table,
                                              route->protocol, {},
                                              route->family, route->dst_len,
                                              route->tos, route->scope,
                                              route->type, route->flags};
                std::println("{} [CHANGED] {} → gRPC FAILED: {}",
                            ts, dest, result.error());
            }
        }
        /* ---- REMOVED -------------------------------------------------------- */
        else
        {
            auto it = ctx->routes.find(key);
            if (it == ctx->routes.end())
            {
                std::println("{} [REMOVED] {} via {} dev {} (not tracked – "
                             "no gRPC call)",
                            ts, dest, gw, iface);
                std::cout.flush();
                return;
            }

            const std::string id = it->second.srmdId;
            ctx->routes.erase(it);

            if (id.empty())
            {
                std::println("{} [REMOVED] {} via {} dev {} "
                             "(no server ID – no gRPC call)",
                            ts, dest, gw, iface);
            }
            else
            {
                auto result = ctx->client->removeRoute(id);
                if (result)
                {
                    std::println("{} [REMOVED] {} via {} dev {} id={}",
                                ts, dest, gw, iface, id.substr(0, 8) + "…");
                }
                else
                {
                    std::println("{} [REMOVED] {} via {} dev {} → "
                                 "gRPC FAILED: {}",
                                ts, dest, gw, iface, result.error());
                }
            }
        }

        /* Publish updated table via UDP (port 9003) instead of printing. */
        if (ctx->udpServer)
            ctx->udpServer->setRouteData(serializeRouteTable(ctx->routes));
    }

    std::cout.flush();
}

/* ---------------------------------------------------------------------------
 * File-scope fds for the three startup (background) netlink monitors.
 * Initialised by main() before any command runs; closed by signal handlers
 * or on normal exit to unblock the background threads.
 * ------------------------------------------------------------------------- */
static volatile int g_startup_route_fd   = -1;
static volatile int g_startup_neigh_fd   = -1;
static volatile int g_startup_nexthop_fd = -1;

/* File-scope fd used by the signal handler to unblock netlink_run(). */
static volatile int g_watch_fd = -1;

/** @brief Stop flag for the 'run' daemon command (set by signal handler). */
static volatile sig_atomic_t g_run_stop = 0;

/** @brief SIGINT/SIGTERM handler for the 'run' daemon command.
 *
 *  Sets g_run_stop and closes startup netlink fds so all background
 *  monitor threads unblock and can be joined cleanly. */
static void runSigHandler(int /*signo*/)
{
    g_run_stop = 1;
    if (g_startup_route_fd >= 0)
    {
        netlink_close(g_startup_route_fd);
        g_startup_route_fd = -1;
    }
    if (g_startup_neigh_fd >= 0)
    {
        netlink_neigh_close(g_startup_neigh_fd);
        g_startup_neigh_fd = -1;
    }
    if (g_startup_nexthop_fd >= 0)
    {
        netlink_nexthop_close(g_startup_nexthop_fd);
        g_startup_nexthop_fd = -1;
    }
}

/** @brief SIGINT/SIGTERM handler for the watch command.
 *
 *  Closes the watch fd AND the startup neighbor/nexthop fds so all
 *  background threads unblock and can be joined. */
static void watchSigHandler(int /*signo*/)
{
    if (g_watch_fd >= 0)
    {
        netlink_close(g_watch_fd);
        g_watch_fd = -1;
    }
    if (g_startup_neigh_fd >= 0)
    {
        netlink_neigh_close(g_startup_neigh_fd);
        g_startup_neigh_fd = -1;
    }
    if (g_startup_nexthop_fd >= 0)
    {
        netlink_nexthop_close(g_startup_nexthop_fd);
        g_startup_nexthop_fd = -1;
    }
}

/** @brief Stop flag for the grpc-proc-demo command (set by signal handler). */
static volatile sig_atomic_t g_demo_stop = 0;

/** @brief SIGINT/SIGTERM handler for the grpc-proc-demo command. */
static void demoSigHandler(int /*signo*/)
{
    g_demo_stop = 1;
}

/**
 * @brief Runs the netlink /32 OSPF route watcher with gRPC forwarding.
 *
 * The @p udpServer is owned by the caller (created at startup and shared
 * across all three monitors).  The startup route background thread must
 * already have been stopped before this is called.
 *
 * Startup sequence:
 *  1. Reads the current kernel routing table and builds an initial dynamic
 *     table of IPv4 /32 OSPF routes; each existing route is pushed to srmd
 *     via AddRoute and the server-assigned ID is stored.
 *  2. Publishes the initial route snapshot via UDP (port 9003).
 *  3. Requests this node's loopback address from the server (used to enrich
 *     ADDED events with SOT interface data).
 *  4. Opens a NETLINK_ROUTE socket and enters the monitoring loop; every
 *     subsequent kernel route event updates the dynamic table and is printed.
 *
 * Runs until SIGINT / SIGTERM is received.
 *
 * @param client          Constructed and connected @c RouteClient.
 * @param configLoopback  Fallback loopback from the client config file.
 * @param udpServer       Shared UDP table server (already started by caller).
 * @return @c EXIT_SUCCESS on clean stop; @c EXIT_FAILURE if the netlink
 *         socket could not be opened.
 */
static int cmdNetlinkWatch(sra::RouteClient& client,
                           const std::string& configLoopback,
                           sra::UdpTableServer& udpServer)
{
    // ── Step 1: Read existing kernel /32 OSPF routes ─────────────────────
    std::println("[Watch] Reading /32 OSPF routes from kernel routing table…");

    WatchCtx ctx{&client, {}, configLoopback, &udpServer};

    try
    {
        sra::RoutingManager rm;
        auto routesResult = rm.listRoutes(AF_INET, RT_TABLE_UNSPEC);
        if (routesResult)
        {
            int found = 0;
            for (const auto& kr : *routesResult)
            {
                if (kr.prefixLen != 32
                    || kr.protocol != RTPROT_OSPF
                    || kr.type     != RTN_UNICAST)
                {
                    continue;
                }

                /* Push to srmd so it is reflected in the server's route table. */
                auto result = client.addRoute(
                    kr.destination, kr.gateway, kr.interfaceName, kr.metric,
                    srmd::v1::ADDRESS_FAMILY_IPV4,
                    srmd::v1::ROUTE_PROTOCOL_OSPF);

                WatchRoute wr{kr.destination, kr.gateway,
                              kr.interfaceName, kr.interfaceIndex,
                              kr.metric, kr.table, kr.protocol, {},
                              static_cast<uint8_t>(kr.family), kr.prefixLen,
                              0u, kr.scope, kr.type, 0u};
                if (result)
                {
                    wr.srmdId = result->id();
                }
                else
                {
                    std::println(std::cerr,
                                 "[Startup]   AddRoute {} failed: {}",
                                 kr.destination, result.error());
                }
                const std::string key =
                    kr.destination + "|" + kr.gateway + "|" + kr.interfaceName;
                ctx.routes[key] = wr;
                ++found;
            }
            std::println("[Startup] Found {} /32 OSPF route(s) in kernel.", found);
        }
        else
        {
            std::println(std::cerr,
                         "[Startup] Warning: could not read kernel routes: {}",
                         routesResult.error());
        }
    }
    catch (const std::exception& e)
    {
        std::println(std::cerr,
                     "[Startup] Warning: RoutingManager init failed: {}",
                     e.what());
    }

    // ── Step 2: Publish initial route table via UDP (port 9003) ─────────
    udpServer.setRouteData(serializeRouteTable(ctx.routes));

    // ── Step 3: Request loopback from server ─────────────────────────────
    std::println("\n[Startup] Requesting loopback from server…");
    auto lbResult = client.requestLoopback();
    if (lbResult)
    {
        ctx.loopback = *lbResult;
        std::println("[Startup] Loopback from server: '{}'", ctx.loopback);
    }
    else
    {
        std::println("[Startup] No loopback from server ({}); "
                     "using config loopback: '{}'",
                     lbResult.error(), ctx.loopback);
    }

    // ── Step 3b: GetLoopbacks then GetAllRoutes ───────────────────────────
    if (!ctx.loopback.empty())
    {
        std::println("[Startup] GetLoopbacks for loopback '{}'…", ctx.loopback);
        auto glResult = client.getLoopbacks(ctx.loopback);
        if (glResult)
        {
            std::println("[Startup] GetLoopbacks: {} — {} interface(s)",
                         glResult->message(), glResult->interfaces_size());
        }
        else
        {
            std::println("[Startup] GetLoopbacks failed: {}", glResult.error());
        }

        std::println("[Startup] GetAllRoutes…");
        auto arResult = client.getAllRoutes();
        if (arResult)
        {
            printAllRoutes(*arResult);
        }
        else
        {
            std::println("[Startup] GetAllRoutes failed: {}", arResult.error());
        }
    }

    // ── Step 4: Start monitoring ──────────────────────────────────────────
    /* Install signal handlers. */
    struct sigaction sa{};
    sa.sa_handler = watchSigHandler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    int fd = netlink_init();
    if (fd < 0)
    {
        std::println(std::cerr, "[Monitor] Error: netlink_init failed: {}",
                     std::strerror(errno));
        return EXIT_FAILURE;
    }
    g_watch_fd = fd;

    std::println("\n[Monitor] Watching for IPv4 /32 OSPF route events"
                 " → forwarding to srmd …");
    if (!ctx.loopback.empty())
    {
        std::println("[Monitor] Loopback {} → GetLoopbacks on each OSPF ADDED"
                     " event", ctx.loopback);
    }
    std::println("[Monitor] (Ctrl-C to stop)\n");
    std::cout.flush();

    netlink_run(fd, nlWatchCb, &ctx);

    netlink_close(g_watch_fd); /* no-op if already closed by signal handler */

    std::println("\n[Monitor] Stopped.");
    return EXIT_SUCCESS;
}

// ---------------------------------------------------------------------------
// Neighbor monitoring command
// ---------------------------------------------------------------------------

/**
 * @brief Maps an address family byte to a short display string.
 */
static std::string familyLabel(uint8_t family)
{
    switch (family)
    {
    case AF_INET:   return "inet";
    case AF_INET6:  return "inet6";
    case AF_BRIDGE: return "bridge";
    default:        return std::to_string(static_cast<int>(family));
    }
}

/**
 * @brief Maps a NUD_* neighbor state bitmask to a human-readable string.
 */
static std::string nudStateLabel(uint16_t state)
{
    switch (state)
    {
    case NUD_INCOMPLETE: return "incomplete";
    case NUD_REACHABLE:  return "reachable";
    case NUD_STALE:      return "stale";
    case NUD_DELAY:      return "delay";
    case NUD_PROBE:      return "probe";
    case NUD_FAILED:     return "failed";
    case NUD_NOARP:      return "noarp";
    case NUD_PERMANENT:  return "permanent";
    case NUD_NONE:       return "none";
    default:             return std::format("0x{:02x}", state);
    }
}

/**
 * @brief Maps NTF_* neighbor flag bits to a compact flag string (e.g. "RSP").
 *
 * Letter codes: R=router, S=self, M=master, P=proxy, X=ext-learned,
 *               O=offloaded, K=sticky, U=use.
 */
static std::string ntfFlagsLabel(uint8_t flags)
{
    if (flags == 0)
        return "-";
    std::string s;
    if (flags & NTF_USE)         s += 'U';
    if (flags & NTF_SELF)        s += 'S';
    if (flags & NTF_MASTER)      s += 'M';
    if (flags & NTF_PROXY)       s += 'P';
    if (flags & NTF_EXT_LEARNED) s += 'X';
#ifdef NTF_OFFLOADED
    if (flags & NTF_OFFLOADED)   s += 'O';
#endif
#ifdef NTF_STICKY
    if (flags & NTF_STICKY)      s += 'K';
#endif
    if (flags & NTF_ROUTER)      s += 'R';
    return s.empty() ? std::format("0x{:02x}", flags) : s;
}

/**
 * @brief Formats a raw link-layer address as "xx:xx:xx:xx:xx:xx".
 *
 * Returns "(none)" when lladdr_len == 0.
 */
static std::string formatMac(const uint8_t *addr, uint8_t len)
{
    if (len == 0)
        return "(none)";
    std::string s;
    s.reserve(static_cast<std::size_t>(len) * 3);
    for (uint8_t i = 0; i < len; ++i)
    {
        if (i > 0)
            s += ':';
        s += std::format("{:02x}", addr[i]);
    }
    return s;
}

/**
 * @brief A single row in the dynamic neighbor table.
 *
 * All fields mirror the corresponding @c netlink_neigh_t members so the
 * table carries the complete ndmsg header and every NDA_* attribute.
 */
struct NeighEntry
{
    /* ── From struct ndmsg ─────────────────────────────────────────────── */
    uint8_t  family{0};          ///< ndm_family: AF_INET/AF_INET6/AF_BRIDGE
    int      ifindex{0};         ///< ndm_ifindex: outgoing interface index
    std::string ifname;          ///< Resolved interface name
    uint16_t state{0};           ///< ndm_state: NUD_* bitmask
    uint8_t  flags{0};           ///< ndm_flags: NTF_* bitmask
    uint8_t  type{0};            ///< ndm_type: RTN_*
    /* ── NDA_DST ───────────────────────────────────────────────────────── */
    std::string dst;             ///< Destination IP string
    /* ── NDA_LLADDR ────────────────────────────────────────────────────── */
    std::string mac;             ///< Formatted link-layer address (xx:xx:…)
    /* ── NDA_CACHEINFO ─────────────────────────────────────────────────── */
    uint32_t confirmed_ms{0};    ///< ndm_confirmed: ms since last confirmation
    uint32_t used_ms{0};         ///< ndm_used: ms since last use
    uint32_t updated_ms{0};      ///< ndm_updated: ms since last update
    uint32_t refcnt{0};          ///< ndm_refcnt: reference count
    /* ── NDA_PROBES ────────────────────────────────────────────────────── */
    uint32_t probes{0};          ///< ARP/NDP probe count
    /* ── NDA_VLAN ──────────────────────────────────────────────────────── */
    uint16_t vlan{0};            ///< VLAN ID (bridge; 0 = absent)
    /* ── NDA_MASTER ────────────────────────────────────────────────────── */
    uint32_t master{0};          ///< Master interface index (0 = absent)
    std::string master_name;     ///< Resolved master interface name
    /* ── NDA_IFINDEX ───────────────────────────────────────────────────── */
    uint32_t nh_ifindex{0};      ///< NDA_IFINDEX: nexthop interface override
    /* ── NDA_PROTOCOL ──────────────────────────────────────────────────── */
    uint8_t  protocol{0};        ///< Routing protocol that installed entry
};

/**
 * @brief Pretty-prints the dynamic neighbor table using box-drawing lines.
 *
 * Columns mirror every field of netlink_neigh_t so the operator can inspect
 * the complete netlink payload.  The table is keyed by family|ifindex|dst.
 */
static void printNeighborTable(
    const std::map<std::string, NeighEntry>& neighbors)
{
    // ── Column content widths ─────────────────────────────────────────────
    // Family  : "bridge"   = 6   → 6
    // Dst     : IPv6 max   = 39  → 39
    // MAC     : "xx:xx:xx:xx:xx:xx" = 17 → 17
    // Iface   : IFNAMSIZ-1 = 15  → 13
    // Idx     : 5 digits   →  5
    // State   : "incomplete"= 10 → 10
    // Flags   : "USMPXOKR" = 8  →  6
    // Cfm(ms) : 10 digits  → 10
    // Used(ms): 10 digits  → 9
    // Upd(ms) : 10 digits  → 9
    // Ref     : 6 digits   →  5
    // Probes  : 6 digits   →  6
    constexpr int cF = 6, cD = 39, cL = 17, cI = 13, cX = 5, cS = 10,
                  cG = 6, cC = 10, cU = 9,  cP = 9,  cR = 5, cQ = 6;

    auto hbar = [](int n) {
        std::string s;
        s.reserve(static_cast<std::size_t>(n) * 3);
        for (int i = 0; i < n; ++i) s += "─";
        return s;
    };

    auto mkBorder = [&](const char* l, const char* j, const char* r) {
        return std::string(l)
             + hbar(cF+2) + j + hbar(cD+2) + j + hbar(cL+2) + j
             + hbar(cI+2) + j + hbar(cX+2) + j + hbar(cS+2) + j
             + hbar(cG+2) + j + hbar(cC+2) + j + hbar(cU+2) + j
             + hbar(cP+2) + j + hbar(cR+2) + j + hbar(cQ+2) + r;
    };

    const std::string top    = mkBorder("┌", "┬", "┐");
    const std::string mid    = mkBorder("├", "┼", "┤");
    const std::string bottom = mkBorder("└", "┴", "┘");

    auto printRow = [&](const std::string& fam,
                        const std::string& dst,
                        const std::string& mac,
                        const std::string& ifc,
                        const std::string& idx,
                        const std::string& sta,
                        const std::string& flg,
                        const std::string& cfm,
                        const std::string& usd,
                        const std::string& upd,
                        const std::string& ref,
                        const std::string& prb)
    {
        std::println(
            "│ {:<{}} │ {:<{}} │ {:<{}} │ {:<{}} │ {:>{}} │ {:<{}} │ {:<{}} │ {:>{}} │ {:>{}} │ {:>{}} │ {:>{}} │ {:>{}} │",
            fam, cF, dst, cD, mac, cL, ifc, cI,
            idx, cX, sta, cS, flg, cG,
            cfm, cC, usd, cU, upd, cP,
            ref, cR, prb, cQ);
    };

    std::println("\n Neighbor Table  ({} entry/entries)", neighbors.size());
    std::println("{}", top);
    printRow("Family", "Destination", "MAC", "Interface",
             "Idx",    "State",       "Flags",
             "Cfm(ms)", "Used(ms)", "Upd(ms)", "Ref", "Probes");

    if (neighbors.empty())
    {
        std::println("{}", bottom);
        return;
    }

    std::println("{}", mid);

    for (const auto& [key, n] : neighbors)
    {
        printRow(
            familyLabel(n.family),
            n.dst.empty() ? "(none)" : n.dst,
            n.mac.empty() ? "(none)" : n.mac,
            n.ifname.empty() ? std::to_string(n.ifindex) : n.ifname,
            std::to_string(n.ifindex),
            nudStateLabel(n.state),
            ntfFlagsLabel(n.flags),
            std::to_string(n.confirmed_ms),
            std::to_string(n.used_ms),
            std::to_string(n.updated_ms),
            std::to_string(n.refcnt),
            std::to_string(n.probes));
    }

    std::println("{}", bottom);
}

/**
 * @brief Serializes the neighbor table to a text snapshot for UDP delivery.
 *
 * Format (one entry per line after the header):
 * @code
 *   NEIGHBORS <count>
 *   family=<f> dst=<ip> mac=<mac> iface=<if> ifidx=<n> state=<s> flags=<f>
 *   …
 * @endcode
 */
static std::string serializeNeighborTable(
    const std::map<std::string, NeighEntry>& neighbors)
{
    std::ostringstream os;
    os << "NEIGHBORS " << neighbors.size() << '\n';
    for (const auto& [key, n] : neighbors)
    {
        os << std::format(
            "family={} dst={} mac={} iface={} ifidx={} state={} flags={}"
            " type={} confirmed_ms={} used_ms={} updated_ms={} refcnt={}"
            " probes={} vlan={} master={} master_name={} nh_ifindex={}"
            " proto={}\n",
            familyLabel(n.family),
            n.dst.empty()    ? "(none)" : n.dst,
            n.mac.empty()    ? "(none)" : n.mac,
            n.ifname.empty() ? std::to_string(n.ifindex) : n.ifname,
            n.ifindex,
            nudStateLabel(n.state),
            ntfFlagsLabel(n.flags),
            static_cast<unsigned>(n.type),
            n.confirmed_ms,
            n.used_ms,
            n.updated_ms,
            n.refcnt,
            n.probes,
            n.vlan,
            n.master,
            n.master_name.empty() ? "-" : n.master_name,
            n.nh_ifindex,
            protoLabel(n.protocol));
    }
    return os.str();
}

/**
 * @brief Context forwarded via user_data to the neighbor netlink callback.
 */
struct NeighCtx
{
    mutable std::shared_mutex         mtx;       ///< Guards neighbors
    std::map<std::string, NeighEntry> neighbors; ///< "family|ifidx|dst" → entry
    sra::UdpTableServer*              udpServer; ///< UDP publisher (port 9001); may be nullptr
};

/**
 * @brief Inserts or removes one neighbor entry from @p ctx.
 *
 * Shared by both the silent populate callback (used during the initial dump)
 * and the display callback (used during the live-event loop).
 */
static void nlNeighUpdate(netlink_neigh_event_t  event,
                          const netlink_neigh_t *n,
                          NeighCtx              *ctx)
{
    const std::string key = familyLabel(n->family)
                          + "|" + std::to_string(n->ifindex)
                          + "|" + (n->dst[0] ? n->dst : "(none)");

    if (event == NETLINK_NEIGH_REMOVED)
    {
        ctx->neighbors.erase(key);
        return;
    }

    NeighEntry e;
    e.family       = n->family;
    e.ifindex      = n->ifindex;
    e.ifname       = n->ifname;
    e.state        = n->state;
    e.flags        = n->flags;
    e.type         = n->type;
    e.dst          = n->dst;
    e.mac          = formatMac(n->lladdr, n->lladdr_len);
    e.confirmed_ms = n->confirmed_ms;
    e.used_ms      = n->used_ms;
    e.updated_ms   = n->updated_ms;
    e.refcnt       = n->refcnt;
    e.probes       = n->probes;
    e.vlan         = n->vlan;
    e.master       = n->master;
    e.master_name  = n->master_name;
    e.nh_ifindex   = n->nh_ifindex;
    e.protocol     = n->protocol;
    ctx->neighbors[key] = e;
}

/**
 * @brief Silent populate callback — updates the in-memory table only.
 *
 * Used during the initial RTM_GETNEIGH dump so the table is not redrawn
 * once per entry.  After the dump completes the caller prints the table once.
 */
static void nlNeighPopulateCb(netlink_neigh_event_t  event,
                               const netlink_neigh_t *n,
                               void                  *user_data)
{
    nlNeighUpdate(event, n, static_cast<NeighCtx*>(user_data));
}

/**
 * @brief Live-event callback — updates the in-memory table and publishes via UDP.
 *
 * Used after the initial dump, for ongoing RTM_NEWNEIGH / RTM_DELNEIGH events.
 * The neighbor table is NOT redrawn on screen; only the NetLink event itself is
 * logged.  The updated snapshot is pushed to all UDP subscribers on port 9001.
 */
static void nlNeighCb(netlink_neigh_event_t     event,
                      const netlink_neigh_t    *n,
                      void                     *user_data)
{
    auto* ctx = static_cast<NeighCtx*>(user_data);

    const char* evLabel = (event == NETLINK_NEIGH_ADDED)   ? "ADDED"
                        : (event == NETLINK_NEIGH_REMOVED) ? "REMOVED"
                                                           : "CHANGED";
    std::println("[Neighbors] {} {} on {} (state={})",
                 evLabel,
                 n->dst[0] ? n->dst : "(no dst)",
                 n->ifname[0] ? n->ifname : std::to_string(n->ifindex).c_str(),
                 nudStateLabel(n->state));

    nlNeighUpdate(event, n, ctx);

    /* Publish updated snapshot to UDP subscribers (port 9001). */
    if (ctx->udpServer)
        ctx->udpServer->setNeighborData(serializeNeighborTable(ctx->neighbors));

    std::cout.flush();
}

/* File-scope fd used by the neighbor signal handler. */
static volatile int g_neigh_fd = -1;

/** @brief SIGINT/SIGTERM handler for the neighbors command.
 *
 *  Closes the neighbors fd AND the startup route/nexthop fds so all
 *  background threads unblock and can be joined. */
static void neighSigHandler(int /*signo*/)
{
    if (g_neigh_fd >= 0)
    {
        netlink_neigh_close(g_neigh_fd);
        g_neigh_fd = -1;
    }
    if (g_startup_route_fd >= 0)
    {
        netlink_close(g_startup_route_fd);
        g_startup_route_fd = -1;
    }
    if (g_startup_nexthop_fd >= 0)
    {
        netlink_nexthop_close(g_startup_nexthop_fd);
        g_startup_nexthop_fd = -1;
    }
}

/**
 * @brief Runs the kernel neighbor table monitor.
 *
 * The @p udpServer is owned by the caller (created at startup and shared
 * across all three monitors).  This function opens its own NETLINK_ROUTE
 * socket, does a full RTM_GETNEIGH dump to populate the table, then enters
 * the live-event loop.  Runs until SIGINT / SIGTERM.
 *
 * @param udpServer  Shared UDP table server (already started by caller).
 * @return EXIT_SUCCESS on clean stop, EXIT_FAILURE if the socket failed.
 */
static int cmdWatchNeighbors(sra::UdpTableServer& udpServer)
{
    std::println("[Neighbors] Opening netlink neighbor monitor…");
    std::println("[Neighbors] Table available via UDP port {}",
                 sra::UDP_PORT_NEIGHBORS);

    int fd = netlink_neigh_init();
    if (fd < 0)
    {
        std::println(std::cerr,
                     "[Neighbors] Error: netlink_neigh_init failed: {}",
                     std::strerror(errno));
        return EXIT_FAILURE;
    }
    g_neigh_fd = fd;

    struct sigaction sa{};
    sa.sa_handler = neighSigHandler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    // ── Step 1: Read the current neighbor table from the kernel ──────────
    std::println("[Neighbors] Reading neighbor table from kernel…");
    NeighCtx ctx;
    ctx.udpServer = &udpServer;
    const int dumped = netlink_neigh_dump(fd, nlNeighPopulateCb, &ctx);
    if (dumped < 0)
    {
        std::println(std::cerr,
                     "[Neighbors] Warning: initial read failed: {}",
                     std::strerror(errno));
    }
    else
    {
        std::println("[Neighbors] Read complete: {} entry/entries.", dumped);
    }

    // ── Step 2: Publish initial snapshot via UDP ─────────────────────────
    udpServer.setNeighborData(serializeNeighborTable(ctx.neighbors));

    // ── Enter live-event loop ─────────────────────────────────────────────
    std::println("\n[Neighbors] Watching for kernel neighbor events…");
    std::println("[Neighbors] (Ctrl-C to stop)\n");
    std::cout.flush();

    netlink_neigh_run(fd, nlNeighCb, &ctx);

    netlink_neigh_close(g_neigh_fd);

    std::println("\n[Neighbors] Stopped.");
    return EXIT_SUCCESS;
}

// ---------------------------------------------------------------------------
// Nexthop monitoring command
// ---------------------------------------------------------------------------

/**
 * @brief Maps an RT_SCOPE_* value to a short display string.
 */
static std::string scopeLabel(uint8_t scope)
{
    switch (scope)
    {
    case RT_SCOPE_UNIVERSE: return "global";
    case RT_SCOPE_SITE:     return "site";
    case RT_SCOPE_LINK:     return "link";
    case RT_SCOPE_HOST:     return "host";
    case RT_SCOPE_NOWHERE:  return "nowhere";
    default:                return std::to_string(static_cast<int>(scope));
    }
}

/**
 * @brief Maps RTNH_F_* nexthop flags to a compact string (e.g. "dead|onlink").
 */
static std::string rtnhFlagsLabel(uint32_t flags)
{
    if (flags == 0)
        return "-";
    std::vector<std::string> parts;
    if (flags & RTNH_F_DEAD)        parts.emplace_back("dead");
    if (flags & RTNH_F_PERVASIVE)   parts.emplace_back("perv");
    if (flags & RTNH_F_ONLINK)      parts.emplace_back("onlink");
    if (flags & RTNH_F_OFFLOAD)     parts.emplace_back("offload");
    if (flags & RTNH_F_LINKDOWN)    parts.emplace_back("down");
    if (flags & RTNH_F_UNRESOLVED)  parts.emplace_back("unresolved");
    if (parts.empty())
        return std::format("0x{:08x}", flags);
    std::string s;
    for (std::size_t i = 0; i < parts.size(); ++i)
    {
        if (i > 0) s += '|';
        s += parts[i];
    }
    return s;
}

/**
 * @brief Formats a nexthop group as "id(w+1),id(w+1),…" (truncated to fit).
 */
static std::string formatNhGroup(const netlink_nexthop_grp_t *grp,
                                 uint32_t                     count,
                                 int                          maxlen)
{
    if (count == 0)
        return "-";
    std::string s;
    for (uint32_t i = 0; i < count; ++i)
    {
        if (i > 0) s += ',';
        s += std::to_string(grp[i].id)
           + '('
           + std::to_string(static_cast<int>(grp[i].weight) + 1)
           + ')';
    }
    if (static_cast<int>(s.size()) > maxlen)
        s = s.substr(0, static_cast<std::size_t>(maxlen - 1)) + "…";
    return s;
}

/**
 * @brief A single row in the dynamic nexthop table.
 *
 * All fields mirror the corresponding @c netlink_nexthop_t members so the
 * table carries the complete nhmsg header and every NHA_* attribute.
 */
struct NexthopEntry
{
    /* ── From struct nhmsg ─────────────────────────────────────────────── */
    uint32_t id{0};           ///< NHA_ID: unique nexthop identifier
    uint8_t  family{0};       ///< nh_family: AF_INET/AF_INET6/AF_UNSPEC
    uint8_t  scope{0};        ///< nh_scope: RT_SCOPE_*
    uint8_t  protocol{0};     ///< nh_protocol: RTPROT_*
    uint32_t flags{0};        ///< nh_flags: RTNH_F_* bitmask
    /* ── NHA_OIF ───────────────────────────────────────────────────────── */
    uint32_t oif{0};          ///< Output interface index (0 = absent)
    std::string oif_name;     ///< Resolved interface name
    /* ── NHA_GATEWAY ───────────────────────────────────────────────────── */
    std::string gateway;      ///< Gateway address string ("" = absent)
    /* ── NHA_BLACKHOLE ─────────────────────────────────────────────────── */
    uint8_t  blackhole{0};    ///< 1 if this is a blackhole nexthop
    /* ── NHA_FDB ───────────────────────────────────────────────────────── */
    uint8_t  fdb{0};          ///< 1 if nexthop is offloaded to FDB
    /* ── NHA_MASTER ────────────────────────────────────────────────────── */
    uint32_t master{0};       ///< Master device index (0 = absent)
    std::string master_name;  ///< Resolved master device name
    /* ── NHA_GROUP ─────────────────────────────────────────────────────── */
    std::string group_str;    ///< Pre-formatted group member list
    uint32_t group_count{0};  ///< Number of members in the nexthop group
    /* ── NHA_GROUP_TYPE ────────────────────────────────────────────────── */
    uint16_t group_type{0};   ///< 0=mpath (ECMP), 1=resilient
    /* ── NHA_ENCAP_TYPE ────────────────────────────────────────────────── */
    uint16_t encap_type{0};   ///< Encapsulation type (LWTUNNEL_ENCAP_*)
};

/**
 * @brief Pretty-prints the dynamic nexthop table using box-drawing lines.
 *
 * Columns mirror every field of netlink_nexthop_t.
 */
static void printNexthopTable(
    const std::map<uint32_t, NexthopEntry>& nexthops)
{
    // ── Column widths ─────────────────────────────────────────────────────
    // ID       : 6 digits      →  6
    // Family   : "inet6" = 5   →  6
    // Scope    : "nowhere" = 7 →  7
    // Protocol : "zebra" = 5   →  8
    // Flags    : "unresolved"=10 → 12
    // OIF      : IFNAMSIZ-1=15 → 13
    // Gateway  : IPv6=39       → 25  (most will be IPv4)
    // Group    : list          → 28
    // GrpType  : "resilient"=9 →  9
    // BH       : "yes/no"=3    →  3
    // FDB      : "yes/no"=3    →  3
    // Master   : IFNAMSIZ-1    → 12
    constexpr int cI = 6, cF = 6, cO = 7, cP = 8, cL = 12, cN = 13,
                  cG = 25, cR = 28, cT = 9, cB = 3, cD = 3, cM = 12;

    auto hbar = [](int n) {
        std::string s;
        s.reserve(static_cast<std::size_t>(n) * 3);
        for (int i = 0; i < n; ++i) s += "─";
        return s;
    };

    auto mkBorder = [&](const char* l, const char* j, const char* r) {
        return std::string(l)
             + hbar(cI+2) + j + hbar(cF+2) + j + hbar(cO+2) + j
             + hbar(cP+2) + j + hbar(cL+2) + j + hbar(cN+2) + j
             + hbar(cG+2) + j + hbar(cR+2) + j + hbar(cT+2) + j
             + hbar(cB+2) + j + hbar(cD+2) + j + hbar(cM+2) + r;
    };

    const std::string top    = mkBorder("┌", "┬", "┐");
    const std::string mid    = mkBorder("├", "┼", "┤");
    const std::string bottom = mkBorder("└", "┴", "┘");

    auto printRow = [&](const std::string& id,
                        const std::string& fam,
                        const std::string& scp,
                        const std::string& pro,
                        const std::string& flg,
                        const std::string& oif,
                        const std::string& gw,
                        const std::string& grp,
                        const std::string& gtp,
                        const std::string& bh,
                        const std::string& fdb,
                        const std::string& mst)
    {
        std::println(
            "│ {:>{}} │ {:<{}} │ {:<{}} │ {:<{}} │ {:<{}} │ {:<{}} │ {:<{}} │ {:<{}} │ {:<{}} │ {:<{}} │ {:<{}} │ {:<{}} │",
            id,  cI, fam, cF, scp, cO, pro, cP,
            flg, cL, oif, cN, gw,  cG,
            grp, cR, gtp, cT, bh,  cB, fdb, cD, mst, cM);
    };

    std::println("\n Nexthop Table  ({} entry/entries)", nexthops.size());
    std::println("{}", top);
    printRow("ID", "Family", "Scope", "Protocol",
             "Flags", "OIF", "Gateway",
             "Group", "GrpType", "BH", "FDB", "Master");

    if (nexthops.empty())
    {
        std::println("{}", bottom);
        return;
    }

    std::println("{}", mid);

    for (const auto& [id, nh] : nexthops)
    {
        const std::string oif = nh.oif_name.empty()
                                    ? (nh.oif ? std::to_string(nh.oif) : "-")
                                    : nh.oif_name;
        const std::string mst = nh.master_name.empty()
                                    ? (nh.master ? std::to_string(nh.master) : "-")
                                    : nh.master_name;
        const std::string gtp = nh.group_type == 0
                                    ? (nh.group_str == "-" ? "-" : "mpath")
                                    : "resilient";
        printRow(
            std::to_string(nh.id),
            familyLabel(nh.family),
            scopeLabel(nh.scope),
            protoLabel(nh.protocol),
            rtnhFlagsLabel(nh.flags),
            oif,
            nh.gateway.empty() ? "-" : nh.gateway,
            nh.group_str,
            gtp,
            nh.blackhole ? "yes" : "no",
            nh.fdb       ? "yes" : "no",
            mst);
    }

    std::println("{}", bottom);
}

/**
 * @brief Serializes the nexthop table to a text snapshot for UDP delivery.
 *
 * Format (one entry per line after the header):
 * @code
 *   NEXTHOPS <count>
 *   id=<n> family=<f> scope=<s> proto=<p> flags=<fl> oif=<if> gw=<gw> group=<g> bh=<b> fdb=<f> master=<m>
 *   …
 * @endcode
 */
static std::string serializeNexthopTable(
    const std::map<uint32_t, NexthopEntry>& nexthops)
{
    std::ostringstream os;
    os << "NEXTHOPS " << nexthops.size() << '\n';
    for (const auto& [id, nh] : nexthops)
    {
        const std::string oif = nh.oif_name.empty()
            ? (nh.oif ? std::to_string(nh.oif) : "-")
            : nh.oif_name;
        const std::string mst = nh.master_name.empty()
            ? (nh.master ? std::to_string(nh.master) : "-")
            : nh.master_name;
        const std::string gtp = nh.group_type == 0
            ? (nh.group_str == "-" ? "-" : "mpath")
            : "resilient";
        os << std::format(
            "id={} family={} scope={} proto={} flags={} oif={} gw={}"
            " group={} group_count={} group_type={} encap_type={}"
            " bh={} fdb={} master={}\n",
            nh.id,
            familyLabel(nh.family),
            scopeLabel(nh.scope),
            protoLabel(nh.protocol),
            rtnhFlagsLabel(nh.flags),
            oif,
            nh.gateway.empty() ? "-" : nh.gateway,
            nh.group_str,
            nh.group_count,
            gtp,
            nh.encap_type,
            nh.blackhole ? "yes" : "no",
            nh.fdb       ? "yes" : "no",
            mst);
    }
    return os.str();
}

/**
 * @brief Context forwarded via user_data to the nexthop netlink callback.
 */
struct NexthopCtx
{
    std::map<uint32_t, NexthopEntry> nexthops;             ///< nexthop ID → entry
    sra::UdpTableServer*             udpServer{nullptr};   ///< UDP publisher (port 9002)
    sra::VrfTable*                   vrfTable{nullptr};    ///< VRF table from GetAllRoutes
    sra::VrfUdpClient*               vrfClient{nullptr};   ///< Unix-domain client to ud_server
    NeighCtx*                        neighCtx{nullptr};    ///< adjacency table (shared with neigh thread)
};

/**
 * @brief Inserts or removes one nexthop entry from @p ctx.
 *
 * Shared by both the silent populate callback (initial dump) and the
 * display callback (live-event loop).
 */
static void nlNexthopUpdate(netlink_nexthop_event_t  event,
                             const netlink_nexthop_t *nh,
                             NexthopCtx              *ctx)
{
    if (event == NETLINK_NEXTHOP_REMOVED)
    {
        ctx->nexthops.erase(nh->id);
        return;
    }

    NexthopEntry e;
    e.id          = nh->id;
    e.family      = nh->family;
    e.scope       = nh->scope;
    e.protocol    = nh->protocol;
    e.flags       = nh->flags;
    e.oif         = nh->oif;
    e.oif_name    = nh->oif_name;
    e.gateway     = nh->gateway;
    e.blackhole   = nh->blackhole;
    e.fdb         = nh->fdb;
    e.master      = nh->master;
    e.master_name = nh->master_name;
    e.group_str   = formatNhGroup(nh->group, nh->group_count, 28);
    e.group_count = nh->group_count;
    e.group_type  = nh->group_type;
    e.encap_type  = nh->encap_type;
    ctx->nexthops[nh->id] = e;
}

/**
 * @brief Silent populate callback — updates the in-memory table only.
 *
 * Used during the initial RTM_GETNEXTHOP dump so the table is not redrawn
 * once per object.  After the dump completes the caller prints the table once.
 */
static void nlNexthopPopulateCb(netlink_nexthop_event_t  event,
                                 const netlink_nexthop_t *nh,
                                 void                    *user_data)
{
    nlNexthopUpdate(event, nh, static_cast<NexthopCtx*>(user_data));
}

/**
 * @brief Live-event callback — updates the in-memory table and publishes via UDP.
 *
 * Used after the initial dump, for ongoing RTM_NEWNEXTHOP / RTM_DELNEXTHOP
 * events.  The nexthop table is NOT redrawn on screen; only the NetLink event
 * itself is logged.  The updated snapshot is pushed to UDP subscribers on
 * port 9002.
 */
static void nlNexthopCb(netlink_nexthop_event_t   event,
                        const netlink_nexthop_t  *nh,
                        void                     *user_data)
{
    auto* ctx = static_cast<NexthopCtx*>(user_data);

    const char* evLabel = (event == NETLINK_NEXTHOP_ADDED)   ? "ADDED"
                        : (event == NETLINK_NEXTHOP_REMOVED) ? "REMOVED"
                                                             : "CHANGED";
    std::println("[Nexthops] {} id={} gw={} oif={}",
                 evLabel,
                 nh->id,
                 nh->gateway[0] ? nh->gateway : "-",
                 nh->oif_name[0] ? nh->oif_name : std::to_string(nh->oif).c_str());

    nlNexthopUpdate(event, nh, ctx);

    /* Publish updated snapshot to UDP subscribers (port 9002). */
    if (ctx->udpServer)
        ctx->udpServer->setNexthopData(serializeNexthopTable(ctx->nexthops));

    std::cout.flush();
}

/* File-scope fd used by the nexthop signal handler. */
static volatile int g_nexthop_fd = -1;

/** @brief SIGINT/SIGTERM handler for the nexthops command.
 *
 *  Closes the nexthops fd AND the startup route/neigh fds so all
 *  background threads unblock and can be joined. */
static void nexthopSigHandler(int /*signo*/)
{
    if (g_nexthop_fd >= 0)
    {
        netlink_nexthop_close(g_nexthop_fd);
        g_nexthop_fd = -1;
    }
    if (g_startup_route_fd >= 0)
    {
        netlink_close(g_startup_route_fd);
        g_startup_route_fd = -1;
    }
    if (g_startup_neigh_fd >= 0)
    {
        netlink_neigh_close(g_startup_neigh_fd);
        g_startup_neigh_fd = -1;
    }
}

/**
 * @brief Runs the kernel nexthop object monitor.
 *
 * The @p udpServer is owned by the caller (created at startup and shared
 * across all three monitors).  This function opens its own socket, does a
 * full RTM_GETNEXTHOP dump to populate the table, then enters the live-event
 * loop.  Requires Linux 5.3+.  Runs until SIGINT / SIGTERM.
 *
 * @param udpServer  Shared UDP table server (already started by caller).
 * @return EXIT_SUCCESS on clean stop, EXIT_FAILURE if the socket failed.
 */
static int cmdWatchNexthops(sra::UdpTableServer& udpServer)
{
    std::println("[Nexthops] Opening netlink nexthop monitor…");
    std::println("[Nexthops] Table available via UDP port {}",
                 sra::UDP_PORT_NEXTHOPS);

    int fd = netlink_nexthop_init();
    if (fd < 0)
    {
        std::println(std::cerr,
                     "[Nexthops] Error: netlink_nexthop_init failed: {}",
                     std::strerror(errno));
        return EXIT_FAILURE;
    }
    g_nexthop_fd = fd;

    struct sigaction sa{};
    sa.sa_handler = nexthopSigHandler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    // ── Step 1: Read all nexthop objects from the kernel ─────────────────
    std::println("[Nexthops] Reading nexthop objects from kernel…");
    NexthopCtx ctx;
    ctx.udpServer = &udpServer;
    const int dumped = netlink_nexthop_dump(fd, nlNexthopPopulateCb, &ctx);
    if (dumped < 0)
    {
        std::println(std::cerr,
                     "[Nexthops] Warning: dump failed: {} "
                     "(kernel may not support nexthop objects; requires 5.3+)",
                     std::strerror(errno));
    }
    else
    {
        std::println("[Nexthops] Read complete: {} object(s).", dumped);
    }

    // ── Step 2: Publish initial snapshot via UDP ──────────────────────────
    udpServer.setNexthopData(serializeNexthopTable(ctx.nexthops));

    // ── Step 3: Watch for live nexthop events ────────────────────────────
    std::println("\n[Nexthops] Watching for kernel nexthop events…");
    std::println("[Nexthops] (Ctrl-C to stop)\n");
    std::cout.flush();

    netlink_nexthop_run(fd, nlNexthopCb, &ctx);

    netlink_nexthop_close(g_nexthop_fd);

    std::println("\n[Nexthops] Stopped.");
    return EXIT_SUCCESS;
}

// ---------------------------------------------------------------------------
// grpc-proc-demo command
// ---------------------------------------------------------------------------

/**
 * @brief Thread-safe queue used to pass request payloads from the producer
 *        thread to the main dispatch loop.
 */
struct DemoRequestQueue
{
    std::queue<sra::RequestPayload> items;   ///< Pending payloads.
    std::mutex                      mutex;   ///< Guards @c items.
    std::condition_variable         cv;      ///< Notified when an item is pushed.
};

/**
 * @brief Demonstrates GrpcProc with a background producer and a non-blocking
 *        poll loop in main.
 *
 * Architecture:
 *  - **Producer thread** – enqueues a @ref sra::GetLoopbackParams every 5 s.
 *  - **Main loop** – dequeues payloads, submits them to @ref sra::GrpcProc,
 *    and polls for completed responses using @c tryGetResponse (non-blocking,
 *    as in @c exampleNonBlockingPoll).
 *
 * Runs until SIGINT / SIGTERM is received.
 *
 * @param client  Connected @c RouteClient.
 * @return @c EXIT_SUCCESS on clean stop.
 */
static int cmdGrpcProcDemo(sra::RouteClient& client)
{
    using namespace std::chrono_literals;

    /* Install signal handlers. */
    struct sigaction sa{};
    sa.sa_handler = demoSigHandler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    std::println("grpc-proc-demo: running (Ctrl-C to stop)");

    // ── Shared intermediate request queue ────────────────────────────────
    DemoRequestQueue demoQueue;

    // ── Producer thread ───────────────────────────────────────────────────
    // Creates a new GetLoopback request every 5 seconds and places it in
    // the queue.  The main loop drains the queue and forwards to GrpcProc.
    std::thread producer([&demoQueue]
    {
        uint64_t seq = 0;
        while (!g_demo_stop)
        {
            {
                std::lock_guard<std::mutex> lock(demoQueue.mutex);
                demoQueue.items.push(sra::GetLoopbackParams{});
                ++seq;
                std::println("  [producer] Enqueued GetLoopback request #{}", seq);
            }
            demoQueue.cv.notify_one();

            // Sleep 5 s in 100 ms steps so that the stop flag is checked
            // promptly on shutdown.
            for (int i = 0; i < 50 && !g_demo_stop; ++i)
            {
                std::this_thread::sleep_for(100ms);
            }
        }
    });

    // ── Start the grpc_proc thread ────────────────────────────────────────
    sra::GrpcProc proc(client, /*autoStart=*/true);
    std::println("  grpc_proc thread started.");

    // IDs of requests submitted to GrpcProc but not yet retrieved.
    std::vector<uint64_t> inFlight;

    // ── Main infinite loop ────────────────────────────────────────────────
    while (!g_demo_stop)
    {
        // 1. Drain the intermediate queue: submit every pending payload to
        //    GrpcProc and record the assigned ID for later polling.
        {
            std::unique_lock<std::mutex> lock(demoQueue.mutex);
            while (!demoQueue.items.empty())
            {
                sra::RequestPayload payload = std::move(demoQueue.items.front());
                demoQueue.items.pop();
                lock.unlock(); // release while submitting to avoid holding during RPC queue op

                const uint64_t id = proc.submit(std::move(payload));
                inFlight.push_back(id);
                std::println("  [main] Submitted request id={}", id);

                lock.lock(); // re-acquire before checking queue again
            }
        }

        // 2. Non-blocking poll for completed responses (mirrors
        //    exampleNonBlockingPoll).  Each completed response is printed
        //    and removed from the in-flight list.
        for (auto it = inFlight.begin(); it != inFlight.end(); )
        {
            auto resp = proc.tryGetResponse(*it);
            if (resp)
            {
                std::visit(
                    [](const auto& result)
                    {
                        using T = std::decay_t<decltype(result)>;
                        if constexpr (std::is_same_v<T, sra::GetLoopbackResult>)
                        {
                            if (result)
                            {
                                std::println("  [main] GetLoopback OK  → \"{}\"",
                                             *result);
                            }
                            else
                            {
                                std::println("  [main] GetLoopback ERR → {}",
                                             result.error());
                            }
                        }
                    },
                    resp->payload);

                it = inFlight.erase(it);
            }
            else
            {
                ++it;
            }
        }

        std::this_thread::sleep_for(5ms);
    }

    // ── Graceful shutdown ─────────────────────────────────────────────────
    demoQueue.cv.notify_all(); // unblock producer if waiting
    producer.join();
    proc.stop();

    std::println("\ngrpc-proc-demo: stopped.");
    return EXIT_SUCCESS;
}

// ---------------------------------------------------------------------------
// Startup (background) netlink monitoring
// ---------------------------------------------------------------------------

/**
 * @brief Context for the startup route background monitor.
 *
 * Unlike @c WatchCtx this context has no gRPC client; it only maintains the
 * in-memory route table and publishes snapshots to the shared UDP server.
 */
struct StartupRouteCtx
{
    std::map<std::string, WatchRoute> routes; ///< "dest|gw|iface" → WatchRoute
    sra::UdpTableServer*              udpServer{nullptr};
};

/**
 * @brief Silent live-route callback for the startup background thread.
 *
 * Updates the in-memory route table and publishes to UDP without any
 * console output (so it does not interfere with other command output).
 */
static void startupRouteLiveCb(netlink_event_t          event,
                                const netlink_route32_t *route,
                                void                    *user_data)
{
    auto* ctx = static_cast<StartupRouteCtx*>(user_data);

    char dst_buf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &route->dst, dst_buf, sizeof(dst_buf));
    const std::string dest = std::string(dst_buf) + "/"
                           + std::to_string(static_cast<unsigned>(route->dst_len));

    char gw_buf[INET_ADDRSTRLEN] = {};
    if (route->gateway.s_addr != 0)
        inet_ntop(AF_INET, &route->gateway, gw_buf, sizeof(gw_buf));
    const std::string gw    = gw_buf;
    const std::string iface = route->ifname;
    const std::string key   = dest + "|" + gw + "|" + iface;

    if (event == NETLINK_ROUTE_REMOVED)
    {
        ctx->routes.erase(key);
    }
    else
    {
        ctx->routes[key] = WatchRoute{dest, gw, iface,
                                      route->ifindex,
                                      route->metric, route->table,
                                      route->protocol, {},
                                      route->family, route->dst_len,
                                      route->tos, route->scope,
                                      route->type, route->flags};
    }

    if (ctx->udpServer)
        ctx->udpServer->setRouteData(serializeRouteTable(ctx->routes));
}

/**
 * @brief Silent live-neighbor callback for the startup background thread.
 *
 * Updates the in-memory neighbor table and publishes to UDP without printing.
 */
static void startupNeighLiveCb(netlink_neigh_event_t  event,
                                const netlink_neigh_t *n,
                                void                  *user_data)
{
    auto* ctx = static_cast<NeighCtx*>(user_data);
    std::string snapshot;
    {
        std::unique_lock lock(ctx->mtx);
        nlNeighUpdate(event, n, ctx);
        if (ctx->udpServer)
            snapshot = serializeNeighborTable(ctx->neighbors);
    }
    if (ctx->udpServer && !snapshot.empty())
        ctx->udpServer->setNeighborData(std::move(snapshot));
}

/**
 * @brief Returns true if @p ip appears as a destination in the adjacency table.
 *
 * The caller must NOT hold ctx.mtx; this function acquires a shared lock
 * itself so it is safe to call from any thread.
 */
static bool neighborHasIp(NeighCtx& ctx, const std::string& ip)
{
    std::shared_lock lock(ctx.mtx);
    for (const auto& [key, entry] : ctx.neighbors)
        if (entry.dst == ip)
            return true;
    return false;
}

/** @brief One's-complement checksum used in ICMP headers. */
static uint16_t icmpChecksum(const void* data, std::size_t len)
{
    const auto* p = static_cast<const uint16_t*>(data);
    uint32_t sum = 0;
    while (len > 1) { sum += *p++; len -= 2; }
    if (len) sum += *reinterpret_cast<const uint8_t*>(p);
    sum  = (sum >> 16) + (sum & 0xffff);
    sum += (sum >> 16);
    return static_cast<uint16_t>(~sum);
}

/**
 * @brief Sends a raw ICMP Echo Request to @p destIp.
 *
 * This probes the local ARP/NDP layer so that the nexthop IP gets resolved
 * into the adjacency table even before the first data packet arrives.
 */
static void sendIcmpEchoRequest(const std::string& destIp)
{
    int sock = ::socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (sock < 0)
    {
        std::println(std::cerr, "[ICMP] socket() failed for {}: {}",
                     destIp, ::strerror(errno));
        return;
    }

    struct icmphdr pkt{};
    pkt.type             = ICMP_ECHO;
    pkt.code             = 0;
    pkt.un.echo.id       = ::htons(static_cast<uint16_t>(::getpid() & 0xffff));
    pkt.un.echo.sequence = ::htons(1);
    pkt.checksum         = icmpChecksum(&pkt, sizeof(pkt));

    struct sockaddr_in dst{};
    dst.sin_family = AF_INET;
    ::inet_pton(AF_INET, destIp.c_str(), &dst.sin_addr);

    if (::sendto(sock, &pkt, sizeof(pkt), 0,
                 reinterpret_cast<const sockaddr*>(&dst), sizeof(dst)) < 0)
    {
        std::println(std::cerr, "[ICMP] sendto() to {} failed: {}",
                     destIp, ::strerror(errno));
    }
    else
    {
        std::println("[ICMP] sent echo request to {} to probe adjacency", destIp);
    }
    ::close(sock);
}

/**
 * @brief Checks whether @p nh exists in the VRF table and, if so, builds and
 *        submits a SingleRouteRequest (SINGLE_ROUTE, type=1) to ud_server.
 *
 * Called on every nexthop ADDED / CHANGED / REMOVED event.  Only nni
 * interface entries that carry a matching nexthop gateway are included in the
 * request.  Prefix data is taken exclusively from the VRF table; the nexthop
 * ID is taken directly from the kernel event.
 *
 * @param nh         Kernel nexthop descriptor from the netlink event.
 * @param vrfTable   Local VRF table populated from GetAllRoutes.  May be null.
 * @param vrfClient  Unix-domain client used to submit the request.  May be null.
 * @param neighCtx   Adjacency table shared with the neighbor monitor thread.  May be null.
 */
static void sendVrfRouteForNexthop(const netlink_nexthop_t* nh,
                                   sra::VrfTable*           vrfTable,
                                   sra::VrfUdpClient*       vrfClient,
                                   NeighCtx*                neighCtx)
{
    if (!vrfTable || !vrfClient)
        return;

    // Only unicast nexthops with a valid gateway address are relevant.
    if (!nh->gateway[0])
        return;

    const std::string gateway = nh->gateway;

    if (!vrfTable->hasNexthop(gateway))
    {
        std::println("[Nexthops] nexthop id={} gw={} not found in VRF table — skip",
                     nh->id, gateway);
        return;
    }

    // If the nexthop is not yet in the adjacency table, send an ICMP echo
    // request to trigger ARP/NDP resolution before the route is installed.
    if (neighCtx && !neighborHasIp(*neighCtx, gateway))
    {
        std::println("[Nexthops] nexthop gw={} id={} absent from adjacency table — "
                     "sending ICMP echo request", gateway, nh->id);
        sendIcmpEchoRequest(gateway);
    }

    const auto routes = vrfTable->findByNexthop(gateway);
    std::println("[Nexthops] nexthop id={} gw={} matched {} entry/entries; "
                 "generating SingleRouteRequest (SINGLE_ROUTE, type=1)",
                 nh->id, gateway, routes.size());

    // Parse gateway to binary (network byte order).
    struct in_addr nhAddr{};
    if (::inet_pton(AF_INET, gateway.c_str(), &nhAddr) != 1)
    {
        std::println(std::cerr,
                     "[Nexthops] invalid gateway address '{}' — skip", gateway);
        return;
    }
    const auto* nhBytes =
        reinterpret_cast<const std::uint8_t*>(&nhAddr.s_addr);

    cmdproto::SingleRouteRequest req;

    for (const auto& route : routes)
    {
        // Only nni interfaces carry nexthop-based routes.
        if (route.interface_type() != "nni")
            continue;
        if (route.nexthop().empty())
            continue;

        cmdproto::Interface iface{};

        // Null-padded fixed-width interface name.
        const std::string& ifn = route.interface_name();
        for (std::size_t k = 0;
             k < cmdproto::IFACE_NAME_SIZE && k < ifn.size(); ++k)
        {
            iface.iface_name[k] = ifn[k];
        }

        iface.nexthop_addr_ipv4 = {nhBytes[0], nhBytes[1],
                                   nhBytes[2], nhBytes[3]};
        iface.nexthop_id_ipv4   = nh->id;

        // Convert each prefix string "A.B.C.D/N" to binary PrefixIpv4.
        for (const auto& pfx : route.prefixes())
        {
            const std::string& pfxStr = pfx.prefix();
            const auto slash = pfxStr.rfind('/');
            if (slash == std::string::npos)
                continue;
            const std::string addrStr = pfxStr.substr(0, slash);
            const auto maskLen = static_cast<std::uint8_t>(
                std::stoul(pfxStr.substr(slash + 1)));

            struct in_addr pfxAddr{};
            if (::inet_pton(AF_INET, addrStr.c_str(), &pfxAddr) != 1)
                continue;
            const auto* pfxBytes =
                reinterpret_cast<const std::uint8_t*>(&pfxAddr.s_addr);

            iface.prefixes.push_back(cmdproto::PrefixIpv4{
                {pfxBytes[0], pfxBytes[1], pfxBytes[2], pfxBytes[3]},
                maskLen});
        }

        std::println("[Nexthops]   iface='{}' nexthop='{}' "
                     "nexthop_id={} prefixes={}",
                     route.interface_name(), gateway, nh->id,
                     iface.prefixes.size());

        req.interfaces.push_back(std::move(iface));
    }

    if (req.interfaces.empty())
    {
        std::println("[Nexthops] no nni entries for nexthop gw={} id={} — skip",
                     gateway, nh->id);
        return;
    }

    std::println("[Nexthops] submitting SingleRouteRequest "
                 "({} interface(s)) to ud_server for nexthop gw={} id={}",
                 req.interfaces.size(), gateway, nh->id);

    vrfClient->submit(std::move(req));
}

/**
 * @brief Silent live-nexthop callback for the startup background thread.
 *
 * Updates the in-memory nexthop table, publishes the snapshot to UDP
 * subscribers, and — when the VRF table and VrfUdpClient are wired up —
 * checks whether the changed nexthop appears in the VRF table and sends a
 * SingleRoute request to ud_server.
 */
static void startupNexthopLiveCb(netlink_nexthop_event_t  event,
                                  const netlink_nexthop_t *nh,
                                  void                    *user_data)
{
    auto* ctx = static_cast<NexthopCtx*>(user_data);

    const char* evLabel = (event == NETLINK_NEXTHOP_ADDED)   ? "ADDED"
                        : (event == NETLINK_NEXTHOP_REMOVED) ? "REMOVED"
                                                             : "CHANGED";
    std::println("[Nexthops] {} id={} gw={} oif={} — checking VRF table",
                 evLabel,
                 nh->id,
                 nh->gateway[0] ? nh->gateway : "-",
                 nh->oif_name[0] ? nh->oif_name
                                 : std::to_string(nh->oif).c_str());

    // Update in-memory nexthop table first so ctx->nexthops is current.
    nlNexthopUpdate(event, nh, ctx);

    if (ctx->udpServer)
        ctx->udpServer->setNexthopData(serializeNexthopTable(ctx->nexthops));

    // For every event (ADDED / CHANGED / REMOVED) check the VRF table and
    // re-apply routes if this nexthop is referenced.
    sendVrfRouteForNexthop(nh, ctx->vrfTable, ctx->vrfClient, ctx->neighCtx);
}

/**
 * @brief SIGINT/SIGTERM handler for commands that don't install their own.
 *
 * Closes all three startup monitoring fds so background threads unblock.
 */
static void startupSigHandler(int /*signo*/)
{
    if (g_startup_route_fd >= 0)
    {
        netlink_close(g_startup_route_fd);
        g_startup_route_fd = -1;
    }
    if (g_startup_neigh_fd >= 0)
    {
        netlink_neigh_close(g_startup_neigh_fd);
        g_startup_neigh_fd = -1;
    }
    if (g_startup_nexthop_fd >= 0)
    {
        netlink_nexthop_close(g_startup_nexthop_fd);
        g_startup_nexthop_fd = -1;
    }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

/**
 * @brief sra entry point.
 *
 * @param argc Argument count.
 * @param argv Argument vector.
 * @return EXIT_SUCCESS or EXIT_FAILURE.
 */
int main(int argc, char* argv[])
{
    /* Tie printf (used by nl_log_ospf in netlink.c) and std::cout to the
     * same buffer so their output does not interleave on the same line. */
    std::ios::sync_with_stdio(true);

    // -----------------------------------------------------------------------
    // Global options
    // -----------------------------------------------------------------------
    po::options_description global("sra – Switch Route Application");
    // clang-format off
    global.add_options()
        ("help,h",   "Print help and exit")
        ("version,v","Print version and exit")
        ("config,c",
            po::value<std::string>()->default_value("config/config.json"),
            "Path to client JSON config file")
        ("server,s",
            po::value<std::string>()->default_value(std::string{}),
            "srmd server address  [host:port]  (overrides config file)")
        ("switches,w",
            po::value<std::string>()->default_value(std::string{}),
            "Path to switch_config.json  (multi-server mode)")
        ("sot",
            po::value<std::string>()->default_value(std::string{}),
            "Path to route_sot_v2.json  (Source-of-Truth; parsed before any"
            " gRPC connection is opened)")
        ("node-ip",
            po::value<std::string>()->default_value(std::string{}),
            "Management IPv4 of the node to look up in the SOT"
            "  (single-server sync mode)")
        ("timeout,t",
            po::value<int>()->default_value(0),
            "Per-RPC timeout in seconds  (overrides config file; 0 = use config)")
        ("tls",      "Use TLS for the gRPC channel  (overrides config file)")
        ("ca-cert",
            po::value<std::string>()->default_value(std::string{}),
            "Path to PEM CA certificate for TLS")
        ("logstream",
            po::value<std::string>()->default_value("stderr"),
            "unix_domain layer log destination: \"stderr\" (default),"
            " \"stdout\", absolute file path, or \"\" to disable")
        ("loglevel",
            po::value<std::string>()->default_value("1"),
            "Minimum log level for unix_domain layer:"
            " DEBUG|1 (raw socket bytes + hex dumps),"
            " INFO|2 (decoded summaries),"
            " NOTICE|3, WARNING|4, ERR|5  [default: 1]")
        ("command",  po::value<std::string>(),
            "Command: test | sync | echo | add | remove | get | list | watch"
            " | neighbors | nexthops"
            " | set-loopback | get-loopback | get-loopbacks | grpc-proc-demo"
            " | vrf-route-add | run")
        ("args",     po::value<std::vector<std::string>>(),
            "Command arguments");
    // clang-format on

    po::positional_options_description pos;
    pos.add("command", 1);
    pos.add("args", -1);

    po::variables_map vm;
    try
    {
        po::store(po::command_line_parser(argc, argv)
                      .options(global)
                      .positional(pos)
                      .allow_unregistered()
                      .run(),
                  vm);
        po::notify(vm);
    }
    catch (const po::error& ex)
    {
        std::cerr << "Error: " << ex.what() << "\n\n" << global << '\n';
        return EXIT_FAILURE;
    }

    // Initialise the unix_domain protocol-layer logger as early as possible so
    // that all logger::log / logger::log_hex calls (including those in
    // vrf_udp_client, cmd_proto, etc.) are active for the lifetime of the run.
    {
        const std::string& lvlStr = vm["loglevel"].as<std::string>();
        int lvl = logger::DEBUG;
        if      (lvlStr == "DEBUG"   || lvlStr == "1") lvl = logger::DEBUG;
        else if (lvlStr == "INFO"    || lvlStr == "2") lvl = logger::INFO;
        else if (lvlStr == "NOTICE"  || lvlStr == "3") lvl = logger::NOTICE;
        else if (lvlStr == "WARNING" || lvlStr == "4") lvl = logger::WARNING;
        else if (lvlStr == "ERR"     || lvlStr == "5") lvl = logger::ERR;
        else {
            try { lvl = std::stoi(lvlStr); } catch (...) {}
        }
        const std::string& logstream = vm["logstream"].as<std::string>();
        logger::init(logstream, lvl);
        logger::log(logger::DEBUG, "sra",
                    std::format("logger active: stream='{}' level={}", logstream, lvl));
    }

    if (vm.count("help") || !vm.count("command"))
    {
        std::println("sra – Switch Route Application");
        std::println("Usage: sra [options] <command> [args...]");
        std::println("");
        std::println("Config file (config/config.json) sets server host/port,");
        std::println("TLS, and timeout defaults.  CLI flags override them.");
        std::println("");
        std::println("Commands:");
        std::println(
            "  test                    Full Echo+CRUD round-trip test");
        std::println("  sync                    Push SOT routes to server(s)");
        std::println("  echo   <message>        Send Echo RPC");
        std::println("  add    <dest> [gw] [iface] [metric]  Add route");
        std::println("  remove <id>             Remove route by ID");
        std::println("  get    <id>             Get route by ID");
        std::println("  list   [--active]       List routes");
        std::println("  watch                   Listen for kernel /32 route "
                     "events and forward to srmd");
        std::println("  neighbors               Dump and watch kernel neighbor "
                     "(ARP/NDP) table — no gRPC required");
        std::println("  nexthops                Dump and watch kernel nexthop "
                     "objects (Linux 5.3+) — no gRPC required");
        std::println("  set-loopback <address>  Store a loopback address on the server");
        std::println("  get-loopback            Retrieve the stored loopback address");
        std::println("  get-loopbacks <loopback>  Query SOT interface list for a loopback (IPv4 or IPv6)");
        std::println("  grpc-proc-demo          Run async GrpcProc demo with periodic GetLoopback requests");
        std::println("  vrf-route-add [socket]  Query srmd via gRPC then send a SingleRouteRequest");
        std::println("                          to the ud_server Unix-domain socket (one-shot)");
        std::println("                          (default socket: /tmp/ud_server.sock)");
        std::println("  run [socket]            Full SRA daemon mode (continuous):");
        std::println("                            1. RequestLoopback → SOT auth check via srmd");
        std::println("                            2. GetLoopbacks / GetAllRoutes via srmd");
        std::println("                            3. Build SingleRouteRequest (nni interfaces only)");
        std::println("                            4. Send to ud_server via Unix-domain socket");
        std::println("                            5. Loop until SIGINT/SIGTERM");
        std::println("                          ud_server socket default: /tmp/ud_server.sock");
        std::println("                          NOTE: ud_server is a separate process; srmd has");
        std::println("                                no Unix-domain socket — gRPC only.");
        std::println("");
        std::cout << global << '\n';
        return vm.count("help") ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    if (vm.count("version"))
    {
        std::println("sra (Switch Route Application) {}",
                     rtsrv::build::kProjectVersion);
        std::println("  Build : #{} ({})",
                     rtsrv::build::kBuildNumber,
                     rtsrv::build::kBuildTimestamp);
        std::println("  Binary: {}", rtsrv::build::kVersionedBinaryName);
        return EXIT_SUCCESS;
    }

    // -----------------------------------------------------------------------
    // Load client config file; CLI flags override individual fields
    // -----------------------------------------------------------------------
    const std::string configPath = vm["config"].as<std::string>();
    common::ClientConfig clientCfg;
    {
        auto cfgResult = common::loadClientConfig(configPath);
        if (cfgResult)
        {
            clientCfg = std::move(*cfgResult);
        }
        else
        {
            // Non-fatal: warn and continue with defaults
            std::println(std::cerr,
                         "Warning: could not load config '{}': {} "
                         "(using defaults)",
                         configPath,
                         cfgResult.error());
        }
    }

    // CLI --server overrides config host+port when provided
    const std::string serverCli = vm["server"].as<std::string>();
    const std::string server =
        serverCli.empty() ? clientCfg.target() : serverCli;

    const std::string switchesPath = vm["switches"].as<std::string>();
    const std::string sotPath = vm["sot"].as<std::string>();
    const std::string nodeIp = vm["node-ip"].as<std::string>();

    // CLI --timeout overrides config when non-zero
    const int timeoutCli = vm["timeout"].as<int>();
    const int timeout = (timeoutCli > 0) ? timeoutCli : clientCfg.timeout_seconds;

    // CLI --tls overrides config
    const bool useTls = (vm.count("tls") > 0) || clientCfg.tls_enabled;

    // CLI --ca-cert overrides config
    const std::string caCertCli = vm["ca-cert"].as<std::string>();
    const std::string caCert = caCertCli.empty() ? clientCfg.ca_cert : caCertCli;

    const std::string command = vm["command"].as<std::string>();

    const std::vector<std::string> args =
        vm.count("args") ? vm["args"].as<std::vector<std::string>>()
                         : std::vector<std::string>{};

    // -----------------------------------------------------------------------
    // "sync" command – parse SOT *before* opening any gRPC connections
    // -----------------------------------------------------------------------
    if (command == "sync")
    {
        if (sotPath.empty())
        {
            std::println(std::cerr, "Error: 'sync' requires --sot <path>");
            return EXIT_FAILURE;
        }

        // Parse the SOT file first – no network connections yet
        std::println("sra  build #{}  Parsing SOT: {}",
                     rtsrv::build::kBuildNumber,
                     sotPath);

        auto sotResult = sra::loadSotConfig(sotPath);
        if (!sotResult)
        {
            std::println(
                std::cerr, "Error loading SOT config: {}", sotResult.error());
            return EXIT_FAILURE;
        }

        // Print what was loaded so the operator can verify before pushing
        printSotSummary(*sotResult);

        // Multi-server sync via switch_config.json
        if (!switchesPath.empty())
        {
            auto cfgResult = sra::loadSwitchConfig(switchesPath);
            if (!cfgResult)
            {
                std::println(std::cerr,
                             "Error loading switch config: {}",
                             cfgResult.error());
                return EXIT_FAILURE;
            }
            return cmdSyncMulti(
                *sotResult, *cfgResult, useTls, caCert, timeout);
        }

        // Single-server sync: --server + --node-ip required
        if (nodeIp.empty())
        {
            std::println(
                std::cerr,
                "Error: single-server sync requires --node-ip <management-ip>"
                " (the key used in nodes_by_loopback)");
            return EXIT_FAILURE;
        }

        std::println("\n{}", std::string(60, '='));
        std::println("Switch : {}  node-ip={}", server, nodeIp);
        std::println("{}", std::string(60, '='));

        sra::RouteClient client(server, useTls, caCert, timeout);
        return syncOneServer(client, nodeIp, server, *sotResult);
    }

    // -----------------------------------------------------------------------
    // Multi-server test via switch_config.json
    // -----------------------------------------------------------------------
    if (command == "test" && !switchesPath.empty())
    {
        std::println("sra  build #{}  switches={}  tls={}",
                     rtsrv::build::kBuildNumber,
                     switchesPath,
                     useTls);

        auto cfgResult = sra::loadSwitchConfig(switchesPath);
        if (!cfgResult)
        {
            std::println(std::cerr,
                         "Error loading switch config: {}",
                         cfgResult.error());
            return EXIT_FAILURE;
        }
        return cmdMultiTest(*cfgResult, useTls, caCert, timeout);
    }

    // -----------------------------------------------------------------------
    // Startup: initialise all three netlink tables (routes, neighbors,
    // nexthops) from the kernel and start silent background monitor threads
    // that keep them current.  A shared UdpTableServer publishes snapshots:
    //   port 9001 – ARP/NDP neighbor table
    //   port 9002 – nexthop object table
    //   port 9003 – IPv4 /32 routing table
    // -----------------------------------------------------------------------
    sra::UdpTableServer startupUdpServer;

    // Contexts (stack-allocated; threads hold raw pointers; RAII guard joins)
    StartupRouteCtx startupRouteCtx;
    NeighCtx        startupNeighCtx;
    NexthopCtx      startupNhCtx;
    startupRouteCtx.udpServer = &startupUdpServer;
    startupNeighCtx.udpServer = &startupUdpServer;
    startupNhCtx.udpServer    = &startupUdpServer;

    // ── Step 1: Populate route table (all IPv4 /32 unicast) ──────────────
    try
    {
        sra::RoutingManager rm;
        auto routesResult = rm.listRoutes(AF_INET, RT_TABLE_UNSPEC);
        if (routesResult)
        {
            for (const auto& kr : *routesResult)
            {
                if (kr.prefixLen != 32 || kr.type != RTN_UNICAST)
                    continue;
                const std::string key = kr.destination + "|"
                                      + kr.gateway    + "|"
                                      + kr.interfaceName;
                startupRouteCtx.routes[key] = WatchRoute{
                    kr.destination, kr.gateway, kr.interfaceName,
                    kr.interfaceIndex, kr.metric, kr.table,
                    kr.protocol, {},
                    static_cast<uint8_t>(kr.family), kr.prefixLen,
                    0u, kr.scope, kr.type, 0u};
            }
        }
    }
    catch (const std::exception& ex)
    {
        std::println(std::cerr,
                     "[Startup] Warning: RoutingManager init failed: {}",
                     ex.what());
    }
    startupUdpServer.setRouteData(serializeRouteTable(startupRouteCtx.routes));

    // ── Step 2: Dump neighbor table ───────────────────────────────────────
    {
        int nfd = netlink_neigh_init();
        if (nfd >= 0)
        {
            netlink_neigh_dump(nfd, nlNeighPopulateCb, &startupNeighCtx);
            netlink_neigh_close(nfd);
        }
    }
    startupUdpServer.setNeighborData(
        serializeNeighborTable(startupNeighCtx.neighbors));

    // ── Step 3: Dump nexthop table ────────────────────────────────────────
    {
        int nhfd = netlink_nexthop_init();
        if (nhfd >= 0)
        {
            netlink_nexthop_dump(nhfd, nlNexthopPopulateCb, &startupNhCtx);
            netlink_nexthop_close(nhfd);
        }
    }
    startupUdpServer.setNexthopData(
        serializeNexthopTable(startupNhCtx.nexthops));

    // ── Step 4: Start the UDP server (data is pre-populated) ─────────────
    if (!startupUdpServer.start())
    {
        std::println(std::cerr,
                     "[Startup] Warning: UDP table server failed to start");
    }

    // ── Step 5: Open live monitoring fds ─────────────────────────────────
    g_startup_route_fd   = netlink_init();
    g_startup_neigh_fd   = netlink_neigh_init();
    g_startup_nexthop_fd = netlink_nexthop_init();

    // ── Step 6: Launch background monitor threads ─────────────────────────
    std::thread startupRouteThread;
    std::thread startupNeighThread;
    std::thread startupNhThread;

    if (g_startup_route_fd >= 0)
    {
        const int rfd = g_startup_route_fd;
        startupRouteThread = std::thread(
            [rfd, rctx = &startupRouteCtx]()
            {
                netlink_run(rfd, startupRouteLiveCb, rctx);
            });
    }
    if (g_startup_neigh_fd >= 0)
    {
        const int nfd = g_startup_neigh_fd;
        startupNeighThread = std::thread(
            [nfd, nctx = &startupNeighCtx]()
            {
                netlink_neigh_run(nfd, startupNeighLiveCb, nctx);
            });
    }
    if (g_startup_nexthop_fd >= 0)
    {
        const int nhfd = g_startup_nexthop_fd;
        startupNhThread = std::thread(
            [nhfd, nhctx = &startupNhCtx]()
            {
                netlink_nexthop_run(nhfd, startupNexthopLiveCb, nhctx);
            });
    }

    // ── RAII cleanup guard ─────────────────────────────────────────────────
    // Joins all background threads and stops the UDP server on every exit
    // path (normal return, exception, or early return from a command).
    struct StartupGuard
    {
        std::thread&         routeThread;
        std::thread&         neighThread;
        std::thread&         nhThread;
        sra::UdpTableServer& udpSrv;

        ~StartupGuard()
        {
            if (g_startup_route_fd >= 0)
            {
                netlink_close(g_startup_route_fd);
                g_startup_route_fd = -1;
            }
            if (g_startup_neigh_fd >= 0)
            {
                netlink_neigh_close(g_startup_neigh_fd);
                g_startup_neigh_fd = -1;
            }
            if (g_startup_nexthop_fd >= 0)
            {
                netlink_nexthop_close(g_startup_nexthop_fd);
                g_startup_nexthop_fd = -1;
            }
            if (routeThread.joinable()) routeThread.join();
            if (neighThread.joinable()) neighThread.join();
            if (nhThread.joinable())    nhThread.join();
            udpSrv.stop();
        }
    } startupGuard{startupRouteThread, startupNeighThread,
                   startupNhThread, startupUdpServer};

    // ── Install default signal handler (commands install their own) ───────
    {
        struct sigaction sa{};
        sa.sa_handler = startupSigHandler;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGINT,  &sa, nullptr);
        sigaction(SIGTERM, &sa, nullptr);
    }

    // -----------------------------------------------------------------------
    // Local-only commands (no gRPC connection required)
    // -----------------------------------------------------------------------
    if (command == "neighbors")
    {
        // Stop the startup neighbor monitor before running the interactive
        // one (cmdWatchNeighbors installs its own signal handler).
        if (g_startup_neigh_fd >= 0)
        {
            netlink_neigh_close(g_startup_neigh_fd);
            g_startup_neigh_fd = -1;
        }
        if (startupNeighThread.joinable())
            startupNeighThread.join();
        return cmdWatchNeighbors(startupUdpServer);
    }

    if (command == "nexthops")
    {
        // Stop the startup nexthop monitor before running the interactive one.
        if (g_startup_nexthop_fd >= 0)
        {
            netlink_nexthop_close(g_startup_nexthop_fd);
            g_startup_nexthop_fd = -1;
        }
        if (startupNhThread.joinable())
            startupNhThread.join();
        return cmdWatchNexthops(startupUdpServer);
    }

    // -----------------------------------------------------------------------
    // Single-server commands
    // -----------------------------------------------------------------------
    std::println("sra  build #{}  server={}  tls={}",
                 rtsrv::build::kBuildNumber,
                 server,
                 useTls);

    sra::RouteClient client(server, useTls, caCert, timeout);

    // -----------------------------------------------------------------------
    // Startup: request loopback from server based on this client's IP.
    // Skipped for the "watch" command – cmdNetlinkWatch performs this step
    // itself after displaying the initial kernel route table.
    // -----------------------------------------------------------------------
    std::string activeLoopback = clientCfg.loopback;
    if (command != "watch" && command != "neighbors" && command != "nexthops"
        && command != "vrf-route-add")
    {
        std::println("[Startup] Requesting loopback from server based on client IP...");
        auto lbResult = client.requestLoopback();
        if (lbResult)
        {
            std::println("[Startup] Loopback received from server: '{}'", *lbResult);
            activeLoopback = *lbResult;
        }
        else
        {
            std::println("[Startup] No loopback from server ({}); "
                         "falling back to config loopback: '{}'",
                         lbResult.error(),
                         activeLoopback);
        }
        std::println("[Startup] Active loopback set to: '{}'", activeLoopback);
    }

    if (command == "test")
    {
        return cmdTest(client);
    }

    if (command == "echo")
    {
        const std::string msg = args.empty() ? "ping from sra" : args[0];
        auto result = client.echo(msg);
        if (!result)
        {
            std::println(std::cerr, "Error: {}", result.error());
            return EXIT_FAILURE;
        }
        std::println("Echo response   : '{}'", result->message());
        std::println("Server ID       : {}", result->server_id());
        std::println("Server version  : {}", result->server_version());
        std::println("Server ts (µs)  : {}", result->server_ts_us());
        return EXIT_SUCCESS;
    }

    if (command == "add")
    {
        if (args.empty())
        {
            std::println(std::cerr,
                         "Usage: sra add <destination> [gateway] "
                         "[interface] [metric]");
            return EXIT_FAILURE;
        }
        const std::string dest = args[0];
        const std::string gw = args.size() > 1 ? args[1] : "";
        const std::string iface = args.size() > 2 ? args[2] : "";
        const uint32_t metric =
            args.size() > 3 ? static_cast<uint32_t>(std::stoul(args[3])) : 0;

        auto result = client.addRoute(dest, gw, iface, metric);
        if (!result)
        {
            std::println(std::cerr, "Error: {}", result.error());
            return EXIT_FAILURE;
        }
        std::println("Route added:");
        printRoute(*result);
        return EXIT_SUCCESS;
    }

    if (command == "remove")
    {
        if (args.empty())
        {
            std::println(std::cerr, "Usage: sra remove <id>");
            return EXIT_FAILURE;
        }
        auto result = client.removeRoute(args[0]);
        if (!result)
        {
            std::println(std::cerr, "Error: {}", result.error());
            return EXIT_FAILURE;
        }
        std::println("Route {} removed.", args[0]);
        return EXIT_SUCCESS;
    }

    if (command == "get")
    {
        if (args.empty())
        {
            std::println(std::cerr, "Usage: sra get <id>");
            return EXIT_FAILURE;
        }
        auto result = client.getRoute(args[0]);
        if (!result)
        {
            std::println(std::cerr, "Error: {}", result.error());
            return EXIT_FAILURE;
        }
        printRoute(*result);
        return EXIT_SUCCESS;
    }

    if (command == "list")
    {
        const bool activeOnly =
            std::ranges::find(args, std::string("--active")) != args.end();
        auto result = client.listRoutes(activeOnly);
        if (!result)
        {
            std::println(std::cerr, "Error: {}", result.error());
            return EXIT_FAILURE;
        }
        std::println("{} route(s):", result->size());
        for (const auto& r : *result)
        {
            std::println("────────────────────");
            printRoute(r);
        }
        if (result->empty())
        {
            std::println("  (route table is empty)");
        }
        return EXIT_SUCCESS;
    }

    if (command == "watch")
    {
        // Stop the startup route monitor before running the interactive one
        // (cmdNetlinkWatch installs its own signal handler).
        if (g_startup_route_fd >= 0)
        {
            netlink_close(g_startup_route_fd);
            g_startup_route_fd = -1;
        }
        if (startupRouteThread.joinable())
            startupRouteThread.join();
        return cmdNetlinkWatch(client, activeLoopback, startupUdpServer);
    }

    if (command == "set-loopback")
    {
        if (args.empty())
        {
            std::println(std::cerr, "Usage: sra set-loopback <address>");
            return EXIT_FAILURE;
        }
        auto result = client.setLoopback(args[0]);
        if (!result)
        {
            std::println(std::cerr, "Error: {}", result.error());
            return EXIT_FAILURE;
        }
        std::println("Loopback address set: {}", *result);
        return EXIT_SUCCESS;
    }

    if (command == "get-loopback")
    {
        auto result = client.getLoopback();
        if (!result)
        {
            std::println(std::cerr, "Error: {}", result.error());
            return EXIT_FAILURE;
        }
        std::println("Loopback address: {}",
                     result->empty() ? "(not set)" : *result);
        return EXIT_SUCCESS;
    }

    if (command == "get-loopbacks")
    {
        if (args.empty())
        {
            std::println(std::cerr, "Usage: sra get-loopbacks <loopback-address>");
            return EXIT_FAILURE;
        }
        auto result = client.getLoopbacks(args[0]);
        if (!result)
        {
            std::println(std::cerr, "Error: {}", result.error());
            return EXIT_FAILURE;
        }
        std::println("GetLoopbacks: {}", result->message());
        std::println("{} interface(s):", result->interfaces_size());
        for (const auto& iface : result->interfaces())
        {
            std::println("────────────────────");
            std::println("  Name         : {}", iface.name());
            std::println("  Type         : {}", iface.type());
            std::println("  Local address: {}", iface.local_address());
            std::println("  Nexthop      : {}",
                         iface.nexthop().empty() ? "(none)" : iface.nexthop());
            std::println("  Weight       : {}", iface.weight());
            std::println("  Description  : {}", iface.description());
            if (iface.prefixes_size() > 0)
            {
                std::println("  Prefixes ({}):", iface.prefixes_size());
                for (const auto& pfx : iface.prefixes())
                {
                    std::println("    {} weight={} role='{}' desc='{}'",
                                 pfx.prefix(), pfx.weight(),
                                 pfx.role(), pfx.description());
                }
            }
        }
        if (result->interfaces_size() == 0)
        {
            std::println("  (no interfaces)");
        }
        return EXIT_SUCCESS;
    }

    if (command == "grpc-proc-demo")
    {
        return cmdGrpcProcDemo(client);
    }

    // -----------------------------------------------------------------------
    // 'run' command — full SRA daemon mode
    //
    // Two separate connections are maintained:
    //   • srmd   — gRPC over TCP (--server flag); route management / SOT auth.
    //   • ud_server — separate process; Unix-domain socket (first positional
    //                 arg, default /tmp/ud_server.sock); receives VRF route-add
    //                 commands encoded in the udproto/routeproto/cmdproto stack.
    //   srmd has NO Unix-domain socket — gRPC only.
    //
    // Startup sequence:
    //   1. Start GrpcProc background thread (all gRPC calls go to srmd).
    //   2. [srmd gRPC] RequestLoopback → PERMISSION_DENIED = IP not in SOT → exit.
    //   3. [srmd gRPC] GetLoopbacks(loopback) → log VRF/interface/prefix config.
    //   4. Display nexthop / neighbor / /32 route tables (kernel netlink).
    //   5. [srmd gRPC] GetAllRoutes → build SingleRouteRequest for nni interfaces,
    //      nexthop_id looked up from the kernel nexthop table.
    //   6. [ud_server Unix socket] Submit SingleRouteRequest via VrfUdpClient.
    //   7. Main loop: keep running until SIGINT/SIGTERM.
    //
    // Non-blocking I/O:
    //   - gRPC  : RPCs submitted to GrpcProc; executed in a background thread.
    //   - Unix  : VrfUdpClient uses O_NONBLOCK + poll() for all socket I/O.
    // -----------------------------------------------------------------------
    if (command == "run")
    {
        // Install signal handler for the run command.
        {
            struct sigaction sa{};
            sa.sa_handler = runSigHandler;
            sigemptyset(&sa.sa_mask);
            sigaction(SIGINT,  &sa, nullptr);
            sigaction(SIGTERM, &sa, nullptr);
        }

        std::println("[run] SRA daemon starting — server={} tls={}",
                     server, useTls);

        // ── Step 1: Start GrpcProc (non-blocking gRPC) ──────────────────────
        sra::GrpcProc grpcProc(client, /*autoStart=*/true);
        std::println("[run] GrpcProc started (non-blocking gRPC thread)");

        // ── Step 2: RequestLoopback ──────────────────────────────────────────
        std::println("[run] [gRPC] Submitting RequestLoopback…");
        const uint64_t lbId = grpcProc.submit(sra::RequestLoopbackParams{});
        auto lbResp = grpcProc.waitForResponse(lbId, std::chrono::seconds(30));
        if (!lbResp)
        {
            std::println(std::cerr, "[run] RequestLoopback: timeout");
            return EXIT_FAILURE;
        }

        const auto& lbResult =
            std::get<9>(lbResp->payload); // index 9 = RequestLoopbackResult
        if (!lbResult)
        {
            std::println(std::cerr,
                         "[run] RequestLoopback failed: {} — "
                         "SRA IP not in SOT, closing gRPC connection",
                         lbResult.error());
            grpcProc.stop();
            return EXIT_FAILURE;
        }

        const std::string loopback = *lbResult;
        std::println("[run] Loopback from SRMD: '{}'", loopback);

        // ── Step 3: GetLoopbacks ─────────────────────────────────────────────
        std::println("[run] [gRPC] Submitting GetLoopbacks('{}')…", loopback);
        const uint64_t glId =
            grpcProc.submit(sra::GetLoopbacksParams{loopback});
        auto glResp =
            grpcProc.waitForResponse(glId, std::chrono::seconds(30));
        if (glResp)
        {
            const auto& glResult =
                std::get<sra::GetLoopbacksResult>(glResp->payload);
            if (glResult)
            {
                std::println("[run] GetLoopbacks: {} — {} interface(s)",
                             glResult->message(),
                             glResult->interfaces_size());
                for (const auto& ifc : glResult->interfaces())
                {
                    std::println("[run]   iface='{}' type={} local={} "
                                 "nexthop={} weight={}",
                                 ifc.name(), ifc.type(), ifc.local_address(),
                                 ifc.nexthop().empty() ? "(none)" : ifc.nexthop(),
                                 ifc.weight());
                    for (const auto& pfx : ifc.prefixes())
                    {
                        std::println("[run]     prefix={} weight={} role={}",
                                     pfx.prefix(), pfx.weight(), pfx.role());
                    }
                }
            }
            else
            {
                std::println("[run] GetLoopbacks failed: {}",
                             glResult.error());
            }
        }

        // ── Step 4: Log kernel tables (populated during startup) ─────────────
        std::println("[run] Nexthop table : {} entry/entries",
                     startupNhCtx.nexthops.size());
        for (const auto& [id, nh] : startupNhCtx.nexthops)
        {
            std::println("[run]   nexthop id={} gw='{}' oif='{}' proto={}",
                         id,
                         nh.gateway.empty() ? "-" : nh.gateway,
                         nh.oif_name.empty()
                             ? std::to_string(nh.oif)
                             : nh.oif_name,
                         static_cast<unsigned>(nh.protocol));
        }

        std::println("[run] Neighbor table: {} entry/entries",
                     startupNeighCtx.neighbors.size());
        std::println("[run] /32 route table: {} entry/entries",
                     startupRouteCtx.routes.size());

        // ── Step 5: GetAllRoutes ─────────────────────────────────────────────
        std::println("[run] [gRPC] Submitting GetAllRoutes…");
        const uint64_t arId = grpcProc.submit(sra::GetAllRoutesParams{});
        auto arResp =
            grpcProc.waitForResponse(arId, std::chrono::seconds(30));
        if (!arResp)
        {
            std::println(std::cerr, "[run] GetAllRoutes: timeout");
            return EXIT_FAILURE;
        }

        const auto& arResult =
            std::get<sra::GetAllRoutesResult>(arResp->payload);
        if (!arResult)
        {
            std::println(std::cerr, "[run] GetAllRoutes failed: {}",
                         arResult.error());
            return EXIT_FAILURE;
        }

        printAllRoutes(*arResult);

        // ── Step 6: Build SingleRouteRequest from GetAllRoutes (nni only) ─────
        // For each VrfRoute with interface_type == "nni":
        //   • look up nexthop_id from the kernel nexthop table by gateway IP
        //   • convert each prefix string to binary PrefixIpv4 entries
        const std::string udSocketPath =
            args.empty() ? "/tmp/ud_server.sock" : args[0];

        sra::VrfUdpClient vrfClient(udSocketPath);
        vrfClient.start();
        std::println("[run] VrfUdpClient started (non-blocking Unix socket='{}')",
                     udSocketPath);

        // ── Load VRF table and arm the nexthop event handler ─────────────────
        // Future nexthop ADDED / CHANGED / REMOVED events will look up the
        // affected gateway in this table and re-submit a SingleRouteRequest
        // (SINGLE_ROUTE, type=1) to ud_server whenever a match is found.
        sra::VrfTable vrfTable;
        vrfTable.load(*arResult);
        startupNhCtx.vrfTable  = &vrfTable;
        startupNhCtx.vrfClient = &vrfClient;
        startupNhCtx.neighCtx  = &startupNeighCtx;
        std::println("[run] VRF table loaded: {} entry/entries; "
                     "nexthop event handler armed",
                     vrfTable.size());

        cmdproto::SingleRouteRequest singleReq;
        int nniCount = 0;

        for (const auto& route : arResult->routes())
        {
            if (route.interface_type() != "nni")
                continue;
            if (route.nexthop().empty())
                continue;

            // Check the adjacency table; probe the nexthop with an ICMP echo
            // request if it has not yet been resolved to a MAC address.
            if (!neighborHasIp(startupNeighCtx, route.nexthop()))
            {
                std::println("[run] nexthop '{}' absent from adjacency table — "
                             "sending ICMP echo request", route.nexthop());
                sendIcmpEchoRequest(route.nexthop());
            }

            // Look up nexthop_id from the kernel nexthop table by gateway IP.
            uint32_t nexthopId = 0;
            for (const auto& [id, nh] : startupNhCtx.nexthops)
            {
                if (nh.gateway == route.nexthop())
                {
                    nexthopId = id;
                    break;
                }
            }

            // Parse nexthop address into binary form.
            struct in_addr nhAddr{};
            if (::inet_pton(AF_INET, route.nexthop().c_str(), &nhAddr) != 1)
            {
                std::println(std::cerr,
                             "[run] invalid nexthop address '{}' — skipping",
                             route.nexthop());
                continue;
            }
            const auto* nhBytes =
                reinterpret_cast<const std::uint8_t*>(&nhAddr.s_addr);

            cmdproto::Interface iface{};
            // Interface name (null-padded fixed array).
            const std::string& ifn = route.interface_name();
            for (std::size_t k = 0;
                 k < cmdproto::IFACE_NAME_SIZE && k < ifn.size(); ++k)
            {
                iface.iface_name[k] = ifn[k];
            }
            iface.nexthop_addr_ipv4 = {nhBytes[0], nhBytes[1],
                                       nhBytes[2], nhBytes[3]};
            iface.nexthop_id_ipv4   = nexthopId;

            // Convert each prefix string "A.B.C.D/N" to PrefixIpv4.
            for (const auto& pfx : route.prefixes())
            {
                const std::string& pfxStr = pfx.prefix();
                const auto slash = pfxStr.rfind('/');
                if (slash == std::string::npos)
                    continue;
                const std::string addrStr = pfxStr.substr(0, slash);
                const auto maskLen = static_cast<std::uint8_t>(
                    std::stoul(pfxStr.substr(slash + 1)));

                struct in_addr pfxAddr{};
                if (::inet_pton(AF_INET, addrStr.c_str(), &pfxAddr) != 1)
                    continue;
                const auto* pfxBytes =
                    reinterpret_cast<const std::uint8_t*>(&pfxAddr.s_addr);

                iface.prefixes.push_back(cmdproto::PrefixIpv4{
                    {pfxBytes[0], pfxBytes[1], pfxBytes[2], pfxBytes[3]},
                    maskLen});
            }

            std::println("[run] nni interface: iface='{}' nexthop='{}' "
                         "nexthop_id={} prefixes={}",
                         route.interface_name(), route.nexthop(),
                         nexthopId, iface.prefixes.size());

            singleReq.interfaces.push_back(std::move(iface));
            ++nniCount;
        }

        if (nniCount > 0)
        {
            std::println("[run] Submitting SingleRouteRequest ({} nni interface(s)) "
                         "to Unix socket…", nniCount);
            vrfClient.submit(std::move(singleReq));
        }
        else
        {
            std::println("[run] No nni interfaces found in GetAllRoutes "
                         "response — no route-add request sent");
        }

        // ── Step 7: Main daemon loop ──────────────────────────────────────────
        // Netlink background threads (routes, neighbors, nexthops) keep
        // running and update the in-memory tables automatically.
        // The gRPC channel stays open via GrpcProc.
        // The VrfUdpClient thread awaits further submit() calls.
        std::println("[run] Daemon running (Ctrl-C to stop)…");
        while (!g_run_stop)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        std::println("[run] Shutdown signal received — stopping");

        // shutdown(SHUT_RD) is the POSIX-guaranteed way to unblock a blocking
        // recv() in another thread on a socket fd.  close() alone does NOT
        // unblock recv() on Linux.  pthread_kill(SIGINT) only delivers EINTR,
        // but all three *_run loops treat EINTR as "continue" — so the thread
        // never exits that way.
        // After shutdown the *_run loop's next recv() returns with a non-EINTR
        // error (ENOTCONN / EBADF), which causes return -1 → thread exits.
        // Setting the globals to -1 prevents the StartupGuard destructor from
        // double-closing them.
        auto stopNetlinkFd = [](int& fd, void (*closeFn)(int)) noexcept {
            if (fd >= 0) {
                ::shutdown(fd, SHUT_RD);
                closeFn(fd);
                fd = -1;
            }
        };
        stopNetlinkFd(g_startup_route_fd,   netlink_close);
        stopNetlinkFd(g_startup_neigh_fd,   netlink_neigh_close);
        stopNetlinkFd(g_startup_nexthop_fd, netlink_nexthop_close);

        vrfClient.stop();
        grpcProc.stop();
        return EXIT_SUCCESS; // StartupGuard joins threads (now exiting)
    }

    if (command == "vrf-route-add")
    {
        const std::string socketPath =
            args.empty() ? "/tmp/ud_server.sock" : args[0];

        std::println("[vrf-route-add] socket={} — starting VRF thread", socketPath);

        sra::VrfUdpClient vrfClient(socketPath);
        vrfClient.start();

        // RequestLoopback to identify this node in the SOT.
        std::println("[vrf-route-add] requesting loopback from srmd…");
        auto lbResult = client.requestLoopback();
        if (lbResult)
        {
            std::println("[vrf-route-add] loopback from srmd: '{}'", *lbResult);
            activeLoopback = *lbResult;
        }
        else
        {
            std::println("[vrf-route-add] loopback from srmd: {} (using config: '{}')",
                         lbResult.error(), activeLoopback);
        }

        // GetAllRoutes then build and submit a SingleRouteRequest (nni only).
        std::println("[vrf-route-add] GetAllRoutes…");
        auto arResult = client.getAllRoutes();
        if (arResult)
        {
            printAllRoutes(*arResult);

            cmdproto::SingleRouteRequest singleReq;
            for (const auto& route : arResult->routes())
            {
                if (route.interface_type() != "nni")
                    continue;
                if (route.nexthop().empty())
                    continue;

                struct in_addr nhAddr{};
                if (::inet_pton(AF_INET, route.nexthop().c_str(), &nhAddr) != 1)
                    continue;
                const auto* nhBytes =
                    reinterpret_cast<const std::uint8_t*>(&nhAddr.s_addr);

                cmdproto::Interface iface{};
                const std::string& ifn = route.interface_name();
                for (std::size_t k = 0;
                     k < cmdproto::IFACE_NAME_SIZE && k < ifn.size(); ++k)
                    iface.iface_name[k] = ifn[k];
                iface.nexthop_addr_ipv4 = {nhBytes[0], nhBytes[1],
                                           nhBytes[2], nhBytes[3]};
                iface.nexthop_id_ipv4   = 0; // no kernel nexthop table in this mode

                for (const auto& pfx : route.prefixes())
                {
                    const std::string& pfxStr = pfx.prefix();
                    const auto slash = pfxStr.rfind('/');
                    if (slash == std::string::npos)
                        continue;
                    struct in_addr pfxAddr{};
                    if (::inet_pton(AF_INET, pfxStr.substr(0, slash).c_str(),
                                    &pfxAddr) != 1)
                        continue;
                    const auto* pb =
                        reinterpret_cast<const std::uint8_t*>(&pfxAddr.s_addr);
                    const auto maskLen = static_cast<std::uint8_t>(
                        std::stoul(pfxStr.substr(slash + 1)));
                    iface.prefixes.push_back(cmdproto::PrefixIpv4{
                        {pb[0], pb[1], pb[2], pb[3]}, maskLen});
                }

                singleReq.interfaces.push_back(std::move(iface));
            }

            if (!singleReq.interfaces.empty())
            {
                std::println("[vrf-route-add] submitting {} nni interface(s) to "
                             "Unix socket…",
                             singleReq.interfaces.size());
                vrfClient.submit(std::move(singleReq));
            }
            else
            {
                std::println("[vrf-route-add] no nni interfaces found — "
                             "no request sent");
            }
        }
        else
        {
            std::println("[vrf-route-add] GetAllRoutes failed: {}",
                         arResult.error());
        }

        vrfClient.stop();
        return EXIT_SUCCESS;
    }

    std::println(std::cerr, "Unknown command: '{}'", command);
    std::println(std::cerr, "Run 'sra --help' for usage.");
    return EXIT_FAILURE;
}
