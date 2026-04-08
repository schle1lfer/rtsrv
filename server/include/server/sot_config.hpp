/**
 * @file server/include/server/sot_config.hpp
 * @brief Source-of-Truth (SOT) configuration loader for srmd.
 *
 * Parses a @c route_sot_v2.json file that describes the intended routing
 * state of every managed network node (switch/leaf/spine).  The parsed data
 * is consumed by srmd @em before any gRPC connections are opened, so that the
 * desired state is fully validated in memory first.
 *
 * Expected JSON top-level structure:
 * @code
 * {
 *   "nodes_by_loopback": {
 *     "<management-ipv4>": {
 *       "hostname":  "<string>",
 *       "loopbacks": { "ipv4": "<string>", "ipv6": "<string>" },
 *       "vrfs": {
 *         "<vrf-name>": {
 *           "ipv4": {
 *             "interfaces": {
 *               "<if-name>": {
 *                 "type":          "nni" | "uni",
 *                 "local_address": "<cidr>",
 *                 "nexthop":       "<ipv4>",
 *                 "weight":        <uint>,
 *                 "description":   "<string>",
 *                 "prefixes": [
 *                   { "prefix": "<cidr>", "weight": <uint>,
 *                     "role": "<string>", "description": "<string>" }
 *                 ]
 *               }
 *             }
 *           }
 *         }
 *       }
 *     }
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
#include <vector>

namespace srmd
{

// ---------------------------------------------------------------------------
// Leaf data structures (bottom-up)
// ---------------------------------------------------------------------------

/**
 * @brief A single IP prefix entry attached to an interface.
 *
 * Represents one element of the @c prefixes array inside an interface block.
 */
struct SotPrefix
{
    /** @brief Destination prefix in CIDR notation (e.g. "10.0.1.0/24"). */
    std::string prefix;

    /** @brief Routing weight / preference value. */
    uint32_t weight{0};

    /** @brief Semantic role of this prefix (e.g. "server_network"). */
    std::string role;

    /** @brief Human-readable description (e.g. "VM_pool#1"). */
    std::string description;
};

/**
 * @brief IPv4 interface configuration within a VRF.
 *
 * Corresponds to one entry of the @c interfaces map under
 * @c vrfs.<vrf>.ipv4.interfaces.
 */
struct SotInterface
{
    /** @brief Interface name derived from the JSON object key (e.g.
     * "Ethernet0"). */
    std::string name;

    /**
     * @brief Interface type.
     *
     * Known values: @c "nni" (Network-to-Network Interface) or
     * @c "uni" (User-to-Network Interface).
     */
    std::string type;

    /** @brief Local IP address with prefix length (CIDR), e.g.
     * "192.168.1.2/30". */
    std::string local_address;

    /** @brief IPv4 next-hop address; may be empty for uni interfaces. */
    std::string nexthop;

    /** @brief Interface weight / preference. */
    uint32_t weight{0};

    /** @brief Human-readable description (e.g. "Server#1"). */
    std::string description;

    /** @brief Prefixes reachable via this interface. */
    std::vector<SotPrefix> prefixes;
};

/**
 * @brief IPv4 configuration block within a VRF.
 *
 * Wraps the interface list from @c vrfs.<vrf>.ipv4.
 */
struct SotVrfIpv4
{
    /** @brief List of configured interfaces (may be empty). */
    std::vector<SotInterface> interfaces;
};

/**
 * @brief IPv6 configuration block within a VRF.
 *
 * Wraps the interface list from @c vrfs.<vrf>.ipv6.
 */
struct SotVrfIpv6
{
    /** @brief List of configured interfaces (may be empty). */
    std::vector<SotInterface> interfaces;
};

/**
 * @brief Virtual Routing and Forwarding (VRF) instance configuration.
 *
 * Corresponds to one entry in the @c vrfs object of a node.
 */
struct SotVrf
{
    /** @brief VRF name derived from the JSON object key (e.g. "default"). */
    std::string name;

    /** @brief IPv4 configuration for this VRF. */
    SotVrfIpv4 ipv4;

    /** @brief IPv6 configuration for this VRF. */
    SotVrfIpv6 ipv6;
};

/**
 * @brief Loopback addresses of a node.
 */
struct SotLoopbacks
{
    /** @brief Primary IPv4 loopback (e.g. "1.1.1.1"). */
    std::string ipv4;

    /** @brief Primary IPv6 loopback (e.g. "fd00:100::1"). */
    std::string ipv6;
};

/**
 * @brief Complete configuration entry for a single managed network node.
 *
 * Represents one element of the @c nodes_by_loopback map.
 */
struct SotNode
{
    /**
     * @brief Management IPv4 address used as the JSON map key
     *        (e.g. "10.124.224.60").
     *
     * This is the key under @c nodes_by_loopback and is also the address
     * stored in @c SwitchEntry::ipv4 in @c switch_config.json so that srmd
     * can correlate switch entries with SOT nodes.
     */
    std::string management_ip;

    /** @brief Device hostname (e.g. "ST82-LEAF-60"). */
    std::string hostname;

    /** @brief Loopback (router-ID) addresses. */
    SotLoopbacks loopbacks;

    /** @brief List of VRF configurations; at least one element expected. */
    std::vector<SotVrf> vrfs;

    /**
     * @brief Returns a pointer to the VRF with the given @p name, or
     * @c nullptr if not found.
     *
     * @param name  VRF name to search for (e.g. "default").
     * @return Pointer into @c vrfs, or @c nullptr.
     */
    [[nodiscard]] const SotVrf* findVrf(const std::string& name) const noexcept;
};

/**
 * @brief The complete Source-of-Truth routing configuration.
 *
 * Contains all nodes keyed by their management IPv4 address.
 */
struct SotConfig
{
    /** @brief All managed nodes parsed from @c nodes_by_loopback. */
    std::vector<SotNode> nodes;

    /**
     * @brief Looks up a node by its management IPv4 address.
     *
     * @param managementIp  The IPv4 address used as the JSON key
     *                      (e.g. "10.124.224.60").
     * @return Const pointer to the matching @c SotNode, or @c nullptr.
     */
    [[nodiscard]] const SotNode*
    findByManagementIp(const std::string& managementIp) const noexcept;

    /**
     * @brief Returns the total count of prefixes across all nodes,
     *        VRFs, and interfaces.
     *
     * Useful for a quick pre-flight sanity check before issuing gRPC calls.
     *
     * @return Cumulative prefix count.
     */
    [[nodiscard]] std::size_t totalPrefixCount() const noexcept;
};

// ---------------------------------------------------------------------------
// Loader
// ---------------------------------------------------------------------------

/**
 * @brief Loads and parses a @c route_sot_v2.json file.
 *
 * The entire file is read, validated, and deserialised into a @c SotConfig
 * value.  All mandatory fields are checked; unknown extra fields are silently
 * ignored.
 *
 * This function is designed to be called @em before any gRPC connections are
 * opened, so that the desired routing state is fully validated in memory first.
 *
 * @param path  Filesystem path to the JSON SOT file.
 * @return      Populated @c SotConfig on success, or an error string
 *              describing the first validation failure encountered.
 */
[[nodiscard]] std::expected<SotConfig, std::string>
loadSotConfig(const std::string& path);

} // namespace srmd
