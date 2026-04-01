/**
 * @file common/src/config.cpp
 * @brief Implementation of the shared JSON configuration loader.
 *
 * Uses Boost.JSON for parsing.  Returns std::expected so the caller can
 * handle errors without exceptions if preferred.
 *
 * @version 1.0
 */

#include "common/config.hpp"

#include <boost/json.hpp>
#include <format>
#include <fstream>
#include <sstream>

namespace common
{

// ---------------------------------------------------------------------------
// ListenConfig::target
// ---------------------------------------------------------------------------

std::string ListenConfig::target() const
{
    return std::format("{}:{}", listen_host, listen_port);
}

std::string ClientConfig::target() const
{
    // Wrap bare IPv6 addresses in brackets for the gRPC target format.
    if (server_host.find(':') != std::string::npos &&
        server_host.front() != '[')
    {
        return std::format("[{}]:{}", server_host, server_port);
    }
    return std::format("{}:{}", server_host, server_port);
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace
{

/**
 * @brief Reads the entire contents of a file into a string.
 */
std::expected<std::string, std::string> readFile(const std::string& path)
{
    std::ifstream ifs(path);
    if (!ifs.is_open())
    {
        return std::unexpected(std::format("Cannot open file: '{}'", path));
    }
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return oss.str();
}

/**
 * @brief Extracts a string value from a JSON object, with a default.
 */
std::expected<std::string, std::string> getString(
    const boost::json::object& obj, std::string_view key, std::string dflt = {})
{
    if (!obj.contains(key))
    {
        return dflt;
    }
    const auto& val = obj.at(key);
    if (!val.is_string())
    {
        return std::unexpected(std::format("Key '{}' must be a string", key));
    }
    return std::string(val.get_string());
}

/**
 * @brief Extracts a boolean value from a JSON object, with a default.
 */
std::expected<bool, std::string>
getBool(const boost::json::object& obj, std::string_view key, bool dflt = false)
{
    if (!obj.contains(key))
    {
        return dflt;
    }
    const auto& val = obj.at(key);
    if (!val.is_bool())
    {
        return std::unexpected(std::format("Key '{}' must be a boolean", key));
    }
    return val.get_bool();
}

/**
 * @brief Extracts an integer value from a JSON object, with a default.
 */
std::expected<int, std::string>
getInt(const boost::json::object& obj, std::string_view key, int dflt = 0)
{
    if (!obj.contains(key))
    {
        return dflt;
    }
    const auto& val = obj.at(key);
    if (val.is_int64())
    {
        return static_cast<int>(val.get_int64());
    }
    if (val.is_uint64())
    {
        return static_cast<int>(val.get_uint64());
    }
    return std::unexpected(std::format("Key '{}' must be an integer", key));
}

/**
 * @brief Parses the "daemon" section of the config object.
 */
std::expected<DaemonConfig, std::string>
parseDaemon(const boost::json::object& obj)
{
    DaemonConfig cfg;

    auto pidFile = getString(obj, "pid_file", "/run/srmd/srmd.pid");
    if (!pidFile)
    {
        return std::unexpected(pidFile.error());
    }
    cfg.pid_file = std::move(*pidFile);

    auto logFile = getString(obj, "log_file", "/var/log/srmd/srmd.log");
    if (!logFile)
    {
        return std::unexpected(logFile.error());
    }
    cfg.log_file = std::move(*logFile);

    auto logLevel = getString(obj, "log_level", "info");
    if (!logLevel)
    {
        return std::unexpected(logLevel.error());
    }
    cfg.log_level = std::move(*logLevel);

    auto workingDir = getString(obj, "working_dir", "/");
    if (!workingDir)
    {
        return std::unexpected(workingDir.error());
    }
    cfg.working_dir = std::move(*workingDir);

    return cfg;
}

/**
 * @brief Parses the "server" section of the config object.
 */
std::expected<ListenConfig, std::string>
parseListen(const boost::json::object& obj)
{
    ListenConfig cfg;

    auto host = getString(obj, "listen_host", "0.0.0.0");
    if (!host)
    {
        return std::unexpected(host.error());
    }
    cfg.listen_host = std::move(*host);

    auto port = getInt(obj, "listen_port", 50051);
    if (!port)
    {
        return std::unexpected(port.error());
    }
    if (*port < 1 || *port > 65535)
    {
        return std::unexpected(
            std::format("listen_port {} out of range [1, 65535]", *port));
    }
    cfg.listen_port = static_cast<uint16_t>(*port);

    auto tlsEnabled = getBool(obj, "tls_enabled", false);
    if (!tlsEnabled)
    {
        return std::unexpected(tlsEnabled.error());
    }
    cfg.tls_enabled = *tlsEnabled;

    auto cert = getString(obj, "tls_cert");
    if (!cert)
    {
        return std::unexpected(cert.error());
    }
    cfg.tls_cert = std::move(*cert);

    auto key = getString(obj, "tls_key");
    if (!key)
    {
        return std::unexpected(key.error());
    }
    cfg.tls_key = std::move(*key);

    return cfg;
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

std::expected<ServerConfig, std::string>
loadServerConfig(const std::string& path)
{
    auto raw = readFile(path);
    if (!raw)
    {
        return std::unexpected(raw.error());
    }

    boost::json::error_code ec;
    const boost::json::value root = boost::json::parse(*raw, ec);
    if (ec)
    {
        return std::unexpected(
            std::format("JSON parse error in '{}': {}", path, ec.message()));
    }
    if (!root.is_object())
    {
        return std::unexpected(
            std::format("'{}': root must be a JSON object", path));
    }

    const auto& rootObj = root.get_object();
    ServerConfig config;

    if (rootObj.contains("daemon"))
    {
        const auto& v = rootObj.at("daemon");
        if (!v.is_object())
        {
            return std::unexpected(
                std::format("'{}': 'daemon' must be an object", path));
        }
        auto daemon = parseDaemon(v.get_object());
        if (!daemon)
        {
            return std::unexpected(daemon.error());
        }
        config.daemon = std::move(*daemon);
    }

    if (rootObj.contains("server"))
    {
        const auto& v = rootObj.at("server");
        if (!v.is_object())
        {
            return std::unexpected(
                std::format("'{}': 'server' must be an object", path));
        }
        auto listen = parseListen(v.get_object());
        if (!listen)
        {
            return std::unexpected(listen.error());
        }
        config.server = std::move(*listen);
    }

    return config;
}

// ---------------------------------------------------------------------------
// parseClient – "server" section of config.json
// ---------------------------------------------------------------------------

namespace
{

std::expected<ClientConfig, std::string>
parseClient(const boost::json::object& obj)
{
    ClientConfig cfg;

    auto host = getString(obj, "host", "127.0.0.1");
    if (!host)
    {
        return std::unexpected(host.error());
    }
    cfg.server_host = std::move(*host);

    auto port = getInt(obj, "port", 50051);
    if (!port)
    {
        return std::unexpected(port.error());
    }
    if (*port < 1 || *port > 65535)
    {
        return std::unexpected(
            std::format("port {} out of range [1, 65535]", *port));
    }
    cfg.server_port = static_cast<uint16_t>(*port);

    auto tlsEnabled = getBool(obj, "tls_enabled", false);
    if (!tlsEnabled)
    {
        return std::unexpected(tlsEnabled.error());
    }
    cfg.tls_enabled = *tlsEnabled;

    auto caCert = getString(obj, "ca_cert");
    if (!caCert)
    {
        return std::unexpected(caCert.error());
    }
    cfg.ca_cert = std::move(*caCert);

    auto timeout = getInt(obj, "timeout", 10);
    if (!timeout)
    {
        return std::unexpected(timeout.error());
    }
    cfg.timeout_seconds = *timeout;

    return cfg;
}

} // namespace

std::expected<ClientConfig, std::string>
loadClientConfig(const std::string& path)
{
    auto raw = readFile(path);
    if (!raw)
    {
        return std::unexpected(raw.error());
    }

    boost::json::error_code ec;
    const boost::json::value root = boost::json::parse(*raw, ec);
    if (ec)
    {
        return std::unexpected(
            std::format("JSON parse error in '{}': {}", path, ec.message()));
    }
    if (!root.is_object())
    {
        return std::unexpected(
            std::format("'{}': root must be a JSON object", path));
    }

    const auto& rootObj = root.get_object();
    ClientConfig config;

    if (rootObj.contains("server"))
    {
        const auto& v = rootObj.at("server");
        if (!v.is_object())
        {
            return std::unexpected(
                std::format("'{}': 'server' must be an object", path));
        }
        auto client = parseClient(v.get_object());
        if (!client)
        {
            return std::unexpected(client.error());
        }
        config = std::move(*client);
    }

    return config;
}

} // namespace common
