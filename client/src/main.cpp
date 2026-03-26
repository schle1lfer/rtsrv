/**
 * @file client/src/main.cpp
 * @brief Entry point for sra – the Switch Route Application.
 *
 * sra is a command-line client for the srmd Switch Route Manager daemon.
 * It connects to a running srmd instance via gRPC and exposes route
 * management commands.
 *
 * Usage:
 * @code
 *   sra [options] <command> [command-args]
 *
 *   Global options:
 *     -s, --server <addr>   srmd address  [default: localhost:50051]
 *     -t, --timeout <sec>   RPC deadline  [default: 10]
 *         --tls             Use TLS channel
 *         --ca-cert <path>  CA certificate for TLS verification
 *     -v, --version         Print version and exit
 *     -h, --help            Print this help and exit
 *
 *   Commands:
 *     test                  Run a full Echo + CRUD round-trip test sequence
 *     echo  <message>       Send an Echo RPC and print the response
 *     add   <dest> [gw] [iface] [metric]   Add a route
 *     remove <id>           Remove a route by ID
 *     get    <id>           Retrieve a route by ID
 *     list  [--active]      List all routes (--active filters to active only)
 * @endcode
 *
 * @version 1.0
 */

#include "build_info.hpp"
#include "client/route_client.hpp"

#include <boost/program_options.hpp>
#include <chrono>
#include <cstdlib>
#include <format>
#include <iostream>
#include <print>
#include <string>
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
 * @brief Runs the full test sequence: Echo → AddRoute → GetRoute →
 *        ListRoutes → RemoveRoute → ListRoutes.
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
        ("server,s",
            po::value<std::string>()->default_value("localhost:50051"),
            "srmd server address  [host:port]")
        ("timeout,t",
            po::value<int>()->default_value(10),
            "Per-RPC timeout in seconds")
        ("tls",      "Use TLS for the gRPC channel")
        ("ca-cert",
            po::value<std::string>()->default_value(std::string{}),
            "Path to PEM CA certificate for TLS")
        ("command",  po::value<std::string>(),
            "Command: test | echo | add | remove | get | list")
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
        std::println("Commands:");
        std::println(
            "  test                    Full Echo+CRUD round-trip test");
        std::println("  echo   <message>        Send Echo RPC");
        std::println("  add    <dest> [gw] [iface] [metric]  Add route");
        std::println("  remove <id>             Remove route by ID");
        std::println("  get    <id>             Get route by ID");
        std::println("  list   [--active]       List routes");
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

    const std::string server = vm["server"].as<std::string>();
    const int timeout = vm["timeout"].as<int>();
    const bool useTls = vm.count("tls") > 0;
    const std::string caCert = vm["ca-cert"].as<std::string>();
    const std::string command = vm["command"].as<std::string>();

    const std::vector<std::string> args =
        vm.count("args") ? vm["args"].as<std::vector<std::string>>()
                         : std::vector<std::string>{};

    std::println("sra  build #{}  server={}  tls={}",
                 rtsrv::build::kBuildNumber,
                 server,
                 useTls);

    // -----------------------------------------------------------------------
    // Connect
    // -----------------------------------------------------------------------
    sra::RouteClient client(server, useTls, caCert, timeout);

    // -----------------------------------------------------------------------
    // Dispatch command
    // -----------------------------------------------------------------------
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

    std::println(std::cerr, "Unknown command: '{}'", command);
    std::println(std::cerr, "Run 'sra --help' for usage.");
    return EXIT_FAILURE;
}
