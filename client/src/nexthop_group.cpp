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
        /* Delete ECMP group first (removes references to member sra_nhids). */
        if (g.kernel_nhid != 0)
            netlink_nexthop_delete(g.kernel_nhid);
        /* Then delete each SRA-owned individual nexthop. */
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

    /* Fetch the OSPF nexthop's gateway + oif before taking the lock. */
    netlink_nexthop_t ospf_nh;
    if (netlink_nexthop_get_by_id(ospf_nhid, &ospf_nh) < 0)
        return 0;
    if (ospf_nh.gateway[0] == '\0' || ospf_nh.oif == 0)
        return 0;

    std::unique_lock lock(mutex_);
    auto& g = groups_[loopback_ipv4];
    if (g.loopback_ipv4.empty())
        g.loopback_ipv4 = loopback_ipv4;

    /* If this ospf_nhid is already tracked, only update weight. */
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
        /* Create an SRA-owned single nexthop mirroring the OSPF one. */
        const uint32_t sra_id = netlink_nexthop_single_create(
            ospf_nh.family, ospf_nh.gateway, ospf_nh.oif);
        if (sra_id == 0)
            return 0;

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
    uint32_t sra_to_delete = 0;

    g.members.erase(
        std::remove_if(g.members.begin(), g.members.end(),
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

    if (sra_to_delete == 0)
        return false; /* ospf_nhid was not a member */

    if (g.members.empty())
    {
        /* Group now empty: delete ECMP group, then SRA nexthop. */
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
            netlink_nexthop_group_update(g.kernel_nhid, kgrp.data(),
                                         static_cast<uint32_t>(kgrp.size()));
        }
        /* Delete the SRA nexthop after the group no longer references it. */
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

    /* Fetch OSPF nexthop details before taking the lock. */
    netlink_nexthop_t ospf_nh;
    if (netlink_nexthop_get_by_id(ospf_nhid, &ospf_nh) < 0)
        return 0;
    if (ospf_nh.gateway[0] == '\0' || ospf_nh.oif == 0)
        return 0;

    /* Create the new SRA nexthop before taking the lock. */
    const uint32_t sra_id = netlink_nexthop_single_create(
        ospf_nh.family, ospf_nh.gateway, ospf_nh.oif);
    if (sra_id == 0)
        return 0;

    std::unique_lock lock(mutex_);
    auto& g = groups_[loopback_ipv4];
    if (g.loopback_ipv4.empty())
        g.loopback_ipv4 = loopback_ipv4;

    /* Delete old ECMP group (releases references), then old SRA nexthops. */
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

uint32_t NexhopGroupManager::sync_from_ospf(const std::string& loopback_ipv4,
                                              uint32_t           ospf_nhid)
{
    if (ospf_nhid == 0)
        return 0;

    /* Fetch the OSPF nexthop to determine if it is a single path or a group. */
    netlink_nexthop_t ospf_nh;
    if (netlink_nexthop_get_by_id(ospf_nhid, &ospf_nh) < 0)
        return 0;

    /* Build the list of individual OSPF path nhids to mirror. */
    std::vector<std::pair<uint32_t, uint8_t>> paths; /* {nhid, weight} */
    if (ospf_nh.group_count > 0)
    {
        /* OSPF installed an ECMP group — expand all members. */
        for (uint32_t i = 0; i < ospf_nh.group_count; ++i)
            paths.push_back({ospf_nh.group[i].id,
                             static_cast<uint8_t>(ospf_nh.group[i].weight + 1)});
    }
    else if (ospf_nh.gateway[0] != '\0' && ospf_nh.oif != 0)
    {
        /* Single-path nexthop — use directly. */
        paths.push_back({ospf_nhid, 1});
    }
    else
    {
        return 0; /* nexthop has no usable gateway/oif */
    }

    std::unique_lock lock(mutex_);
    auto& g = groups_[loopback_ipv4];
    if (g.loopback_ipv4.empty())
        g.loopback_ipv4 = loopback_ipv4;
    g.ospf_root_nhid = ospf_nhid;

    /* Determine which OSPF path nhids were added and which were removed. */
    std::vector<uint32_t> to_remove;
    for (const auto& m : g.members)
    {
        bool still_present = false;
        for (const auto& [pid, pw] : paths)
            if (pid == m.ospf_nhid) { still_present = true; break; }
        if (!still_present)
            to_remove.push_back(m.ospf_nhid);
    }

    /* Remove withdrawn members (collect sra_nhids to delete after group update). */
    std::vector<uint32_t> sra_to_delete;
    for (uint32_t old_ospf : to_remove)
    {
        g.members.erase(
            std::remove_if(g.members.begin(), g.members.end(),
                           [old_ospf, &sra_to_delete](const EcmpMember& m)
                           {
                               if (m.ospf_nhid == old_ospf)
                               {
                                   sra_to_delete.push_back(m.sra_nhid);
                                   return true;
                               }
                               return false;
                           }),
            g.members.end());
    }

    /* Add new members. */
    for (const auto& [pid, pw] : paths)
    {
        bool already = false;
        for (const auto& m : g.members)
            if (m.ospf_nhid == pid) { already = true; break; }
        if (already)
            continue;

        /* Fetch the individual OSPF path nexthop to get gateway + oif. */
        netlink_nexthop_t path_nh;
        if (netlink_nexthop_get_by_id(pid, &path_nh) < 0)
            continue;
        if (path_nh.gateway[0] == '\0' || path_nh.oif == 0)
            continue;

        const uint32_t sra_id = netlink_nexthop_single_create(
            path_nh.family, path_nh.gateway, path_nh.oif);
        if (sra_id == 0)
            continue;

        EcmpMember mem;
        mem.ospf_nhid = pid;
        mem.sra_nhid  = sra_id;
        mem.family    = path_nh.family;
        mem.gateway   = path_nh.gateway;
        mem.oif       = path_nh.oif;
        mem.oif_name  = path_nh.oif_name;
        mem.weight    = pw;
        g.members.push_back(std::move(mem));
    }

    /* Push the new member list to the kernel ECMP group. */
    if (!g.members.empty())
    {
        std::vector<netlink_nexthop_grp_t> kgrp(g.members.size());
        toKernelGrp(g.members, kgrp.data());

        if (g.kernel_nhid == 0)
            g.kernel_nhid = netlink_nexthop_group_create(
                kgrp.data(), static_cast<uint32_t>(kgrp.size()));
        else
            netlink_nexthop_group_update(
                g.kernel_nhid,
                kgrp.data(),
                static_cast<uint32_t>(kgrp.size()));
    }
    else if (g.kernel_nhid != 0)
    {
        netlink_nexthop_delete(g.kernel_nhid);
        g.kernel_nhid = 0;
    }

    /* Delete orphaned SRA nexthops after the group no longer references them. */
    for (uint32_t sra : sra_to_delete)
        netlink_nexthop_delete(sra);

    return g.kernel_nhid;
}

uint32_t NexhopGroupManager::sync_ospf_group_change(
    uint32_t                     ospf_nhid,
    const netlink_nexthop_grp_t* members,
    uint32_t                     count)
{
    /* Find the loopback that owns this OSPF root nhid. */
    std::string loopback;
    {
        std::shared_lock rlock(mutex_);
        for (const auto& [key, g] : groups_)
        {
            if (g.ospf_root_nhid == ospf_nhid)
            {
                loopback = key;
                break;
            }
        }
    }
    if (loopback.empty())
        return 0; /* not a tracked root */

    /* Build new path list from the event. */
    std::vector<std::pair<uint32_t, uint8_t>> paths;
    for (uint32_t i = 0; i < count; ++i)
        paths.push_back({members[i].id,
                         static_cast<uint8_t>(members[i].weight + 1)});

    std::unique_lock lock(mutex_);
    auto it = groups_.find(loopback);
    if (it == groups_.end())
        return 0;
    EcmpGroup& g = it->second;

    /* Collect members to remove. */
    std::vector<uint32_t> to_remove;
    for (const auto& m : g.members)
    {
        bool still_present = false;
        for (const auto& [pid, pw] : paths)
            if (pid == m.ospf_nhid) { still_present = true; break; }
        if (!still_present)
            to_remove.push_back(m.ospf_nhid);
    }

    std::vector<uint32_t> sra_to_delete;
    for (uint32_t old_ospf : to_remove)
    {
        g.members.erase(
            std::remove_if(g.members.begin(), g.members.end(),
                           [old_ospf, &sra_to_delete](const EcmpMember& m)
                           {
                               if (m.ospf_nhid == old_ospf)
                               {
                                   sra_to_delete.push_back(m.sra_nhid);
                                   return true;
                               }
                               return false;
                           }),
            g.members.end());
    }

    /* Add new members. */
    for (const auto& [pid, pw] : paths)
    {
        bool already = false;
        for (const auto& m : g.members)
            if (m.ospf_nhid == pid) { already = true; break; }
        if (already)
            continue;

        netlink_nexthop_t path_nh;
        if (netlink_nexthop_get_by_id(pid, &path_nh) < 0)
            continue;
        if (path_nh.gateway[0] == '\0' || path_nh.oif == 0)
            continue;

        const uint32_t sra_id = netlink_nexthop_single_create(
            path_nh.family, path_nh.gateway, path_nh.oif);
        if (sra_id == 0)
            continue;

        EcmpMember mem;
        mem.ospf_nhid = pid;
        mem.sra_nhid  = sra_id;
        mem.family    = path_nh.family;
        mem.gateway   = path_nh.gateway;
        mem.oif       = path_nh.oif;
        mem.oif_name  = path_nh.oif_name;
        mem.weight    = pw;
        g.members.push_back(std::move(mem));
    }

    /* Update the kernel ECMP group. */
    if (!g.members.empty())
    {
        std::vector<netlink_nexthop_grp_t> kgrp(g.members.size());
        toKernelGrp(g.members, kgrp.data());
        if (g.kernel_nhid == 0)
            g.kernel_nhid = netlink_nexthop_group_create(
                kgrp.data(), static_cast<uint32_t>(kgrp.size()));
        else
            netlink_nexthop_group_update(
                g.kernel_nhid,
                kgrp.data(),
                static_cast<uint32_t>(kgrp.size()));
    }
    else if (g.kernel_nhid != 0)
    {
        netlink_nexthop_delete(g.kernel_nhid);
        g.kernel_nhid = 0;
    }

    for (uint32_t sra : sra_to_delete)
        netlink_nexthop_delete(sra);

    return g.kernel_nhid;
}

std::string NexhopGroupManager::find_by_ospf_root(uint32_t nhid) const
{
    std::shared_lock lock(mutex_);
    for (const auto& [key, g] : groups_)
        if (g.ospf_root_nhid == nhid)
            return key;
    return {};
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
