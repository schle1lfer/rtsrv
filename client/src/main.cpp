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
 *         --sot      <path>   Path to route_sot_v2.json (Source-of-Truth)
 *     -t, --timeout  <sec>    RPC deadline (overrides config file)
 *         --tls               Use TLS channel (overrides config file)
 *         --ca-cert  <path>   CA certificate for TLS verification
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
 * @endcode
 *
 * @version 1.0
 */

#include "build_info.hpp"
#include "client/grpc_proc.hpp"
#include "client/route_client.hpp"
#include "client/sot_config.hpp"
#include "client/switch_config.hpp"
#include "common/config.hpp"
#include "server/netlink.h"

#include <arpa/inet.h>
#include <boost/program_options.hpp>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <format>
#include <iostream>
#include <linux/rtnetlink.h>
#include <mutex>
#include <print>
#include <queue>
#include <signal.h>
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
 * @brief Context forwarded via the @c user_data pointer to the netlink callback.
 *
 * Carries the live gRPC client and the local map that tracks which srmd route
 * ID corresponds to each /32 destination we have pushed.
 */
struct WatchCtx
{
    sra::RouteClient*                            client;
    std::unordered_map<std::string, std::string> routeIds; ///< dest/32 → srmd ID
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

    /* Destination string "x.x.x.x/32" */
    char dst_buf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &route->dst, dst_buf, sizeof(dst_buf));
    const std::string dest = std::string(dst_buf) + "/32";

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
        /* ---- ADDED ---------------------------------------------------------- */
        if (event == NETLINK_ROUTE_ADDED)
        {
            auto result = ctx->client->addRoute(
                dest, gw, iface, route->metric,
                srmd::v1::ADDRESS_FAMILY_IPV4, proto);

            if (result)
            {
                ctx->routeIds[dest] = result->id();
                std::println("{} [ADDED]   {} via {} dev {} metric {} → id={}",
                            ts, dest, gw, iface, route->metric,
                            result->id().substr(0, 8) + "…");
            }
            else
            {
                std::println("{} [ADDED]   {} → gRPC FAILED: {}",
                            ts, dest, result.error());
            }
        }
        /* ---- CHANGED -------------------------------------------------------- */
        else if (event == NETLINK_ROUTE_CHANGED)
        {
            /* Remove the stale entry if we hold an ID for this destination. */
            auto it = ctx->routeIds.find(dest);
            if (it != ctx->routeIds.end())
            {
                auto rmResult = ctx->client->removeRoute(it->second);
                if (!rmResult)
                {
                    std::println("{} [CHANGED] {} stale-remove failed: {}",
                                ts, dest, rmResult.error());
                }
                ctx->routeIds.erase(it);
            }

            auto result = ctx->client->addRoute(
                dest, gw, iface, route->metric,
                srmd::v1::ADDRESS_FAMILY_IPV4, proto);

            if (result)
            {
                ctx->routeIds[dest] = result->id();
                std::println("{} [CHANGED] {} via {} dev {} metric {} → id={}",
                            ts, dest, gw, iface, route->metric,
                            result->id().substr(0, 8) + "…");
            }
            else
            {
                std::println("{} [CHANGED] {} → gRPC FAILED: {}",
                            ts, dest, result.error());
            }
        }
        /* ---- REMOVED -------------------------------------------------------- */
        else
        {
            auto it = ctx->routeIds.find(dest);
            if (it == ctx->routeIds.end())
            {
                std::println("{} [REMOVED] {} (not tracked – no gRPC call)",
                            ts, dest);
                return;
            }

            const std::string id = it->second;
            ctx->routeIds.erase(it);

            auto result = ctx->client->removeRoute(id);
            if (result)
            {
                std::println("{} [REMOVED] {} id={}", ts, dest,
                            id.substr(0, 8) + "…");
            }
            else
            {
                std::println("{} [REMOVED] {} → gRPC FAILED: {}",
                            ts, dest, result.error());
            }
        }

    }

    std::cout.flush();
}

/* File-scope fd used by the signal handler to unblock netlink_run(). */
static volatile int g_watch_fd = -1;

/** @brief SIGINT/SIGTERM handler for the watch command. */
static void watchSigHandler(int /*signo*/)
{
    if (g_watch_fd >= 0)
    {
        netlink_close(g_watch_fd);
        g_watch_fd = -1;
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
 * @brief Runs the netlink /32 route watcher, forwarding events to srmd.
 *
 * Opens a @c NETLINK_ROUTE socket and blocks in @c netlink_run() until
 * SIGINT / SIGTERM is received.  Every qualifying kernel route event is
 * forwarded to the connected srmd instance via AddRoute / RemoveRoute.
 *
 * @param client  Constructed and connected @c RouteClient.
 * @return @c EXIT_SUCCESS on clean stop; @c EXIT_FAILURE if the socket
 *         could not be opened.
 */
static int cmdNetlinkWatch(sra::RouteClient& client)
{
    /* Install signal handlers. */
    struct sigaction sa{};
    sa.sa_handler = watchSigHandler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    int fd = netlink_init();
    if (fd < 0)
    {
        std::println(std::cerr, "Error: netlink_init failed: {}",
                     std::strerror(errno));
        return EXIT_FAILURE;
    }
    g_watch_fd = fd;

    std::println("Watching for IPv4 /32 route events → forwarding to {} …",
                 "srmd");
    std::println("  (Ctrl-C to stop)\n");
    std::cout.flush();

    WatchCtx ctx{&client, {}};
    netlink_run(fd, nlWatchCb, &ctx);

    netlink_close(g_watch_fd); /* no-op if already closed by signal handler */

    std::println("\nStopped. {} route(s) tracked at exit.", ctx.routeIds.size());
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
        ("command",  po::value<std::string>(),
            "Command: test | sync | echo | add | remove | get | list | watch"
            " | set-loopback | get-loopback | get-loopbacks | grpc-proc-demo")
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
        std::println("  set-loopback <address>  Store a loopback address on the server");
        std::println("  get-loopback            Retrieve the stored loopback address");
        std::println("  get-loopbacks <loopback>  Query SOT interface list for a loopback (IPv4 or IPv6)");
        std::println("  grpc-proc-demo          Run async GrpcProc demo with periodic GetLoopback requests");
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
    // Single-server commands
    // -----------------------------------------------------------------------
    std::println("sra  build #{}  server={}  tls={}",
                 rtsrv::build::kBuildNumber,
                 server,
                 useTls);

    sra::RouteClient client(server, useTls, caCert, timeout);

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
        return cmdNetlinkWatch(client);
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

    std::println(std::cerr, "Unknown command: '{}'", command);
    std::println(std::cerr, "Run 'sra --help' for usage.");
    return EXIT_FAILURE;
}
