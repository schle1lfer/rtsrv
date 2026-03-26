/**
 * @file client/src/sot_config.cpp
 * @brief Implementation of the Source-of-Truth configuration loader.
 *
 * Parses @c route_sot_v2.json using Boost.JSON.  All public symbols belong to
 * namespace @c sra; internal parsing helpers are kept in an anonymous
 * namespace.
 *
 * @version 1.0
 */

#include "client/sot_config.hpp"

#include <algorithm>
#include <boost/json.hpp>
#include <format>
#include <fstream>
#include <numeric>
#include <sstream>

namespace sra
{

// ---------------------------------------------------------------------------
// SotNode / SotConfig helpers
// ---------------------------------------------------------------------------

const SotVrf* SotNode::findVrf(const std::string& name) const noexcept
{
    const auto it = std::ranges::find_if(vrfs, [&](const SotVrf& v) {
        return v.name == name;
    });
    return it != vrfs.end() ? &*it : nullptr;
}

const SotNode*
SotConfig::findByManagementIp(const std::string& managementIp) const noexcept
{
    const auto it = std::ranges::find_if(nodes, [&](const SotNode& n) {
        return n.management_ip == managementIp;
    });
    return it != nodes.end() ? &*it : nullptr;
}

std::size_t SotConfig::totalPrefixCount() const noexcept
{
    std::size_t total = 0;
    for (const auto& node : nodes)
    {
        for (const auto& vrf : node.vrfs)
        {
            for (const auto& iface : vrf.ipv4.interfaces)
            {
                total += iface.prefixes.size();
            }
        }
    }
    return total;
}

// ---------------------------------------------------------------------------
// Internal parsing helpers
// ---------------------------------------------------------------------------

namespace
{

/**
 * @brief Reads the entire content of a file into a string.
 *
 * @param path  Filesystem path.
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
 * @brief Returns a pointer to the named child of a JSON object, or an error.
 *
 * @param obj   JSON object to search.
 * @param key   Field name to look up.
 * @param ctx   Context string for error messages.
 * @return Const pointer to the value, or an error string.
 */
std::expected<const boost::json::value*, std::string> requireField(
    const boost::json::object& obj, std::string_view key, std::string_view ctx)
{
    const auto* v = obj.if_contains(key);
    if (!v)
    {
        return std::unexpected(
            std::format("{}: missing required field '{}'", ctx, key));
    }
    return v;
}

/**
 * @brief Extracts a required string field from a JSON object.
 *
 * @param obj   JSON object.
 * @param key   Field name.
 * @param ctx   Context string for error messages.
 * @return String value, or an error string.
 */
std::expected<std::string, std::string> getString(
    const boost::json::object& obj, std::string_view key, std::string_view ctx)
{
    auto field = requireField(obj, key, ctx);
    if (!field)
    {
        return std::unexpected(field.error());
    }
    const auto* str = (*field)->if_string();
    if (!str)
    {
        return std::unexpected(
            std::format("{}: field '{}' must be a string", ctx, key));
    }
    return std::string(str->c_str());
}

/**
 * @brief Extracts an optional string field; returns @p fallback if absent.
 *
 * @param obj       JSON object.
 * @param key       Field name.
 * @param fallback  Value when the field is missing.
 * @return String value or fallback.
 */
std::string getStringOpt(const boost::json::object& obj,
                         std::string_view key,
                         std::string fallback = {})
{
    const auto* v = obj.if_contains(key);
    if (!v)
    {
        return fallback;
    }
    const auto* s = v->if_string();
    return s ? std::string(s->c_str()) : fallback;
}

/**
 * @brief Extracts an unsigned integer field from a JSON object.
 *
 * Both @c int64 and @c uint64 JSON values are accepted.
 *
 * @param obj   JSON object.
 * @param key   Field name.
 * @param ctx   Context string for error messages.
 * @return uint32_t value, or an error string.
 */
std::expected<uint32_t, std::string> getUint(const boost::json::object& obj,
                                             std::string_view key,
                                             std::string_view ctx)
{
    auto field = requireField(obj, key, ctx);
    if (!field)
    {
        return std::unexpected(field.error());
    }
    if (const auto* i = (*field)->if_int64())
    {
        if (*i < 0)
        {
            return std::unexpected(
                std::format("{}: field '{}' must be non-negative", ctx, key));
        }
        return static_cast<uint32_t>(*i);
    }
    if (const auto* u = (*field)->if_uint64())
    {
        return static_cast<uint32_t>(*u);
    }
    return std::unexpected(
        std::format("{}: field '{}' must be an integer", ctx, key));
}

// ---------------------------------------------------------------------------
// parse* functions (bottom-up)
// ---------------------------------------------------------------------------

/**
 * @brief Parses a single prefix object from the @c prefixes array.
 *
 * @param obj    JSON object for this prefix.
 * @param ctx    Context string (e.g.
 * "nodes[x].vrfs[default].ifaces[Eth0].prefixes[0]").
 * @return Populated @c SotPrefix, or an error string.
 */
std::expected<SotPrefix, std::string>
parsePrefix(const boost::json::object& obj, std::string_view ctx)
{
    auto prefix = getString(obj, "prefix", ctx);
    if (!prefix)
    {
        return std::unexpected(prefix.error());
    }

    auto weight = getUint(obj, "weight", ctx);
    if (!weight)
    {
        return std::unexpected(weight.error());
    }

    SotPrefix p;
    p.prefix = std::move(*prefix);
    p.weight = *weight;
    p.role = getStringOpt(obj, "role");
    p.description = getStringOpt(obj, "description");
    return p;
}

/**
 * @brief Parses one interface entry from the @c interfaces map.
 *
 * @param ifName  Interface name (JSON object key, e.g. "Ethernet0").
 * @param obj     JSON object for this interface.
 * @param ctx     Parent context string for error messages.
 * @return Populated @c SotInterface, or an error string.
 */
std::expected<SotInterface, std::string>
parseInterface(std::string_view ifName,
               const boost::json::object& obj,
               std::string_view ctx)
{
    const std::string ifCtx = std::format("{}.interfaces[{}]", ctx, ifName);

    auto type = getString(obj, "type", ifCtx);
    if (!type)
    {
        return std::unexpected(type.error());
    }

    auto localAddr = getString(obj, "local_address", ifCtx);
    if (!localAddr)
    {
        return std::unexpected(localAddr.error());
    }

    auto weight = getUint(obj, "weight", ifCtx);
    if (!weight)
    {
        return std::unexpected(weight.error());
    }

    SotInterface iface;
    iface.name = std::string(ifName);
    iface.type = std::move(*type);
    iface.local_address = std::move(*localAddr);
    iface.nexthop = getStringOpt(obj, "nexthop");
    iface.weight = *weight;
    iface.description = getStringOpt(obj, "description");

    // Parse optional prefixes array
    const auto* prefixesVal = obj.if_contains("prefixes");
    if (prefixesVal)
    {
        const auto* arr = prefixesVal->if_array();
        if (!arr)
        {
            return std::unexpected(
                std::format("{}: 'prefixes' must be an array", ifCtx));
        }
        iface.prefixes.reserve(arr->size());
        for (std::size_t i = 0; i < arr->size(); ++i)
        {
            const auto* pObj = (*arr)[i].if_object();
            if (!pObj)
            {
                return std::unexpected(std::format(
                    "{}.prefixes[{}]: element must be a JSON object",
                    ifCtx,
                    i));
            }
            const std::string pCtx = std::format("{}.prefixes[{}]", ifCtx, i);
            auto pfx = parsePrefix(*pObj, pCtx);
            if (!pfx)
            {
                return std::unexpected(pfx.error());
            }
            iface.prefixes.push_back(std::move(*pfx));
        }
    }

    return iface;
}

/**
 * @brief Parses the @c ipv4 sub-object of a VRF.
 *
 * @param obj   JSON object for @c vrfs.<vrf>.ipv4.
 * @param ctx   Context string.
 * @return Populated @c SotVrfIpv4, or an error string.
 */
std::expected<SotVrfIpv4, std::string>
parseVrfIpv4(const boost::json::object& obj, std::string_view ctx)
{
    const std::string ipv4Ctx = std::format("{}.ipv4", ctx);

    SotVrfIpv4 v4;

    const auto* ifacesVal = obj.if_contains("interfaces");
    if (!ifacesVal)
    {
        // An absent or missing interfaces key is treated as an empty map.
        return v4;
    }
    const auto* ifacesObj = ifacesVal->if_object();
    if (!ifacesObj)
    {
        return std::unexpected(
            std::format("{}.interfaces: must be a JSON object", ipv4Ctx));
    }

    v4.interfaces.reserve(ifacesObj->size());
    for (const auto& [ifName, ifVal] : *ifacesObj)
    {
        const auto* ifObj = ifVal.if_object();
        if (!ifObj)
        {
            return std::unexpected(std::format(
                "{}.interfaces[{}]: must be a JSON object", ipv4Ctx, ifName));
        }
        auto iface = parseInterface(ifName, *ifObj, ipv4Ctx);
        if (!iface)
        {
            return std::unexpected(iface.error());
        }
        v4.interfaces.push_back(std::move(*iface));
    }

    return v4;
}

/**
 * @brief Parses one VRF entry.
 *
 * @param vrfName  VRF name (JSON object key, e.g. "default").
 * @param obj      JSON object for this VRF.
 * @param nodeCtx  Parent node context string.
 * @return Populated @c SotVrf, or an error string.
 */
std::expected<SotVrf, std::string> parseVrf(std::string_view vrfName,
                                            const boost::json::object& obj,
                                            std::string_view nodeCtx)
{
    const std::string vrfCtx = std::format("{}.vrfs[{}]", nodeCtx, vrfName);

    SotVrf vrf;
    vrf.name = std::string(vrfName);

    const auto* ipv4Val = obj.if_contains("ipv4");
    if (ipv4Val)
    {
        const auto* ipv4Obj = ipv4Val->if_object();
        if (!ipv4Obj)
        {
            return std::unexpected(
                std::format("{}.ipv4: must be a JSON object", vrfCtx));
        }
        auto v4 = parseVrfIpv4(*ipv4Obj, vrfCtx);
        if (!v4)
        {
            return std::unexpected(v4.error());
        }
        vrf.ipv4 = std::move(*v4);
    }

    return vrf;
}

/**
 * @brief Parses one node entry from @c nodes_by_loopback.
 *
 * @param managementIp  Management IPv4 address (JSON object key).
 * @param obj           JSON object for this node.
 * @return Populated @c SotNode, or an error string.
 */
std::expected<SotNode, std::string> parseNode(std::string_view managementIp,
                                              const boost::json::object& obj)
{
    const std::string ctx = std::format("nodes_by_loopback[{}]", managementIp);

    auto hostname = getString(obj, "hostname", ctx);
    if (!hostname)
    {
        return std::unexpected(hostname.error());
    }

    // Parse loopbacks
    SotLoopbacks loopbacks;
    const auto* lbField = obj.if_contains("loopbacks");
    if (lbField)
    {
        const auto* lbObj = lbField->if_object();
        if (!lbObj)
        {
            return std::unexpected(
                std::format("{}.loopbacks: must be a JSON object", ctx));
        }
        loopbacks.ipv4 = getStringOpt(*lbObj, "ipv4");
        loopbacks.ipv6 = getStringOpt(*lbObj, "ipv6");
    }

    // Parse VRFs
    std::vector<SotVrf> vrfs;
    const auto* vrfsField = obj.if_contains("vrfs");
    if (vrfsField)
    {
        const auto* vrfsObj = vrfsField->if_object();
        if (!vrfsObj)
        {
            return std::unexpected(
                std::format("{}.vrfs: must be a JSON object", ctx));
        }
        vrfs.reserve(vrfsObj->size());
        for (const auto& [vrfName, vrfVal] : *vrfsObj)
        {
            const auto* vrfObj = vrfVal.if_object();
            if (!vrfObj)
            {
                return std::unexpected(std::format(
                    "{}.vrfs[{}]: must be a JSON object", ctx, vrfName));
            }
            auto vrf = parseVrf(vrfName, *vrfObj, ctx);
            if (!vrf)
            {
                return std::unexpected(vrf.error());
            }
            vrfs.push_back(std::move(*vrf));
        }
    }

    SotNode node;
    node.management_ip = std::string(managementIp);
    node.hostname = std::move(*hostname);
    node.loopbacks = loopbacks;
    node.vrfs = std::move(vrfs);
    return node;
}

} // namespace

// ---------------------------------------------------------------------------
// loadSotConfig
// ---------------------------------------------------------------------------

std::expected<SotConfig, std::string> loadSotConfig(const std::string& path)
{
    // 1. Read file
    auto content = readFile(path);
    if (!content)
    {
        return std::unexpected(content.error());
    }

    // 2. Parse JSON
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

    const auto* rootObj = root.if_object();
    if (!rootObj)
    {
        return std::unexpected(
            std::format("'{}': top-level value must be a JSON object", path));
    }

    // 3. Require "nodes_by_loopback"
    const auto* nblField = rootObj->if_contains("nodes_by_loopback");
    if (!nblField)
    {
        return std::unexpected(std::format(
            "'{}': missing required top-level field 'nodes_by_loopback'",
            path));
    }
    const auto* nblObj = nblField->if_object();
    if (!nblObj)
    {
        return std::unexpected(std::format(
            "'{}': 'nodes_by_loopback' must be a JSON object", path));
    }

    // 4. Parse each node
    SotConfig cfg;
    cfg.nodes.reserve(nblObj->size());

    for (const auto& [managementIp, nodeVal] : *nblObj)
    {
        const auto* nodeObj = nodeVal.if_object();
        if (!nodeObj)
        {
            return std::unexpected(std::format(
                "nodes_by_loopback[{}]: must be a JSON object", managementIp));
        }
        auto node = parseNode(managementIp, *nodeObj);
        if (!node)
        {
            return std::unexpected(node.error());
        }
        cfg.nodes.push_back(std::move(*node));
    }

    return cfg;
}

} // namespace sra
