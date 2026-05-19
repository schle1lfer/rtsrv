/**
 * @file client/include/client/nexthop_group.hpp
 * @brief In-memory ECMP nexthop group manager for SRA.
 *
 * Provides a thread-safe registry of per-loopback ECMP nexthop groups.
 * Each group is keyed by a remote loopback IPv4 address and accumulates the
 * kernel nexthop object IDs (nhids) reported by netlink /32 OSPF route events.
 *
 * The manager is the authoritative source of truth for which nhid SRA should
 * programme into the ud_server ROUTE_ADD @c nexthop_id_ipv4 field.  When the
 * kernel assigns a composite ECMP group nexthop object for a multi-path /32
 * route, the group's NHA_ID is itself the nhid carried by the route event, so
 * a single add_member() call correctly represents any degree of ECMP.
 *
 * @par Typical SRA lifecycle
 * @code
 *   sra::NexhopGroupManager mgr;
 *
 *   // Startup — one group per remaining loopback returned by
 *   // GetRemainingLoopbacks / getRemainingNodes().
 *   mgr.create("2.2.2.2", "spine-01");
 *   mgr.create("3.3.3.3", "spine-02");
 *
 *   // Netlink OSPF /32 ADDED event — seed the group with the kernel nhid
 *   // and immediately obtain the effective id for ROUTE_ADD.
 *   mgr.add_member("2.2.2.2", 363);
 *   uint32_t nhid = mgr.effective_nhid("2.2.2.2");  // → 363
 *
 *   // Netlink OSPF /32 CHANGED event — kernel reassigned the nhid.
 *   mgr.update_member("2.2.2.2", 364);
 *
 *   // Netlink OSPF /32 REMOVED event.
 *   mgr.remove_member("2.2.2.2", 364);
 *
 *   // Shut-down or node departure.
 *   mgr.remove("2.2.2.2");
 * @endcode
 *
 * @note The manager does NOT create or modify kernel nexthop objects.  It
 *       tracks the IDs of objects that the kernel routing daemon (OSPF) has
 *       already installed.
 *
 * @version 1.0
 */

#pragma once

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
 * @brief One member of an ECMP nexthop group.
 *
 * Represents a single kernel nexthop object that contributes to a loopback's
 * ECMP group.  The @c nhid is the value of the NHA_ID attribute as reported
 * by the kernel via a netlink /32 OSPF route event (RTA_NH_ID).
 */
struct EcmpMember
{
    uint32_t nhid{0};  ///< Kernel nexthop object ID (NHA_ID); always > 0.
    uint8_t weight{1}; ///< Relative forwarding weight (1 = equal-cost).
};

// ---------------------------------------------------------------------------
// EcmpGroup
// ---------------------------------------------------------------------------

/**
 * @brief ECMP nexthop group for one remote loopback.
 *
 * Collects all kernel nexthop object IDs associated with a given remote
 * loopback IP.  Members are added when OSPF /32 ADD or CHANGED events arrive
 * and removed on OSPF /32 REMOVED events.
 *
 * The @c effective_nhid() method returns the ID to place in the ud_server
 * ROUTE_ADD @c nexthop_id_ipv4 field.  It selects the most recently added
 * member, which is correct in the common single-path case and in the ECMP
 * case where the kernel itself produces a composite group object.
 */
struct EcmpGroup
{
    std::string loopback_ipv4;       ///< Remote loopback address (group key).
    std::string hostname;            ///< Human-readable node name.
    std::vector<EcmpMember> members; ///< Active kernel nhids in arrival order.

    /**
     * @brief Returns the nexthop ID to programme into ROUTE_ADD.
     *
     * Selects the most recently added member.  Returns 0 when the group has
     * no members (the group was created but no OSPF event has arrived yet, or
     * all members have been removed).
     */
    [[nodiscard]] uint32_t effective_nhid() const noexcept;

    /** @brief Returns true when the member list is empty. */
    [[nodiscard]] bool empty() const noexcept { return members.empty(); }

    /** @brief Returns the number of member nexthop objects. */
    [[nodiscard]] std::size_t size() const noexcept { return members.size(); }
};

// ---------------------------------------------------------------------------
// NexhopGroupManager
// ---------------------------------------------------------------------------

/**
 * @brief Thread-safe registry of per-loopback ECMP nexthop groups.
 *
 * All public methods are safe to call concurrently from multiple threads.
 * Read-only queries (get(), effective_nhid(), contains(), size(), for_each())
 * take a shared lock and may execute in parallel.  Mutating operations take
 * an exclusive lock.
 *
 * @note @c for_each() holds a shared lock during iteration; the callback must
 *       not call any mutating method on the same manager instance.
 */
class NexhopGroupManager
{
public:
    NexhopGroupManager() = default;
    ~NexhopGroupManager() = default;

    NexhopGroupManager(const NexhopGroupManager&) = delete;
    NexhopGroupManager& operator=(const NexhopGroupManager&) = delete;

    // ── Lifecycle ──────────────────────────────────────────────────────────

    /**
     * @brief Creates an empty ECMP group for @p loopback_ipv4.
     *
     * If a group already exists for the given loopback this call is a no-op;
     * the existing group and its members are left unchanged.
     *
     * @param loopback_ipv4  Remote loopback IPv4 address (e.g. "2.2.2.2").
     * @param hostname       Human-readable node name stored in the group
     *                       for logging and diagnostics.
     */
    void create(const std::string& loopback_ipv4, const std::string& hostname);

    /**
     * @brief Removes the group for @p loopback_ipv4 together with all its
     *        members.
     *
     * @return @c true if a group existed and was erased; @c false if no group
     *         was found for the given loopback.
     */
    bool remove(const std::string& loopback_ipv4);

    /**
     * @brief Removes every group, leaving the manager empty.
     */
    void clear();

    // ── Member management ──────────────────────────────────────────────────

    /**
     * @brief Adds a kernel nexthop object ID to the group for
     *        @p loopback_ipv4.
     *
     * If @p nhid is already present in the group the existing entry's weight
     * is updated and no duplicate is appended.  If no group exists for the
     * loopback a new empty group is created on the fly.
     *
     * A value of @p nhid == 0 is silently ignored.
     *
     * @param loopback_ipv4  Remote loopback IPv4 address.
     * @param nhid           Kernel nexthop object ID to add (must be > 0).
     * @param weight         Relative path weight (1 = equal-cost default).
     */
    void add_member(const std::string& loopback_ipv4,
                    uint32_t nhid,
                    uint8_t weight = 1);

    /**
     * @brief Removes the kernel nexthop object ID @p nhid from the group for
     *        @p loopback_ipv4.
     *
     * @return @c true if the member was found and removed; @c false if the
     *         group does not exist or @p nhid was not a member.
     */
    bool remove_member(const std::string& loopback_ipv4, uint32_t nhid);

    /**
     * @brief Replaces all members of the group with a single entry
     *        (@p nhid, @p weight).
     *
     * Equivalent to clearing the member list and calling add_member().
     * Call this on @c NETLINK_ROUTE_CHANGED events where the kernel has
     * reassigned the nexthop object ID for a /32 route.
     *
     * A value of @p nhid == 0 is silently ignored.
     *
     * @param loopback_ipv4  Remote loopback IPv4 address.
     * @param nhid           Replacement kernel nexthop object ID (must be > 0).
     * @param weight         Relative path weight (1 = equal-cost default).
     */
    void update_member(const std::string& loopback_ipv4,
                       uint32_t nhid,
                       uint8_t weight = 1);

    // ── Queries ────────────────────────────────────────────────────────────

    /**
     * @brief Returns a copy of the group for @p loopback_ipv4.
     *
     * @return The group if it exists; a default-constructed (empty) EcmpGroup
     *         otherwise.
     */
    [[nodiscard]] EcmpGroup get(const std::string& loopback_ipv4) const;

    /**
     * @brief Returns the effective nexthop ID for @p loopback_ipv4.
     *
     * @return The effective nhid (> 0) from the group's most recent member,
     *         or 0 if the group is absent or has no members.
     */
    [[nodiscard]] uint32_t
    effective_nhid(const std::string& loopback_ipv4) const;

    /**
     * @brief Returns @c true if a group exists for @p loopback_ipv4.
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
     *        pair.
     *
     * The pairs are visited in ascending lexicographic order of
     * @c loopback_ipv4.  @p fn receives const references and must not call
     * any mutating method on this manager while the iteration is in progress
     * (the shared lock is held for the duration of the call).
     *
     * @param fn  Callable with signature
     *            <tt>void(const std::string&, const EcmpGroup&)</tt>.
     */
    void for_each(
        std::function<void(const std::string&, const EcmpGroup&)> fn) const;

private:
    mutable std::shared_mutex mutex_; ///< Guards @c groups_.
    std::map<std::string, EcmpGroup> groups_; ///< loopback_ipv4 → EcmpGroup.
};

} // namespace sra
