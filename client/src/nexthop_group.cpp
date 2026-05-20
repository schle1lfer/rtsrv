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
        out[i].id = members[i].sra_nhid;
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
    std::unique_lock lock(mutex_);
    for (auto& [key, g] : groups_)
    {
        /* Delete the ECMP group first to drop references to SRA member nhids. */
        if (g.kernel_nhid != 0)
            netlink_nexthop_delete(g.kernel_nhid);
        /* Then delete each SRA-owned individual nexthop object. */
        for (const auto& m : g.members)
            if (m.sra_nhid != 0)
                netlink_nexthop_delete(m.sra_nhid);
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
            netlink_nexthop_delete(g.kernel_nhid);
        for (const auto& m : g.members)
            if (m.sra_nhid != 0)
                netlink_nexthop_delete(m.sra_nhid);
    }
    groups_.clear();
}

// ---------------------------------------------------------------------------
// NexhopGroupManager — member management
// ---------------------------------------------------------------------------

uint32_t NexhopGroupManager::add_member(const std::string& loopback_ipv4,
                                         uint32_t ospf_nhid,
                                         uint8_t weight)
{
    if (ospf_nhid == 0)
        return 0;

    /* Look up the OSPF nexthop to obtain its gateway address and output
     * interface.  This is done before acquiring the lock because it issues a
     * RTM_GETNEXTHOP netlink request that may block briefly. */
    netlink_nexthop_t ospf_nh;
    if (netlink_nexthop_get_by_id(ospf_nhid, &ospf_nh) < 0)
        return 0; /* OSPF nexthop not found or netlink error */

    if (ospf_nh.gateway[0] == '\0' || ospf_nh.oif == 0)
        return 0; /* nexthop has no gateway/oif — cannot mirror */

    std::unique_lock lock(mutex_);
    auto& g = groups_[loopback_ipv4];
    if (g.loopback_ipv4.empty())
        g.loopback_ipv4 = loopback_ipv4;

    /* If this ospf_nhid is already tracked, only update the weight. */
    for (auto& m : g.members)
    {
        if (m.ospf_nhid == ospf_nhid)
        {
            if (m.weight == weight)
                return g.kernel_nhid;
            m.weight = weight;
            goto do_update;
        }
    }

    {
        /* Create an SRA-owned individual nexthop that mirrors the OSPF one. */
        const uint32_t sra_id = netlink_nexthop_single_create(
            ospf_nh.family,
            ospf_nh.gateway,
            ospf_nh.oif);
        if (sra_id == 0)
            return 0; /* kernel rejected the single nexthop creation */

        EcmpMember mem;
        mem.ospf_nhid = ospf_nhid;
        mem.sra_nhid  = sra_id;
        mem.family    = ospf_nh.family;
        mem.gateway   = ospf_nh.gateway;
        mem.oif       = ospf_nh.oif;
        mem.oif_name  = ospf_nh.oif_name;
        mem.weight    = weight;
        g.members.push_back(std::move(mem));
    }

do_update:
    {
        std::vector<netlink_nexthop_grp_t> kgrp(g.members.size());
        toKernelGrp(g.members, kgrp.data());

        if (g.kernel_nhid == 0)
        {
            const uint32_t kid = netlink_nexthop_group_create(
                kgrp.data(), static_cast<uint32_t>(kgrp.size()));
            g.kernel_nhid = kid;
        }
        else
        {
            netlink_nexthop_group_update(
                g.kernel_nhid,
                kgrp.data(),
                static_cast<uint32_t>(kgrp.size()));
        }
    }

    return g.kernel_nhid;
}

bool NexhopGroupManager::remove_member(const std::string& loopback_ipv4,
                                        uint32_t ospf_nhid)
{
    std::unique_lock lock(mutex_);
    const auto it = groups_.find(loopback_ipv4);
    if (it == groups_.end())
        return false;

    EcmpGroup& g = it->second;

    /* Find the member by ospf_nhid and collect its sra_nhid for deletion. */
    uint32_t sra_to_delete = 0;
    g.members.erase(
        std::remove_if(
            g.members.begin(), g.members.end(),
            [ospf_nhid, &sra_to_delete](const EcmpMember& m)
            {
                if (m.ospf_nhid == ospf_nhid)
                {
                    sra_to_delete = m.sra_nhid;
                    return true;
                }
                return false;
            }),
        g.members.end());

    if (sra_to_delete == 0 && g.members.size() == g.members.size())
    {
        /* No member was found — check differently (erase returned same end). */
    }
    /* A cleaner check: if sra_to_delete is still 0 after erase, nothing was
     * removed. But the lambda sets it on match, so trust that. */
    if (sra_to_delete == 0)
        return false; /* ospf_nhid was not a member */

    if (g.members.empty())
    {
        /* Group empty — delete the ECMP group first, then the SRA nexthop. */
        if (g.kernel_nhid != 0)
        {
            netlink_nexthop_delete(g.kernel_nhid);
            g.kernel_nhid = 0;
        }
        netlink_nexthop_delete(sra_to_delete);
    }
    else
    {
        /* Update the kernel group with the reduced member list. */
        if (g.kernel_nhid != 0)
        {
            std::vector<netlink_nexthop_grp_t> kgrp(g.members.size());
            toKernelGrp(g.members, kgrp.data());
            netlink_nexthop_group_update(g.kernel_nhid,
                                         kgrp.data(),
                                         static_cast<uint32_t>(kgrp.size()));
        }
        /* Delete the SRA individual nexthop after updating the group. */
        netlink_nexthop_delete(sra_to_delete);
    }

    return true;
}

uint32_t NexhopGroupManager::update_member(const std::string& loopback_ipv4,
                                            uint32_t ospf_nhid,
                                            uint8_t weight)
{
    if (ospf_nhid == 0)
        return 0;

    /* Look up the OSPF nexthop before acquiring the lock. */
    netlink_nexthop_t ospf_nh;
    if (netlink_nexthop_get_by_id(ospf_nhid, &ospf_nh) < 0)
        return 0;

    if (ospf_nh.gateway[0] == '\0' || ospf_nh.oif == 0)
        return 0;

    /* Create the SRA-owned individual nexthop before acquiring the lock. */
    const uint32_t sra_id = netlink_nexthop_single_create(
        ospf_nh.family, ospf_nh.gateway, ospf_nh.oif);
    if (sra_id == 0)
        return 0;

    std::unique_lock lock(mutex_);
    auto& g = groups_[loopback_ipv4];
    if (g.loopback_ipv4.empty())
        g.loopback_ipv4 = loopback_ipv4;

    /* Delete all existing SRA individual nexthops (release before lock drop). */
    /* Group first (drops references), then individuals. */
    if (g.kernel_nhid != 0)
    {
        netlink_nexthop_delete(g.kernel_nhid);
        g.kernel_nhid = 0;
    }
    for (const auto& m : g.members)
        if (m.sra_nhid != 0)
            netlink_nexthop_delete(m.sra_nhid);

    g.members.clear();

    EcmpMember mem;
    mem.ospf_nhid = ospf_nhid;
    mem.sra_nhid  = sra_id;
    mem.family    = ospf_nh.family;
    mem.gateway   = ospf_nh.gateway;
    mem.oif       = ospf_nh.oif;
    mem.oif_name  = ospf_nh.oif_name;
    mem.weight    = weight;
    g.members.push_back(std::move(mem));

    netlink_nexthop_grp_t kgrp;
    toKernelGrp(g.members, &kgrp);
    g.kernel_nhid = netlink_nexthop_group_create(&kgrp, 1);

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
