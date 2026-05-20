/**
 * @file client/src/main.cpp
 * @brief Entry point for sra – the Switch Route Application.
 *
 * sra is a command-line client for the srmd Switch Route Manager daemon.
 * It connects to one or more running srmd instances via gRPC and exposes
 * route management commands.
 *
 * @verbatim
 * Usage: sra [options] <command> [command-args]
 *
 * Global options:
 *   -c, --config <path>   client config.json [default: config/config.json]
 *   -s, --server <addr>   single srmd address (overrides config file)
 *   -w, --switches <path> switch_config.json (multi-server mode)
 *       --sot <path>      Source-of-Truth JSON file
 *       --node-ip <ip>    mgmt IP in SOT (required for sync)
 *   -t, --timeout <sec>   RPC deadline in seconds (overrides config)
 *       --tls             use TLS channel (overrides config file)
 *       --ca-cert <path>  CA certificate for TLS verification
 *       --logstream <d>   duplicate logs to: stdout or stderr (file always
 * written)
 *       --loglevel <n>    min level: DEBUG|1 INFO|2 NOTICE|3 ERR|5
 *   -v, --version         print version and exit
 *   -h, --help            print this help and exit
 *
 * Commands:
 *   test               full Echo + CRUD round-trip test sequence
 *   sync               push SOT prefix routes to servers (--sot required)
 *   echo <msg>         send Echo RPC and print response
 *   add <dst> [gw] [iface] [metric]   add a route
 *   remove <id>        remove a route by ID
 *   get <id>           retrieve a route by ID
 *   list [--active]    list all (or active-only) routes
 *   watch              forward kernel /32 netlink events to srmd
 *   neighbors          dump/watch kernel ARP/NDP neighbor table
 *   nexthops           dump/watch kernel nexthop objects (Linux 5.3+)
 *   set-loopback <a>   store loopback address on server
 *   get-loopback       retrieve loopback address from server
 *   get-loopbacks <lb> query SOT interface list for a loopback
 *   add-del-list [s]   one-shot: RequestLoopback, GetLoopbacks,
 *                      ROUTE_ADD, ROUTE_DEL, ROUTE_LIST via ud_server
 *   run [sock]         daemon: sync SOT routes, monitor nexthop events
 *   grpc-proc-demo     run async GrpcProc background thread demo
 * @endverbatim
 *
 * @version 1.0
 */

#include "build_info.hpp"
#include "client/grpc_proc.hpp"
#include "client/netlink_neigh.h"
#include "client/netlink_nexthop.h"
#include "client/nexthop_group.hpp"
#include "client/redis_rib.hpp"
#include "client/route_client.hpp"
#include "client/routing.hpp"
#include "client/sot_config.hpp"
#include "client/sra_udp_client.hpp"
#include "client/switch_config.hpp"
#include "client/udp_table_server.hpp"
#include "client/vrf_table.hpp"
#include "common/config.hpp"
#include "common/log.hpp"
#include "lib/cmd_proto.hpp"
#include "lib/logger.hpp"
#include "server/netlink.h"

#include <arpa/inet.h>
#include <linux/neighbour.h>
#include <linux/rtnetlink.h>
#include <netinet/ip_icmp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <boost/program_options.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <format>
#include <iostream>
#include <map>
#include <mutex>
#include <print>
#include <queue>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <variant>
#include <vector>

namespace po = boost::program_options;

int prepare_route_add_remain_lb(
    sra::SraUdpClient& vrfClient,
    const std::string& loopback_ipv4,
    std::expected<srmd::v1::GetNodePrefixesResponse, std::string>& prefixes,
    const std::vector<const sra::KernelRoute*>& ospf32,
    sra::RedisRib& redisRib);

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
                         p.prefix(),
                         p.weight(),
                         p.role(),
                         p.description());
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
    rtsrv::log::info("Step 1: Echo");
    const std::string echoMsg =
        "Hello from sra! Switch Route Application test.";
    const auto t0 = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();

    auto echoResult = client.echo(echoMsg);
    if (!echoResult)
    {
        rtsrv::log::err(std::format("[FAIL] Echo: {}", echoResult.error()));
        return EXIT_FAILURE;
    }

    const auto t1 = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();

    rtsrv::log::info(std::format("[OK] Echo received back  : '{}'",
                                 echoResult->message()));
    rtsrv::log::info(std::format("[OK] Server ID           : {}",
                                 echoResult->server_id()));
    rtsrv::log::info(std::format("[OK] Server version      : {}",
                                 echoResult->server_version()));
    rtsrv::log::info(std::format("[OK] Round-trip latency  : {} us", t1 - t0));

    // ------------------------------------------------------------------
    // Step 2: Heartbeat
    // ------------------------------------------------------------------
    rtsrv::log::info("Step 2: Heartbeat");
    auto hbResult = client.heartbeat(1);
    if (!hbResult)
    {
        rtsrv::log::err(
            std::format("[FAIL] Heartbeat: {}", hbResult.error()));
        allOk = false;
    }
    else
    {
        rtsrv::log::info(std::format("[OK] Sequence: {}  server_ts: {} us",
                                     hbResult->sequence(),
                                     hbResult->server_ts_us()));
    }

    // ------------------------------------------------------------------
    // Step 3: AddRoute (IPv4 default)
    // ------------------------------------------------------------------
    rtsrv::log::info("Step 3: AddRoute (default via 192.168.1.1)");
    auto addResult = client.addRoute("default", "192.168.1.1", "eth0", 100);
    if (!addResult)
    {
        rtsrv::log::err(std::format("[FAIL] AddRoute: {}", addResult.error()));
        allOk = false;
    }
    else
    {
        rtsrv::log::info(std::format("[OK] Route created: id={}",
                                     addResult->id()));
    }

    // ------------------------------------------------------------------
    // Step 4: AddRoute (10.0.0.0/8)
    // ------------------------------------------------------------------
    rtsrv::log::info("Step 4: AddRoute (10.0.0.0/8 via 10.0.0.1)");
    auto addResult2 = client.addRoute("10.0.0.0/8", "10.0.0.1", "eth1", 50);
    if (!addResult2)
    {
        rtsrv::log::err(
            std::format("[FAIL] AddRoute2: {}", addResult2.error()));
        allOk = false;
    }
    else
    {
        rtsrv::log::info(std::format("[OK] Route created: id={}",
                                     addResult2->id()));
    }

    // ------------------------------------------------------------------
    // Step 5: ListRoutes
    // ------------------------------------------------------------------
    rtsrv::log::info("Step 5: ListRoutes");
    auto listResult = client.listRoutes();
    if (!listResult)
    {
        rtsrv::log::err(
            std::format("[FAIL] ListRoutes: {}", listResult.error()));
        allOk = false;
    }
    else
    {
        rtsrv::log::info(std::format("[OK] {} route(s) in table:",
                                     listResult->size()));
        for (const auto& r : *listResult)
        {
            rtsrv::log::info(std::format("  route id={} dest={} gw={} "
                                         "iface={} metric={}",
                                         r.id(), r.destination(), r.gateway(),
                                         r.interface_name(), r.metric()));
        }
    }

    // ------------------------------------------------------------------
    // Step 6: GetRoute by ID
    // ------------------------------------------------------------------
    if (addResult)
    {
        rtsrv::log::info(std::format("Step 6: GetRoute ({}...)",
                                     addResult->id().substr(0, 8)));
        auto getResult = client.getRoute(addResult->id());
        if (!getResult)
        {
            rtsrv::log::err(
                std::format("[FAIL] GetRoute: {}", getResult.error()));
            allOk = false;
        }
        else
        {
            rtsrv::log::info(std::format("[OK] Retrieved: id={} dest={}",
                                         getResult->id(),
                                         getResult->destination()));
        }
    }

    // ------------------------------------------------------------------
    // Step 7: RemoveRoute (first route)
    // ------------------------------------------------------------------
    if (addResult)
    {
        rtsrv::log::info(std::format("Step 7: RemoveRoute ({}...)",
                                     addResult->id().substr(0, 8)));
        auto removeResult = client.removeRoute(addResult->id());
        if (!removeResult)
        {
            rtsrv::log::err(std::format("[FAIL] RemoveRoute: {}",
                                        removeResult.error()));
            allOk = false;
        }
        else
        {
            rtsrv::log::info("[OK] Route removed");
        }
    }

    // ------------------------------------------------------------------
    // Step 8: ListRoutes again (should be 1 remaining)
    // ------------------------------------------------------------------
    rtsrv::log::info("Step 8: ListRoutes (after remove)");
    auto listResult2 = client.listRoutes();
    if (!listResult2)
    {
        rtsrv::log::err(
            std::format("[FAIL] ListRoutes2: {}", listResult2.error()));
        allOk = false;
    }
    else
    {
        rtsrv::log::info(std::format("[OK] {} route(s) remaining:",
                                     listResult2->size()));
        for (const auto& r : *listResult2)
        {
            rtsrv::log::info(std::format("  route id={} dest={} gw={} "
                                         "iface={} metric={}",
                                         r.id(), r.destination(), r.gateway(),
                                         r.interface_name(), r.metric()));
        }
    }

    // ------------------------------------------------------------------
    // Summary
    // ------------------------------------------------------------------
    rtsrv::log::info(std::format("Test result: {}",
                                 allOk ? "ALL PASSED" : "SOME FAILURES"));
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
static void logSotSummary(const sra::SotConfig& cfg)
{
    rtsrv::log::info(std::format("SOT parsed: {} node(s), {} prefix(es) total",
                                 cfg.nodes.size(),
                                 cfg.totalPrefixCount()));

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

        rtsrv::log::info(
            std::format("SOT node: {} ({})  lo4={}  vrfs={}  ifaces={}  prefixes={}",
                        node.hostname,
                        node.management_ip,
                        node.loopbacks.ipv4,
                        node.vrfs.size(),
                        ifaceCount,
                        prefixCount));
    }
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
                    rtsrv::log::err(std::format(
                        "[FAIL] AddRoute {}/{} via {} ({}): {}",
                        pfx.prefix,
                        iface.name,
                        iface.nexthop,
                        vrf.name,
                        result.error()));
                }
                else
                {
                    rtsrv::log::info(std::format(
                        "[OK] {} via {} iface={} metric={}  id={}...",
                        pfx.prefix,
                        iface.nexthop,
                        iface.name,
                        pfx.weight,
                        result->id().substr(0, 8)));
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
        rtsrv::log::warn(std::format(
            "[WARN] {} ({}): no SOT entry found — skipping",
            label,
            managementIp));
        return EXIT_FAILURE;
    }

    rtsrv::log::info(std::format("SOT node  : {} ({})",
                                 node->hostname, node->management_ip));
    rtsrv::log::info(std::format("Loopback  : ipv4={} ipv6={}",
                                 node->loopbacks.ipv4, node->loopbacks.ipv6));

    const std::size_t pushed = pushNodeRoutes(client, *node);
    rtsrv::log::info(std::format("=> {} route(s) pushed", pushed));

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
        rtsrv::log::info(std::format("Switch: {}  ({})", sw.label(),
                                     sw.target()));

        sra::RouteClient client(
            sw.target(), useTls, caCert, timeout, sw.login, sw.password);

        const int rc = syncOneServer(client, sw.ipv4, sw.label(), sot);
        results.push_back({sw.label(), rc});
    }

    // Aggregate summary
    rtsrv::log::info(std::format("SOT sync summary  ({} server(s))",
                                 switches.size()));
    bool anyFailed = false;
    for (const auto& r : results)
    {
        const bool ok = (r.exitCode == EXIT_SUCCESS);
        if (!ok)
        {
            anyFailed = true;
        }
        rtsrv::log::info(std::format("  {}  {}",
                                     ok ? "[OK  ]" : "[FAIL]", r.label));
    }
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
        rtsrv::log::info(std::format("Switch: {}  ({})  login={}",
                                     sw.label(), sw.target(), sw.login));

        sra::RouteClient client(
            sw.target(), useTls, caCert, timeout, sw.login, sw.password);

        const int rc = cmdTest(client);
        results.push_back({sw.label(), rc});
    }

    // Print aggregate summary
    rtsrv::log::info(std::format("Multi-server test summary  ({} server(s))",
                                 switches.size()));

    bool anyFailed = false;
    for (const auto& r : results)
    {
        const bool passed = (r.exitCode == EXIT_SUCCESS);
        if (!passed)
        {
            anyFailed = true;
        }
        rtsrv::log::info(std::format("  {}  {}",
                                     passed ? "[PASS]" : "[FAIL]", r.label));
    }

    return anyFailed ? EXIT_FAILURE : EXIT_SUCCESS;
}

// ---------------------------------------------------------------------------
// Netlink watch command
// ---------------------------------------------------------------------------

/**
 * @brief Maps a Linux rtnetlink protocol byte to the closest srmd
 * RouteProtocol.
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
    case RTPROT_OSPF:
        return srmd::v1::ROUTE_PROTOCOL_OSPF;
    case RTPROT_BGP:
        return srmd::v1::ROUTE_PROTOCOL_BGP;
    case RTPROT_RIP:
        return srmd::v1::ROUTE_PROTOCOL_RIP;
    case RTPROT_KERNEL:
        return srmd::v1::ROUTE_PROTOCOL_CONNECTED;
    case RTPROT_BOOT:
        return srmd::v1::ROUTE_PROTOCOL_CONNECTED;
    default:
        return srmd::v1::ROUTE_PROTOCOL_STATIC;
    }
}

/** @brief One member of a nexthop group, with its raw kernel weight. */
struct NhGroupMember
{
    uint32_t id{0};    ///< Nexthop ID of this group member
    uint8_t weight{0}; ///< Raw kernel weight; actual weight = weight + 1
};

/**
 * @brief A single row in the dynamic nexthop table.
 *
 * All fields mirror the corresponding @c netlink_nexthop_t members so the
 * table carries the complete nhmsg header and every NHA_* attribute.
 */
struct NexthopEntry
{
    /* ── From struct nhmsg ─────────────────────────────────────────────── */
    uint32_t id{0};      ///< NHA_ID: unique nexthop identifier
    uint8_t family{0};   ///< nh_family: AF_INET/AF_INET6/AF_UNSPEC
    uint8_t scope{0};    ///< nh_scope: RT_SCOPE_*
    uint8_t protocol{0}; ///< nh_protocol: RTPROT_*
    uint32_t flags{0};   ///< nh_flags: RTNH_F_* bitmask
    /* ── NHA_OIF ───────────────────────────────────────────────────────── */
    uint32_t oif{0};      ///< Output interface index (0 = absent)
    std::string oif_name; ///< Resolved interface name
    /* ── NHA_GATEWAY ───────────────────────────────────────────────────── */
    std::string gateway; ///< Gateway address string ("" = absent)
    /* ── NHA_BLACKHOLE ─────────────────────────────────────────────────── */
    uint8_t blackhole{0}; ///< 1 if this is a blackhole nexthop
    /* ── NHA_FDB ───────────────────────────────────────────────────────── */
    uint8_t fdb{0}; ///< 1 if nexthop is offloaded to FDB
    /* ── NHA_MASTER ────────────────────────────────────────────────────── */
    uint32_t master{0};      ///< Master device index (0 = absent)
    std::string master_name; ///< Resolved master device name
    /* ── NHA_GROUP ─────────────────────────────────────────────────────── */
    bool is_group{false}; ///< True when NHA_GROUP is present
    std::vector<NhGroupMember> group_members; ///< Group member IDs and weights
    /* ── NHA_GROUP_TYPE ────────────────────────────────────────────────── */
    uint16_t group_type{0}; ///< 0=mpath (ECMP), 1=resilient
    /* ── NHA_ENCAP_TYPE ────────────────────────────────────────────────── */
    uint16_t encap_type{0}; ///< Encapsulation type (LWTUNNEL_ENCAP_*)
};

/** @brief One resolved nexthop entry inside a WatchRoute. */
struct NhInfo
{
    std::string gateway; ///< Next-hop gateway IP ("" if none).
    std::string dev;     ///< Outgoing interface name ("" if none).
    uint32_t ifindex{0}; ///< Outgoing interface index (0 if absent).
    uint8_t weight{1};   ///< Actual nexthop weight (1 = equal-cost default).
};

/**
 * @brief A single entry in the dynamic /32 route table.
 *
 * One entry per destination — ECMP is represented as multiple NhInfo elements
 * in the nexthops list.  When the route uses a kernel nexthop object the nhid
 * field carries the object ID so callers can distinguish simple from ECMP.
 */
struct WatchRoute
{
    /* ── Destination ───────────────────────────────────────────────────── */
    std::string dest; ///< Destination prefix (e.g. "10.0.0.1/32").
    /* ── Nexthop resolution ─────────────────────────────────────────────── */
    uint32_t nhid{0};             ///< RTA_NH_ID nexthop object ID (0 = none).
    std::vector<NhInfo> nexthops; ///< Resolved nexthops (gateway, dev, weight).
    /* ── Route attributes ──────────────────────────────────────────────── */
    uint32_t metric{0}; ///< RTA_PRIORITY: route metric / preference.
    uint32_t table{0};  ///< RTA_TABLE / rtm_table: routing table ID.
    /* ── From struct rtmsg ─────────────────────────────────────────────── */
    uint8_t protocol{0}; ///< rtm_protocol: RTPROT_OSPF, RTPROT_ZEBRA, …
    /* ── gRPC tracking ─────────────────────────────────────────────────── */
    std::string srmdId; ///< Server-assigned ID (empty when push failed).
    /* ── Remaining struct rtmsg fields ──────────────────────────────────── */
    uint8_t family{0};  ///< rtm_family: AF_INET = 2.
    uint8_t dst_len{0}; ///< rtm_dst_len: prefix length (32 for host routes).
    uint8_t tos{0};     ///< rtm_tos: type-of-service filter value.
    uint8_t scope{0};   ///< rtm_scope: RT_SCOPE_* (universe/link/host/…).
    uint8_t type{0};    ///< rtm_type: RTN_UNICAST, RTN_BLACKHOLE, …
    uint32_t flags{0};  ///< rtm_flags: RTM_F_* bitmask.
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
    case RTPROT_KERNEL:
        return "kernel";
    case RTPROT_BOOT:
        return "boot";
    case RTPROT_STATIC:
        return "static";
    case RTPROT_OSPF:
        return "ospf";
    case RTPROT_BGP:
        return "bgp";
    case RTPROT_RIP:
        return "rip";
    case RTPROT_ZEBRA:
        return "zebra";
    default:
        return std::to_string(static_cast<int>(proto));
    }
}

/* Forward declarations for helpers defined later in this file. */
static std::string familyLabel(uint8_t family);
static std::string scopeLabel(uint8_t scope);
static std::vector<NhInfo>
resolveNhid(uint32_t nhid, const std::map<uint32_t, NexthopEntry>* nexthops);
// Populate-only nexthop callback — accepts std::map<uint32_t,NexthopEntry>*
// as user_data; defined after NexthopEntry (no NexthopCtx needed).
static void nlNhDumpToMapCb(netlink_nexthop_event_t event,
                            const netlink_nexthop_t* nh,
                            void* user_data);

/**
 * @brief Pretty-prints the dynamic /32 OSPF route table.
 *
 * One block per destination — ECMP routes list their nexthops indented below
 * the main route line, matching the output of "ip route show" with nhid:
 *
 * @code
 *  OSPF /32 Route Table  (2 route(s))
 *  ──────────────────────────────────────────────────────────────────────
 *  2.2.2.2/32  nhid 363  proto ospf  metric 20  table 254  id abcdef12…
 *    nexthop via 192.168.0.2  dev Ethernet46  weight 1
 *    nexthop via 192.168.0.6  dev Ethernet47  weight 1
 *  ──────────────────────────────────────────────────────────────────────
 * @endcode
 *
 * @param routes  Ordered map of destination → WatchRoute (one entry per dest).
 */
static void printRouteTable(const std::map<std::string, WatchRoute>& routes)
{
    const std::string sep(72, '-');

    std::println("\n OSPF /32 Route Table  ({} route(s))", routes.size());
    std::println("{}", sep);

    if (routes.empty())
    {
        std::println("  (empty)");
        std::println("{}", sep);
        return;
    }

    for (const auto& [key, r] : routes)
    {
        const std::string nhidStr =
            r.nhid ? std::format("nhid {}", r.nhid) : "nhid -";
        const std::string sid =
            r.srmdId.empty() ? "(pending)" : r.srmdId.substr(0, 8) + "…";

        std::println("  {}  {}  proto {}  metric {}  table {}  id {}",
                     r.dest,
                     nhidStr,
                     protoLabel(r.protocol),
                     r.metric,
                     r.table ? std::to_string(r.table) : "-",
                     sid);

        if (r.nexthops.empty())
        {
            std::println("    (no nexthop info)");
        }
        else
        {
            for (const auto& nh : r.nexthops)
            {
                const std::string gw =
                    nh.gateway.empty() ? "-" : "via " + nh.gateway;
                const std::string dev = nh.dev.empty() ? "-" : "dev " + nh.dev;
                std::println("    nexthop {}  {}  weight {}",
                             gw,
                             dev,
                             static_cast<unsigned>(nh.weight));
            }
        }
    }

    std::println("{}", sep);
}

/**
 * @brief Serializes the route table to a text snapshot for UDP delivery.
 *
 * Format:
 * @code
 *   ROUTES <count>
 *   dest=<prefix> nhid=<n> metric=<m> table=<t> proto=<p> id=<srmd-id>
 *     nexthop gw=<gw> dev=<dev> weight=<w>
 *     …
 * @endcode
 */
static std::string
serializeRouteTable(const std::map<std::string, WatchRoute>& routes)
{
    std::ostringstream os;
    os << "ROUTES " << routes.size() << '\n';
    for (const auto& [key, r] : routes)
    {
        os << std::format(
            "dest={} nhid={} metric={} table={} proto={}"
            " family={} dst_len={} tos={} scope={} type={} flags=0x{:08x}"
            " id={}\n",
            r.dest,
            r.nhid,
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

/** @brief Per-loopback entry saved from GetRemainingLoopbacks, keyed by
 *  loopback_ipv4 string.  Written once during startup (before the monitor
 *  thread starts), then read-only from the callback. */
struct LoopbackCbEntry
{
    std::string hostname;
    std::vector<cmdproto::PrefixIpv4> prefixes; ///< Pre-parsed prefix list
    std::vector<std::string>
        prefix_strings;    ///< Original CIDR strings (for Redis)
    uint32_t last_nhid{0}; ///< nhid last sent to ud_server
};

/** @brief Global map from loopback_ipv4 → LoopbackCbEntry.
 *  Populated after GetRemainingLoopbacks completes; read by nlWatchCb and
 *  addDelListOspfCb. */
static std::map<std::string, LoopbackCbEntry> g_loopback_cb_map;

/**
 * @brief Global ECMP nexthop group manager.
 *
 * One EcmpGroup is created for every remote loopback returned by
 * GetRemainingLoopbacks / getRemainingNodes() during SRA startup.  As netlink
 * /32 OSPF route events arrive the corresponding group's member list is
 * updated and its group_nhid() is used as the nexthop_id_ipv4 in each
 * ud_server ROUTE_ADD command.
 *
 * Written once (create() calls) before the netlink monitor thread starts;
 * add_member() / remove_member() / update_member() are called exclusively
 * from the single netlink callback thread, so no concurrent writes occur.
 */
static sra::NexhopGroupManager g_ecmp_groups;

/**
 * @brief Logs the complete ECMP nexthop group table to the application logger.
 *
 * Renders a fixed-width ASCII table with one row per managed loopback.
 * Each row shows the loopback IP, the remote hostname, the number of kernel
 * nexthop object IDs currently in the group, the full comma-separated ID
 * list (with per-member weights when weight ≠ 1), and the effective nhid
 * that will be placed in the next ud_server ROUTE_ADD command.
 *
 * Intended to be called immediately after every /32 OSPF netlink event so
 * the operator can observe the live state of every ECMP group in a single
 * snapshot.
 *
 * Example output:
 * @code
 *   [Watch] ECMP group table (2 group(s)) — after OSPF /32 ADD 2.2.2.2/32
 *   ────────────────────────────────────────────────────────────────────────────
 *     Loopback           Hostname               Members  Nhids               Effective
 *   ────────────────────────────────────────────────────────────────────────────
 *     2.2.2.2            spine-01                     1  363                       363
 *     3.3.3.3            spine-02                     0  (none)                      -
 *   ────────────────────────────────────────────────────────────────────────────
 * @endcode
 *
 * @param trigger  Human-readable description of the event that caused this
 *                 snapshot (e.g. @c "ADD 2.2.2.2/32" or @c "DEL 3.3.3.3/32").
 * @param logctx   Log-context prefix prepended to every log line
 *                 (e.g. @c "[Watch]" or @c "[add-del-list]").
 */
static void logEcmpGroupTable(const std::string& trigger,
                               const std::string& logctx = "[ECMP]")
{
    constexpr std::size_t SEP_LEN = 80;
    const std::string sep(SEP_LEN, '-');
    const std::size_t total = g_ecmp_groups.size();

    rtsrv::log::info(std::format(
        "{} ECMP group table ({} group(s)) — after OSPF /32 {}",
        logctx, total, trigger));
    rtsrv::log::info(sep);
    rtsrv::log::info(std::format(
        "  {:<18} {:<22} {:>7}  {:<32}  {:>10}",
        "Loopback", "Hostname", "Members", "OSPF nhid → SRA nhid", "Group nhid"));
    rtsrv::log::info(sep);

    if (total == 0)
    {
        rtsrv::log::info(std::format("{}   (no groups registered)", logctx));
    }
    else
    {
        g_ecmp_groups.for_each(
            [&](const std::string& loopback, const sra::EcmpGroup& g)
            {
                /* Build member list: "100→200,101→201(w2)" per entry. */
                std::string nhidList;
                for (const auto& m : g.members)
                {
                    if (!nhidList.empty())
                        nhidList += ',';
                    nhidList += std::format("{}→{}", m.ospf_nhid, m.sra_nhid);
                    if (m.weight != 1)
                        nhidList += std::format("(w{})", m.weight);
                }
                if (nhidList.empty())
                    nhidList = "(none)";

                const uint32_t eff = g.group_nhid();
                const std::string effStr = eff ? std::to_string(eff) : "-";

                rtsrv::log::info(std::format(
                    "  {:<18} {:<22} {:>7}  {:<32}  {:>10}",
                    loopback,
                    g.hostname,
                    g.members.size(),
                    nhidList,
                    effStr));
            });
    }

    rtsrv::log::info(sep);
}

/**
 * @brief Context forwarded via the @c user_data pointer to the netlink
 * callback.
 *
 * Carries the live gRPC client and the dynamic route table that tracks every
 * /32 OSPF route event together with its srmd server ID.
 */
struct WatchCtx
{
    sra::RouteClient* client;
    std::map<std::string, WatchRoute>
        routes;           ///< dest → WatchRoute (one per dest)
    std::string loopback; ///< Node's own loopback IP for GetLoopbacks
    sra::UdpTableServer*
        udpServer; ///< UDP publisher (port 9003); may be nullptr
    std::map<uint32_t, NexthopEntry>
        nexthopTable; ///< nhid → entry for resolving ECMP
    sra::SraUdpClient* sraClient{
        nullptr}; ///< ud_server client for ROUTE_ADD (may be nullptr)
};

/**
 * @brief Netlink callback: translates each /32 route event into a gRPC call.
 *
 * Called by @c netlink_run() for every IPv4 /32 unicast route event that
 * passes the kernel filter.  Behaviour per event type:
 *
 * - NETLINK_ROUTE_ADDED   → AddRoute RPC; stores the returned server ID.
 * - NETLINK_ROUTE_CHANGED → RemoveRoute (if we hold an ID for that dest) then
 *                           AddRoute with the new attributes; updates stored
 * ID.
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
static void nlWatchCb(netlink_event_t event,
                      const netlink_route32_t* route,
                      void* user_data)
{
    auto* ctx = static_cast<WatchCtx*>(user_data);

    /* UTC timestamp */
    const auto now = std::chrono::system_clock::now();
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                        now.time_since_epoch())
                        .count();
    const std::time_t sec = static_cast<std::time_t>(us / 1'000'000);
    const int ms = static_cast<int>((us % 1'000'000) / 1000);
    std::tm tm_utc{};
    gmtime_r(&sec, &tm_utc);
    const std::string ts = std::format("{:02d}:{:02d}:{:02d}.{:03d}",
                                       tm_utc.tm_hour,
                                       tm_utc.tm_min,
                                       tm_utc.tm_sec,
                                       ms);

    /* Destination string "x.x.x.x/<len>" */
    char dst_buf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &route->dst, dst_buf, sizeof(dst_buf));
    const std::string dest =
        std::string(dst_buf) + "/" +
        std::to_string(static_cast<unsigned>(route->dst_len));

    /* Gateway string (empty when nexthop is 0.0.0.0) */
    char gw_buf[INET_ADDRSTRLEN] = {};
    if (route->gateway.s_addr != 0)
    {
        inet_ntop(AF_INET, &route->gateway, gw_buf, sizeof(gw_buf));
    }
    const std::string gw = gw_buf;
    const std::string iface = route->ifname;

    const srmd::v1::RouteProtocol proto = mapRtProtocol(route->protocol);

    /* Helper: build a WatchRoute from this netlink message, resolving nhid
     * from the context's nexthop table when available. */
    auto makeWatchRoute = [&](const std::string& srmdId) -> WatchRoute {
        WatchRoute wr;
        wr.dest = dest;
        wr.nhid = route->nhid;
        wr.metric = route->metric;
        wr.table = route->table;
        wr.protocol = route->protocol;
        wr.srmdId = srmdId;
        wr.family = route->family;
        wr.dst_len = route->dst_len;
        wr.tos = route->tos;
        wr.scope = route->scope;
        wr.type = route->type;
        wr.flags = route->flags;
        if (route->nhid != 0)
            wr.nexthops = resolveNhid(route->nhid, &ctx->nexthopTable);
        if (wr.nexthops.empty() && (!gw.empty() || !iface.empty()))
            wr.nexthops.push_back({gw, iface, route->ifindex, 1});
        return wr;
    };

    /* Helper: for every RTM_NEWROUTE / RTM_DELROUTE event whose destination
     * matches a known remaining loopback, update the ECMP group membership
     * and submit a ROUTE_ADD to ud_server using the group's effective nhid.
     *
     * ADDED  → add_member()    : new path arrived, extend the ECMP group.
     * CHANGED→ update_member() : kernel reassigned the nhid; replace members.
     * REMOVED→ remove_member() : path gone; re-programme with remaining nhid
     *                            (if any still present in the group).
     */
    auto sendRouteAdd = [&](netlink_event_t ev, uint32_t nhid) {
        if (!ctx->sraClient)
            return;
        // Strip the prefix length ("/32") to obtain the loopback key.
        const auto slash = dest.rfind('/');
        const std::string loKey =
            (slash != std::string::npos) ? dest.substr(0, slash) : dest;
        auto it = g_loopback_cb_map.find(loKey);
        if (it == g_loopback_cb_map.end())
            return;
        LoopbackCbEntry& entry = it->second;

        // Update the SRA-owned kernel ECMP group according to the event type.
        // add_member / update_member return the kernel group nhid directly.
        const uint32_t prevGroupNhid = g_ecmp_groups.group_nhid(loKey);
        uint32_t groupNhid = 0;
        if (ev == NETLINK_ROUTE_ADDED)
            groupNhid = g_ecmp_groups.add_member(loKey, nhid);
        else if (ev == NETLINK_ROUTE_CHANGED)
            groupNhid = g_ecmp_groups.update_member(loKey, nhid);
        else // NETLINK_ROUTE_REMOVED
        {
            g_ecmp_groups.remove_member(loKey, nhid);
            groupNhid = g_ecmp_groups.group_nhid(loKey);
        }

        // Log when SRA creates a brand-new multi-path nexthop group object.
        if (prevGroupNhid == 0 && groupNhid != 0)
        {
            rtsrv::log::info(std::format(
                "{} [OSPF/32] *** SRA created new ECMP multi-path nexthop:"
                " loopback='{}' kernel_nhid={} members={}"
                " (RTM_NEWNEXTHOP NLM_F_CREATE, kernel-assigned NHA_ID)",
                ts, loKey, groupNhid, g_ecmp_groups.get(loKey).size()));
        }

        const char* evLabel = (ev == NETLINK_ROUTE_ADDED)     ? "ADD"
                              : (ev == NETLINK_ROUTE_REMOVED) ? "DEL"
                                                              : "CHG";
        rtsrv::log::info(std::format(
            "{} [OSPF/32] {} loopback='{}' (hostname='{}') ospf_nhid={}"
            " ecmp_members={} sra_group_nhid={} — {}",
            ts,
            evLabel,
            loKey,
            entry.hostname,
            nhid,
            g_ecmp_groups.get(loKey).size(),
            groupNhid,
            groupNhid ? "sending ROUTE_ADD" : "group empty, skipping"));

        if (groupNhid == 0)
            return; // group emptied or kernel error — skip ROUTE_ADD

        if (entry.prefixes.empty())
        {
            rtsrv::log::info(std::format(
                "{} [OSPF/32] no prefixes for '{}' — skipping ROUTE_ADD",
                ts,
                loKey));
            return;
        }
        struct in_addr nhAddr
        {};
        ::inet_pton(AF_INET, loKey.c_str(), &nhAddr);
        const auto* nb = reinterpret_cast<const std::uint8_t*>(&nhAddr.s_addr);
        cmdproto::Interface iface_entry{};
        static constexpr std::string_view kUnneeded = "unneeded";
        for (std::size_t k = 0;
             k < cmdproto::IFACE_NAME_SIZE && k < kUnneeded.size();
             ++k)
            iface_entry.iface_name[k] = kUnneeded[k];
        iface_entry.nexthop_addr_ipv4 = {nb[0], nb[1], nb[2], nb[3]};
        iface_entry.nexthop_id_ipv4 = groupNhid; // SRA-owned kernel group id
        iface_entry.prefixes = entry.prefixes;
        cmdproto::SingleRouteRequest req;
        req.vrfs_name = "RemainLoopbaks";
        req.interfaces.push_back(std::move(iface_entry));
        rtsrv::log::info(std::format(
            "{} [OSPF/32] submitting ROUTE_ADD for '{}' sra_group_nhid={}"
            " prefixes={}",
            ts,
            loKey,
            groupNhid,
            req.interfaces[0].prefixes.size()));
        ctx->sraClient->submitAdd(std::move(req));
    };

    // only for OSPF now
    if (proto == srmd::v1::ROUTE_PROTOCOL_OSPF)
    {
        /* Key is the destination only — one entry per /32 prefix. */
        const std::string key = dest;

        /* ---- ADDED ----------------------------------------------------------
         */
        if (event == NETLINK_ROUTE_ADDED)
        {
            auto result = ctx->client->addRoute(dest,
                                                gw,
                                                iface,
                                                route->metric,
                                                srmd::v1::ADDRESS_FAMILY_IPV4,
                                                proto);

            if (result)
            {
                ctx->routes[key] = makeWatchRoute(result->id());
                rtsrv::log::info(std::format(
                    "{} [ADDED]   {} via {} dev {} metric {} -> id={}...",
                    ts, dest, gw, iface, route->metric,
                    result->id().substr(0, 8)));

                /* If we know our own loopback, ask the server for the full
                 * interface+prefix list and log the entry matching this
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
                            rtsrv::log::info(std::format(
                                "{}   SOT nexthop {} -> iface \"{}\" "
                                "(type={} local={} weight={} desc={})",
                                ts, gw, intf.name(), intf.type(),
                                intf.local_address(), intf.weight(),
                                intf.description()));
                            for (const auto& pfx : intf.prefixes())
                            {
                                rtsrv::log::info(std::format(
                                    "{}     prefix {} weight={} role={} "
                                    "desc={}",
                                    ts, pfx.prefix(), pfx.weight(),
                                    pfx.role(), pfx.description()));
                            }
                        }
                        if (!found)
                        {
                            rtsrv::log::info(std::format(
                                "{} [ADDED]   SOT: no interface with nexthop "
                                "{} found for loopback {}",
                                ts, gw, ctx->loopback));
                        }
                    }
                    else
                    {
                        rtsrv::log::warn(std::format(
                            "{} [ADDED]   GetLoopbacks FAILED: {}",
                            ts, lbResult.error()));
                    }
                }
            }
            else
            {
                /* Track with empty srmdId so REMOVED events still clean up. */
                ctx->routes[key] = makeWatchRoute({});
                rtsrv::log::err(std::format(
                    "{} [ADDED]   {} -> gRPC FAILED: {}",
                    ts, dest, result.error()));
            }
            sendRouteAdd(NETLINK_ROUTE_ADDED, route->nhid);
        }
        /* ---- CHANGED --------------------------------------------------------
         */
        else if (event == NETLINK_ROUTE_CHANGED)
        {
            /* Remove the existing entry for this destination from srmd. */
            auto existing = ctx->routes.find(key);
            if (existing != ctx->routes.end() &&
                !existing->second.srmdId.empty())
            {
                auto rmResult =
                    ctx->client->removeRoute(existing->second.srmdId);
                if (!rmResult)
                {
                    rtsrv::log::warn(std::format(
                        "{} [CHANGED] {} stale-remove failed: {}",
                        ts, dest, rmResult.error()));
                }
                ctx->routes.erase(existing);
            }

            auto result = ctx->client->addRoute(dest,
                                                gw,
                                                iface,
                                                route->metric,
                                                srmd::v1::ADDRESS_FAMILY_IPV4,
                                                proto);

            if (result)
            {
                ctx->routes[key] = makeWatchRoute(result->id());
                rtsrv::log::info(std::format(
                    "{} [CHANGED] {} via {} dev {} metric {} -> id={}...",
                    ts, dest, gw, iface, route->metric,
                    result->id().substr(0, 8)));
            }
            else
            {
                ctx->routes[key] = makeWatchRoute({});
                rtsrv::log::err(std::format(
                    "{} [CHANGED] {} -> gRPC FAILED: {}",
                    ts, dest, result.error()));
            }
            sendRouteAdd(NETLINK_ROUTE_CHANGED, route->nhid);
        }
        /* ---- REMOVED --------------------------------------------------------
         */
        else
        {
            auto it = ctx->routes.find(key);
            if (it == ctx->routes.end())
            {
                rtsrv::log::info(std::format(
                    "{} [REMOVED] {} (not tracked — no gRPC call)", ts, dest));
            }
            else
            {
                const std::string id = it->second.srmdId;
                ctx->routes.erase(it);

                if (id.empty())
                {
                    rtsrv::log::info(std::format(
                        "{} [REMOVED] {} (no server ID — no gRPC call)",
                        ts, dest));
                }
                else
                {
                    auto result = ctx->client->removeRoute(id);
                    if (result)
                    {
                        rtsrv::log::info(std::format(
                            "{} [REMOVED] {} id={}...",
                            ts, dest, id.substr(0, 8)));
                    }
                    else
                    {
                        rtsrv::log::err(std::format(
                            "{} [REMOVED] {} -> gRPC FAILED: {}",
                            ts, dest, result.error()));
                    }
                }
            }
            sendRouteAdd(NETLINK_ROUTE_REMOVED, route->nhid);
        }

        /* Log the ECMP group table snapshot after every OSPF /32 event. */
        {
            const char* evStr = (event == NETLINK_ROUTE_ADDED)     ? "ADD"
                                : (event == NETLINK_ROUTE_REMOVED) ? "DEL"
                                                                    : "CHG";
            logEcmpGroupTable(std::format("{} {}", evStr, dest), "[Watch]");
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
static volatile int g_startup_route_fd = -1;
static volatile int g_startup_neigh_fd = -1;
static volatile int g_startup_nexthop_fd = -1;

/* File-scope fd used by the signal handler to unblock netlink_run(). */
static volatile int g_watch_fd = -1;

/** @brief Stop flag for the 'run' daemon command (set by signal handler). */
static volatile sig_atomic_t g_run_stop = 0;

/* Thread handles for the three background netlink monitors.
 * Set after thread creation; zeroed by StartupGuard before join().
 * Used by signal handlers to pthread_kill() sleeping threads so their
 * blocking recv() returns EINTR, after which the fd is already closed
 * (EBADF on retry) and netlink_*_run() exits. */
static volatile pthread_t g_startup_route_tid = 0;
static volatile pthread_t g_startup_neigh_tid = 0;
static volatile pthread_t g_startup_nexthop_tid = 0;

/* One-shot flag: prevents cascading pthread_kill loops when the signal
 * handler is re-entered on a background thread. */
static volatile sig_atomic_t g_bg_threads_interrupted = 0;

/** @brief Sends SIGINT to each background netlink thread (once).
 *
 *  shutdown(SHUT_RD) on AF_NETLINK sockets returns EOPNOTSUPP on Linux,
 *  and close() alone does not unblock a blocking recv() in another thread.
 *  pthread_kill() is the only async-signal-safe way to deliver EINTR to the
 *  target thread so that the netlink_*_run() loop retries recv() on the
 *  now-closed fd and gets EBADF, causing a clean exit. */
static void interruptBackgroundThreads() noexcept
{
    if (g_bg_threads_interrupted)
        return;
    g_bg_threads_interrupted = 1;
    const pthread_t self = ::pthread_self();
    if (g_startup_route_tid && g_startup_route_tid != self)
        ::pthread_kill(static_cast<pthread_t>(g_startup_route_tid), SIGINT);
    if (g_startup_neigh_tid && g_startup_neigh_tid != self)
        ::pthread_kill(static_cast<pthread_t>(g_startup_neigh_tid), SIGINT);
    if (g_startup_nexthop_tid && g_startup_nexthop_tid != self)
        ::pthread_kill(static_cast<pthread_t>(g_startup_nexthop_tid), SIGINT);
}

/** @brief SIGINT/SIGTERM handler for the 'run' daemon command.
 *
 *  Sets g_run_stop and closes startup netlink fds so all background
 *  monitor threads unblock and can be joined cleanly. */
static void runSigHandler(int /*signo*/)
{
    g_run_stop = 1;
    if (g_startup_route_fd >= 0)
    {
        ::shutdown(g_startup_route_fd, SHUT_RD);
        netlink_close(g_startup_route_fd);
        g_startup_route_fd = -1;
    }
    if (g_startup_neigh_fd >= 0)
    {
        ::shutdown(g_startup_neigh_fd, SHUT_RD);
        netlink_neigh_close(g_startup_neigh_fd);
        g_startup_neigh_fd = -1;
    }
    if (g_startup_nexthop_fd >= 0)
    {
        ::shutdown(g_startup_nexthop_fd, SHUT_RD);
        netlink_nexthop_close(g_startup_nexthop_fd);
        g_startup_nexthop_fd = -1;
    }
    interruptBackgroundThreads();
}

/** @brief SIGINT/SIGTERM handler for the watch command.
 *
 *  Closes the watch fd AND the startup neighbor/nexthop fds so all
 *  background threads unblock and can be joined. */
static void watchSigHandler(int /*signo*/)
{
    if (g_watch_fd >= 0)
    {
        ::shutdown(g_watch_fd, SHUT_RD);
        netlink_close(g_watch_fd);
        g_watch_fd = -1;
    }
    if (g_startup_neigh_fd >= 0)
    {
        ::shutdown(g_startup_neigh_fd, SHUT_RD);
        netlink_neigh_close(g_startup_neigh_fd);
        g_startup_neigh_fd = -1;
    }
    if (g_startup_nexthop_fd >= 0)
    {
        ::shutdown(g_startup_nexthop_fd, SHUT_RD);
        netlink_nexthop_close(g_startup_nexthop_fd);
        g_startup_nexthop_fd = -1;
    }
    interruptBackgroundThreads();
}

/** @brief Stop flag for the grpc-proc-demo command (set by signal handler). */
static volatile sig_atomic_t g_demo_stop = 0;

/** @brief SIGINT/SIGTERM handler for the grpc-proc-demo command. */
static void demoSigHandler(int /*signo*/)
{
    g_demo_stop = 1;
}

/** @brief Stop flag for the 'add-del-list' daemon command (set by signal
 * handler). */
static volatile sig_atomic_t g_add_del_list_stop = 0;

/** @brief Netlink fd used by the add-del-list OSPF monitor thread.
 *  Closed by the signal handler to unblock netlink_run(). */
static volatile int g_add_del_list_nl_fd = -1;

/** @brief pthread handle for the OSPF netlink monitor thread.
 *  Set after thread creation so the signal handler can pthread_kill() it.
 *  close() alone does not unblock a blocking recv() in another thread on
 *  Linux; pthread_kill(SIGINT) delivers EINTR so netlink_run() retries
 *  recv() on the now-closed fd and gets EBADF, causing a clean exit. */
static volatile pthread_t g_add_del_list_nl_tid = 0;

/** @brief Context forwarded to addDelListOspfCb via user_data. */
struct AddDelListCbCtx
{
    sra::SraUdpClient* vrfClient{nullptr};
    sra::RedisRib* redisRib{nullptr};
};

static AddDelListCbCtx g_add_del_list_cb_ctx;

/** @brief SIGINT/SIGTERM handler for the 'add-del-list' command. */
static void addDelListSigHandler(int /*signo*/)
{
    g_add_del_list_stop = 1;
    const int fd = g_add_del_list_nl_fd;
    if (fd >= 0)
    {
        g_add_del_list_nl_fd = -1;
        netlink_close(fd);
    }
    const pthread_t tid = g_add_del_list_nl_tid;
    if (tid && tid != ::pthread_self())
        ::pthread_kill(tid, SIGINT);
}

/**
 * @brief Netlink callback for /32 OSPF route events in add-del-list mode.
 *
 * Invoked by netlink_run() for every IPv4 /32 unicast route event.
 * Events with a protocol other than RTPROT_OSPF are silently ignored so
 * that only OSPF host-routes produce log output.
 *
 * @param event      NETLINK_ROUTE_ADDED, _CHANGED, or _REMOVED.  All three
 *                   trigger a ROUTE_ADD to ud_server when the destination
 *                   matches a known remaining loopback.
 * @param route      Parsed /32 route descriptor; valid for this call only.
 * @param user_data  Pointer to the global AddDelListCbCtx (vrfClient +
 * redisRib).
 */
static void addDelListOspfCb(netlink_event_t event,
                             const netlink_route32_t* route,
                             void* user_data)
{
    if (route->protocol != RTPROT_OSPF)
        return;

    const char* evLabel = (event == NETLINK_ROUTE_ADDED)     ? "ADD"
                          : (event == NETLINK_ROUTE_REMOVED) ? "DEL"
                                                             : "CHG";
    char dst[INET_ADDRSTRLEN];
    char gw[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &route->dst, dst, sizeof(dst));
    if (route->gateway.s_addr != 0)
        inet_ntop(AF_INET, &route->gateway, gw, sizeof(gw));

    rtsrv::log::info(std::format(
        "[add-del-list] [OSPF/32] {} {}/32 via {} dev {} metric {}",
        evLabel,
        dst,
        gw[0] ? gw : "(none)",
        route->ifname[0] ? route->ifname : "?",
        route->metric));

    auto it = g_loopback_cb_map.find(dst);
    if (it == g_loopback_cb_map.end())
        return;

    LoopbackCbEntry& entry = it->second;

    auto* ctx = static_cast<AddDelListCbCtx*>(user_data);
    if (!ctx || !ctx->vrfClient)
        return;

    // Update the SRA-owned kernel ECMP group and obtain its NHA_ID.
    const uint32_t prevGroupNhid = g_ecmp_groups.group_nhid(dst);
    uint32_t groupNhid = 0;
    if (event == NETLINK_ROUTE_ADDED)
        groupNhid = g_ecmp_groups.add_member(dst, route->nhid);
    else if (event == NETLINK_ROUTE_CHANGED)
        groupNhid = g_ecmp_groups.update_member(dst, route->nhid);
    else // NETLINK_ROUTE_REMOVED
    {
        g_ecmp_groups.remove_member(dst, route->nhid);
        groupNhid = g_ecmp_groups.group_nhid(dst);
    }

    // Log when SRA creates a brand-new multi-path nexthop group object.
    if (prevGroupNhid == 0 && groupNhid != 0)
    {
        rtsrv::log::info(std::format(
            "[add-del-list] *** SRA created new ECMP multi-path nexthop:"
            " loopback='{}' kernel_nhid={} members={}"
            " (RTM_NEWNEXTHOP NLM_F_CREATE, kernel-assigned NHA_ID)",
            dst, groupNhid, g_ecmp_groups.get(dst).size()));
    }

    rtsrv::log::info(std::format(
        "[add-del-list] [OSPF/32] {} loopback='{}' (hostname='{}') ospf_nhid={}"
        " ecmp_members={} sra_group_nhid={} — {}",
        evLabel,
        dst,
        entry.hostname,
        route->nhid,
        g_ecmp_groups.get(dst).size(),
        groupNhid,
        groupNhid ? "sending ROUTE_ADD" : "group empty, skipping"));

    if (groupNhid == 0)
        return; // group emptied or kernel error — skip ROUTE_ADD

    if (entry.prefixes.empty())
    {
        rtsrv::log::info(std::format(
            "[add-del-list] [OSPF/32] no prefixes for '{}' — "
            "skipping ROUTE_ADD",
            dst));
        return;
    }

    // Build nexthop address bytes from the loopback IP (dst).
    struct in_addr nhAddr
    {};
    ::inet_pton(AF_INET, dst, &nhAddr);
    const auto* nb = reinterpret_cast<const std::uint8_t*>(&nhAddr.s_addr);

    cmdproto::Interface iface{};
    static constexpr std::string_view kUnneeded = "unneeded";
    for (std::size_t k = 0;
         k < cmdproto::IFACE_NAME_SIZE && k < kUnneeded.size();
         ++k)
        iface.iface_name[k] = kUnneeded[k];
    iface.nexthop_addr_ipv4 = {nb[0], nb[1], nb[2], nb[3]};
    iface.nexthop_id_ipv4 = groupNhid; // SRA-owned kernel group NHA_ID
    iface.prefixes = entry.prefixes;

    cmdproto::SingleRouteRequest req;
    req.vrfs_name = "RemainLoopbaks";
    req.interfaces.push_back(std::move(iface));

    rtsrv::log::info(std::format(
        "[add-del-list] [OSPF/32] submitting ROUTE_ADD for '{}'"
        " sra_group_nhid={} prefixes={}",
        dst,
        groupNhid,
        req.interfaces[0].prefixes.size()));

    if (ctx->redisRib && ctx->redisRib->connected())
    {
        for (const auto& pfxStr : entry.prefix_strings)
            ctx->redisRib->set(pfxStr, std::to_string(groupNhid));
    }

    ctx->vrfClient->submitAdd(std::move(req));

    /* Log the full ECMP group table after every OSPF /32 event so the
     * operator can track every group's membership in a single snapshot. */
    logEcmpGroupTable(std::format("{} {}/32", evLabel, dst), "[add-del-list]");
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
    rtsrv::log::info(
        "[Watch] Reading /32 OSPF routes from kernel routing table...");

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
                if (kr.prefixLen != 32 || kr.protocol != RTPROT_OSPF ||
                    kr.type != RTN_UNICAST)
                {
                    continue;
                }

                /* Push to srmd so it is reflected in the server's route table.
                 */
                const std::string firstGw = kr.nexthops.empty()
                                                ? std::string{}
                                                : kr.nexthops[0].gateway;
                const std::string firstIface =
                    kr.nexthops.empty() ? std::string{}
                                        : kr.nexthops[0].interfaceName;
                auto result = client.addRoute(kr.destination,
                                              firstGw,
                                              firstIface,
                                              kr.metric,
                                              srmd::v1::ADDRESS_FAMILY_IPV4,
                                              srmd::v1::ROUTE_PROTOCOL_OSPF);

                WatchRoute wr;
                wr.dest = kr.destination;
                wr.nhid = kr.nhid;
                wr.metric = kr.metric;
                wr.table = kr.table;
                wr.protocol = kr.protocol;
                wr.family = static_cast<uint8_t>(kr.family);
                wr.dst_len = kr.prefixLen;
                wr.scope = kr.scope;
                wr.type = kr.type;
                if (!kr.nexthops.empty())
                {
                    for (const auto& knh : kr.nexthops)
                        wr.nexthops.push_back({knh.gateway,
                                               knh.interfaceName,
                                               knh.interfaceIndex,
                                               knh.weight});
                }
                else if (kr.nhid != 0)
                {
                    wr.nexthops = resolveNhid(kr.nhid, &ctx.nexthopTable);
                }
                if (result)
                {
                    wr.srmdId = result->id();
                }
                else
                {
                    rtsrv::log::err(std::format(
                        "[Startup]   AddRoute {} failed: {}",
                        kr.destination,
                        result.error()));
                }
                ctx.routes[kr.destination] = std::move(wr);
                ++found;
            }
            rtsrv::log::info(std::format(
                "[Startup] Found {} /32 OSPF route(s) in kernel.", found));
        }
        else
        {
            rtsrv::log::warn(std::format(
                "[Startup] could not read kernel routes: {}",
                routesResult.error()));
        }
    }
    catch (const std::exception& e)
    {
        rtsrv::log::warn(std::format(
            "[Startup] RoutingManager init failed: {}", e.what()));
    }

    // ── Step 1b: Dump kernel nexthop table for nhid resolution ───────────
    {
        int nhfd = netlink_nexthop_init();
        if (nhfd >= 0)
        {
            netlink_nexthop_dump(nhfd, nlNhDumpToMapCb, &ctx.nexthopTable);
            netlink_nexthop_close(nhfd);
        }
    }
    // Re-resolve any nhid-based routes now that the nexthop table is ready.
    for (auto& [key, wr] : ctx.routes)
    {
        if (wr.nhid != 0 && wr.nexthops.empty())
            wr.nexthops = resolveNhid(wr.nhid, &ctx.nexthopTable);
    }

    // ── Step 2: Publish initial route table via UDP (port 9003) ─────────
    udpServer.setRouteData(serializeRouteTable(ctx.routes));

    // ── Step 3: Request loopback from server ─────────────────────────────
    rtsrv::log::info("[Startup] Requesting loopback from server...");
    auto lbResult = client.requestLoopback();
    if (lbResult)
    {
        ctx.loopback = *lbResult;
        rtsrv::log::info(std::format("[Startup] Loopback from server: '{}'",
                                     ctx.loopback));
    }
    else
    {
        rtsrv::log::info(std::format(
            "[Startup] No loopback from server ({}); using config loopback: '{}'",
            lbResult.error(),
            ctx.loopback));
    }

    // ── Step 3b: GetLoopbacks then GetAllRoutes ───────────────────────────
    if (!ctx.loopback.empty())
    {
        rtsrv::log::info(std::format(
            "[Startup] GetLoopbacks for loopback '{}'...", ctx.loopback));
        auto glResult = client.getLoopbacks(ctx.loopback);
        if (glResult)
        {
            rtsrv::log::info(std::format(
                "[Startup] GetLoopbacks: {} — {} interface(s)",
                glResult->message(),
                glResult->interfaces_size()));
        }
        else
        {
            rtsrv::log::warn(std::format("[Startup] GetLoopbacks failed: {}",
                                         glResult.error()));
        }

        rtsrv::log::info("[Startup] GetAllRoutes...");
        auto arResult = client.getAllRoutes();
        if (!arResult)
        {
            rtsrv::log::warn(std::format("[Startup] GetAllRoutes failed: {}",
                                         arResult.error()));
        }
    }

    // ── Step 3c: GetRemainingLoopbacks → populate global loopback map ────
    {
        rtsrv::log::info(
            "[Watch] fetching remaining nodes for loopback map...");
        auto rnResult = client.getRemainingNodes();
        if (!rnResult)
        {
            rtsrv::log::warn(std::format("[Watch] GetRemainingNodes failed: {}",
                                         rnResult.error()));
        }
        else
        {
            rtsrv::log::info(std::format("[Watch] remaining nodes: {}",
                                         rnResult->nodes_size()));
            for (const auto& node : rnResult->nodes())
            {
                auto nodeGl = client.getLoopbacksByNodeIp(node.management_ip());
                if (!nodeGl)
                {
                    rtsrv::log::warn(std::format(
                        "[Watch]   GetLoopbacksByNodeIp '{}' failed: {}",
                        node.management_ip(),
                        nodeGl.error()));
                    continue;
                }
                LoopbackCbEntry cbEntry;
                cbEntry.hostname = node.hostname();
                for (const auto& pfx : nodeGl->prefixes())
                {
                    const std::string& pfxStr = pfx.prefix();
                    const auto slash = pfxStr.rfind('/');
                    if (slash == std::string::npos)
                        continue;
                    struct in_addr pfxAddr
                    {};
                    if (::inet_pton(AF_INET,
                                    pfxStr.substr(0, slash).c_str(),
                                    &pfxAddr) != 1)
                        continue;
                    const auto* pb =
                        reinterpret_cast<const std::uint8_t*>(&pfxAddr.s_addr);
                    const auto maskLen = static_cast<std::uint8_t>(
                        std::stoul(pfxStr.substr(slash + 1)));
                    cbEntry.prefixes.push_back(cmdproto::PrefixIpv4{
                        {pb[0], pb[1], pb[2], pb[3]}, maskLen});
                    cbEntry.prefix_strings.push_back(pfxStr);
                }
                // Seed last_nhid from the kernel snapshot in ctx.routes.
                const std::string& loopback = node.loopback_ipv4();
                for (const auto& [rdest, wr] : ctx.routes)
                {
                    const auto sl = rdest.rfind('/');
                    if (sl != std::string::npos &&
                        rdest.substr(0, sl) == loopback)
                    {
                        cbEntry.last_nhid = wr.nhid;
                        break;
                    }
                }
                rtsrv::log::info(std::format(
                    "[Watch] global map: loopback='{}' hostname='{}'"
                    " prefixes={} initial_nhid={}",
                    loopback,
                    cbEntry.hostname,
                    cbEntry.prefixes.size(),
                    cbEntry.last_nhid));
                g_loopback_cb_map[loopback] = std::move(cbEntry);

                // Create an ECMP group for this loopback.  If a seed nhid is
                // available from the kernel route snapshot, add it so the
                // first ROUTE_ADD can be issued immediately before any new
                // OSPF event arrives.
                g_ecmp_groups.create(loopback, node.hostname());
                if (g_loopback_cb_map[loopback].last_nhid != 0)
                    g_ecmp_groups.add_member(
                        loopback, g_loopback_cb_map[loopback].last_nhid);
                rtsrv::log::info(std::format(
                    "[Watch] ECMP group created: loopback='{}' hostname='{}'"
                    " seed_nhid={}",
                    loopback,
                    node.hostname(),
                    g_loopback_cb_map[loopback].last_nhid));
            }
        }
    }

    // ── Step 3d: Connect SraUdpClient for ROUTE_ADD to ud_server ─────────
    sra::SraUdpClient watchSraClient("/tmp/ud_server.sock");
    watchSraClient.start();
    ctx.sraClient = &watchSraClient;
    rtsrv::log::info("[Watch] SraUdpClient started (/tmp/ud_server.sock)");

    // ── Step 4: Start monitoring ──────────────────────────────────────────
    /* Install signal handlers. */
    struct sigaction sa
    {};
    sa.sa_handler = watchSigHandler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    int fd = netlink_init();
    if (fd < 0)
    {
        rtsrv::log::err(std::format("[Monitor] netlink_init failed: {}",
                                    std::strerror(errno)));
        watchSraClient.stop();
        return EXIT_FAILURE;
    }
    g_watch_fd = fd;

    rtsrv::log::info(
        "[Monitor] Watching for IPv4 /32 OSPF route events — forwarding to "
        "srmd...");
    if (!ctx.loopback.empty())
    {
        rtsrv::log::info(std::format(
            "[Monitor] Loopback {} — GetLoopbacks on each OSPF ADDED event",
            ctx.loopback));
    }
    rtsrv::log::info("[Monitor] running (Ctrl-C to stop)");
    std::cout.flush();

    netlink_run(fd, nlWatchCb, &ctx);

    netlink_close(g_watch_fd); /* no-op if already closed by signal handler */

    watchSraClient.stop();

    rtsrv::log::info("[Monitor] Stopped.");
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
    case AF_INET:
        return "inet";
    case AF_INET6:
        return "inet6";
    case AF_BRIDGE:
        return "bridge";
    default:
        return std::to_string(static_cast<int>(family));
    }
}

/**
 * @brief Maps a NUD_* neighbor state bitmask to a human-readable string.
 */
static std::string nudStateLabel(uint16_t state)
{
    switch (state)
    {
    case NUD_INCOMPLETE:
        return "incomplete";
    case NUD_REACHABLE:
        return "reachable";
    case NUD_STALE:
        return "stale";
    case NUD_DELAY:
        return "delay";
    case NUD_PROBE:
        return "probe";
    case NUD_FAILED:
        return "failed";
    case NUD_NOARP:
        return "noarp";
    case NUD_PERMANENT:
        return "permanent";
    case NUD_NONE:
        return "none";
    default:
        return std::format("0x{:02x}", state);
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
    if (flags & NTF_USE)
        s += 'U';
    if (flags & NTF_SELF)
        s += 'S';
    if (flags & NTF_MASTER)
        s += 'M';
    if (flags & NTF_PROXY)
        s += 'P';
    if (flags & NTF_EXT_LEARNED)
        s += 'X';
#ifdef NTF_OFFLOADED
    if (flags & NTF_OFFLOADED)
        s += 'O';
#endif
#ifdef NTF_STICKY
    if (flags & NTF_STICKY)
        s += 'K';
#endif
    if (flags & NTF_ROUTER)
        s += 'R';
    return s.empty() ? std::format("0x{:02x}", flags) : s;
}

/**
 * @brief Formats a raw link-layer address as "xx:xx:xx:xx:xx:xx".
 *
 * Returns "(none)" when lladdr_len == 0.
 */
static std::string formatMac(const uint8_t* addr, uint8_t len)
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
    uint8_t family{0};  ///< ndm_family: AF_INET/AF_INET6/AF_BRIDGE
    int ifindex{0};     ///< ndm_ifindex: outgoing interface index
    std::string ifname; ///< Resolved interface name
    uint16_t state{0};  ///< ndm_state: NUD_* bitmask
    uint8_t flags{0};   ///< ndm_flags: NTF_* bitmask
    uint8_t type{0};    ///< ndm_type: RTN_*
    /* ── NDA_DST ───────────────────────────────────────────────────────── */
    std::string dst; ///< Destination IP string
    /* ── NDA_LLADDR ────────────────────────────────────────────────────── */
    std::string mac; ///< Formatted link-layer address (xx:xx:…)
    /* ── NDA_CACHEINFO ─────────────────────────────────────────────────── */
    uint32_t confirmed_ms{0}; ///< ndm_confirmed: ms since last confirmation
    uint32_t used_ms{0};      ///< ndm_used: ms since last use
    uint32_t updated_ms{0};   ///< ndm_updated: ms since last update
    uint32_t refcnt{0};       ///< ndm_refcnt: reference count
    /* ── NDA_PROBES ────────────────────────────────────────────────────── */
    uint32_t probes{0}; ///< ARP/NDP probe count
    /* ── NDA_VLAN ──────────────────────────────────────────────────────── */
    uint16_t vlan{0}; ///< VLAN ID (bridge; 0 = absent)
    /* ── NDA_MASTER ────────────────────────────────────────────────────── */
    uint32_t master{0};      ///< Master interface index (0 = absent)
    std::string master_name; ///< Resolved master interface name
    /* ── NDA_IFINDEX ───────────────────────────────────────────────────── */
    uint32_t nh_ifindex{0}; ///< NDA_IFINDEX: nexthop interface override
    /* ── NDA_PROTOCOL ──────────────────────────────────────────────────── */
    uint8_t protocol{0}; ///< Routing protocol that installed entry
};

/**
 * @brief Pretty-prints the dynamic neighbor table using box-drawing lines.
 *
 * Columns mirror every field of netlink_neigh_t so the operator can inspect
 * the complete netlink payload.  The table is keyed by family|ifindex|dst.
 */
static void
printNeighborTable(const std::map<std::string, NeighEntry>& neighbors)
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
    constexpr int cF = 6, cD = 39, cL = 17, cI = 13, cX = 5, cS = 10, cG = 6,
                  cC = 10, cU = 9, cP = 9, cR = 5, cQ = 6;

    auto hbar = [](int n) {
        std::string s;
        s.reserve(static_cast<std::size_t>(n) * 3);
        for (int i = 0; i < n; ++i)
            s += "─";
        return s;
    };

    auto mkBorder = [&](const char* l, const char* j, const char* r) {
        return std::string(l) + hbar(cF + 2) + j + hbar(cD + 2) + j +
               hbar(cL + 2) + j + hbar(cI + 2) + j + hbar(cX + 2) + j +
               hbar(cS + 2) + j + hbar(cG + 2) + j + hbar(cC + 2) + j +
               hbar(cU + 2) + j + hbar(cP + 2) + j + hbar(cR + 2) + j +
               hbar(cQ + 2) + r;
    };

    const std::string top = mkBorder("┌", "┬", "┐");
    const std::string mid = mkBorder("├", "┼", "┤");
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
                        const std::string& prb) {
        std::println("│ {:<{}} │ {:<{}} │ {:<{}} │ {:<{}} │ {:>{}} │ {:<{}} │ "
                     "{:<{}} │ {:>{}} │ {:>{}} │ {:>{}} │ {:>{}} │ {:>{}} │",
                     fam,
                     cF,
                     dst,
                     cD,
                     mac,
                     cL,
                     ifc,
                     cI,
                     idx,
                     cX,
                     sta,
                     cS,
                     flg,
                     cG,
                     cfm,
                     cC,
                     usd,
                     cU,
                     upd,
                     cP,
                     ref,
                     cR,
                     prb,
                     cQ);
    };

    std::println("\n Neighbor Table  ({} entry/entries)", neighbors.size());
    std::println("{}", top);
    printRow("Family",
             "Destination",
             "MAC",
             "Interface",
             "Idx",
             "State",
             "Flags",
             "Cfm(ms)",
             "Used(ms)",
             "Upd(ms)",
             "Ref",
             "Probes");

    if (neighbors.empty())
    {
        std::println("{}", bottom);
        return;
    }

    std::println("{}", mid);

    for (const auto& [key, n] : neighbors)
    {
        printRow(familyLabel(n.family),
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
static std::string
serializeNeighborTable(const std::map<std::string, NeighEntry>& neighbors)
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
            n.dst.empty() ? "(none)" : n.dst,
            n.mac.empty() ? "(none)" : n.mac,
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
    mutable std::shared_mutex mtx;               ///< Guards neighbors
    std::map<std::string, NeighEntry> neighbors; ///< "family|ifidx|dst" → entry
    sra::UdpTableServer*
        udpServer; ///< UDP publisher (port 9001); may be nullptr
};

/**
 * @brief Inserts or removes one neighbor entry from @p ctx.
 *
 * Shared by both the silent populate callback (used during the initial dump)
 * and the display callback (used during the live-event loop).
 */
static void nlNeighUpdate(netlink_neigh_event_t event,
                          const netlink_neigh_t* n,
                          NeighCtx* ctx)
{
    const std::string key = familyLabel(n->family) + "|" +
                            std::to_string(n->ifindex) + "|" +
                            (n->dst[0] ? n->dst : "(none)");

    if (event == NETLINK_NEIGH_REMOVED)
    {
        ctx->neighbors.erase(key);
        return;
    }

    NeighEntry e;
    e.family = n->family;
    e.ifindex = n->ifindex;
    e.ifname = n->ifname;
    e.state = n->state;
    e.flags = n->flags;
    e.type = n->type;
    e.dst = n->dst;
    e.mac = formatMac(n->lladdr, n->lladdr_len);
    e.confirmed_ms = n->confirmed_ms;
    e.used_ms = n->used_ms;
    e.updated_ms = n->updated_ms;
    e.refcnt = n->refcnt;
    e.probes = n->probes;
    e.vlan = n->vlan;
    e.master = n->master;
    e.master_name = n->master_name;
    e.nh_ifindex = n->nh_ifindex;
    e.protocol = n->protocol;
    ctx->neighbors[key] = e;
}

/**
 * @brief Silent populate callback — updates the in-memory table only.
 *
 * Used during the initial RTM_GETNEIGH dump so the table is not redrawn
 * once per entry.  After the dump completes the caller prints the table once.
 */
static void nlNeighPopulateCb(netlink_neigh_event_t event,
                              const netlink_neigh_t* n,
                              void* user_data)
{
    nlNeighUpdate(event, n, static_cast<NeighCtx*>(user_data));
}

/**
 * @brief Live-event callback — updates the in-memory table and publishes via
 * UDP.
 *
 * Used after the initial dump, for ongoing RTM_NEWNEIGH / RTM_DELNEIGH events.
 * The neighbor table is NOT redrawn on screen; only the NetLink event itself is
 * logged.  The updated snapshot is pushed to all UDP subscribers on port 9001.
 */
static void nlNeighCb(netlink_neigh_event_t event,
                      const netlink_neigh_t* n,
                      void* user_data)
{
    auto* ctx = static_cast<NeighCtx*>(user_data);

    const char* evLabel = (event == NETLINK_NEIGH_ADDED)     ? "ADDED"
                          : (event == NETLINK_NEIGH_REMOVED) ? "REMOVED"
                                                             : "CHANGED";
    rtsrv::log::info(std::format(
        "[Neighbors] {} {} on {} (state={})",
        evLabel,
        n->dst[0] ? n->dst : "(no dst)",
        n->ifname[0] ? n->ifname : std::to_string(n->ifindex).c_str(),
        nudStateLabel(n->state)));

    nlNeighUpdate(event, n, ctx);

    /* Publish updated snapshot to UDP subscribers (port 9001). */
    if (ctx->udpServer)
        ctx->udpServer->setNeighborData(serializeNeighborTable(ctx->neighbors));
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
        ::shutdown(g_neigh_fd, SHUT_RD);
        netlink_neigh_close(g_neigh_fd);
        g_neigh_fd = -1;
    }
    if (g_startup_route_fd >= 0)
    {
        ::shutdown(g_startup_route_fd, SHUT_RD);
        netlink_close(g_startup_route_fd);
        g_startup_route_fd = -1;
    }
    if (g_startup_nexthop_fd >= 0)
    {
        ::shutdown(g_startup_nexthop_fd, SHUT_RD);
        netlink_nexthop_close(g_startup_nexthop_fd);
        g_startup_nexthop_fd = -1;
    }
    interruptBackgroundThreads();
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
    rtsrv::log::info("[Neighbors] Opening netlink neighbor monitor...");
    rtsrv::log::info(std::format("[Neighbors] Table available via UDP port {}",
                                 sra::UDP_PORT_NEIGHBORS));

    int fd = netlink_neigh_init();
    if (fd < 0)
    {
        rtsrv::log::err(std::format("[Neighbors] netlink_neigh_init failed: {}",
                                    std::strerror(errno)));
        return EXIT_FAILURE;
    }
    g_neigh_fd = fd;

    struct sigaction sa
    {};
    sa.sa_handler = neighSigHandler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    // ── Step 1: Read the current neighbor table from the kernel ──────────
    rtsrv::log::info("[Neighbors] Reading neighbor table from kernel...");
    NeighCtx ctx;
    ctx.udpServer = &udpServer;
    const int dumped = netlink_neigh_dump(fd, nlNeighPopulateCb, &ctx);
    if (dumped < 0)
    {
        rtsrv::log::warn(std::format("[Neighbors] initial read failed: {}",
                                     std::strerror(errno)));
    }
    else
    {
        rtsrv::log::info(std::format("[Neighbors] Read complete: {} entry/entries.",
                                     dumped));
    }

    // ── Step 2: Publish initial snapshot via UDP ─────────────────────────
    udpServer.setNeighborData(serializeNeighborTable(ctx.neighbors));

    // ── Enter live-event loop ─────────────────────────────────────────────
    rtsrv::log::info("[Neighbors] Watching for kernel neighbor events...");
    rtsrv::log::info("[Neighbors] running (Ctrl-C to stop)");

    netlink_neigh_run(fd, nlNeighCb, &ctx);

    netlink_neigh_close(g_neigh_fd);

    rtsrv::log::info("[Neighbors] Stopped.");
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
    case RT_SCOPE_UNIVERSE:
        return "global";
    case RT_SCOPE_SITE:
        return "site";
    case RT_SCOPE_LINK:
        return "link";
    case RT_SCOPE_HOST:
        return "host";
    case RT_SCOPE_NOWHERE:
        return "nowhere";
    default:
        return std::to_string(static_cast<int>(scope));
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
    if (flags & RTNH_F_DEAD)
        parts.emplace_back("dead");
    if (flags & RTNH_F_PERVASIVE)
        parts.emplace_back("perv");
    if (flags & RTNH_F_ONLINK)
        parts.emplace_back("onlink");
    if (flags & RTNH_F_OFFLOAD)
        parts.emplace_back("offload");
    if (flags & RTNH_F_LINKDOWN)
        parts.emplace_back("down");
    if (flags & RTNH_F_UNRESOLVED)
        parts.emplace_back("unresolved");
    if (parts.empty())
        return std::format("0x{:08x}", flags);
    std::string s;
    for (std::size_t i = 0; i < parts.size(); ++i)
    {
        if (i > 0)
            s += '|';
        s += parts[i];
    }
    return s;
}

/** @brief Formats a nexthop group member list as "id,id,…" for display. */
static std::string formatGroupIds(const std::vector<NhGroupMember>& members)
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

/**
 * @brief Populate-only netlink nexthop callback that writes directly into a
 *        @c std::map<uint32_t,NexthopEntry>.
 *
 * Used when a full NexthopCtx is not available (e.g., the one-shot dump in
 * cmdNetlinkWatch).  Passed as user_data should be a pointer to the map.
 */
static void nlNhDumpToMapCb(netlink_nexthop_event_t event,
                            const netlink_nexthop_t* nh,
                            void* user_data)
{
    auto* tbl = static_cast<std::map<uint32_t, NexthopEntry>*>(user_data);
    if (event == NETLINK_NEXTHOP_REMOVED)
    {
        tbl->erase(nh->id);
        return;
    }
    NexthopEntry e;
    e.id = nh->id;
    e.family = nh->family;
    e.scope = nh->scope;
    e.protocol = nh->protocol;
    e.flags = nh->flags;
    e.oif = nh->oif;
    e.oif_name = nh->oif_name;
    e.gateway = nh->gateway;
    e.blackhole = nh->blackhole;
    e.fdb = nh->fdb;
    e.master = nh->master;
    e.master_name = nh->master_name;
    e.is_group = (nh->group_count > 0);
    for (uint32_t i = 0; i < nh->group_count; ++i)
        e.group_members.push_back({nh->group[i].id, nh->group[i].weight});
    e.group_type = nh->group_type;
    e.encap_type = nh->encap_type;
    (*tbl)[nh->id] = std::move(e);
}

/**
 * @brief Pretty-prints the dynamic nexthop table using box-drawing lines.
 *
 * Columns: ID, Family, Scope, Protocol, Flags, OIF, Gateway,
 *          Grp?, Group IDs, GrpType, BH, FDB, Master
 */
static void printNexthopTable(const std::map<uint32_t, NexthopEntry>& nexthops)
{
    // ── Column widths ─────────────────────────────────────────────────────
    // ID       : 6 digits        →  6
    // Family   : "inet6" = 5     →  6
    // Scope    : "nowhere" = 7   →  7
    // Protocol : "zebra" = 5     →  8
    // Flags    : "unresolved"=10 → 12
    // OIF      : IFNAMSIZ-1=15   → 13
    // Gateway  : IPv6=39         → 25
    // Grp?     : "yes"/"no"      →  3
    // Group IDs: "354,364"       → 20
    // GrpType  : "resilient"=9   →  9
    // BH       : "yes/no"=3      →  3
    // FDB      : "yes/no"=3      →  3
    // Master   : IFNAMSIZ-1      → 12
    constexpr int cI = 6, cF = 6, cO = 7, cP = 8, cL = 12, cN = 13, cG = 25,
                  cQ = 3, cR = 20, cT = 9, cB = 3, cD = 3, cM = 12;

    auto hbar = [](int n) {
        std::string s;
        s.reserve(static_cast<std::size_t>(n) * 3);
        for (int i = 0; i < n; ++i)
            s += "─";
        return s;
    };

    auto mkBorder = [&](const char* l, const char* j, const char* r) {
        return std::string(l) + hbar(cI + 2) + j + hbar(cF + 2) + j +
               hbar(cO + 2) + j + hbar(cP + 2) + j + hbar(cL + 2) + j +
               hbar(cN + 2) + j + hbar(cG + 2) + j + hbar(cQ + 2) + j +
               hbar(cR + 2) + j + hbar(cT + 2) + j + hbar(cB + 2) + j +
               hbar(cD + 2) + j + hbar(cM + 2) + r;
    };

    const std::string top = mkBorder("┌", "┬", "┐");
    const std::string mid = mkBorder("├", "┼", "┤");
    const std::string bottom = mkBorder("└", "┴", "┘");

    auto printRow = [&](const std::string& id,
                        const std::string& fam,
                        const std::string& scp,
                        const std::string& pro,
                        const std::string& flg,
                        const std::string& oif,
                        const std::string& gw,
                        const std::string& grpFlag,
                        const std::string& grpIds,
                        const std::string& gtp,
                        const std::string& bh,
                        const std::string& fdb,
                        const std::string& mst) {
        std::println(
            "│ {:>{}} │ {:<{}} │ {:<{}} │ {:<{}} │ {:<{}} │ {:<{}} │ "
            "{:<{}} │ {:<{}} │ {:<{}} │ {:<{}} │ {:<{}} │ {:<{}} │ {:<{}} │",
            id,
            cI,
            fam,
            cF,
            scp,
            cO,
            pro,
            cP,
            flg,
            cL,
            oif,
            cN,
            gw,
            cG,
            grpFlag,
            cQ,
            grpIds,
            cR,
            gtp,
            cT,
            bh,
            cB,
            fdb,
            cD,
            mst,
            cM);
    };

    std::println("\n Nexthop Table  ({} entry/entries)", nexthops.size());
    std::println("{}", top);
    printRow("ID",
             "Family",
             "Scope",
             "Protocol",
             "Flags",
             "OIF",
             "Gateway",
             "Grp",
             "Group IDs",
             "GrpType",
             "BH",
             "FDB",
             "Master");

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
        const std::string mst =
            nh.master_name.empty()
                ? (nh.master ? std::to_string(nh.master) : "-")
                : nh.master_name;
        const std::string gtp =
            nh.is_group ? (nh.group_type == 0 ? "mpath" : "resilient") : "-";
        printRow(std::to_string(nh.id),
                 familyLabel(nh.family),
                 scopeLabel(nh.scope),
                 protoLabel(nh.protocol),
                 rtnhFlagsLabel(nh.flags),
                 oif,
                 nh.gateway.empty() ? "-" : nh.gateway,
                 nh.is_group ? "yes" : "no",
                 formatGroupIds(nh.group_members),
                 gtp,
                 nh.blackhole ? "yes" : "no",
                 nh.fdb ? "yes" : "no",
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
 *   id=<n> family=<f> scope=<s> proto=<p> flags=<fl> oif=<if> gw=<gw>
 *   is_group=yes/no group_ids=354,364 group_type=mpath encap_type=<n>
 *   bh=<b> fdb=<f> master=<m>
 * @endcode
 */
static std::string
serializeNexthopTable(const std::map<uint32_t, NexthopEntry>& nexthops)
{
    std::ostringstream os;
    os << "NEXTHOPS " << nexthops.size() << '\n';
    for (const auto& [id, nh] : nexthops)
    {
        const std::string oif = nh.oif_name.empty()
                                    ? (nh.oif ? std::to_string(nh.oif) : "-")
                                    : nh.oif_name;
        const std::string mst =
            nh.master_name.empty()
                ? (nh.master ? std::to_string(nh.master) : "-")
                : nh.master_name;
        const std::string gtp =
            nh.is_group ? (nh.group_type == 0 ? "mpath" : "resilient") : "-";
        os << std::format(
            "id={} family={} scope={} proto={} flags={} oif={} gw={}"
            " is_group={} group_ids={} group_type={} encap_type={}"
            " bh={} fdb={} master={}\n",
            nh.id,
            familyLabel(nh.family),
            scopeLabel(nh.scope),
            protoLabel(nh.protocol),
            rtnhFlagsLabel(nh.flags),
            oif,
            nh.gateway.empty() ? "-" : nh.gateway,
            nh.is_group ? "yes" : "no",
            formatGroupIds(nh.group_members),
            gtp,
            nh.encap_type,
            nh.blackhole ? "yes" : "no",
            nh.fdb ? "yes" : "no",
            mst);
    }
    return os.str();
}

/**
 * @brief Context forwarded via user_data to the nexthop netlink callback.
 */
struct NexthopCtx
{
    std::map<uint32_t, NexthopEntry> nexthops; ///< nexthop ID → entry
    sra::UdpTableServer* udpServer{nullptr};   ///< UDP publisher (port 9002)
    sra::VrfTable* vrfTable{nullptr};          ///< VRF table from GetAllRoutes
    sra::SraUdpClient* vrfClient{nullptr}; ///< Unix-domain client to ud_server
    NeighCtx* neighCtx{nullptr}; ///< adjacency table (shared with neigh thread)
};

/**
 * @brief Inserts or removes one nexthop entry from @p ctx.
 *
 * Shared by both the silent populate callback (initial dump) and the
 * display callback (live-event loop).
 */
static void nlNexthopUpdate(netlink_nexthop_event_t event,
                            const netlink_nexthop_t* nh,
                            NexthopCtx* ctx)
{
    if (event == NETLINK_NEXTHOP_REMOVED)
    {
        ctx->nexthops.erase(nh->id);
        return;
    }

    NexthopEntry e;
    e.id = nh->id;
    e.family = nh->family;
    e.scope = nh->scope;
    e.protocol = nh->protocol;
    e.flags = nh->flags;
    e.oif = nh->oif;
    e.oif_name = nh->oif_name;
    e.gateway = nh->gateway;
    e.blackhole = nh->blackhole;
    e.fdb = nh->fdb;
    e.master = nh->master;
    e.master_name = nh->master_name;
    e.is_group = (nh->group_count > 0);
    e.group_members.clear();
    for (uint32_t i = 0; i < nh->group_count; ++i)
        e.group_members.push_back({nh->group[i].id, nh->group[i].weight});
    e.group_type = nh->group_type;
    e.encap_type = nh->encap_type;
    ctx->nexthops[nh->id] = e;
}

/**
 * @brief Silent populate callback — updates the in-memory table only.
 *
 * Used during the initial RTM_GETNEXTHOP dump so the table is not redrawn
 * once per object.  After the dump completes the caller prints the table once.
 */
static void nlNexthopPopulateCb(netlink_nexthop_event_t event,
                                const netlink_nexthop_t* nh,
                                void* user_data)
{
    nlNexthopUpdate(event, nh, static_cast<NexthopCtx*>(user_data));
}

/**
 * @brief Live-event callback — updates the in-memory table and publishes via
 * UDP.
 *
 * Used after the initial dump, for ongoing RTM_NEWNEXTHOP / RTM_DELNEXTHOP
 * events.  The nexthop table is NOT redrawn on screen; only the NetLink event
 * itself is logged.  The updated snapshot is pushed to UDP subscribers on
 * port 9002.
 */
static void nlNexthopCb(netlink_nexthop_event_t event,
                        const netlink_nexthop_t* nh,
                        void* user_data)
{
    auto* ctx = static_cast<NexthopCtx*>(user_data);

    const char* evLabel = (event == NETLINK_NEXTHOP_ADDED)     ? "ADDED"
                          : (event == NETLINK_NEXTHOP_REMOVED) ? "REMOVED"
                                                               : "CHANGED";
    rtsrv::log::info(std::format(
        "[Nexthops] {} id={} gw={} oif={}",
        evLabel,
        nh->id,
        nh->gateway[0] ? nh->gateway : "-",
        nh->oif_name[0] ? nh->oif_name : std::to_string(nh->oif).c_str()));

    nlNexthopUpdate(event, nh, ctx);

    /* Publish updated snapshot to UDP subscribers (port 9002). */
    if (ctx->udpServer)
        ctx->udpServer->setNexthopData(serializeNexthopTable(ctx->nexthops));
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
        ::shutdown(g_nexthop_fd, SHUT_RD);
        netlink_nexthop_close(g_nexthop_fd);
        g_nexthop_fd = -1;
    }
    if (g_startup_route_fd >= 0)
    {
        ::shutdown(g_startup_route_fd, SHUT_RD);
        netlink_close(g_startup_route_fd);
        g_startup_route_fd = -1;
    }
    if (g_startup_neigh_fd >= 0)
    {
        ::shutdown(g_startup_neigh_fd, SHUT_RD);
        netlink_neigh_close(g_startup_neigh_fd);
        g_startup_neigh_fd = -1;
    }
    interruptBackgroundThreads();
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
    rtsrv::log::info("[Nexthops] Opening netlink nexthop monitor...");
    rtsrv::log::info(std::format("[Nexthops] Table available via UDP port {}",
                                 sra::UDP_PORT_NEXTHOPS));

    int fd = netlink_nexthop_init();
    if (fd < 0)
    {
        rtsrv::log::err(std::format(
            "[Nexthops] netlink_nexthop_init failed: {}",
            std::strerror(errno)));
        return EXIT_FAILURE;
    }
    g_nexthop_fd = fd;

    struct sigaction sa
    {};
    sa.sa_handler = nexthopSigHandler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    // ── Step 1: Read all nexthop objects from the kernel ─────────────────
    rtsrv::log::info("[Nexthops] Reading nexthop objects from kernel...");
    NexthopCtx ctx;
    ctx.udpServer = &udpServer;
    const int dumped = netlink_nexthop_dump(fd, nlNexthopPopulateCb, &ctx);
    if (dumped < 0)
    {
        rtsrv::log::warn(std::format(
            "[Nexthops] dump failed: {} "
            "(kernel may not support nexthop objects; requires 5.3+)",
            std::strerror(errno)));
    }
    else
    {
        rtsrv::log::info(std::format("[Nexthops] Read complete: {} object(s).",
                                     dumped));
    }

    // ── Step 2: Publish initial snapshot via UDP ──────────────────────────
    udpServer.setNexthopData(serializeNexthopTable(ctx.nexthops));

    // ── Step 3: Watch for live nexthop events ────────────────────────────
    rtsrv::log::info("[Nexthops] Watching for kernel nexthop events...");
    rtsrv::log::info("[Nexthops] running (Ctrl-C to stop)");

    netlink_nexthop_run(fd, nlNexthopCb, &ctx);

    netlink_nexthop_close(g_nexthop_fd);

    rtsrv::log::info("[Nexthops] Stopped.");
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
    std::queue<sra::RequestPayload> items; ///< Pending payloads.
    std::mutex mutex;                      ///< Guards @c items.
    std::condition_variable cv;            ///< Notified when an item is pushed.
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
    struct sigaction sa
    {};
    sa.sa_handler = demoSigHandler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    rtsrv::log::info("[grpc-proc-demo] running (Ctrl-C to stop)");

    // ── Shared intermediate request queue ────────────────────────────────
    DemoRequestQueue demoQueue;

    // ── Producer thread ───────────────────────────────────────────────────
    // Creates a new GetLoopback request every 5 seconds and places it in
    // the queue.  The main loop drains the queue and forwards to GrpcProc.
    std::thread producer([&demoQueue] {
        uint64_t seq = 0;
        while (!g_demo_stop)
        {
            {
                std::lock_guard<std::mutex> lock(demoQueue.mutex);
                demoQueue.items.push(sra::GetLoopbackParams{});
                ++seq;
                rtsrv::log::info(std::format(
                    "[grpc-proc-demo] Enqueued GetLoopback request #{}", seq));
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
    rtsrv::log::info("[grpc-proc-demo] grpc_proc thread started.");

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
                sra::RequestPayload payload =
                    std::move(demoQueue.items.front());
                demoQueue.items.pop();
                lock.unlock(); // release while submitting to avoid holding
                               // during RPC queue op

                const uint64_t id = proc.submit(std::move(payload));
                inFlight.push_back(id);
                rtsrv::log::info(std::format(
                    "[grpc-proc-demo] Submitted request id={}", id));

                lock.lock(); // re-acquire before checking queue again
            }
        }

        // 2. Non-blocking poll for completed responses (mirrors
        //    exampleNonBlockingPoll).  Each completed response is printed
        //    and removed from the in-flight list.
        for (auto it = inFlight.begin(); it != inFlight.end();)
        {
            auto resp = proc.tryGetResponse(*it);
            if (resp)
            {
                std::visit(
                    [](const auto& result) {
                        using T = std::decay_t<decltype(result)>;
                        if constexpr (std::is_same_v<T, sra::GetLoopbackResult>)
                        {
                            if (result)
                            {
                                rtsrv::log::info(std::format(
                                    "[grpc-proc-demo] GetLoopback OK: \"{}\"",
                                    *result));
                            }
                            else
                            {
                                rtsrv::log::warn(std::format(
                                    "[grpc-proc-demo] GetLoopback ERR: {}",
                                    result.error()));
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

    rtsrv::log::info("[grpc-proc-demo] stopped.");
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
 * nexthops points to the shared nexthop table for resolving nhid → NhInfo.
 */
struct StartupRouteCtx
{
    std::map<std::string, WatchRoute>
        routes; ///< dest → WatchRoute (one per dst)
    sra::UdpTableServer* udpServer{nullptr};
    const std::map<uint32_t, NexthopEntry>* nexthops{
        nullptr}; ///< read-only NH table
};

/**
 * @brief Resolves a nexthop object ID to a list of NhInfo entries.
 *
 * Looks up @p nhid in @p nexthops.  If it is a group, expands each member
 * recursively by one level.  Returns an empty vector when nhid is 0 or
 * the table is null / missing the entry.
 */
static std::vector<NhInfo>
resolveNhid(uint32_t nhid, const std::map<uint32_t, NexthopEntry>* nexthops)
{
    if (!nexthops || nhid == 0)
        return {};
    auto it = nexthops->find(nhid);
    if (it == nexthops->end())
        return {};
    const auto& nh = it->second;
    if (nh.is_group)
    {
        std::vector<NhInfo> result;
        for (const auto& m : nh.group_members)
        {
            auto jt = nexthops->find(m.id);
            if (jt == nexthops->end())
                continue;
            NhInfo info;
            info.gateway = jt->second.gateway;
            info.dev = jt->second.oif_name;
            info.ifindex = jt->second.oif;
            info.weight = static_cast<uint8_t>(m.weight + 1);
            result.push_back(std::move(info));
        }
        return result;
    }
    NhInfo info;
    info.gateway = nh.gateway;
    info.dev = nh.oif_name;
    info.ifindex = nh.oif;
    info.weight = 1;
    return {std::move(info)};
}

/**
 * @brief Silent live-route callback for the startup background thread.
 *
 * Updates the in-memory route table (one entry per destination) and publishes
 * to UDP without any console output.
 */
static void startupRouteLiveCb(netlink_event_t event,
                               const netlink_route32_t* route,
                               void* user_data)
{
    auto* ctx = static_cast<StartupRouteCtx*>(user_data);

    char dst_buf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &route->dst, dst_buf, sizeof(dst_buf));
    const std::string dest =
        std::string(dst_buf) + "/" +
        std::to_string(static_cast<unsigned>(route->dst_len));

    if (event == NETLINK_ROUTE_REMOVED)
    {
        ctx->routes.erase(dest);
    }
    else
    {
        WatchRoute wr;
        wr.dest = dest;
        wr.nhid = route->nhid;
        wr.metric = route->metric;
        wr.table = route->table;
        wr.protocol = route->protocol;
        wr.family = route->family;
        wr.dst_len = route->dst_len;
        wr.tos = route->tos;
        wr.scope = route->scope;
        wr.type = route->type;
        wr.flags = route->flags;

        // Resolve nexthops from the nexthop object table when nhid is set.
        if (route->nhid != 0)
        {
            wr.nexthops = resolveNhid(route->nhid, ctx->nexthops);
        }

        // Fall back to gateway/iface embedded in the route message.
        if (wr.nexthops.empty())
        {
            char gw_buf[INET_ADDRSTRLEN] = {};
            if (route->gateway.s_addr != 0)
                inet_ntop(AF_INET, &route->gateway, gw_buf, sizeof(gw_buf));
            if (gw_buf[0] || route->ifname[0])
            {
                NhInfo nh;
                nh.gateway = gw_buf;
                nh.dev = route->ifname;
                nh.ifindex = route->ifindex;
                nh.weight = 1;
                wr.nexthops.push_back(std::move(nh));
            }
        }

        ctx->routes[dest] = std::move(wr);
    }

    if (ctx->udpServer)
        ctx->udpServer->setRouteData(serializeRouteTable(ctx->routes));
}

/**
 * @brief Silent live-neighbor callback for the startup background thread.
 *
 * Updates the in-memory neighbor table and publishes to UDP without printing.
 */
static void startupNeighLiveCb(netlink_neigh_event_t event,
                               const netlink_neigh_t* n,
                               void* user_data)
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
    while (len > 1)
    {
        sum += *p++;
        len -= 2;
    }
    if (len)
        sum += *reinterpret_cast<const uint8_t*>(p);
    sum = (sum >> 16) + (sum & 0xffff);
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
        rtsrv::log::warn(std::format("[ICMP] socket() failed for {}: {}",
                                     destIp, ::strerror(errno)));
        return;
    }

    struct icmphdr pkt
    {};
    pkt.type = ICMP_ECHO;
    pkt.code = 0;
    pkt.un.echo.id = ::htons(static_cast<uint16_t>(::getpid() & 0xffff));
    pkt.un.echo.sequence = ::htons(1);
    pkt.checksum = icmpChecksum(&pkt, sizeof(pkt));

    struct sockaddr_in dst
    {};
    dst.sin_family = AF_INET;
    ::inet_pton(AF_INET, destIp.c_str(), &dst.sin_addr);

    if (::sendto(sock,
                 &pkt,
                 sizeof(pkt),
                 0,
                 reinterpret_cast<const sockaddr*>(&dst),
                 sizeof(dst)) < 0)
    {
        rtsrv::log::warn(std::format("[ICMP] sendto() to {} failed: {}",
                                     destIp, ::strerror(errno)));
    }
    else
    {
        rtsrv::log::info(std::format(
            "[ICMP] sent echo request to {} to probe adjacency", destIp));
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
 * @param vrfClient  Unix-domain client used to submit the request.  May be
 * null.
 * @param neighCtx   Adjacency table shared with the neighbor monitor thread.
 * May be null.
 */
static void sendVrfRouteForNexthop(const netlink_nexthop_t* nh,
                                   sra::VrfTable* vrfTable,
                                   sra::SraUdpClient* vrfClient,
                                   NeighCtx* neighCtx)
{
    if (!vrfTable || !vrfClient)
        return;

    // Only unicast nexthops with a valid gateway address are relevant.
    if (!nh->gateway[0])
        return;

    const std::string gateway = nh->gateway;

    if (!vrfTable->hasNexthop(gateway))
    {
        rtsrv::log::info(std::format(
            "[Nexthops] nexthop id={} gw={} not found in VRF table — skip",
            nh->id, gateway));
        return;
    }

    // Only send an ICMP echo if the nexthop is already in the adjacency table
    // (i.e. its MAC address is resolved).  Sending to an unresolved address
    // would fail silently and serves no useful purpose here.
    if (neighCtx && neighborHasIp(*neighCtx, gateway))
    {
        rtsrv::log::info(std::format(
            "[Nexthops] nexthop gw={} id={} in adjacency table — "
            "sending ICMP echo request",
            gateway, nh->id));
        sendIcmpEchoRequest(gateway);
    }

    const auto routes = vrfTable->findByNexthop(gateway);
    rtsrv::log::info(std::format(
        "[Nexthops] nexthop id={} gw={} matched {} entry/entries; "
        "generating SingleRouteRequest (SINGLE_ROUTE, type=1)",
        nh->id, gateway, routes.size()));

    // Parse gateway to binary (network byte order).
    struct in_addr nhAddr
    {};
    if (::inet_pton(AF_INET, gateway.c_str(), &nhAddr) != 1)
    {
        rtsrv::log::warn(std::format(
            "[Nexthops] invalid gateway address '{}' — skip", gateway));
        return;
    }
    const auto* nhBytes = reinterpret_cast<const std::uint8_t*>(&nhAddr.s_addr);

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
        for (std::size_t k = 0; k < cmdproto::IFACE_NAME_SIZE && k < ifn.size();
             ++k)
        {
            iface.iface_name[k] = ifn[k];
        }

        iface.nexthop_addr_ipv4 = {
            nhBytes[0], nhBytes[1], nhBytes[2], nhBytes[3]};
        iface.nexthop_id_ipv4 = nh->id;

        // Convert each prefix string "A.B.C.D/N" to binary PrefixIpv4.
        for (const auto& pfx : route.prefixes())
        {
            const std::string& pfxStr = pfx.prefix();
            const auto slash = pfxStr.rfind('/');
            if (slash == std::string::npos)
                continue;
            const std::string addrStr = pfxStr.substr(0, slash);
            const auto maskLen =
                static_cast<std::uint8_t>(std::stoul(pfxStr.substr(slash + 1)));

            struct in_addr pfxAddr
            {};
            if (::inet_pton(AF_INET, addrStr.c_str(), &pfxAddr) != 1)
                continue;
            const auto* pfxBytes =
                reinterpret_cast<const std::uint8_t*>(&pfxAddr.s_addr);

            iface.prefixes.push_back(cmdproto::PrefixIpv4{
                {pfxBytes[0], pfxBytes[1], pfxBytes[2], pfxBytes[3]}, maskLen});
        }

        rtsrv::log::info(std::format(
            "[Nexthops]   iface='{}' nexthop='{}' nexthop_id={} prefixes={}",
            route.interface_name(), gateway, nh->id, iface.prefixes.size()));

        req.interfaces.push_back(std::move(iface));
    }

    if (req.interfaces.empty())
    {
        rtsrv::log::info(std::format(
            "[Nexthops] no nni entries for nexthop gw={} id={} — skip",
            gateway, nh->id));
        return;
    }

    rtsrv::log::info(std::format(
        "[Nexthops] submitting SingleRouteRequest ({} interface(s)) "
        "to ud_server for nexthop gw={} id={}",
        req.interfaces.size(), gateway, nh->id));

    vrfClient->submitAdd(std::move(req));
}

/**
 * @brief Silent live-nexthop callback for the startup background thread.
 *
 * Updates the in-memory nexthop table, publishes the snapshot to UDP
 * subscribers, and — when the VRF table and SraUdpClient are wired up —
 * checks whether the changed nexthop appears in the VRF table and sends a
 * SingleRoute request to ud_server.
 */
static void startupNexthopLiveCb(netlink_nexthop_event_t event,
                                 const netlink_nexthop_t* nh,
                                 void* user_data)
{
    auto* ctx = static_cast<NexthopCtx*>(user_data);

    const char* evLabel = (event == NETLINK_NEXTHOP_ADDED)     ? "ADDED"
                          : (event == NETLINK_NEXTHOP_REMOVED) ? "REMOVED"
                                                               : "CHANGED";
    rtsrv::log::info(std::format(
        "[Nexthops] {} id={} gw={} oif={} — checking VRF table",
        evLabel,
        nh->id,
        nh->gateway[0] ? nh->gateway : "-",
        nh->oif_name[0] ? nh->oif_name : std::to_string(nh->oif).c_str()));

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
        ::shutdown(g_startup_route_fd, SHUT_RD);
        netlink_close(g_startup_route_fd);
        g_startup_route_fd = -1;
    }
    if (g_startup_neigh_fd >= 0)
    {
        ::shutdown(g_startup_neigh_fd, SHUT_RD);
        netlink_neigh_close(g_startup_neigh_fd);
        g_startup_neigh_fd = -1;
    }
    if (g_startup_nexthop_fd >= 0)
    {
        ::shutdown(g_startup_nexthop_fd, SHUT_RD);
        netlink_nexthop_close(g_startup_nexthop_fd);
        g_startup_nexthop_fd = -1;
    }
    interruptBackgroundThreads();
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
    // Phase 1: Early logging – console sink active before config is loaded.
    // -----------------------------------------------------------------------
    {
        rtsrv::log::Config early;
        early.app_name               = "sra";
        early.facility               = rtsrv::log::Facility::User;
        early.min_severity           = rtsrv::log::Severity::Debug;
        early.console.enabled = true;
        // human_readable defaults to false → RFC 5424 format from the start.
        if (auto r = rtsrv::log::init(early); !r)
            std::fprintf(stderr, "[sra] early log init failed: %s\n",
                         r.error().c_str());
    }

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
            po::value<std::string>()->default_value(std::string{}),
            "Duplicate unix_domain log output to \"stdout\" or \"stderr\" in"
            " addition to the default log file /var/log/sra.log.<timestamp>")
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
            " | add-del-list | run")
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
        rtsrv::log::err(std::format("command-line error: {}", ex.what()));
        std::cerr << global << '\n';
        return EXIT_FAILURE;
    }

    // Phase 2: Re-initialise the RFC 5424 logger with the configured file sink.
    // Logs always go to /var/log/sra.log.<timestamp> (symlinked from
    // /var/log/sra.log).  --logstream stdout/stderr additionally mirrors
    // every log line to that stream.
    {
        const std::string& lvlStr    = vm["loglevel"].as<std::string>();
        const std::string& extraStream = vm["logstream"].as<std::string>();

        rtsrv::log::Severity sev = rtsrv::log::Severity::Debug;
        if (lvlStr == "INFO" || lvlStr == "2")
            sev = rtsrv::log::Severity::Info;
        else if (lvlStr == "NOTICE" || lvlStr == "3")
            sev = rtsrv::log::Severity::Notice;
        else if (lvlStr == "WARNING" || lvlStr == "4")
            sev = rtsrv::log::Severity::Warning;
        else if (lvlStr == "ERR" || lvlStr == "5")
            sev = rtsrv::log::Severity::Error;

        static constexpr std::string_view kLogFileBase = "/var/log/sra.log";
        const auto ts = static_cast<long long>(std::time(nullptr));
        const std::string timestampedPath =
            std::string{kLogFileBase} + "." + std::to_string(ts);

        ::unlink(std::string{kLogFileBase}.c_str());
        if (::symlink(timestampedPath.c_str(),
                      std::string{kLogFileBase}.c_str()) != 0)
        {
            std::fprintf(stderr,
                         "[sra] cannot create symlink '%s' -> '%s': %s\n",
                         std::string{kLogFileBase}.c_str(),
                         timestampedPath.c_str(),
                         std::strerror(errno));
        }

        rtsrv::log::Config cfg;
        cfg.app_name               = "sra";
        cfg.facility               = rtsrv::log::Facility::User;
        cfg.min_severity           = sev;
        cfg.file.enabled = true;
        cfg.file.path    = timestampedPath;
        // human_readable defaults to false → RFC 5424 wire format in file.
        if (extraStream == "stderr" || extraStream == "stdout")
            cfg.console.enabled = true;
        // human_readable defaults to false → RFC 5424 wire format on console.
        if (auto r = rtsrv::log::init(cfg); !r)
            std::fprintf(stderr, "[sra] log init failed: %s\n",
                         r.error().c_str());

        rtsrv::log::dbg(std::format(
            "sra logger active: file='{}' extra='{}' level={}",
            timestampedPath, extraStream, static_cast<int>(sev)));
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
        std::println(
            "  set-loopback <address>  Store a loopback address on the server");
        std::println(
            "  get-loopback            Retrieve the stored loopback address");
        std::println("  get-loopbacks <loopback>  Query SOT interface list for "
                     "a loopback (IPv4 or IPv6)");
        std::println("  grpc-proc-demo          Run async GrpcProc demo with "
                     "periodic GetLoopback requests");
        std::println("  add-del-list  [socket]  RequestLoopback → GetLoopbacks"
                     " → ADD → DEL → LIST");
        std::println("                          Interface, nexthop and prefix"
                     " data come from SRMD (GetLoopbacks).");
        std::println("                          (default socket:"
                     " /tmp/ud_server.sock)");
        std::println(
            "  run [socket]            Full SRA daemon mode (continuous):");
        std::println("                            1. RequestLoopback → SOT "
                     "auth check via srmd");
        std::println("                            2. GetLoopbacks / "
                     "GetAllRoutes via srmd");
        std::println("                            3. Build SingleRouteRequest "
                     "(nni interfaces only)");
        std::println("                            4. Send to ud_server via "
                     "Unix-domain socket");
        std::println(
            "                            5. Loop until SIGINT/SIGTERM");
        std::println("                          ud_server socket default: "
                     "/tmp/ud_server.sock");
        std::println("                          NOTE: ud_server is a separate "
                     "process; srmd has");
        std::println("                                no Unix-domain socket — "
                     "gRPC only.");
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
            rtsrv::log::warn(std::format(
                "could not load config '{}': {} (using defaults)",
                configPath,
                cfgResult.error()));
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
    const int timeout =
        (timeoutCli > 0) ? timeoutCli : clientCfg.timeout_seconds;

    // CLI --tls overrides config
    const bool useTls = (vm.count("tls") > 0) || clientCfg.tls_enabled;

    // CLI --ca-cert overrides config
    const std::string caCertCli = vm["ca-cert"].as<std::string>();
    const std::string caCert =
        caCertCli.empty() ? clientCfg.ca_cert : caCertCli;

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
            rtsrv::log::err("'sync' requires --sot <path>");
            return EXIT_FAILURE;
        }

        // Parse the SOT file first – no network connections yet
        rtsrv::log::info(std::format("sra  build #{}  Parsing SOT: {}",
                                     rtsrv::build::kBuildNumber,
                                     sotPath));

        auto sotResult = sra::loadSotConfig(sotPath);
        if (!sotResult)
        {
            rtsrv::log::err(
                std::format("Error loading SOT config: {}", sotResult.error()));
            return EXIT_FAILURE;
        }

        logSotSummary(*sotResult);

        // Multi-server sync via switch_config.json
        if (!switchesPath.empty())
        {
            auto cfgResult = sra::loadSwitchConfig(switchesPath);
            if (!cfgResult)
            {
                rtsrv::log::err(std::format("Error loading switch config: {}",
                                            cfgResult.error()));
                return EXIT_FAILURE;
            }
            return cmdSyncMulti(
                *sotResult, *cfgResult, useTls, caCert, timeout);
        }

        // Single-server sync: --server + --node-ip required
        if (nodeIp.empty())
        {
            rtsrv::log::err(
                "single-server sync requires --node-ip <management-ip>"
                " (the key used in nodes_by_loopback)");
            return EXIT_FAILURE;
        }

        rtsrv::log::info(std::format("Switch: {}  node-ip={}", server, nodeIp));

        sra::RouteClient client(server, useTls, caCert, timeout);
        return syncOneServer(client, nodeIp, server, *sotResult);
    }

    // -----------------------------------------------------------------------
    // Multi-server test via switch_config.json
    // -----------------------------------------------------------------------
    if (command == "test" && !switchesPath.empty())
    {
        rtsrv::log::info(std::format("sra  build #{}  switches={}  tls={}",
                                     rtsrv::build::kBuildNumber,
                                     switchesPath,
                                     useTls));

        auto cfgResult = sra::loadSwitchConfig(switchesPath);
        if (!cfgResult)
        {
            rtsrv::log::err(std::format("Error loading switch config: {}",
                                        cfgResult.error()));
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
    NeighCtx startupNeighCtx;
    NexthopCtx startupNhCtx;
    startupRouteCtx.udpServer = &startupUdpServer;
    startupNeighCtx.udpServer = &startupUdpServer;
    startupNhCtx.udpServer = &startupUdpServer;

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
                WatchRoute wr;
                wr.dest = kr.destination;
                wr.nhid = kr.nhid;
                wr.metric = kr.metric;
                wr.table = kr.table;
                wr.protocol = kr.protocol;
                wr.family = static_cast<uint8_t>(kr.family);
                wr.dst_len = kr.prefixLen;
                wr.scope = kr.scope;
                wr.type = kr.type;
                if (!kr.nexthops.empty())
                {
                    for (const auto& knh : kr.nexthops)
                        wr.nexthops.push_back({knh.gateway,
                                               knh.interfaceName,
                                               knh.interfaceIndex,
                                               knh.weight});
                }
                // nhid-based routes: nexthops resolved in step 3b below.
                startupRouteCtx.routes[kr.destination] = std::move(wr);
            }
        }
    }
    catch (const std::exception& ex)
    {
        rtsrv::log::warn(std::format("[Startup] RoutingManager init failed: {}",
                                     ex.what()));
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

    // ── Step 3b: Wire nexthop table into route context and re-resolve ─────
    // Now that both tables are populated, point the route context at the
    // nexthop map so live callbacks and initial routes can resolve nhid.
    startupRouteCtx.nexthops = &startupNhCtx.nexthops;
    for (auto& [key, wr] : startupRouteCtx.routes)
    {
        if (wr.nhid != 0 && wr.nexthops.empty())
            wr.nexthops = resolveNhid(wr.nhid, startupRouteCtx.nexthops);
    }
    startupUdpServer.setRouteData(serializeRouteTable(startupRouteCtx.routes));

    // ── Step 4: Start the UDP server (data is pre-populated) ─────────────
    if (!startupUdpServer.start())
    {
        rtsrv::log::warn("[Startup] UDP table server failed to start");
    }

    // ── Step 5: Open live monitoring fds ─────────────────────────────────
    g_startup_route_fd = netlink_init();
    g_startup_neigh_fd = netlink_neigh_init();
    g_startup_nexthop_fd = netlink_nexthop_init();

    // ── Step 6: Launch background monitor threads ─────────────────────────
    std::thread startupRouteThread;
    std::thread startupNeighThread;
    std::thread startupNhThread;

    if (g_startup_route_fd >= 0)
    {
        const int rfd = g_startup_route_fd;
        startupRouteThread = std::thread([rfd, rctx = &startupRouteCtx]() {
            netlink_run(rfd, startupRouteLiveCb, rctx);
        });
        g_startup_route_tid = startupRouteThread.native_handle();
    }
    if (g_startup_neigh_fd >= 0)
    {
        const int nfd = g_startup_neigh_fd;
        startupNeighThread = std::thread([nfd, nctx = &startupNeighCtx]() {
            netlink_neigh_run(nfd, startupNeighLiveCb, nctx);
        });
        g_startup_neigh_tid = startupNeighThread.native_handle();
    }
    if (g_startup_nexthop_fd >= 0)
    {
        const int nhfd = g_startup_nexthop_fd;
        startupNhThread = std::thread([nhfd, nhctx = &startupNhCtx]() {
            netlink_nexthop_run(nhfd, startupNexthopLiveCb, nhctx);
        });
        g_startup_nexthop_tid = startupNhThread.native_handle();
    }

    // ── RAII cleanup guard ─────────────────────────────────────────────────
    // Joins all background threads and stops the UDP server on every exit
    // path (normal return, exception, or early return from a command).
    struct StartupGuard
    {
        std::thread& routeThread;
        std::thread& neighThread;
        std::thread& nhThread;
        sra::UdpTableServer& udpSrv;

        ~StartupGuard()
        {
            if (g_startup_route_fd >= 0)
            {
                ::shutdown(g_startup_route_fd, SHUT_RD);
                netlink_close(g_startup_route_fd);
                g_startup_route_fd = -1;
            }
            if (g_startup_neigh_fd >= 0)
            {
                ::shutdown(g_startup_neigh_fd, SHUT_RD);
                netlink_neigh_close(g_startup_neigh_fd);
                g_startup_neigh_fd = -1;
            }
            if (g_startup_nexthop_fd >= 0)
            {
                ::shutdown(g_startup_nexthop_fd, SHUT_RD);
                netlink_nexthop_close(g_startup_nexthop_fd);
                g_startup_nexthop_fd = -1;
            }
            // Wake any thread still blocked in recv() before zeroing their
            // IDs.  Without this, a normal exit from a command (e.g.
            // add-del-list) reaches join() with the TIDs already cleared,
            // making interruptBackgroundThreads() a no-op from then on and
            // leaving the netlink threads stuck forever.
            interruptBackgroundThreads();
            // Zero thread IDs *after* the interrupt so that a concurrent
            // signal delivery cannot pthread_kill() an already-joined thread.
            g_startup_route_tid = 0;
            g_startup_neigh_tid = 0;
            g_startup_nexthop_tid = 0;
            if (routeThread.joinable())
                routeThread.join();
            if (neighThread.joinable())
                neighThread.join();
            if (nhThread.joinable())
                nhThread.join();
            udpSrv.stop();
        }
    } startupGuard{startupRouteThread,
                   startupNeighThread,
                   startupNhThread,
                   startupUdpServer};

    // ── Install default signal handler (commands install their own) ───────
    {
        struct sigaction sa
        {};
        sa.sa_handler = startupSigHandler;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGINT, &sa, nullptr);
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
            ::shutdown(g_startup_neigh_fd, SHUT_RD);
            netlink_neigh_close(g_startup_neigh_fd);
            g_startup_neigh_fd = -1;
        }
        g_startup_neigh_tid = 0;
        if (startupNeighThread.joinable())
            startupNeighThread.join();
        return cmdWatchNeighbors(startupUdpServer);
    }

    if (command == "nexthops")
    {
        // Stop the startup nexthop monitor before running the interactive one.
        if (g_startup_nexthop_fd >= 0)
        {
            ::shutdown(g_startup_nexthop_fd, SHUT_RD);
            netlink_nexthop_close(g_startup_nexthop_fd);
            g_startup_nexthop_fd = -1;
        }
        g_startup_nexthop_tid = 0;
        if (startupNhThread.joinable())
            startupNhThread.join();
        return cmdWatchNexthops(startupUdpServer);
    }

    // -----------------------------------------------------------------------
    // Single-server commands
    // -----------------------------------------------------------------------
    rtsrv::log::info(std::format("sra  build #{}  server={}  tls={}",
                                 rtsrv::build::kBuildNumber,
                                 server,
                                 useTls));

    sra::RouteClient client(server, useTls, caCert, timeout);

    // -----------------------------------------------------------------------
    // Startup: request loopback from server based on this client's IP.
    // Skipped for the "watch" command – cmdNetlinkWatch performs this step
    // itself after displaying the initial kernel route table.
    // -----------------------------------------------------------------------
    std::string activeLoopback = clientCfg.loopback;
    if (command != "watch" && command != "neighbors" && command != "nexthops" &&
        command != "add-del-list")
    {
        rtsrv::log::info(
            "[Startup] Requesting loopback from server based on client IP...");
        auto lbResult = client.requestLoopback();
        if (lbResult)
        {
            rtsrv::log::info(std::format(
                "[Startup] Loopback received from server: '{}'", *lbResult));
            activeLoopback = *lbResult;
        }
        else
        {
            rtsrv::log::warn(std::format(
                "[Startup] No loopback from server ({}); "
                "falling back to config loopback: '{}'",
                lbResult.error(),
                activeLoopback));
        }
        rtsrv::log::info(std::format("[Startup] Active loopback set to: '{}'",
                                     activeLoopback));
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
            rtsrv::log::err(std::format("echo: {}", result.error()));
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
            rtsrv::log::err(
                "Usage: sra add <destination> [gateway] [interface] [metric]");
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
            rtsrv::log::err(std::format("add route: {}", result.error()));
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
            rtsrv::log::err("Usage: sra remove <id>");
            return EXIT_FAILURE;
        }
        auto result = client.removeRoute(args[0]);
        if (!result)
        {
            rtsrv::log::err(std::format("remove route: {}", result.error()));
            return EXIT_FAILURE;
        }
        std::println("Route {} removed.", args[0]);
        return EXIT_SUCCESS;
    }

    if (command == "get")
    {
        if (args.empty())
        {
            rtsrv::log::err("Usage: sra get <id>");
            return EXIT_FAILURE;
        }
        auto result = client.getRoute(args[0]);
        if (!result)
        {
            rtsrv::log::err(std::format("get route: {}", result.error()));
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
            rtsrv::log::err(std::format("list routes: {}", result.error()));
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
            ::shutdown(g_startup_route_fd, SHUT_RD);
            netlink_close(g_startup_route_fd);
            g_startup_route_fd = -1;
        }
        g_startup_route_tid = 0;
        if (startupRouteThread.joinable())
            startupRouteThread.join();
        return cmdNetlinkWatch(client, activeLoopback, startupUdpServer);
    }

    if (command == "set-loopback")
    {
        if (args.empty())
        {
            rtsrv::log::err("Usage: sra set-loopback <address>");
            return EXIT_FAILURE;
        }
        auto result = client.setLoopback(args[0]);
        if (!result)
        {
            rtsrv::log::err(std::format("set-loopback: {}", result.error()));
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
            rtsrv::log::err(std::format("get-loopback: {}", result.error()));
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
            rtsrv::log::err("Usage: sra get-loopbacks <loopback-address>");
            return EXIT_FAILURE;
        }
        auto result = client.getLoopbacks(args[0]);
        if (!result)
        {
            rtsrv::log::err(std::format("get-loopbacks: {}", result.error()));
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
                                 pfx.prefix(),
                                 pfx.weight(),
                                 pfx.role(),
                                 pfx.description());
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
    //                 commands encoded in the udproto/routeproto/cmdproto
    //                 stack.
    //   srmd has NO Unix-domain socket — gRPC only.
    //
    // Startup sequence:
    //   1. Start GrpcProc background thread (all gRPC calls go to srmd).
    //   2. [srmd gRPC] RequestLoopback → PERMISSION_DENIED = IP not in SOT →
    //   exit.
    //   3. [srmd gRPC] GetLoopbacks(loopback) → log VRF/interface/prefix
    //   config.
    //   4. Display nexthop / neighbor / /32 route tables (kernel netlink).
    //   5. [srmd gRPC] GetAllRoutes → build SingleRouteRequest for nni
    //   interfaces,
    //      nexthop_id looked up from the kernel nexthop table.
    //   6. [ud_server Unix socket] Submit SingleRouteRequest via SraUdpClient.
    //   7. Main loop: keep running until SIGINT/SIGTERM.
    //
    // Non-blocking I/O:
    //   - gRPC  : RPCs submitted to GrpcProc; executed in a background thread.
    //   - Unix  : SraUdpClient uses O_NONBLOCK + poll() for all socket I/O.
    // -----------------------------------------------------------------------
    if (command == "run")
    {
        // Install signal handler for the run command.
        {
            struct sigaction sa
            {};
            sa.sa_handler = runSigHandler;
            sigemptyset(&sa.sa_mask);
            sigaction(SIGINT, &sa, nullptr);
            sigaction(SIGTERM, &sa, nullptr);
        }

        rtsrv::log::info(std::format(
            "[run] SRA daemon starting — server={} tls={}", server, useTls));

        // ── Step 1: Start GrpcProc (non-blocking gRPC) ──────────────────────
        sra::GrpcProc grpcProc(client, /*autoStart=*/true);
        rtsrv::log::info("[run] GrpcProc started (non-blocking gRPC thread)");

        // ── Step 2: RequestLoopback ──────────────────────────────────────────
        rtsrv::log::info("[run] [gRPC] Submitting RequestLoopback...");
        const uint64_t lbId = grpcProc.submit(sra::RequestLoopbackParams{});
        auto lbResp = grpcProc.waitForResponse(lbId, std::chrono::seconds(30));
        if (!lbResp)
        {
            rtsrv::log::err("[run] RequestLoopback: timeout");
            return EXIT_FAILURE;
        }

        const auto& lbResult =
            std::get<9>(lbResp->payload); // index 9 = RequestLoopbackResult
        if (!lbResult)
        {
            rtsrv::log::err(std::format(
                "[run] RequestLoopback failed: {} — "
                "SRA IP not in SOT, closing gRPC connection",
                lbResult.error()));
            grpcProc.stop();
            return EXIT_FAILURE;
        }

        const std::string loopback = *lbResult;
        rtsrv::log::info(
            std::format("[run] Loopback from SRMD: '{}'", loopback));

        // ── Step 3: GetLoopbacks ─────────────────────────────────────────────
        rtsrv::log::info(
            std::format("[run] [gRPC] Submitting GetLoopbacks('{}')...",
                        loopback));
        const uint64_t glId =
            grpcProc.submit(sra::GetLoopbacksParams{loopback});
        auto glResp = grpcProc.waitForResponse(glId, std::chrono::seconds(30));
        if (glResp)
        {
            const auto& glResult =
                std::get<sra::GetLoopbacksResult>(glResp->payload);
            if (glResult)
            {
                rtsrv::log::info(std::format(
                    "[run] GetLoopbacks: {} — {} interface(s)",
                    glResult->message(),
                    glResult->interfaces_size()));
                for (const auto& ifc : glResult->interfaces())
                {
                    rtsrv::log::info(std::format(
                        "[run]   iface='{}' type={} local={} "
                        "nexthop={} weight={}",
                        ifc.name(),
                        ifc.type(),
                        ifc.local_address(),
                        ifc.nexthop().empty() ? "(none)" : ifc.nexthop(),
                        ifc.weight()));
                    for (const auto& pfx : ifc.prefixes())
                    {
                        rtsrv::log::info(std::format(
                            "[run]     prefix={} weight={} role={}",
                            pfx.prefix(),
                            pfx.weight(),
                            pfx.role()));
                    }
                }
            }
            else
            {
                rtsrv::log::warn(std::format("[run] GetLoopbacks failed: {}",
                                             glResult.error()));
            }
        }

        // ── Step 4: Log kernel tables (populated during startup) ─────────────
        rtsrv::log::info(std::format("[run] Nexthop table : {} entry/entries",
                                     startupNhCtx.nexthops.size()));
        for (const auto& [id, nh] : startupNhCtx.nexthops)
        {
            rtsrv::log::info(std::format(
                "[run]   nexthop id={} gw='{}' oif='{}' proto={}",
                id,
                nh.gateway.empty() ? "-" : nh.gateway,
                nh.oif_name.empty() ? std::to_string(nh.oif) : nh.oif_name,
                static_cast<unsigned>(nh.protocol)));
        }

        rtsrv::log::info(std::format("[run] Neighbor table: {} entry/entries",
                                     startupNeighCtx.neighbors.size()));
        rtsrv::log::info(std::format("[run] /32 route table: {} entry/entries",
                                     startupRouteCtx.routes.size()));

        // ── Step 5: GetAllRoutes ─────────────────────────────────────────────
        rtsrv::log::info("[run] [gRPC] Submitting GetAllRoutes...");
        const uint64_t arId = grpcProc.submit(sra::GetAllRoutesParams{});
        auto arResp = grpcProc.waitForResponse(arId, std::chrono::seconds(30));
        if (!arResp)
        {
            rtsrv::log::err("[run] GetAllRoutes: timeout");
            return EXIT_FAILURE;
        }

        const auto& arResult =
            std::get<sra::GetAllRoutesResult>(arResp->payload);
        if (!arResult)
        {
            rtsrv::log::err(
                std::format("[run] GetAllRoutes failed: {}", arResult.error()));
            return EXIT_FAILURE;
        }

        printAllRoutes(*arResult);

        // ── Step 6: Build SingleRouteRequest from GetAllRoutes (nni only)
        // ───── For each VrfRoute with interface_type == "nni":
        //   • look up nexthop_id from the kernel nexthop table by gateway IP
        //   • convert each prefix string to binary PrefixIpv4 entries
        const std::string udSocketPath =
            args.empty() ? "/tmp/ud_server.sock" : args[0];

        sra::SraUdpClient vrfClient(udSocketPath);
        vrfClient.start();
        rtsrv::log::info(std::format(
            "[run] SraUdpClient started (non-blocking Unix socket='{}')",
            udSocketPath));

        // ── Load VRF table and arm the nexthop event handler ─────────────────
        // Future nexthop ADDED / CHANGED / REMOVED events will look up the
        // affected gateway in this table and re-submit a SingleRouteRequest
        // (SINGLE_ROUTE, type=1) to ud_server whenever a match is found.
        sra::VrfTable vrfTable;
        vrfTable.load(*arResult);
        startupNhCtx.vrfTable = &vrfTable;
        startupNhCtx.vrfClient = &vrfClient;
        startupNhCtx.neighCtx = &startupNeighCtx;
        rtsrv::log::info(std::format(
            "[run] VRF table loaded: {} entry/entries; "
            "nexthop event handler armed",
            vrfTable.size()));

        int nniCount = 0;

        for (const auto& gateway : vrfTable.nexthops())
        {
            // Only send ICMP if the nexthop is already resolved in the
            // adjacency table (MAC known).
            if (neighborHasIp(startupNeighCtx, gateway))
            {
                rtsrv::log::info(std::format(
                    "[run] nexthop '{}' in adjacency table — "
                    "sending ICMP echo request",
                    gateway));
                sendIcmpEchoRequest(gateway);
            }

            // Look up nexthop_id from the kernel nexthop table by gateway IP.
            uint32_t nexthopId = 0;
            for (const auto& [id, nh] : startupNhCtx.nexthops)
            {
                if (nh.gateway == gateway)
                {
                    nexthopId = id;
                    break;
                }
            }

            // Parse nexthop address into binary form.
            struct in_addr nhAddr
            {};
            if (::inet_pton(AF_INET, gateway.c_str(), &nhAddr) != 1)
            {
                rtsrv::log::warn(std::format(
                    "[run] invalid nexthop address '{}' — skipping", gateway));
                continue;
            }
            const auto* nhBytes =
                reinterpret_cast<const std::uint8_t*>(&nhAddr.s_addr);

            // Build a SingleRouteRequest for this nexthop's nni routes only.
            cmdproto::SingleRouteRequest req;
            const auto routes = vrfTable.findByNexthop(gateway);
            for (const auto& route : routes)
            {
                if (route.interface_type() != "nni")
                    continue;
                if (route.nexthop().empty())
                    continue;

                if (req.vrfs_name.empty())
                    req.vrfs_name = route.vrf_name();

                cmdproto::Interface iface{};
                const std::string& ifn = route.interface_name();
                for (std::size_t k = 0;
                     k < cmdproto::IFACE_NAME_SIZE && k < ifn.size();
                     ++k)
                {
                    iface.iface_name[k] = ifn[k];
                }
                iface.nexthop_addr_ipv4 = {
                    nhBytes[0], nhBytes[1], nhBytes[2], nhBytes[3]};
                iface.nexthop_id_ipv4 = nexthopId;

                for (const auto& pfx : route.prefixes())
                {
                    const std::string& pfxStr = pfx.prefix();
                    const auto slash = pfxStr.rfind('/');
                    if (slash == std::string::npos)
                        continue;
                    const std::string addrStr = pfxStr.substr(0, slash);
                    const auto maskLen = static_cast<std::uint8_t>(
                        std::stoul(pfxStr.substr(slash + 1)));

                    struct in_addr pfxAddr
                    {};
                    if (::inet_pton(AF_INET, addrStr.c_str(), &pfxAddr) != 1)
                        continue;
                    const auto* pfxBytes =
                        reinterpret_cast<const std::uint8_t*>(&pfxAddr.s_addr);

                    iface.prefixes.push_back(cmdproto::PrefixIpv4{
                        {pfxBytes[0], pfxBytes[1], pfxBytes[2], pfxBytes[3]},
                        maskLen});
                }

                rtsrv::log::info(std::format(
                    "[run] nni interface: iface='{}' nexthop='{}' "
                    "nexthop_id={} prefixes={}",
                    route.interface_name(),
                    gateway,
                    nexthopId,
                    iface.prefixes.size()));

                req.interfaces.push_back(std::move(iface));
                ++nniCount;
            }

            if (req.interfaces.empty())
            {
                rtsrv::log::info(std::format(
                    "[run] nexthop '{}' has no nni interfaces — skip",
                    gateway));
                continue;
            }

            rtsrv::log::info(std::format(
                "[run] Submitting SingleRouteRequest ({} nni interface(s)) "
                "for nexthop '{}' to Unix socket...",
                req.interfaces.size(),
                gateway));
            vrfClient.submitAdd(std::move(req));
        }

        if (nniCount == 0)
        {
            rtsrv::log::warn("[run] No nni interfaces found in GetAllRoutes "
                             "response — no route-add request sent");
        }

        // ── Step 7: Main daemon loop
        // ────────────────────────────────────────── Netlink background threads
        // (routes, neighbors, nexthops) keep running and update the in-memory
        // tables automatically. The gRPC channel stays open via GrpcProc. The
        // SraUdpClient thread awaits further submit() calls.
        rtsrv::log::info("[run] Daemon running (Ctrl-C to stop)...");
        while (!g_run_stop)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        rtsrv::log::info("[run] Shutdown signal received — stopping");

        // runSigHandler() already closed the fds and called
        // interruptBackgroundThreads().  The stopNetlinkFd calls below are
        // no-ops when the fds are already -1, but they handle the edge case
        // where the signal arrived before this shutdown path ran.
        //
        // Shutdown sequence for each background thread:
        //  1. close(fd) in signal handler — marks the fd as invalid.
        //  2. pthread_kill(tid, SIGINT)   — interrupts blocking recv() with
        //  EINTR.
        //  3. netlink_*_run EINTR path    — retries recv() on now-closed fd.
        //  4. recv() returns EBADF        — netlink_*_run returns -1, thread
        //  exits.
        //  5. StartupGuard::~StartupGuard() joins the thread successfully.
        //
        // Note: shutdown(SHUT_RD) on AF_NETLINK returns EOPNOTSUPP on Linux;
        // it is kept as a best-effort attempt for other socket types.
        auto stopNetlinkFd = [](volatile int& fd,
                                void (*closeFn)(int)) noexcept {
            if (fd >= 0)
            {
                ::shutdown(fd, SHUT_RD);
                closeFn(fd);
                fd = -1;
            }
        };
        stopNetlinkFd(g_startup_route_fd, netlink_close);
        stopNetlinkFd(g_startup_neigh_fd, netlink_neigh_close);
        stopNetlinkFd(g_startup_nexthop_fd, netlink_nexthop_close);

        vrfClient.stop();
        grpcProc.stop();
        return EXIT_SUCCESS; // StartupGuard joins threads (now exiting)
    }

    if (command == "add-del-list")
    {
        std::vector<const sra::KernelRoute*> ospf32;

        const std::string socketPath =
            args.empty() ? "/tmp/ud_server.sock" : args[0];

        rtsrv::log::info(std::format("[add-del-list] socket={} — starting",
                                     socketPath));

        // Install signal handler so CTRL+C triggers a clean shutdown instead
        // of leaving the process stuck in StartupGuard::~StartupGuard().
        {
            struct sigaction sa
            {};
            sa.sa_handler = addDelListSigHandler;
            sigemptyset(&sa.sa_mask);
            sigaction(SIGINT, &sa, nullptr);
            sigaction(SIGTERM, &sa, nullptr);
        }

        sra::SraUdpClient vrfClient(socketPath);
        vrfClient.start();

        // ── Step 1: RequestLoopback ──────────────────────────────────────────
        rtsrv::log::info("[add-del-list] requesting loopback from srmd...");
        auto lbResult = client.requestLoopback();
        if (lbResult)
        {
            rtsrv::log::info(std::format(
                "[add-del-list] loopback from srmd: '{}'", *lbResult));
            activeLoopback = *lbResult;
        }
        else
        {
            rtsrv::log::info(std::format(
                "[add-del-list] loopback from srmd: {} (using config: '{}')",
                lbResult.error(),
                activeLoopback));
        }

        if (activeLoopback.empty())
        {
            rtsrv::log::err("[add-del-list] no loopback available — "
                            "cannot fetch prefixes");
            vrfClient.stop();
            return EXIT_FAILURE;
        }

        // ── Step 2: GetLoopbacks ─────────────────────────────────────────────
        rtsrv::log::info(std::format(
            "[add-del-list] GetLoopbacks('{}')...", activeLoopback));
        auto glResult = client.getLoopbacks(activeLoopback);
        if (!glResult)
        {
            rtsrv::log::err(std::format("[add-del-list] GetLoopbacks failed: {}",
                                        glResult.error()));
            vrfClient.stop();
            return EXIT_FAILURE;
        }

        rtsrv::log::info(std::format(
            "[add-del-list] GetLoopbacks: {} — {} interface(s)",
            glResult->message(),
            glResult->interfaces_size()));

        // ── Nexthop adjacency check for Loopbacks ────────────────────────────
        // For each interface returned by GetLoopbacks, extract the nexthop IP.
        // If it is not yet in the adjacency (ARP) table, send an ICMP echo to
        // trigger ARP resolution so the nexthop MAC becomes available.
        for (const auto& li : glResult->interfaces())
        {
            const std::string& nh = li.nexthop();
            if (nh.empty())
                continue;
            struct in_addr nhCheck
            {};
            if (::inet_pton(AF_INET, nh.c_str(), &nhCheck) != 1)
                continue;
            if (!neighborHasIp(startupNeighCtx, nh))
            {
                rtsrv::log::info(std::format(
                    "[add-del-list] nexthop '{}' not in adjacency table — "
                    "sending ICMP echo request",
                    nh));
                sendIcmpEchoRequest(nh);
            }
            else
            {
                rtsrv::log::info(std::format(
                    "[add-del-list] nexthop '{}' already in adjacency table",
                    nh));
            }
        }

        // ── Step 2b: GetAllRoutes (to obtain VRF name) ───────────────────────
        std::string vrfsName;
        auto arResult2 = client.getAllRoutes();
        if (arResult2)
        {
            for (const auto& route : arResult2->routes())
            {
                if (route.interface_type() == "nni" &&
                    !route.vrf_name().empty())
                {
                    vrfsName = route.vrf_name();
                    break;
                }
            }
        }
        rtsrv::log::info(
            std::format("[add-del-list] VRF name: '{}'", vrfsName));

        // ── Step 3: Build SingleRouteRequest (nni interfaces only) ───────────
        cmdproto::SingleRouteRequest singleReq;
        singleReq.vrfs_name = vrfsName;

        // Seed the nexthop ID pool from the kernel nexthop table so that IDs
        // already present in the kernel are not allocated again.
        netlink::NexthopIdPool nhPool;
        for (const auto& [id, nh] : startupNhCtx.nexthops)
            nhPool.markUsed(id);

        sra::RedisRib redisRib;
        if (!redisRib.connected())
            rtsrv::log::warn("[add-del-list] Redis not available — "
                             "prefix entries will not be written");
        else
            redisRib.clear();

        for (const auto& li : glResult->interfaces())
        {
            if (li.type() != "nni")
                continue;
            if (li.nexthop().empty())
                continue;

            struct in_addr nhAddr
            {};
            if (::inet_pton(AF_INET, li.nexthop().c_str(), &nhAddr) != 1)
                continue;
            const auto* nhBytes =
                reinterpret_cast<const std::uint8_t*>(&nhAddr.s_addr);

            cmdproto::Interface entry{};
            const std::string& ifn = li.name();
            for (std::size_t k = 0;
                 k < cmdproto::IFACE_NAME_SIZE && k < ifn.size();
                 ++k)
                entry.iface_name[k] = ifn[k];
            entry.nexthop_addr_ipv4 = {
                nhBytes[0], nhBytes[1], nhBytes[2], nhBytes[3]};

            // Look up the kernel nexthop object ID for this gateway IP so that
            // the ud_server can reference the nexthop by ID (e.g. for ECMP).
            uint32_t nhId = 0;
            for (const auto& [id, nh] : startupNhCtx.nexthops)
            {
                if (nh.gateway == li.nexthop())
                {
                    nhId = id;
                    break;
                }
            }

            // If no kernel nexthop exists for this gateway, allocate an ID
            // from the pool and install a new nexthop object so ud_server can
            // reference it by ID.
            if (nhId == 0)
            {
                const uint32_t allocId = nhPool.allocate();
                if (allocId != 0)
                {
                    netlink::NexthopAddParams nhParams;
                    nhParams.id = allocId;
                    nhParams.if_name = li.name();
                    if (auto gw = netlink::parse_ipv4(li.nexthop()); gw)
                    {
                        nhParams.gateway = *gw;
                        nhParams.has_gateway = true;
                    }
                    if (auto r = netlink::add_nexthop(nhParams); r)
                    {
                        nhId = allocId;
                        rtsrv::log::info(std::format(
                            "[add-del-list]   installed nexthop id={} gw='{}'",
                            nhId,
                            li.nexthop()));
                        // Cache in the in-memory table so other interfaces
                        // sharing the same gateway reuse this ID.
                        NexthopEntry poolEnt;
                        poolEnt.id = nhId;
                        poolEnt.gateway = li.nexthop();
                        poolEnt.oif_name = li.name();
                        startupNhCtx.nexthops[nhId] = poolEnt;
                    }
                    else
                    {
                        rtsrv::log::err(std::format(
                            "[add-del-list]   add_nexthop id={} gw='{}' "
                            "failed: {}",
                            allocId,
                            li.nexthop(),
                            r.error().message()));
                        nhPool.release(allocId);
                    }
                }
                else
                {
                    rtsrv::log::err(std::format(
                        "[add-del-list]   nexthop pool exhausted for gw='{}'",
                        li.nexthop()));
                }
            }

            entry.nexthop_id_ipv4 = nhId;

            for (const auto& pfx : li.prefixes())
            {
                const std::string& pfxStr = pfx.prefix();
                const auto slash = pfxStr.rfind('/');
                if (slash == std::string::npos)
                    continue;
                struct in_addr pfxAddr
                {};
                if (::inet_pton(AF_INET,
                                pfxStr.substr(0, slash).c_str(),
                                &pfxAddr) != 1)
                    continue;
                const auto* pb =
                    reinterpret_cast<const std::uint8_t*>(&pfxAddr.s_addr);
                const auto maskLen = static_cast<std::uint8_t>(
                    std::stoul(pfxStr.substr(slash + 1)));
                entry.prefixes.push_back(cmdproto::PrefixIpv4{
                    {pb[0], pb[1], pb[2], pb[3]}, maskLen});

                if (redisRib.connected())
                    redisRib.set(pfxStr, std::to_string(nhId));
            }

            rtsrv::log::info(std::format(
                "[add-del-list]   iface='{}' nexthop='{}' prefixes={}",
                ifn,
                li.nexthop(),
                entry.prefixes.size()));

            singleReq.interfaces.push_back(std::move(entry));
        }

        if (!singleReq.interfaces.empty())
        {
            // ── ROUTE_ADD ────────────────────────────────────────────────────
            rtsrv::log::info(std::format(
                "[add-del-list] [ADD] submitting {} interface(s) to ud_server...",
                singleReq.interfaces.size()));
            auto savedInterfaces = singleReq.interfaces;
            vrfClient.submitAdd(std::move(singleReq));

            // ── ROUTE_DEL (one per prefix) ───────────────────────────────────
            rtsrv::log::info(
                "[add-del-list] [DEL] deleting each added prefix...");
            for (const auto& iface : savedInterfaces)
            {
                for (const auto& pfx : iface.prefixes)
                {
                    cmdproto::RouteDelParams delParams;
                    delParams.dst_addr = pfx.addr;
                    delParams.prefix_len = pfx.mask_len;
                    delParams.gateway = iface.nexthop_addr_ipv4;
                    delParams.if_name = std::string(iface.iface_name.data());
                    delParams.vrfs_name = vrfsName;
                    vrfClient.submitDelete(delParams);
                }
            }

            // ── ROUTE_LIST ───────────────────────────────────────────────────
            rtsrv::log::info("[add-del-list] [LIST] requesting route table...");
            vrfClient.submitList(vrfsName);
        }
        else
        {
            rtsrv::log::info("[add-del-list] no interfaces to submit");
        }

        // ── Step 4: List current OSPF /32 routes from kernel routing table ──

        rtsrv::log::info(
            "[add-del-list] reading OSPF /32 routes from kernel...");
        // Declared here so its lifetime covers all uses of ospf32 (which stores
        // raw pointers into the vector elements).
        std::expected<std::vector<sra::KernelRoute>, std::string> routesResult;
        try
        {
            sra::RoutingManager rm;
            routesResult = rm.listRoutes(AF_INET, RT_TABLE_UNSPEC);
        }
        catch (const std::exception& e)
        {
            rtsrv::log::err(std::format(
                "[add-del-list] RoutingManager exception: {}", e.what()));
        }

        if (!routesResult)
        {
            rtsrv::log::err(std::format(
                "[add-del-list] listRoutes (kernel) failed: {}",
                routesResult.error()));
        }
        else
        {
            for (const auto& kr : *routesResult)
            {
                if (kr.prefixLen == 32 && kr.protocol == RTPROT_OSPF &&
                    kr.type == RTN_UNICAST)
                    ospf32.push_back(&kr);
            }

            rtsrv::log::info(std::format("[add-del-list] OSPF /32 routes: {}",
                                         ospf32.size()));

            std::map<uint32_t, NexthopEntry> nhTable;
            {
                int nhfd = netlink_nexthop_init();
                if (nhfd >= 0)
                {
                    netlink_nexthop_dump(nhfd, nlNhDumpToMapCb, &nhTable);
                    netlink_nexthop_close(nhfd);
                }
                else
                {
                    rtsrv::log::warn(
                        "[add-del-list] netlink_nexthop_init failed, "
                        "nhid-based routes will not be resolved");
                }
            }

            for (const auto* kr : ospf32)
            {
                const bool isEcmp = kr->nexthops.size() > 1;
                const bool isNhid = kr->nexthops.empty() && kr->nhid != 0;

                rtsrv::log::info(std::format(
                    "[add-del-list] dst={} id={} metric={} table={} "
                    "proto=ospf  type={}",
                    kr->destination,
                    kr->nhid,
                    kr->metric,
                    kr->table,
                    isEcmp ? "ecmp"
                           : (isNhid ? "single-path(nhid)" : "single-path")));

                if (isNhid)
                {
                    const auto resolved = resolveNhid(kr->nhid, &nhTable);
                    // if (resolved.empty())
                    //{
                    //     std::println("\t\t\tnexthop: nhid={} (not resolved)",
                    //                  kr->nhid);
                    // }
                    // else
                    {
                        for (const auto& nh : resolved)
                        {
                            rtsrv::log::info(std::format(
                                "[add-del-list]   nexthop: nhid={} gw={} "
                                "iface={} iface_idx={}",
                                kr->nhid,
                                nh.gateway,
                                nh.dev,
                                nh.ifindex));
                        }
                    }
                }
                else
                {
                    for (const auto& nh : kr->nexthops)
                    {
                        rtsrv::log::info(std::format(
                            "[add-del-list]   nexthop: nhid={} gw={} iface={} "
                            "iface_idx={} weight={}",
                            nh.hhid,
                            nh.gateway,
                            nh.interfaceName,
                            nh.interfaceIndex,
                            nh.weight));
                    }
                }
            }
        }

        // ── Step 5: GetRemainingLoopbacks ────────────────────────────────────
        rtsrv::log::info(
            "[add-del-list] fetching remaining nodes from srmd...");
        auto rnResult = client.getRemainingNodes();
        if (!rnResult)
        {
            rtsrv::log::warn(std::format(
                "[add-del-list] GetRemainingNodes failed: {}",
                rnResult.error()));
        }
        else
        {
            rtsrv::log::info(std::format("[add-del-list] remaining nodes: {}",
                                         rnResult->nodes_size()));
            for (const auto& node : rnResult->nodes())
            {
                rtsrv::log::info(std::format(
                    "[add-del-list]   hostname='{}' management_ip='{}' "
                    "loopback_ipv4='{}' loopback_ipv6='{}'",
                    node.hostname(),
                    node.management_ip(),
                    node.loopback_ipv4(),
                    node.loopback_ipv6()));
            }

            // ── Step 6: For each remaining node fetch and log all prefixes ───
            for (const auto& node : rnResult->nodes())
            {
                const std::string& nodeIp = node.management_ip();

                rtsrv::log::info(std::format(
                    "[add-del-list] GetLoopbacksByNodeIp: node='{}' "
                    "node_ip='{}'...",
                    node.hostname(),
                    nodeIp));
                auto nodeGl = client.getLoopbacksByNodeIp(nodeIp);
                if (!nodeGl)
                {
                    rtsrv::log::warn(std::format("[add-del-list]   failed: {}",
                                                 nodeGl.error()));
                    continue;
                }

                rtsrv::log::info(std::format("[add-del-list]   {} prefix(es):",
                                             nodeGl->prefixes_size()));
                for (const auto& pfx : nodeGl->prefixes())
                {
                    rtsrv::log::info(std::format(
                        "[add-del-list]     prefix='{}' weight={} role='{}'"
                        " description='{}'",
                        pfx.prefix(),
                        pfx.weight(),
                        pfx.role(),
                        pfx.description()));
                }

                // ── Save loopback data to global map for the OSPF callback ───
                {
                    LoopbackCbEntry cbEntry;
                    cbEntry.hostname = node.hostname();
                    for (const auto& pfx : nodeGl->prefixes())
                    {
                        const std::string& pfxStr = pfx.prefix();
                        const auto slash = pfxStr.rfind('/');
                        if (slash == std::string::npos)
                            continue;
                        struct in_addr pfxAddr
                        {};
                        if (::inet_pton(AF_INET,
                                        pfxStr.substr(0, slash).c_str(),
                                        &pfxAddr) != 1)
                            continue;
                        const auto* pb = reinterpret_cast<const std::uint8_t*>(
                            &pfxAddr.s_addr);
                        const auto maskLen = static_cast<std::uint8_t>(
                            std::stoul(pfxStr.substr(slash + 1)));
                        cbEntry.prefixes.push_back(cmdproto::PrefixIpv4{
                            {pb[0], pb[1], pb[2], pb[3]}, maskLen});
                        cbEntry.prefix_strings.push_back(pfxStr);
                    }
                    for (const auto* kr : ospf32)
                    {
                        if (kr->destination.contains(node.loopback_ipv4()))
                        {
                            cbEntry.last_nhid = kr->nhid;
                            break;
                        }
                    }
                    rtsrv::log::info(std::format(
                        "[add-del-list] global map: saved loopback='{}'"
                        " hostname='{}' prefixes={} initial_nhid={}",
                        node.loopback_ipv4(),
                        cbEntry.hostname,
                        cbEntry.prefixes.size(),
                        cbEntry.last_nhid));
                    g_loopback_cb_map[node.loopback_ipv4()] =
                        std::move(cbEntry);

                    // Create an ECMP group for this loopback so that the
                    // OSPF /32 netlink monitor (Step 8) can look it up
                    // and update membership on each subsequent route event.
                    // Seed with last_nhid if already known from the kernel
                    // route snapshot so the initial ROUTE_ADD carries the
                    // correct nexthop_id_ipv4 immediately.
                    const uint32_t seedNhid =
                        g_loopback_cb_map[node.loopback_ipv4()].last_nhid;
                    g_ecmp_groups.create(node.loopback_ipv4(), node.hostname());
                    if (seedNhid != 0)
                        g_ecmp_groups.add_member(node.loopback_ipv4(), seedNhid);
                    rtsrv::log::info(std::format(
                        "[add-del-list] ECMP group created: loopback='{}'"
                        " hostname='{}' seed_nhid={}",
                        node.loopback_ipv4(),
                        node.hostname(),
                        seedNhid));
                }

                // ── Nexthop adjacency check for LoopbacksByNodeIp ────────────
                // The nexthop for a remaining node's prefixes is the gateway
                // from the OSPF /32 route whose destination matches the node's
                // loopback.  If that nexthop is not yet in the adjacency table,
                // send an ICMP echo to trigger ARP resolution.
                for (const auto* kr : ospf32)
                {
                    if (!kr->destination.contains(node.loopback_ipv4()))
                        continue;
                    const std::string krGw = kr->nexthops.empty()
                                                 ? std::string{}
                                                 : kr->nexthops[0].gateway;
                    if (krGw.empty())
                        continue;
                    struct in_addr nhCheck
                    {};
                    if (::inet_pton(AF_INET, krGw.c_str(), &nhCheck) != 1)
                        continue;
                    if (!neighborHasIp(startupNeighCtx, krGw))
                    {
                        rtsrv::log::info(std::format(
                            "[add-del-list] nexthop '{}' for node '{}' "
                            "not in adjacency table — sending ICMP echo request",
                            krGw,
                            node.hostname()));
                        sendIcmpEchoRequest(krGw);
                    }
                    else
                    {
                        rtsrv::log::info(std::format(
                            "[add-del-list] nexthop '{}' for node '{}' "
                            "already in adjacency table",
                            krGw,
                            node.hostname()));
                    }
                }

                // send ROUTE_ADD
                // ── Step 7: Prepare ROUTE_ADD for Remaining Loopbacks
                // ────────────────
                prepare_route_add_remain_lb(
                    vrfClient, node.loopback_ipv4(), nodeGl, ospf32, redisRib);
            }
        }

        // ── Step 8: OSPF /32 NetLink monitor (background thread) ────────────
        rtsrv::log::info(
            "[add-del-list] starting OSPF /32 netlink monitor...");
        std::thread ospfNlThread;
        {
            const int nlFd = netlink_init();
            if (nlFd < 0)
            {
                rtsrv::log::warn(std::format(
                    "[add-del-list] netlink_init failed: {} — "
                    "OSPF monitor disabled",
                    std::strerror(errno)));
            }
            else
            {
                g_add_del_list_nl_fd = nlFd;
                g_add_del_list_cb_ctx.vrfClient = &vrfClient;
                g_add_del_list_cb_ctx.redisRib = &redisRib;
                ospfNlThread = std::thread([nlFd]() {
                    netlink_run(nlFd, addDelListOspfCb, &g_add_del_list_cb_ctx);
                });
                g_add_del_list_nl_tid = ospfNlThread.native_handle();
                rtsrv::log::info(std::format(
                    "[add-del-list] OSPF /32 netlink monitor active (fd={})",
                    nlFd));
            }
        }

        // Keep the process (and the SraUdpClient connection) alive until
        // CTRL+C so routes remain programmed in hardware.
        rtsrv::log::info("[add-del-list] running — press Ctrl-C to stop");
        while (!g_add_del_list_stop)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        rtsrv::log::info(
            "[add-del-list] shutdown signal received — stopping");

        // Signal handler already closed g_add_del_list_nl_fd and sent SIGINT
        // to the monitor thread; join it now that it has exited netlink_run().
        g_add_del_list_nl_tid = 0;
        if (ospfNlThread.joinable())
            ospfNlThread.join();

        vrfClient.stop();
        return EXIT_SUCCESS;
    }

    rtsrv::log::err(std::format("Unknown command: '{}'", command));
    rtsrv::log::err("Run 'sra --help' for usage.");
    return EXIT_FAILURE;
}
// std::expected<srmd::v1::GetLoopbacksResponse, std::string> glResult

// dst=2.2.2.2/32 via=192.168.0.2 dev=Ethernet46 metric=20 table=254 proto=ospf
//
int prepare_route_add_remain_lb(
    sra::SraUdpClient& vrfClient,
    const std::string& loopback_ipv4,
    std::expected<srmd::v1::GetNodePrefixesResponse, std::string>& prefixes,
    const std::vector<const sra::KernelRoute*>& ospf32,
    sra::RedisRib& redisRib)
{
    cmdproto::SingleRouteRequest singleReq;
    singleReq.vrfs_name = "RemainLoopbaks";

    rtsrv::log::info(
        std::format("[add-del-list] START for {}", loopback_ipv4));

    rtsrv::log::info(
        std::format("[add-del-list] OSPF /32 routes: {}", ospf32.size()));
    for (const auto* kr : ospf32)
    {
        const std::string krGw =
            kr->nexthops.empty() ? std::string{} : kr->nexthops[0].gateway;
        const std::string krIface = kr->nexthops.empty()
                                        ? std::string{}
                                        : kr->nexthops[0].interfaceName;

        rtsrv::log::info(std::format(
            "[add-del-list]   dst={} via={} dev={} metric={} table={} "
            "proto=ospf",
            kr->destination,
            krGw.empty() ? "(none)" : krGw,
            krIface.empty() ? "?" : krIface,
            kr->metric,
            kr->table));

        if (kr->destination.contains(loopback_ipv4) &&
            (kr->nhid != 0 || !krIface.empty()))
        {
            rtsrv::log::info(std::format("{} in {}", loopback_ipv4,
                                         kr->destination));
            rtsrv::log::info(std::format(
                "[add-del-list] prepare_route_add_remain_lb prepare "
                "ROUTE_ADD for {}",
                loopback_ipv4));

            // Register this kernel nhid in the loopback's ECMP group.
            // The group was created during Step 6 with a seed nhid; calling
            // add_member() here is idempotent if the same nhid was seeded.
            if (kr->nhid != 0)
            {
                const uint32_t prevNhid = g_ecmp_groups.group_nhid(loopback_ipv4);
                const uint32_t newNhid  = g_ecmp_groups.add_member(loopback_ipv4, kr->nhid);
                if (prevNhid == 0 && newNhid != 0)
                    rtsrv::log::info(std::format(
                        "[add-del-list] *** SRA created new ECMP multi-path nexthop:"
                        " loopback='{}' kernel_nhid={} members={}"
                        " (RTM_NEWNEXTHOP NLM_F_CREATE, kernel-assigned NHA_ID)",
                        loopback_ipv4, newNhid,
                        g_ecmp_groups.get(loopback_ipv4).size()));
            }

            // Use the SRA-owned ECMP group's kernel NHA_ID for ROUTE_ADD so
            // that any accumulated members are reflected in the programmed entry.
            const uint32_t groupNhid =
                g_ecmp_groups.group_nhid(loopback_ipv4);

            struct in_addr nhAddr
            {};
            if (::inet_pton(AF_INET, loopback_ipv4.c_str(), &nhAddr) != 1)
                continue;
            const auto* nhBytes =
                reinterpret_cast<const std::uint8_t*>(&nhAddr.s_addr);

            cmdproto::Interface entry{};
            static constexpr std::string_view kUnneeded = "unneeded";
            for (std::size_t k = 0;
                 k < cmdproto::IFACE_NAME_SIZE && k < kUnneeded.size();
                 ++k)
                entry.iface_name[k] = kUnneeded[k];
            entry.nexthop_addr_ipv4 = {
                nhBytes[0], nhBytes[1], nhBytes[2], nhBytes[3]};
            entry.nexthop_id_ipv4 = groupNhid;

            for (const auto& pfx : prefixes->prefixes())
            {
                const std::string& pfxStr = pfx.prefix();
                const auto slash = pfxStr.rfind('/');
                if (slash == std::string::npos)
                    continue;
                struct in_addr pfxAddr
                {};
                if (::inet_pton(AF_INET,
                                pfxStr.substr(0, slash).c_str(),
                                &pfxAddr) != 1)
                    continue;
                const auto* pb =
                    reinterpret_cast<const std::uint8_t*>(&pfxAddr.s_addr);
                const auto maskLen = static_cast<std::uint8_t>(
                    std::stoul(pfxStr.substr(slash + 1)));
                entry.prefixes.push_back(cmdproto::PrefixIpv4{
                    {pb[0], pb[1], pb[2], pb[3]}, maskLen});

                if (redisRib.connected())
                    redisRib.set(pfxStr, std::to_string(groupNhid));
            }

            rtsrv::log::info(std::format(
                "[add-del-list]   iface='unneeded' nexthop={}"
                " ospf_nhid={} sra_group_nhid={} prefixes={}",
                loopback_ipv4,
                kr->nhid,
                groupNhid,
                entry.prefixes.size()));

            if (entry.prefixes.empty())
            {
                rtsrv::log::info(
                    "[add-del-list]   no prefixes — skipping ROUTE_ADD");
                continue;
            }

            singleReq.interfaces.push_back(std::move(entry));
        }
    }
#if 1
    if (!singleReq.interfaces.empty())
    {
        // ── ROUTE_ADD ────────────────────────────────────────────────────
        rtsrv::log::info(std::format(
            "[add-del-list] [ADD] submitting {} interface(s) to ud_server...",
            singleReq.interfaces.size()));
        auto savedInterfaces = singleReq.interfaces;
        vrfClient.submitAdd(std::move(singleReq));
    }
    else
    {
        rtsrv::log::info("[add-del-list] no interfaces to submit");
    }
#endif

    rtsrv::log::info(std::format("[add-del-list] END for {}", loopback_ipv4));
    return 0;
}
