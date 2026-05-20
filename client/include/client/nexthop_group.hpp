/**
 * @file client/include/client/nexthop_group.hpp
 * @brief SRA-owned ECMP nexthop group manager.
 *
 * Manages one kernel nexthop group object per remote loopback.  The group is
 * created in the Linux kernel via RTM_NEWNEXTHOP (netlink_nexthop_group_create)
 * and is entirely independent of the nexthop objects installed by the OSPF
 * routing daemon.  The OSPF-contributed nexthop IDs (nhids) become *members*
 * of the SRA-owned group; the group's kernel-assigned NHA_ID is what SRA
 * programmes into the ud_server ROUTE_ADD @c nexthop_id_ipv4 field.
 *
 * @par Data-plane model
 * @code
 *   Kernel OSPF installs:
 *     nhid=100  via 192.168.0.2  dev Ethernet46   (OSPF-owned, single path)
 *     nhid=101  via 192.168.0.6  dev Ethernet47   (OSPF-owned, single path)
 *
 *   SRA creates its own individual nexthops (mirrors of OSPF ones):
 *     nhid=200  via 192.168.0.2  dev Ethernet46   (SRA-owned single nexthop)
 *     nhid=201  via 192.168.0.6  dev Ethernet47   (SRA-owned single nexthop)
 *
 *   SRA creates ECMP group whose members are the SRA nexthop IDs:
 *     nhid=500  GROUP { 200, 201 }               (SRA-owned ECMP group)
 *
 *   ud_server ROUTE_ADD for 2.2.2.2 → nexthop_id = 500
 * @endcode
 *
 * @par Typical SRA lifecycle
 * @code
 *   sra::NexhopGroupManager mgr;
 *
 *   // Startup: create a tracking entry for every remaining loopback.
 *   mgr.create("2.2.2.2", "spine-01");
 *
 *   // OSPF /32 ADDED — first path; creates the kernel group.
 *   mgr.add_member("2.2.2.2", 100);
 *   uint32_t id = mgr.group_nhid("2.2.2.2");  // → 500 (kernel-assigned)
 *
 *   // OSPF /32 ADDED — second path; updates the kernel group.
 *   mgr.add_member("2.2.2.2", 101);
 *
 *   // OSPF /32 REMOVED — one path withdrawn; updates the kernel group.
 *   mgr.remove_member("2.2.2.2", 100);
 *
 *   // OSPF /32 CHANGED — nhid reassigned; replaces the kernel group members.
 *   mgr.update_member("2.2.2.2", 102);
 *
 *   // Shutdown or node departure — deletes the kernel group.
 *   mgr.remove("2.2.2.2");
 * @endcode
 *
 * @note The manager does NOT create or modify the OSPF-installed individual
 *       nexthop objects; it only manages the SRA-owned group objects that
 *       reference them.
 *
 * @version 2.0
 */

#pragma once

#include "client/netlink_nexthop.h"

#include <cstdint>
#include <functional>
#include <map>
#include <shared_mutex>
#include <string>
#include <vector>

namespace sra
{

// ---------------------------------------------------------------------------
// EcmpMember
// ---------------------------------------------------------------------------

/**
 * @brief One member of an SRA-owned ECMP nexthop group.
 *
 * Represents a single kernel nexthop object (installed by OSPF) that has been
 * added to the SRA-managed ECMP group for a remote loopback.
 */
struct EcmpMember
{
    /**
     * @brief OSPF-installed kernel nexthop ID used to identify and match
     *        route events (RTM_NEWNEXTHOP / RTM_DELNEXTHOP from OSPF).
     *
     * This ID is owned by the routing daemon and must NOT be placed inside
     * the SRA-owned ECMP group — it is used only as a lookup key.
     */
    uint32_t ospf_nhid{0};

    /**
     * @brief NHA_ID of the SRA-created individual kernel nexthop object
     *        (RTM_NEWNEXTHOP without NHA_GROUP) that mirrors the OSPF nexthop.
     *
     * This is the actual member placed inside the SRA-owned ECMP group.  It is
     * created by netlink_nexthop_single_create() using the gateway and oif
     * obtained from the OSPF nexthop via netlink_nexthop_get_by_id(), and is
     * deleted by netlink_nexthop_delete() when removed from the group.
     */
    uint32_t sra_nhid{0};

    uint8_t     family{AF_INET}; ///< AF_INET or AF_INET6.
    std::string gateway;         ///< Gateway IP address string (for logging).
    uint32_t    oif{0};          ///< Output interface index.
    std::string oif_name;        ///< Interface name (for logging).
    uint8_t     weight{1};       ///< Relative forwarding weight (1 = equal-cost).
};

// ---------------------------------------------------------------------------
// EcmpGroup
// ---------------------------------------------------------------------------

/**
 * @brief SRA-owned ECMP nexthop group for one remote loopback.
 *
 * Holds the kernel NHA_ID of the group object created by SRA (@c kernel_nhid)
 * and the list of OSPF-contributed member nhids.  The kernel object is a
 * proper ECMP (mpath) nexthop group; membership changes are pushed to the
 * kernel via netlink_nexthop_group_update() on every add/remove operation.
 *
 * The group is lazily created: @c kernel_nhid is 0 until the first member is
 * added (since the kernel rejects empty groups).
 */
struct EcmpGroup
{
    std::string loopback_ipv4; ///< Remote loopback IPv4 address (group key).
    std::string hostname;      ///< Human-readable remote node name.

    /**
     * @brief NHA_ID of the SRA-created kernel nexthop group.
     *
     * 0 while the group has no members (no kernel object exists yet).
     * Assigned by the kernel on the first add_member() call and remains
     * stable for the lifetime of the group (the same kernel object is updated
     * in place when membership changes; it is only deleted on remove()).
     */
    uint32_t kernel_nhid{0};

    std::vector<EcmpMember> members; ///< OSPF-contributed member nhids.

    /**
     * @brief Returns the kernel NHA_ID to programme into ROUTE_ADD.
     *
     * Returns @c kernel_nhid when the group has at least one member and the
     * kernel object exists; returns 0 otherwise.
     */
    [[nodiscard]] uint32_t group_nhid() const noexcept { return kernel_nhid; }

    /** @brief Returns @c true when the member list is empty. */
    [[nodiscard]] bool empty() const noexcept { return members.empty(); }

    /** @brief Returns the number of OSPF member nhids. */
    [[nodiscard]] std::size_t size() const noexcept { return members.size(); }
};

// ---------------------------------------------------------------------------
// NexhopGroupManager
// ---------------------------------------------------------------------------

/**
 * @brief Thread-safe manager of SRA-owned per-loopback ECMP nexthop groups.
 *
 * One EcmpGroup is tracked for each remote loopback discovered via
 * GetRemainingLoopbacks / getRemainingNodes().  Kernel nexthop group objects
 * are created, updated, and deleted automatically as group membership changes.
 *
 * All public methods are thread-safe: reads hold a shared lock, writes hold
 * an exclusive lock.  Kernel netlink operations are performed while the
 * exclusive lock is held to keep the in-memory state consistent with the
 * kernel state.
 *
 * @note @c for_each() holds a shared lock during iteration; the callback must
 *       not call any mutating method on the same manager instance.
 */
class NexhopGroupManager
{
public:
    NexhopGroupManager() = default;

    /**
     * @brief Destructor — deletes all kernel nexthop group objects that are
     *        still alive.
     */
    ~NexhopGroupManager();

    NexhopGroupManager(const NexhopGroupManager&) = delete;
    NexhopGroupManager& operator=(const NexhopGroupManager&) = delete;

    // ── Lifecycle ──────────────────────────────────────────────────────────

    /**
     * @brief Registers a tracking entry for @p loopback_ipv4.
     *
     * Creates an in-memory EcmpGroup with no members.  No kernel object is
     * created yet; the kernel group is created lazily on the first
     * add_member() call (since the kernel rejects empty groups).
     *
     * No-op if an entry already exists for the given loopback.
     *
     * @param loopback_ipv4  Remote loopback IPv4 address (e.g. "2.2.2.2").
     * @param hostname       Human-readable node name for logging.
     */
    void create(const std::string& loopback_ipv4, const std::string& hostname);

    /**
     * @brief Removes the group for @p loopback_ipv4 and deletes the
     *        corresponding kernel nexthop group object (if one exists).
     *
     * @return @c true if an entry existed and was erased; @c false otherwise.
     */
    bool remove(const std::string& loopback_ipv4);

    /**
     * @brief Removes all groups and deletes every kernel nexthop group object.
     */
    void clear();

    // ── Member management ──────────────────────────────────────────────────

    /**
     * @brief Creates an SRA-owned individual kernel nexthop object that mirrors
     *        the OSPF nexthop identified by @p nhid, then adds it to the SRA
     *        ECMP group.
     *
     * - Calls netlink_nexthop_get_by_id() to look up the OSPF nexthop gateway
     *   and oif.  Returns 0 if the lookup fails.
     * - Calls netlink_nexthop_single_create() to create the SRA-owned individual
     *   nexthop.  Returns 0 if creation fails.
     * - If this is the first member, creates the kernel nexthop group
     *   (RTM_NEWNEXTHOP + NLM_F_CREATE) and stores the assigned NHA_ID in
     *   @c EcmpGroup::kernel_nhid.
     * - If the group already exists, updates the kernel object with the
     *   extended member list (RTM_NEWNEXTHOP + NLM_F_REPLACE).
     * - If @p nhid is already a member, only the weight is updated.
     * - @p nhid == 0 is silently ignored (returns 0).
     * - Creates the tracking entry if it does not yet exist.
     *
     * @param loopback_ipv4  Remote loopback IPv4 address.
     * @param nhid           OSPF-installed nexthop object ID to add (> 0).
     * @param weight         Path weight (1 = equal-cost default).
     * @return The kernel NHA_ID of the group (> 0), or 0 on error.
     */
    uint32_t add_member(const std::string& loopback_ipv4,
                        uint32_t nhid,
                        uint8_t weight = 1);

    /**
     * @brief Removes an OSPF-installed nexthop ID from the group and pushes
     *        the change to the kernel.
     *
     * - If the member list becomes empty after removal, the kernel nexthop
     *   group object is deleted (RTM_DELNEXTHOP) and @c kernel_nhid is reset
     *   to 0.
     * - Otherwise the kernel object is updated with the reduced member list.
     * - The SRA-owned individual nexthop (sra_nhid) is deleted after the group
     *   is updated.
     *
     * @return @c true if the member was present and removed; @c false if the
     *         group does not exist or @p nhid was not a member.
     */
    bool remove_member(const std::string& loopback_ipv4, uint32_t nhid);

    /**
     * @brief Replaces all members with a single new OSPF nexthop ID and
     *        pushes the change to the kernel.
     *
     * - If a kernel group object already exists it is deleted and recreated;
     *   all existing SRA-owned individual nexthops are also deleted.
     * - @p nhid == 0 is silently ignored.
     *
     * Call this on @c NETLINK_ROUTE_CHANGED events where the OSPF daemon
     * reassigns the nexthop object ID for a /32 route.
     *
     * @param loopback_ipv4  Remote loopback IPv4 address.
     * @param nhid           New OSPF nexthop object ID (> 0).
     * @param weight         Path weight (1 = equal-cost default).
     * @return The kernel NHA_ID of the group (> 0), or 0 on kernel error.
     */
    uint32_t update_member(const std::string& loopback_ipv4,
                           uint32_t nhid,
                           uint8_t weight = 1);

    // ── Queries ────────────────────────────────────────────────────────────

    /**
     * @brief Returns a copy of the group for @p loopback_ipv4.
     *
     * @return The group if it exists; a default-constructed EcmpGroup
     *         otherwise (with @c kernel_nhid == 0).
     */
    [[nodiscard]] EcmpGroup get(const std::string& loopback_ipv4) const;

    /**
     * @brief Returns the kernel NHA_ID of the SRA-created group for
     *        @p loopback_ipv4, or 0 if the group is absent or empty.
     */
    [[nodiscard]] uint32_t
    group_nhid(const std::string& loopback_ipv4) const;

    /**
     * @brief Returns @c true if a group entry exists for @p loopback_ipv4.
     */
    [[nodiscard]] bool contains(const std::string& loopback_ipv4) const;

    /**
     * @brief Returns the total number of managed groups.
     */
    [[nodiscard]] std::size_t size() const;

    /**
     * @brief Returns @c true when no groups are managed.
     */
    [[nodiscard]] bool empty() const;

    /**
     * @brief Invokes @p fn once for every managed (loopback_ipv4, EcmpGroup)
     *        pair in ascending lexicographic order of @c loopback_ipv4.
     *
     * A shared lock is held for the duration; @p fn must not call any
     * mutating method on this manager.
     *
     * @param fn  Callable with signature
     *            <tt>void(const std::string&, const EcmpGroup&)</tt>.
     */
    void for_each(
        std::function<void(const std::string&, const EcmpGroup&)> fn) const;

private:
    /**
     * @brief Converts the EcmpMember vector to the C array expected by the
     *        kernel management API.
     *
     * @param members  Source vector.
     * @param out      Destination array (must have capacity ≥ members.size()).
     */
    static void toKernelGrp(const std::vector<EcmpMember>& members,
                             netlink_nexthop_grp_t* out) noexcept;

    mutable std::shared_mutex mutex_; ///< Guards @c groups_.
    std::map<std::string, EcmpGroup> groups_; ///< loopback_ipv4 → EcmpGroup.
};

} // namespace sra
