/**
 * @file server/src/main.cpp
 * @brief Entry point for srmd – the Switch Route Manager Daemon.
 *
 * Start-up sequence:
 *  1. Initialise the RFC 5424 logging API (console sink for early messages).
 *  2. Parse CLI flags (--config, --sot, --foreground, --version, --help).
 *  3. Load and parse the Source-of-Truth (SOT) JSON file.
 *  4. Load and validate the JSON configuration file.
 *  5. Re-initialise logging with the configured file sink (and optional console).
 *  6. Daemonise via double-fork + dup2 (skipped when --foreground is set).
 *  7. Construct the RouteManager and SwitchRouteManagerImpl.
 *  8. Start the gRPC server on the configured listen address.
 *  9. Enter the main event loop (SIGTERM/SIGINT → stop; SIGHUP → reload).
 * 10. Graceful shutdown: stop the gRPC server, flush logging, and exit.
 *
 * @version 1.2
 */

#include "build_info.hpp"
#include "common/config.hpp"
#include "common/log.hpp"
#include "server/daemon.hpp"
#include "server/route_manager.hpp"
#include "server/service_impl.hpp"
#include "server/sot_config.hpp"

#include <grpcpp/security/server_credentials.h>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <unistd.h>

#include <boost/program_options.hpp>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <format>
#include <fstream>
#include <iostream>
#include <memory>
#include <print>
#include <string>
#include <thread>

namespace po = boost::program_options;

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

static constexpr std::string_view kDefaultConfig = "/etc/srmd/srmd.json";

// ---------------------------------------------------------------------------
// Logging helpers
// ---------------------------------------------------------------------------

/**
 * @brief Maps a log-level name to the RFC 5424 Severity enum.
 */
static rtsrv::log::Severity mapLogLevel(const std::string& level) noexcept
{
    if (level == "trace" || level == "debug")
        return rtsrv::log::Severity::Debug;
    if (level == "warning" || level == "warn")
        return rtsrv::log::Severity::Warning;
    if (level == "error")
        return rtsrv::log::Severity::Error;
    if (level == "fatal")
        return rtsrv::log::Severity::Critical;
    return rtsrv::log::Severity::Info;
}

/**
 * @brief (Re-)initialises the RFC 5424 logging API with a file sink.
 *
 * Always writes to @p logBasePath.<epoch_seconds> (RFC 5424 wire format)
 * with a symlink at @p logBasePath.  If @p extraStream is "stdout" or
 * "stderr" a console sink is also enabled.
 */
static void initLogs(const std::string& logBasePath,
                     rtsrv::log::Severity sev,
                     const std::string& extraStream)
{
    const auto ts = static_cast<long long>(std::time(nullptr));
    const std::string timestampedPath =
        logBasePath + "." + std::to_string(ts);

    ::unlink(logBasePath.c_str());
    if (::symlink(timestampedPath.c_str(), logBasePath.c_str()) != 0)
    {
        std::fprintf(stderr,
                     "[srmd] cannot create symlink '%s' -> '%s': %s\n",
                     logBasePath.c_str(),
                     timestampedPath.c_str(),
                     std::strerror(errno));
    }

    rtsrv::log::Config cfg;
    cfg.app_name     = "srmd";
    cfg.facility     = rtsrv::log::Facility::Daemon;
    cfg.min_severity = sev;

    cfg.file.enabled = true;
    cfg.file.path    = timestampedPath;
    // human_readable defaults to false → RFC 5424 wire format in the file.

    if (extraStream == "stderr" || extraStream == "stdout")
        cfg.console.enabled = true;
    // human_readable defaults to false → RFC 5424 wire format on the console.

    if (auto r = rtsrv::log::init(cfg); !r)
        std::fprintf(stderr, "[srmd] log init failed: %s\n", r.error().c_str());
}

// ---------------------------------------------------------------------------
// Hostname helper
// ---------------------------------------------------------------------------

static std::string getHostname()
{
    char buf[256]{};
    if (::gethostname(buf, sizeof(buf) - 1) == 0)
        return std::string(buf);
    return "unknown";
}

// ---------------------------------------------------------------------------
// SOT summary (all output through the logging API)
// ---------------------------------------------------------------------------

static void logSotSummary(const srmd::SotConfig& cfg)
{
    rtsrv::log::info(std::format("SOT parsed: {} node(s), {} prefix(es) total",
                                 cfg.nodes.size(),
                                 cfg.totalPrefixCount()));

    for (const auto& node : cfg.nodes)
    {
        std::size_t ifaceCount   = 0;
        std::size_t prefixCount  = 0;
        for (const auto& vrf : node.vrfs)
        {
            ifaceCount += vrf.ipv4.interfaces.size();
            for (const auto& iface : vrf.ipv4.interfaces)
                prefixCount += iface.prefixes.size();
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

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[])
{
    // -----------------------------------------------------------------------
    // Phase 1: Early logging (RFC 5424 console sink active immediately).
    // This runs before CLI parsing so that all subsequent messages, including
    // errors from option parsing, already use the logging API.
    // -----------------------------------------------------------------------
    {
        rtsrv::log::Config early;
        early.app_name        = "srmd";
        early.facility        = rtsrv::log::Facility::Daemon;
        early.min_severity    = rtsrv::log::Severity::Debug;
        early.console.enabled = true;
        // human_readable defaults to false → RFC 5424 format from the start.
        if (auto r = rtsrv::log::init(early); !r)
            std::fprintf(stderr, "[srmd] early log init failed: %s\n",
                         r.error().c_str());
    }

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
            "Path to the Source-of-Truth JSON file (route_sot)")
        ("foreground,f", "Run in the foreground (skip daemonisation)")
        ("logstream",
            po::value<std::string>()->default_value(std::string{}),
            "Duplicate log output to \"stdout\" or \"stderr\" in addition"
            " to the default log file /var/log/srmd.log.<timestamp>");
    // clang-format on

    po::variables_map vm;
    try
    {
        po::store(po::parse_command_line(argc, argv, desc), vm);
        po::notify(vm);
    }
    catch (const po::error& ex)
    {
        rtsrv::log::err(std::format("Command-line error: {}", ex.what()));
        std::cerr << desc << '\n';
        return EXIT_FAILURE;
    }

    if (vm.count("help"))
    {
        std::cout << desc << '\n';
        return EXIT_SUCCESS;
    }
    if (vm.count("version"))
    {
        // Version output goes to stdout as plain text (user-facing).
        std::println("srmd (Switch Route Manager Daemon) {}",
                     rtsrv::build::kProjectVersion);
        std::println("  Build : #{} ({})",
                     rtsrv::build::kBuildNumber,
                     rtsrv::build::kBuildTimestamp);
        std::println("  Binary: {}", rtsrv::build::kVersionedBinaryName);
        return EXIT_SUCCESS;
    }

    const std::string configPath = vm["config"].as<std::string>();
    const std::string sotPath    = vm["sot"].as<std::string>();
    const bool foreground        = vm.count("foreground") > 0;
    const std::string logstream  = vm["logstream"].as<std::string>();

    // -----------------------------------------------------------------------
    // SOT parsing
    // -----------------------------------------------------------------------
    if (sotPath.empty())
    {
        rtsrv::log::err("server requires --sot <path>");
        return EXIT_FAILURE;
    }

    rtsrv::log::info(std::format("srmd  build #{}  parsing SOT: {}",
                                 rtsrv::build::kBuildNumber,
                                 sotPath));

    auto sotResult = srmd::loadSotConfig(sotPath);
    if (!sotResult)
    {
        rtsrv::log::err(
            std::format("Error loading SOT config: {}", sotResult.error()));
        return EXIT_FAILURE;
    }

    logSotSummary(*sotResult);

#if 0
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
        return cmdSyncMulti(*sotResult, *cfgResult, useTls, caCert, timeout);
    }

    if (nodeIp.empty())
    {
        rtsrv::log::err("single-server sync requires --node-ip <management-ip>");
        return EXIT_FAILURE;
    }

    sra::RouteClient client(server, useTls, caCert, timeout);
    return syncOneServer(client, nodeIp, server, *sotResult);
#endif

    // -----------------------------------------------------------------------
    // Load configuration
    // -----------------------------------------------------------------------
    auto cfgResult = common::loadServerConfig(configPath);
    if (!cfgResult)
    {
        rtsrv::log::err(
            std::format("Configuration error: {}", cfgResult.error()));
        return EXIT_FAILURE;
    }
    const auto& cfg = *cfgResult;

    // -----------------------------------------------------------------------
    // Phase 2: Re-initialise logging with the configured file sink.
    // RFC 5424 format is used for both the file and the optional console.
    // -----------------------------------------------------------------------
    initLogs(cfg.daemon.log_file,
             mapLogLevel(cfg.daemon.log_level),
             foreground ? logstream : std::string{});

    rtsrv::log::info(
        std::format("srmd starting  version={}  build=#{}  built={}  pid={}",
                    rtsrv::build::kProjectVersion,
                    rtsrv::build::kBuildNumber,
                    rtsrv::build::kBuildTimestamp,
                    ::getpid()));

    rtsrv::log::info(std::format(
        "listening on {}  foreground={}", cfg.server.target(), foreground));

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
            rtsrv::log::crit(
                std::format("daemonise() failed: {}", ex.what()));
            return EXIT_FAILURE;
        }
    }

    // -----------------------------------------------------------------------
    // Construct core objects
    // -----------------------------------------------------------------------
    const std::string serverId =
        std::format("{}@{}", rtsrv::build::kVersionedBinaryName, getHostname());

    srmd::RouteManager routeManager;

    srmd::SwitchRouteManagerImpl service(
        routeManager,
        serverId,
        std::string(rtsrv::build::kFullVersion),
        std::move(*sotResult));

    // -----------------------------------------------------------------------
    // Build and start the gRPC server
    // -----------------------------------------------------------------------
    grpc::ServerBuilder builder;

    if (cfg.server.tls_enabled)
    {
        auto loadFile = [](const std::string& path) -> std::string {
            std::ifstream ifs(path);
            return std::string(std::istreambuf_iterator<char>(ifs), {});
        };
        grpc::SslServerCredentialsOptions sslOpts;
        grpc::SslServerCredentialsOptions::PemKeyCertPair kp;
        kp.cert_chain  = loadFile(cfg.server.tls_cert);
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
    builder.SetMaxReceiveMessageSize(4 * 1024 * 1024);
    builder.SetMaxSendMessageSize(4 * 1024 * 1024);

    std::unique_ptr<grpc::Server> grpcServer = builder.BuildAndStart();
    if (!grpcServer)
    {
        rtsrv::log::crit(std::format(
            "failed to start gRPC server on {}", cfg.server.target()));
        return EXIT_FAILURE;
    }

    rtsrv::log::info(std::format("gRPC server listening on {} (TLS={})",
                                 cfg.server.target(),
                                 cfg.server.tls_enabled));

    // -----------------------------------------------------------------------
    // SIGHUP handler: reload config
    // -----------------------------------------------------------------------
    daemon.setSighupHandler([&] {
        rtsrv::log::info("SIGHUP – reloading configuration");
        auto newCfg = common::loadServerConfig(configPath);
        if (!newCfg)
            rtsrv::log::err(std::format("reload failed: {}", newCfg.error()));
        else
            rtsrv::log::info("configuration reloaded");
    });

    // -----------------------------------------------------------------------
    // Main event loop — runs until SIGTERM/SIGINT (CTRL+C)
    // -----------------------------------------------------------------------
    using namespace std::chrono_literals;
    rtsrv::log::info("entering main event loop");

    while (!srmd::Daemon::shouldStop())
    {
        if (daemon.shouldReload())
            rtsrv::log::info("SIGHUP received – reload not implemented");
        std::this_thread::sleep_for(500ms);
    }

    // -----------------------------------------------------------------------
    // Graceful shutdown — logging thread is joined before process exits
    // -----------------------------------------------------------------------
    rtsrv::log::info("shutdown signal received – stopping gRPC server");
    grpcServer->Shutdown(std::chrono::system_clock::now() + 10s);
    grpcServer->Wait();

    rtsrv::log::info(std::format("srmd build #{} exiting cleanly",
                                 rtsrv::build::kBuildNumber));
    rtsrv::log::shutdown();

    return EXIT_SUCCESS;
}
