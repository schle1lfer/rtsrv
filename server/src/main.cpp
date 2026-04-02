/**
 * @file server/src/main.cpp
 * @brief Entry point for srmd – the Switch Route Manager Daemon.
 *
 * Start-up sequence:
 *  1. Parse CLI flags (--config, --foreground, --version, --help).
 *  2. Load and validate the JSON configuration file.
 *  3. Initialise Boost.Log (file in daemon mode; console in foreground mode).
 *  4. Daemonise via double-fork + dup2 (skipped when --foreground is set).
 *  5. Construct the RouteManager and SwitchRouteManagerImpl.
 *  6. Start the gRPC server on the configured listen address.
 *  7. Enter the main event loop (SIGTERM/SIGINT → stop; SIGHUP → reload).
 *  8. Graceful shutdown: stop the gRPC server and exit.
 *
 * @version 1.0
 */

#include "build_info.hpp"
#include "common/config.hpp"
#include "server/daemon.hpp"
#include "server/route_manager.hpp"
#include "server/service_impl.hpp"
#include "server/sot_config.hpp"

#include <grpcpp/security/server_credentials.h>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <unistd.h>

#include <boost/log/core.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/support/date_time.hpp>
#include <boost/log/trivial.hpp>
#include <boost/log/utility/setup/common_attributes.hpp>
#include <boost/log/utility/setup/console.hpp>
#include <boost/log/utility/setup/file.hpp>
#include <boost/program_options.hpp>
#include <chrono>
#include <cstdlib>
#include <format>
#include <fstream>
#include <iostream>
#include <memory>
#include <print>
#include <string>
#include <thread>

namespace po = boost::program_options;
namespace logging = boost::log;
namespace keywords = boost::log::keywords;
namespace expr = boost::log::expressions;

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

static constexpr std::string_view kDefaultConfig = "/etc/srmd/srmd.json";

// ---------------------------------------------------------------------------
// Logging helpers
// ---------------------------------------------------------------------------

/**
 * @brief Maps a log level name string to Boost.Log trivial severity.
 *
 * @param level  One of: trace, debug, info, warning, error, fatal.
 * @return Severity level, defaulting to info on unknown input.
 */
static logging::trivial::severity_level
parseLogLevel(const std::string& level) noexcept
{
    if (level == "trace")
    {
        return logging::trivial::trace;
    }
    if (level == "debug")
    {
        return logging::trivial::debug;
    }
    if (level == "warning" || level == "warn")
    {
        return logging::trivial::warning;
    }
    if (level == "error")
    {
        return logging::trivial::error;
    }
    if (level == "fatal")
    {
        return logging::trivial::fatal;
    }
    return logging::trivial::info;
}

/** @brief Initialises console logging for foreground mode. */
static void initConsoleLogs(logging::trivial::severity_level level)
{
    logging::add_common_attributes();
    logging::add_console_log(
        std::clog,
        keywords::format =
            (expr::stream << expr::format_date_time<boost::posix_time::ptime>(
                                 "TimeStamp", "%Y-%m-%d %H:%M:%S.%f")
                          << " [srmd] [" << logging::trivial::severity << "] "
                          << expr::smessage));
    logging::core::get()->set_filter(logging::trivial::severity >= level);
}

/** @brief Initialises rotating file logging for daemon mode. */
static void initFileLogs(const std::string& logPath,
                         logging::trivial::severity_level level)
{
    logging::add_common_attributes();
    logging::add_file_log(
        keywords::file_name = logPath,
        keywords::rotation_size = 10 * 1024 * 1024,
        keywords::max_files = 5,
        keywords::auto_flush = true,
        keywords::open_mode = std::ios::out | std::ios::app,
        keywords::format =
            (expr::stream << expr::format_date_time<boost::posix_time::ptime>(
                                 "TimeStamp", "%Y-%m-%d %H:%M:%S.%f")
                          << " [srmd] [" << logging::trivial::severity << "] "
                          << expr::smessage));
    logging::core::get()->set_filter(logging::trivial::severity >= level);
}

// ---------------------------------------------------------------------------
// Hostname helper
// ---------------------------------------------------------------------------

static std::string getHostname()
{
    char buf[256]{};
    if (::gethostname(buf, sizeof(buf) - 1) == 0)
    {
        return std::string(buf);
    }
    return "unknown";
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
static void printSotSummary(const srmd::SotConfig& cfg)
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

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

/**
 * @brief srmd entry point.
 *
 * @param argc Argument count.
 * @param argv Argument vector.
 * @return EXIT_SUCCESS on clean shutdown; EXIT_FAILURE on fatal error.
 */
int main(int argc, char* argv[])
{
    // -----------------------------------------------------------------------
    // Command-line parsing
    // -----------------------------------------------------------------------
    po::options_description desc("srmd – Switch Route Manager Daemon");
    // clang-format off
    desc.add_options()
        ("help,h",   "Print help and exit")
        ("version,v","Print version and exit")
        ("config,c",
            po::value<std::string>()->default_value(
                std::string(kDefaultConfig)),
            "Path to the JSON configuration file")
        ("sot,s",
            po::value<std::string>()->default_value(
                std::string(kDefaultConfig)),
            "Path to the JSON route_sot_v2 file (Source-of-Truth)")
        ("foreground,f", "Run in the foreground (skip daemonisation)");
    // clang-format on

    po::variables_map vm;
    try
    {
        po::store(po::parse_command_line(argc, argv, desc), vm);
        po::notify(vm);
    }
    catch (const po::error& ex)
    {
        std::cerr << "Error: " << ex.what() << "\n\n" << desc << '\n';
        return EXIT_FAILURE;
    }

    if (vm.count("help"))
    {
        std::cout << desc << '\n';
        return EXIT_SUCCESS;
    }
    if (vm.count("version"))
    {
        std::println("srmd (Switch Route Manager Daemon) {}",
                     rtsrv::build::kProjectVersion);
        std::println("  Build : #{} ({})",
                     rtsrv::build::kBuildNumber,
                     rtsrv::build::kBuildTimestamp);
        std::println("  Binary: {}", rtsrv::build::kVersionedBinaryName);
        return EXIT_SUCCESS;
    }

    const std::string configPath = vm["config"].as<std::string>();
    const std::string sotPath = vm["sot"].as<std::string>();
    const bool foreground = vm.count("foreground") > 0;

    // -----------------------------------------------------------------------
    // "sync" command – parse SOT *before* opening any gRPC connections
    // -----------------------------------------------------------------------
    if (sotPath.empty())
    {
        std::println(std::cerr, "Error: server requires --sot <path>");
        return EXIT_FAILURE;
    }

    // Parse the SOT file first – no network connections yet
    std::println("srmd  build #{}  Parsing SOT: {}",
                    rtsrv::build::kBuildNumber,
                    sotPath);

    auto sotResult = srmd::loadSotConfig(sotPath);
    if (!sotResult)
    {
        std::println(
            std::cerr, "Error loading SOT config: {}", sotResult.error());
        return EXIT_FAILURE;
    }

    // Print what was loaded so the operator can verify before pushing
    printSotSummary(*sotResult);
#if 0
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
#endif

    // -----------------------------------------------------------------------
    // Load configuration
    // -----------------------------------------------------------------------
    auto cfgResult = common::loadServerConfig(configPath);
    if (!cfgResult)
    {
        std::cerr << "Configuration error: " << cfgResult.error() << '\n';
        return EXIT_FAILURE;
    }
    const auto& cfg = *cfgResult;

    // -----------------------------------------------------------------------
    // Daemonise or stay in foreground
    // -----------------------------------------------------------------------
    srmd::Daemon daemon(cfg.daemon.pid_file, cfg.daemon.working_dir);

    if (!foreground)
    {
        try
        {
            daemon.daemonise();
        }
        catch (const std::exception& ex)
        {
            std::cerr << "daemonise() failed: " << ex.what() << '\n';
            return EXIT_FAILURE;
        }

        try
        {
            initFileLogs(cfg.daemon.log_file,
                         parseLogLevel(cfg.daemon.log_level));
        }
        catch (...)
        {
            return EXIT_FAILURE;
        }
    }
    else
    {
        initConsoleLogs(parseLogLevel(cfg.daemon.log_level));
    }

    BOOST_LOG_TRIVIAL(info)
        << std::format("srmd starting  version={}  build=#{}  built={}  pid={}",
                       rtsrv::build::kProjectVersion,
                       rtsrv::build::kBuildNumber,
                       rtsrv::build::kBuildTimestamp,
                       ::getpid());

    BOOST_LOG_TRIVIAL(info) << std::format(
        "Listening on {}  foreground={}", cfg.server.target(), foreground);

    // -----------------------------------------------------------------------
    // Build server identity string
    // -----------------------------------------------------------------------
    const std::string serverId =
        std::format("{}@{}", rtsrv::build::kVersionedBinaryName, getHostname());

    // -----------------------------------------------------------------------
    // Construct core objects
    // -----------------------------------------------------------------------
    srmd::RouteManager routeManager;

    srmd::SwitchRouteManagerImpl service(
        routeManager, serverId, std::string(rtsrv::build::kFullVersion));

    // -----------------------------------------------------------------------
    // Build and start the gRPC server
    // -----------------------------------------------------------------------
    grpc::ServerBuilder builder;

    if (cfg.server.tls_enabled)
    {
        // Load certificate and key from disk
        auto loadFile = [](const std::string& path) -> std::string {
            std::ifstream ifs(path);
            return std::string(std::istreambuf_iterator<char>(ifs), {});
        };
        grpc::SslServerCredentialsOptions sslOpts;
        grpc::SslServerCredentialsOptions::PemKeyCertPair kp;
        kp.cert_chain = loadFile(cfg.server.tls_cert);
        kp.private_key = loadFile(cfg.server.tls_key);
        sslOpts.pem_key_cert_pairs.push_back(std::move(kp));
        builder.AddListeningPort(cfg.server.target(),
                                 grpc::SslServerCredentials(sslOpts));
    }
    else
    {
        builder.AddListeningPort(cfg.server.target(),
                                 grpc::InsecureServerCredentials());
    }

    builder.RegisterService(&service);
    builder.SetMaxReceiveMessageSize(4 * 1024 * 1024); // 4 MiB
    builder.SetMaxSendMessageSize(4 * 1024 * 1024);

    std::unique_ptr<grpc::Server> grpcServer = builder.BuildAndStart();
    if (!grpcServer)
    {
        BOOST_LOG_TRIVIAL(fatal) << std::format(
            "Failed to start gRPC server on {}", cfg.server.target());
        return EXIT_FAILURE;
    }

    BOOST_LOG_TRIVIAL(info)
        << std::format("gRPC server listening on {} (TLS={})",
                       cfg.server.target(),
                       cfg.server.tls_enabled);

    // -----------------------------------------------------------------------
    // SIGHUP handler: reload config
    // -----------------------------------------------------------------------
    daemon.setSighupHandler([&] {
        BOOST_LOG_TRIVIAL(info) << "SIGHUP – reloading configuration";
        auto newCfg = common::loadServerConfig(configPath);
        if (!newCfg)
        {
            BOOST_LOG_TRIVIAL(error)
                << std::format("Reload failed: {}", newCfg.error());
        }
        else
        {
            BOOST_LOG_TRIVIAL(info) << "Configuration reloaded";
        }
    });

    // -----------------------------------------------------------------------
    // Main event loop
    // -----------------------------------------------------------------------
    using namespace std::chrono_literals;
    BOOST_LOG_TRIVIAL(info) << "Entering main event loop";

    while (!srmd::Daemon::shouldStop())
    {
        if (daemon.shouldReload())
        {
            BOOST_LOG_TRIVIAL(info) << "SIGHUP received – reload not implemented";
        }
        std::this_thread::sleep_for(500ms);
    }

    // -----------------------------------------------------------------------
    // Graceful shutdown
    // -----------------------------------------------------------------------
    BOOST_LOG_TRIVIAL(info)
        << "Shutdown signal received – stopping gRPC server";
    grpcServer->Shutdown(std::chrono::system_clock::now() + 10s);
    grpcServer->Wait();

    BOOST_LOG_TRIVIAL(info) << std::format("srmd build #{} exiting cleanly",
                                           rtsrv::build::kBuildNumber);
    return EXIT_SUCCESS;
}
