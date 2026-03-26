/**
 * @file client/src/switch_config.cpp
 * @brief Implementation of the switch configuration loader.
 *
 * @version 1.0
 */

#include "client/switch_config.hpp"

#include <boost/json.hpp>
#include <format>
#include <fstream>
#include <sstream>

namespace sra
{

// ---------------------------------------------------------------------------
// SwitchEntry helpers
// ---------------------------------------------------------------------------

std::string SwitchEntry::target() const
{
    return std::format("{}:{}", ipv4, grpc_port);
}

std::string SwitchEntry::label() const
{
    return name.empty() ? target() : name;
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace
{

/**
 * @brief Reads the entire content of a file into a string.
 *
 * @param path  Filesystem path to the file.
 * @return File contents, or an error string.
 */
std::expected<std::string, std::string> readFile(const std::string& path)
{
    std::ifstream ifs(path);
    if (!ifs.is_open())
    {
        return std::unexpected(std::format("Cannot open file: {}", path));
    }
    std::ostringstream ss;
    ss << ifs.rdbuf();
    if (ifs.bad())
    {
        return std::unexpected(std::format("Read error on file: {}", path));
    }
    return ss.str();
}

/**
 * @brief Extracts a required string field from a JSON object.
 *
 * @param obj    Source JSON object.
 * @param key    Field name to look up.
 * @param ctx    Human-readable context for error messages (e.g. "switches[0]").
 * @return The string value, or an error string.
 */
std::expected<std::string, std::string> getString(
    const boost::json::object& obj, std::string_view key, std::string_view ctx)
{
    const auto* val = obj.if_contains(key);
    if (!val)
    {
        return std::unexpected(
            std::format("{}: missing required field '{}'", ctx, key));
    }
    const auto* str = val->if_string();
    if (!str)
    {
        return std::unexpected(
            std::format("{}: field '{}' must be a string", ctx, key));
    }
    return std::string(str->c_str());
}

/**
 * @brief Extracts an optional string field from a JSON object.
 *
 * @param obj           Source JSON object.
 * @param key           Field name to look up.
 * @param defaultValue  Value returned when the field is absent.
 * @return The string value (or @p defaultValue).
 */
std::string getStringOpt(const boost::json::object& obj,
                         std::string_view key,
                         std::string defaultValue = {})
{
    const auto* val = obj.if_contains(key);
    if (!val)
    {
        return defaultValue;
    }
    const auto* str = val->if_string();
    return str ? std::string(str->c_str()) : defaultValue;
}

/**
 * @brief Extracts a required integer field from a JSON object.
 *
 * @param obj  Source JSON object.
 * @param key  Field name to look up.
 * @param ctx  Human-readable context for error messages.
 * @return The integer value, or an error string.
 */
std::expected<int64_t, std::string> getInt(const boost::json::object& obj,
                                           std::string_view key,
                                           std::string_view ctx)
{
    const auto* val = obj.if_contains(key);
    if (!val)
    {
        return std::unexpected(
            std::format("{}: missing required field '{}'", ctx, key));
    }
    if (const auto* i = val->if_int64())
    {
        return *i;
    }
    if (const auto* u = val->if_uint64())
    {
        return static_cast<int64_t>(*u);
    }
    return std::unexpected(
        std::format("{}: field '{}' must be an integer", ctx, key));
}

/**
 * @brief Parses one element of the @c switches array.
 *
 * @param elem   JSON object representing a single switch entry.
 * @param index  Zero-based index in the array (used in error messages).
 * @return Populated @c SwitchEntry, or an error string.
 */
std::expected<SwitchEntry, std::string>
parseEntry(const boost::json::object& elem, std::size_t index)
{
    const std::string ctx = std::format("switches[{}]", index);

    auto ipv4 = getString(elem, "ipv4", ctx);
    if (!ipv4)
    {
        return std::unexpected(ipv4.error());
    }

    auto port = getInt(elem, "grpc_port", ctx);
    if (!port)
    {
        return std::unexpected(port.error());
    }
    if (*port < 1 || *port > 65535)
    {
        return std::unexpected(std::format(
            "{}: grpc_port {} is out of range [1, 65535]", ctx, *port));
    }

    auto login = getString(elem, "login", ctx);
    if (!login)
    {
        return std::unexpected(login.error());
    }

    auto password = getString(elem, "password", ctx);
    if (!password)
    {
        return std::unexpected(password.error());
    }

    SwitchEntry entry;
    entry.name = getStringOpt(elem, "name");
    entry.ipv4 = std::move(*ipv4);
    entry.grpc_port = static_cast<uint16_t>(*port);
    entry.login = std::move(*login);
    entry.password = std::move(*password);
    return entry;
}

} // namespace

// ---------------------------------------------------------------------------
// loadSwitchConfig
// ---------------------------------------------------------------------------

std::expected<std::vector<SwitchEntry>, std::string>
loadSwitchConfig(const std::string& path)
{
    // Read the raw file
    auto content = readFile(path);
    if (!content)
    {
        return std::unexpected(content.error());
    }

    // Parse JSON
    boost::json::value root;
    try
    {
        root = boost::json::parse(*content);
    }
    catch (const std::exception& ex)
    {
        return std::unexpected(
            std::format("JSON parse error in '{}': {}", path, ex.what()));
    }

    const auto* obj = root.if_object();
    if (!obj)
    {
        return std::unexpected(
            std::format("'{}': top-level value must be a JSON object", path));
    }

    const auto* switchesVal = obj->if_contains("switches");
    if (!switchesVal)
    {
        return std::unexpected(std::format(
            "'{}': missing required top-level field 'switches'", path));
    }
    const auto* arr = switchesVal->if_array();
    if (!arr)
    {
        return std::unexpected(
            std::format("'{}': field 'switches' must be a JSON array", path));
    }
    if (arr->empty())
    {
        return std::unexpected(
            std::format("'{}': 'switches' array must not be empty", path));
    }

    // Parse each entry
    std::vector<SwitchEntry> entries;
    entries.reserve(arr->size());

    for (std::size_t i = 0; i < arr->size(); ++i)
    {
        const auto* elemObj = (*arr)[i].if_object();
        if (!elemObj)
        {
            return std::unexpected(std::format(
                "switches[{}]: each element must be a JSON object", i));
        }
        auto entry = parseEntry(*elemObj, i);
        if (!entry)
        {
            return std::unexpected(entry.error());
        }
        entries.push_back(std::move(*entry));
    }

    return entries;
}

} // namespace sra
