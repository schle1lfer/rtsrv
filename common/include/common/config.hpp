/**
 * @file common/include/common/config.hpp
 * @brief Shared configuration structures and JSON loader for srmd and sra.
 *
 * The server (srmd) reads a full DaemonConfig plus a ListenConfig from its
 * JSON file.  The client (sra) obtains the server address from the
 * command-line; it does not need a config file.
 *
 * JSON schema for srmd (config/srmd.json):
 * @code{.json}
 * {
 *   "daemon": {
 *     "pid_file"   : "/run/srmd/srmd.pid",
 *     "log_file"   : "/var/log/srmd/srmd.log",
 *     "log_level"  : "info",
 *     "working_dir": "/"
 *   },
 *   "server": {
 *     "listen_host": "0.0.0.0",
 *     "listen_port": 50051,
 *     "tls_enabled": false,
 *     "tls_cert"   : "",
 *     "tls_key"    : ""
 *   }
 * }
 * @endcode
 *
 * @version 1.0
 */

#pragma once

#include <cstdint>
#include <expected>
#include <string>

namespace common
{

// ---------------------------------------------------------------------------
// Sub-structures
// ---------------------------------------------------------------------------

/**
 * @brief Daemon process operational parameters.
 */
struct DaemonConfig
{
    std::string pid_file{"/run/srmd/srmd.pid"};     ///< PID file path.
    std::string log_file{"/var/log/srmd/srmd.log"}; ///< Log file path.
    std::string log_level{
        "info"}; ///< Min log level: trace/debug/info/warning/error/fatal.
    std::string working_dir{"/"}; ///< chdir() target after daemonisation.
};

/**
 * @brief gRPC listen address configuration for the server.
 */
struct ListenConfig
{
    std::string listen_host{"0.0.0.0"}; ///< IP address or hostname to bind.
    uint16_t listen_port{50051};        ///< TCP port to listen on.
    bool tls_enabled{false};            ///< Enable TLS on the listener.
    std::string tls_cert; ///< Path to PEM server certificate (TLS only).
    std::string tls_key;  ///< Path to PEM server private key (TLS only).

    /**
     * @brief Returns the gRPC-compatible listen target string.
     * @return e.g. "0.0.0.0:50051"
     */
    [[nodiscard]] std::string target() const;
};

/**
 * @brief Top-level server configuration container.
 */
struct ServerConfig
{
    DaemonConfig daemon; ///< Daemon process settings.
    ListenConfig server; ///< gRPC listener settings.
};

// ---------------------------------------------------------------------------
// Loader
// ---------------------------------------------------------------------------

/**
 * @brief Parses a JSON server configuration file.
 *
 * @param path  Filesystem path to the JSON file.
 * @return Populated ServerConfig on success, or an error string on failure.
 */
[[nodiscard]] std::expected<ServerConfig, std::string>
loadServerConfig(const std::string& path);

} // namespace common
