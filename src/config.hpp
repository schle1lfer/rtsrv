/**
 * @file config.hpp
 * @brief JSON configuration loader for the rtsrv daemon.
 *
 * Declares the data structures and loader that parse the daemon's JSON
 * configuration file (default: /etc/rtsrv/rtsrv.json). The configuration
 * specifies the list of remote gRPC servers and per-daemon tunables.
 *
 * Configuration file example:
 * @code{.json}
 * {
 *   "daemon": {
 *     "pid_file": "/run/rtsrv/rtsrv.pid",
 *     "log_file": "/var/log/rtsrv/rtsrv.log",
 *     "log_level": "info",
 *     "working_dir": "/"
 *   },
 *   "servers": [
 *     {
 *       "id": "primary",
 *       "host": "server1.example.com",
 *       "port": 50051,
 *       "auth_token": "",
 *       "tls_enabled": true,
 *       "tls_ca_cert": "/etc/rtsrv/ca.pem",
 *       "connect_timeout_s": 10,
 *       "heartbeat_interval_s": 30
 *     }
 *   ]
 * }
 * @endcode
 *
 * @version 1.0
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rtsrv
{

/**
 * @brief Holds connection parameters for a single remote gRPC server.
 */
struct ServerConfig
{
    std::string id;       ///< Human-readable identifier for this server entry.
    std::string host;     ///< Hostname or IP address of the remote server.
    uint16_t port{50051}; ///< TCP port on which the gRPC service listens.

    std::string auth_token; ///< Optional bearer token sent in the Connect RPC.

    bool tls_enabled{false}; ///< Whether to use TLS for the gRPC channel.
    std::string tls_ca_cert; ///< Path to the PEM-encoded CA certificate file
                             ///< (only used when tls_enabled is true).
    std::string tls_client_cert; ///< Path to the PEM client certificate
                                 ///< (mTLS; empty disables mTLS).
    std::string tls_client_key;  ///< Path to the PEM client private key.

    int connect_timeout_s{10}; ///< Seconds to wait when establishing a channel.
    int heartbeat_interval_s{30}; ///< Heartbeat interval override (0 = use
                                  ///< server-advertised interval).
    int reconnect_delay_s{5}; ///< Seconds to wait before reconnecting after a
                              ///< channel failure.
    int max_reconnect_attempts{0}; ///< Maximum reconnect attempts (0 =
                                   ///< unlimited).

    /**
     * @brief Returns "host:port" as a gRPC target string.
     * @return gRPC channel target string.
     */
    [[nodiscard]] std::string target() const;
};

/**
 * @brief Holds daemon-level operational configuration.
 */
struct DaemonConfig
{
    std::string pid_file{"/run/rtsrv/rtsrv.pid"};     ///< Path of the PID file.
    std::string log_file{"/var/log/rtsrv/rtsrv.log"}; ///< Path of the log file.
    std::string log_level{"info"}; ///< Minimum log level: trace, debug, info,
                                   ///< warning, error, fatal.
    std::string working_dir{"/"};  ///< Working directory after daemonisation.
};

/**
 * @brief Top-level configuration container.
 */
struct Config
{
    DaemonConfig daemon;               ///< Daemon operational settings.
    std::vector<ServerConfig> servers; ///< List of remote gRPC servers.
};

/**
 * @brief Loads and validates a JSON configuration file.
 *
 * Parses the file at @p path using Boost.JSON. Throws `std::runtime_error`
 * on parse failure or when a mandatory field is missing/invalid.
 *
 * @param path Absolute or relative path to the JSON configuration file.
 * @return Parsed Config structure.
 * @throws std::runtime_error on I/O errors, JSON parse errors, or validation
 *         failures.
 */
[[nodiscard]] Config loadConfig(const std::string& path);

} // namespace rtsrv
