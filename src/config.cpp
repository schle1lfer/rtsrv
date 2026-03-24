/**
 * @file config.cpp
 * @brief Implementation of the JSON configuration loader.
 *
 * Uses Boost.JSON for parsing. All configuration values are validated after
 * parsing; missing optional fields fall back to their struct defaults.
 *
 * @version 1.0
 */

#include "config.hpp"

#include <boost/json.hpp>
#include <format>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace rtsrv
{

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace
{

/**
 * @brief Reads the entire contents of a file into a string.
 *
 * @param path Filesystem path of the file to read.
 * @return File contents as a UTF-8 string.
 * @throws std::runtime_error if the file cannot be opened.
 */
std::string readFile(const std::string& path)
{
    std::ifstream ifs(path);
    if (!ifs.is_open())
    {
        throw std::runtime_error(
            std::format("Cannot open configuration file: '{}'", path));
    }
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return oss.str();
}

/**
 * @brief Safely extracts a string value from a JSON object.
 *
 * Returns the default value when the key is absent.
 *
 * @param obj     Source JSON object.
 * @param key     Key to look up.
 * @param dflt    Default value when key is absent.
 * @return String value or default.
 * @throws std::runtime_error if the key exists but is not a JSON string.
 */
std::string getString(const boost::json::object& obj,
                      std::string_view key,
                      std::string dflt = {})
{
    if (!obj.contains(key))
    {
        return dflt;
    }
    const auto& val = obj.at(key);
    if (!val.is_string())
    {
        throw std::runtime_error(
            std::format("Config key '{}' must be a string", key));
    }
    return std::string(val.get_string());
}

/**
 * @brief Safely extracts a boolean value from a JSON object.
 *
 * @param obj  Source JSON object.
 * @param key  Key to look up.
 * @param dflt Default value when key is absent.
 * @return Boolean value or default.
 * @throws std::runtime_error if the key exists but is not a JSON bool.
 */
bool getBool(const boost::json::object& obj,
             std::string_view key,
             bool dflt = false)
{
    if (!obj.contains(key))
    {
        return dflt;
    }
    const auto& val = obj.at(key);
    if (!val.is_bool())
    {
        throw std::runtime_error(
            std::format("Config key '{}' must be a boolean", key));
    }
    return val.get_bool();
}

/**
 * @brief Safely extracts an integer value from a JSON object.
 *
 * Accepts JSON int64 and uint64 representations.
 *
 * @param obj  Source JSON object.
 * @param key  Key to look up.
 * @param dflt Default value when key is absent.
 * @return Integer value or default.
 * @throws std::runtime_error if the key exists but is not a JSON number.
 */
int getInt(const boost::json::object& obj, std::string_view key, int dflt = 0)
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
    throw std::runtime_error(
        std::format("Config key '{}' must be an integer", key));
}

/**
 * @brief Parses a single server entry from a JSON object.
 *
 * @param obj JSON object representing one server entry.
 * @return Populated ServerConfig.
 * @throws std::runtime_error if a mandatory field is missing or invalid.
 */
ServerConfig parseServer(const boost::json::object& obj)
{
    ServerConfig srv;

    srv.id = getString(obj, "id", "unnamed");
    srv.host = getString(obj, "host");
    if (srv.host.empty())
    {
        throw std::runtime_error("Server entry is missing mandatory 'host'");
    }

    const int port = getInt(obj, "port", 50051);
    if (port < 1 || port > 65535)
    {
        throw std::runtime_error(std::format(
            "Server '{}': port {} is out of range [1, 65535]", srv.id, port));
    }
    srv.port = static_cast<uint16_t>(port);

    srv.auth_token = getString(obj, "auth_token");
    srv.tls_enabled = getBool(obj, "tls_enabled", false);
    srv.tls_ca_cert = getString(obj, "tls_ca_cert");
    srv.tls_client_cert = getString(obj, "tls_client_cert");
    srv.tls_client_key = getString(obj, "tls_client_key");

    srv.connect_timeout_s = getInt(obj, "connect_timeout_s", 10);
    srv.heartbeat_interval_s = getInt(obj, "heartbeat_interval_s", 30);
    srv.reconnect_delay_s = getInt(obj, "reconnect_delay_s", 5);
    srv.max_reconnect_attempts = getInt(obj, "max_reconnect_attempts", 0);

    return srv;
}

/**
 * @brief Parses the top-level "daemon" section from a JSON object.
 *
 * @param obj JSON object representing the "daemon" block.
 * @return Populated DaemonConfig.
 */
DaemonConfig parseDaemon(const boost::json::object& obj)
{
    DaemonConfig cfg;
    cfg.pid_file = getString(obj, "pid_file", "/run/rtsrv/rtsrv.pid");
    cfg.log_file = getString(obj, "log_file", "/var/log/rtsrv/rtsrv.log");
    cfg.log_level = getString(obj, "log_level", "info");
    cfg.working_dir = getString(obj, "working_dir", "/");
    return cfg;
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

std::string ServerConfig::target() const
{
    return std::format("{}:{}", host, port);
}

Config loadConfig(const std::string& path)
{
    const std::string raw = readFile(path);

    boost::json::error_code ec;
    const boost::json::value root = boost::json::parse(raw, ec);
    if (ec)
    {
        throw std::runtime_error(
            std::format("JSON parse error in '{}': {}", path, ec.message()));
    }

    if (!root.is_object())
    {
        throw std::runtime_error(std::format(
            "Config file '{}': root element must be a JSON object", path));
    }

    const auto& rootObj = root.get_object();
    Config config;

    // Parse optional "daemon" section
    if (rootObj.contains("daemon"))
    {
        const auto& daemonVal = rootObj.at("daemon");
        if (!daemonVal.is_object())
        {
            throw std::runtime_error(
                std::format("Config '{}': 'daemon' must be an object", path));
        }
        config.daemon = parseDaemon(daemonVal.get_object());
    }

    // Parse mandatory "servers" array
    if (!rootObj.contains("servers"))
    {
        throw std::runtime_error(std::format(
            "Config '{}': missing mandatory 'servers' array", path));
    }

    const auto& serversVal = rootObj.at("servers");
    if (!serversVal.is_array())
    {
        throw std::runtime_error(
            std::format("Config '{}': 'servers' must be a JSON array", path));
    }

    for (const auto& elem : serversVal.get_array())
    {
        if (!elem.is_object())
        {
            throw std::runtime_error(std::format(
                "Config '{}': each server entry must be an object", path));
        }
        config.servers.push_back(parseServer(elem.get_object()));
    }

    if (config.servers.empty())
    {
        throw std::runtime_error(std::format(
            "Config '{}': 'servers' array must not be empty", path));
    }

    return config;
}

} // namespace rtsrv
