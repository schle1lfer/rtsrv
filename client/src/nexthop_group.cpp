/**
 * @file client/src/nexthop_group.cpp
 * @brief Implementation of the SRA-owned ECMP nexthop group manager.
 *
 * Kernel nexthop group objects are managed through the pure-C API declared in
 * client/include/client/netlink_nexthop.h:
 *   - netlink_nexthop_group_create() → RTM_NEWNEXTHOP + NLM_F_CREATE
 *   - netlink_nexthop_group_update() → RTM_NEWNEXTHOP + NLM_F_REPLACE
 *   - netlink_nexthop_group_delete() → RTM_DELNEXTHOP
 *
 * The kernel assigns a unique NHA_ID to each SRA-created group; this ID is
 * stable for the group's lifetime (updates preserve the same NHA_ID) and is
 * used as nexthop_id_ipv4 in every ud_server ROUTE_ADD for the corresponding
 * loopback.
 *
 * All public NexhopGroupManager methods are thread-safe: the shared_mutex is
 * held in shared mode for queries and in exclusive mode for mutations.
 * Kernel netlink calls are issued while holding the exclusive lock to keep
 * in-memory state and kernel state consistent.
 *
 * @see client/include/client/nexthop_group.hpp for full API documentation.
 */

#include "client/nexthop_group.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <vector>

namespace sra
{

// ---------------------------------------------------------------------------
// Private helper
// ---------------------------------------------------------------------------

void NexhopGroupManager::toKernelGrp(const std::vector<EcmpMember>& members,
                                      netlink_nexthop_grp_t* out) noexcept
{
    for (std::size_t i = 0; i < members.size(); ++i)
    {
        out[i].id = members[i].nhid;
        out[i].weight = static_cast<uint8_t>(members[i].weight > 0
                                                 ? members[i].weight - 1
                                                 : 0);
        out[i].weight_high = 0;
    }
}

// ---------------------------------------------------------------------------
// NexhopGroupManager — destructor
// ---------------------------------------------------------------------------

NexhopGroupManager::~NexhopGroupManager()
{
    /* Delete every kernel nexthop group object that is still alive. */
    std::unique_lock lock(mutex_);
    for (auto& [key, g] : groups_)
    {
        if (g.kernel_nhid != 0)
            netlink_nexthop_group_delete(g.kernel_nhid);
    }
    groups_.clear();
}

// ---------------------------------------------------------------------------
// NexhopGroupManager — lifecycle
// ---------------------------------------------------------------------------

void NexhopGroupManager::create(const std::string& loopback_ipv4,
                                 const std::string& hostname)
{
    std::unique_lock lock(mutex_);
    if (groups_.contains(loopback_ipv4))
        return; /* already tracked — do not overwrite existing kernel object */

    EcmpGroup g;
    g.loopback_ipv4 = loopback_ipv4;
    g.hostname = hostname;
    /* kernel_nhid stays 0 until the first add_member() call */
    groups_.emplace(loopback_ipv4, std::move(g));
}

bool NexhopGroupManager::remove(const std::string& loopback_ipv4)
{
    std::unique_lock lock(mutex_);
    const auto it = groups_.find(loopback_ipv4);
    if (it == groups_.end())
        return false;

    if (it->second.kernel_nhid != 0)
        netlink_nexthop_group_delete(it->second.kernel_nhid);

    groups_.erase(it);
    return true;
}

void NexhopGroupManager::clear()
{
    std::unique_lock lock(mutex_);
    for (auto& [key, g] : groups_)
    {
        if (g.kernel_nhid != 0)
            netlink_nexthop_group_delete(g.kernel_nhid);
    }
    groups_.clear();
}

// ---------------------------------------------------------------------------
// NexhopGroupManager — member management
// ---------------------------------------------------------------------------

uint32_t NexhopGroupManager::add_member(const std::string& loopback_ipv4,
                                         uint32_t nhid,
                                         uint8_t weight)
{
    if (nhid == 0)
        return 0;

    std::unique_lock lock(mutex_);
    auto& g = groups_[loopback_ipv4]; /* inserts default EcmpGroup if absent */
    if (g.loopback_ipv4.empty())
        g.loopback_ipv4 = loopback_ipv4;

    /* Update weight if nhid is already present; no duplicates. */
    for (auto& m : g.members)
    {
        if (m.nhid == nhid)
        {
            if (m.weight == weight)
                return g.kernel_nhid; /* nothing changed */
            m.weight = weight;
            /* Fall through to update the kernel object. */
            goto do_update;
        }
    }
    g.members.push_back({nhid, weight});

do_update:
    {
        /* Build the kernel group array. */
        std::vector<netlink_nexthop_grp_t> kgrp(g.members.size());
        toKernelGrp(g.members, kgrp.data());

        if (g.kernel_nhid == 0)
        {
            /* First member: create the kernel nexthop group. */
            const uint32_t kid = netlink_nexthop_group_create(
                kgrp.data(), static_cast<uint32_t>(kgrp.size()));
            g.kernel_nhid = kid; /* 0 on error — caller checks group_nhid() */
        }
        else
        {
            /* Subsequent member: update the existing kernel group in place. */
            netlink_nexthop_group_update(
                g.kernel_nhid,
                kgrp.data(),
                static_cast<uint32_t>(kgrp.size()));
        }
    }

    return g.kernel_nhid;
}

bool NexhopGroupManager::remove_member(const std::string& loopback_ipv4,
                                        uint32_t nhid)
{
    std::unique_lock lock(mutex_);
    const auto it = groups_.find(loopback_ipv4);
    if (it == groups_.end())
        return false;

    EcmpGroup& g = it->second;
    const auto before = g.members.size();

    g.members.erase(
        std::remove_if(g.members.begin(),
                       g.members.end(),
                       [nhid](const EcmpMember& m) { return m.nhid == nhid; }),
        g.members.end());

    if (g.members.size() == before)
        return false; /* nhid was not a member */

    if (g.members.empty())
    {
        /* Group is now empty — delete the kernel nexthop group object. */
        if (g.kernel_nhid != 0)
        {
            netlink_nexthop_group_delete(g.kernel_nhid);
            g.kernel_nhid = 0;
        }
    }
    else if (g.kernel_nhid != 0)
    {
        /* Update the kernel group with the reduced member list. */
        std::vector<netlink_nexthop_grp_t> kgrp(g.members.size());
        toKernelGrp(g.members, kgrp.data());
        netlink_nexthop_group_update(g.kernel_nhid,
                                     kgrp.data(),
                                     static_cast<uint32_t>(kgrp.size()));
    }

    return true;
}

uint32_t NexhopGroupManager::update_member(const std::string& loopback_ipv4,
                                            uint32_t nhid,
                                            uint8_t weight)
{
    if (nhid == 0)
        return 0;

    std::unique_lock lock(mutex_);
    auto& g = groups_[loopback_ipv4]; /* inserts if absent */
    if (g.loopback_ipv4.empty())
        g.loopback_ipv4 = loopback_ipv4;

    g.members.clear();
    g.members.push_back({nhid, weight});

    netlink_nexthop_grp_t kgrp;
    toKernelGrp(g.members, &kgrp);

    if (g.kernel_nhid == 0)
    {
        g.kernel_nhid =
            netlink_nexthop_group_create(&kgrp, 1);
    }
    else
    {
        netlink_nexthop_group_update(g.kernel_nhid, &kgrp, 1);
    }

    return g.kernel_nhid;
}

// ---------------------------------------------------------------------------
// NexhopGroupManager — queries
// ---------------------------------------------------------------------------

EcmpGroup NexhopGroupManager::get(const std::string& loopback_ipv4) const
{
    std::shared_lock lock(mutex_);
    const auto it = groups_.find(loopback_ipv4);
    if (it == groups_.end())
        return {};
    return it->second;
}

uint32_t
NexhopGroupManager::group_nhid(const std::string& loopback_ipv4) const
{
    std::shared_lock lock(mutex_);
    const auto it = groups_.find(loopback_ipv4);
    if (it == groups_.end())
        return 0;
    return it->second.kernel_nhid;
}

bool NexhopGroupManager::contains(const std::string& loopback_ipv4) const
{
    std::shared_lock lock(mutex_);
    return groups_.contains(loopback_ipv4);
}

std::size_t NexhopGroupManager::size() const
{
    std::shared_lock lock(mutex_);
    return groups_.size();
}

bool NexhopGroupManager::empty() const
{
    std::shared_lock lock(mutex_);
    return groups_.empty();
}

void NexhopGroupManager::for_each(
    std::function<void(const std::string&, const EcmpGroup&)> fn) const
{
    std::shared_lock lock(mutex_);
    for (const auto& [key, g] : groups_)
        fn(key, g);
}

} // namespace sra
