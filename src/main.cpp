/**
 * @file main.cpp
 * @brief Entry point for the rtsrv real-time service daemon.
 *
 * Bootstraps the daemon in the following sequence:
 *  1. Parse command-line options (config file path, --foreground flag).
 *  2. Load and validate the JSON configuration file.
 *  3. Initialise Boost.Log with the configured log level and log file.
 *  4. Unless --foreground is given, perform the classic double-fork
 *     daemonisation (including dup2-based stdio redirect to /dev/null).
 *  5. Construct a GrpcClientManager and start one GrpcClient per configured
 *     remote server.
 *  6. Enter the main event loop, polling for SIGTERM / SIGINT (stop) and
 *     SIGHUP (reload).
 *  7. On stop: disconnect all gRPC clients cleanly and exit.
 *
 * Command-line synopsis:
 * @code
 *   rtsrv [options]
 *
 *   Options:
 *     -c, --config <path>   Path to JSON config  [default:
 * /etc/rtsrv/rtsrv.json] -f, --foreground      Run in foreground (no
 * daemonisation) -v, --version         Print version and exit -h, --help Print
 * this help and exit
 * @endcode
 *
 * @version 1.0
 */

#include "config.hpp"
#include "daemon.hpp"
#include "grpc_client.hpp"

#include <unistd.h>

#include <boost/log/core.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/sinks/text_file_backend.hpp>
#include <boost/log/sinks/text_ostream_backend.hpp>
#include <boost/log/support/date_time.hpp>
#include <boost/log/trivial.hpp>
#include <boost/log/utility/setup/common_attributes.hpp>
#include <boost/log/utility/setup/console.hpp>
#include <boost/log/utility/setup/file.hpp>
#include <boost/program_options.hpp>
#include <chrono>
#include <cstdlib>
#include <format>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace po = boost::program_options;
namespace logging = boost::log;
namespace keywords = boost::log::keywords;
namespace expr = boost::log::expressions;

// ---------------------------------------------------------------------------
// Version string
// ---------------------------------------------------------------------------

static constexpr std::string_view kVersion = "1.0.0";
static constexpr std::string_view kDefaultConfig = "/etc/rtsrv/rtsrv.json";

// ---------------------------------------------------------------------------
// Logging initialisation
// ---------------------------------------------------------------------------

/**
 * @brief Maps a string log level name to the Boost.Log trivial severity.
 *
 * @param level  One of: trace, debug, info, warning, error, fatal (case
 *               insensitive).
 * @return Corresponding logging::trivial::severity_level.
 * @throws std::invalid_argument if the level string is unrecognised.
 */
static logging::trivial::severity_level parseLogLevel(const std::string& level)
{
    if (level == "trace")
    {
        return logging::trivial::trace;
    }
    if (level == "debug")
    {
        return logging::trivial::debug;
    }
    if (level == "info")
    {
        return logging::trivial::info;
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
    throw std::invalid_argument(std::format("Unknown log level: '{}'", level));
}

/**
 * @brief Initialises Boost.Log for foreground (console) operation.
 *
 * @param level  Minimum log severity.
 */
static void initConsoleLogs(logging::trivial::severity_level level)
{
    logging::add_common_attributes();
    logging::add_console_log(
        std::clog,
        keywords::format =
            (expr::stream << expr::format_date_time<boost::posix_time::ptime>(
                                 "TimeStamp", "%Y-%m-%d %H:%M:%S.%f")
                          << " [" << logging::trivial::severity << "] "
                          << expr::smessage));
    logging::core::get()->set_filter(logging::trivial::severity >= level);
}

/**
 * @brief Initialises Boost.Log to write to a rotating log file.
 *
 * @param logPath  Path of the log file (directory must exist).
 * @param level    Minimum log severity.
 */
static void initFileLogs(const std::string& logPath,
                         logging::trivial::severity_level level)
{
    logging::add_common_attributes();
    logging::add_file_log(
        keywords::file_name = logPath,
        keywords::rotation_size = 10 * 1024 * 1024, // 10 MiB per file
        keywords::max_files = 5,
        keywords::auto_flush = true,
        keywords::open_mode = std::ios::out | std::ios::app,
        keywords::format =
            (expr::stream << expr::format_date_time<boost::posix_time::ptime>(
                                 "TimeStamp", "%Y-%m-%d %H:%M:%S.%f")
                          << " [" << logging::trivial::severity << "] "
                          << expr::smessage));
    logging::core::get()->set_filter(logging::trivial::severity >= level);
}

// ---------------------------------------------------------------------------
// Daemon UUID generation (simple /proc/sys/kernel/random/uuid reader)
// ---------------------------------------------------------------------------

/**
 * @brief Reads a fresh UUID from the kernel.
 *
 * @return UUID string (e.g. "550e8400-e29b-41d4-a716-446655440000").
 */
static std::string generateUuid()
{
    // Use the kernel's built-in UUID generator when available
    std::ifstream uuidFile("/proc/sys/kernel/random/uuid");
    if (uuidFile.is_open())
    {
        std::string uuid;
        std::getline(uuidFile, uuid);
        if (!uuid.empty())
        {
            return uuid;
        }
    }
    // Fallback: use PID + timestamp
    return std::format(
        "rtsrv-{}-{}",
        ::getpid(),
        std::chrono::system_clock::now().time_since_epoch().count());
}

/**
 * @brief Returns the local hostname.
 *
 * @return Hostname string or "unknown" on failure.
 */
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
// Main
// ---------------------------------------------------------------------------

/**
 * @brief Program entry point.
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
    po::options_description desc("rtsrv – Real-Time Service Daemon");
    // clang-format off
    desc.add_options()
        ("help,h",       "Print this help message and exit")
        ("version,v",    "Print version and exit")
        ("config,c",
            po::value<std::string>()->default_value(
                std::string(kDefaultConfig)),
            "Path to the JSON configuration file")
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
        std::cout << "rtsrv " << kVersion << '\n';
        return EXIT_SUCCESS;
    }

    const std::string configPath = vm["config"].as<std::string>();
    const bool foreground = vm.count("foreground") > 0;

    // -----------------------------------------------------------------------
    // Load configuration
    // -----------------------------------------------------------------------
    rtsrv::Config config;
    try
    {
        config = rtsrv::loadConfig(configPath);
    }
    catch (const std::exception& ex)
    {
        std::cerr << "Failed to load configuration: " << ex.what() << '\n';
        return EXIT_FAILURE;
    }

    // -----------------------------------------------------------------------
    // Daemonise (or stay in foreground)
    // -----------------------------------------------------------------------
    rtsrv::Daemon daemon(config.daemon.pid_file, config.daemon.working_dir);

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

        // After daemonise() stdio is /dev/null; use file logging
        try
        {
            initFileLogs(config.daemon.log_file,
                         parseLogLevel(config.daemon.log_level));
        }
        catch (const std::exception& ex)
        {
            // Cannot write to stderr after daemonise; nothing we can do
            return EXIT_FAILURE;
        }
    }
    else
    {
        // Foreground mode: log to console
        try
        {
            initConsoleLogs(parseLogLevel(config.daemon.log_level));
        }
        catch (const std::exception& ex)
        {
            std::cerr << "Log init failed: " << ex.what() << '\n';
            return EXIT_FAILURE;
        }
    }

    BOOST_LOG_TRIVIAL(info)
        << std::format("rtsrv {} starting (pid={} foreground={})",
                       kVersion,
                       ::getpid(),
                       foreground);

    // -----------------------------------------------------------------------
    // gRPC client manager
    // -----------------------------------------------------------------------
    const std::string daemonId = generateUuid();
    const std::string hostname = getHostname();

    BOOST_LOG_TRIVIAL(info)
        << std::format("Daemon ID: {}  Hostname: {}  Servers: {}",
                       daemonId,
                       hostname,
                       config.servers.size());

    rtsrv::GrpcClientManager clientMgr(
        config.servers, daemonId, hostname, std::string(kVersion));

    // Register SIGHUP reload handler
    daemon.setSighupHandler([&] {
        BOOST_LOG_TRIVIAL(info) << "SIGHUP received – reloading configuration";
        try
        {
            const auto newCfg = rtsrv::loadConfig(configPath);
            BOOST_LOG_TRIVIAL(info) << "Configuration reloaded (server "
                                       "reconnects not yet implemented)";
        }
        catch (const std::exception& ex)
        {
            BOOST_LOG_TRIVIAL(error)
                << std::format("Reload failed: {}", ex.what());
        }
    });

    // Start all clients
    clientMgr.startAll();
    BOOST_LOG_TRIVIAL(info) << "All gRPC clients started";

    // -----------------------------------------------------------------------
    // Main event loop
    // -----------------------------------------------------------------------
    using namespace std::chrono_literals;

    BOOST_LOG_TRIVIAL(info) << "Entering main event loop";
    while (!rtsrv::Daemon::shouldStop())
    {
        // Check for SIGHUP reload request
        daemon.shouldReload();

        // Sleep for a short interval before the next iteration
        std::this_thread::sleep_for(500ms);
    }

    // -----------------------------------------------------------------------
    // Shutdown
    // -----------------------------------------------------------------------
    BOOST_LOG_TRIVIAL(info) << "Shutdown signal received; stopping clients";
    clientMgr.stopAll();

    BOOST_LOG_TRIVIAL(info) << "rtsrv exiting cleanly";
    return EXIT_SUCCESS;
}
