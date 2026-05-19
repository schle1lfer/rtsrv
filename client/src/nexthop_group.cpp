/**
 * @file client/src/nexthop_group.cpp
 * @brief Implementation of EcmpGroup and NexhopGroupManager.
 *
 * All public NexhopGroupManager methods are thread-safe.  Read-only queries
 * take a std::shared_lock; mutating operations take a std::unique_lock on the
 * internal shared_mutex.
 *
 * @see client/include/client/nexthop_group.hpp for API documentation.
 */

#include "client/nexthop_group.hpp"

#include <algorithm>
#include <mutex>
#include <shared_mutex>

namespace sra
{

// ---------------------------------------------------------------------------
// EcmpGroup
// ---------------------------------------------------------------------------

uint32_t EcmpGroup::effective_nhid() const noexcept
{
    if (members.empty())
        return 0;
    /* Return the most recently added member — in the ECMP case the kernel
     * assigns a composite group NHA_ID that is itself the nhid carried by
     * the /32 route event, so the last-seen value is always the right one. */
    return members.back().nhid;
}

// ---------------------------------------------------------------------------
// NexhopGroupManager — lifecycle
// ---------------------------------------------------------------------------

void NexhopGroupManager::create(const std::string& loopback_ipv4,
                                 const std::string& hostname)
{
    std::unique_lock lock(mutex_);
    if (groups_.contains(loopback_ipv4))
        return; /* group already exists — do not overwrite */

    EcmpGroup g;
    g.loopback_ipv4 = loopback_ipv4;
    g.hostname = hostname;
    groups_.emplace(loopback_ipv4, std::move(g));
}

bool NexhopGroupManager::remove(const std::string& loopback_ipv4)
{
    std::unique_lock lock(mutex_);
    return groups_.erase(loopback_ipv4) > 0;
}

void NexhopGroupManager::clear()
{
    std::unique_lock lock(mutex_);
    groups_.clear();
}

// ---------------------------------------------------------------------------
// NexhopGroupManager — member management
// ---------------------------------------------------------------------------

void NexhopGroupManager::add_member(const std::string& loopback_ipv4,
                                     uint32_t nhid,
                                     uint8_t weight)
{
    if (nhid == 0)
        return;

    std::unique_lock lock(mutex_);
    auto& g = groups_[loopback_ipv4]; /* inserts a default EcmpGroup if absent */
    if (g.loopback_ipv4.empty())
        g.loopback_ipv4 = loopback_ipv4;

    /* Update weight if nhid is already a member; avoid duplicate entries. */
    for (auto& m : g.members)
    {
        if (m.nhid == nhid)
        {
            m.weight = weight;
            return;
        }
    }
    g.members.push_back({nhid, weight});
}

bool NexhopGroupManager::remove_member(const std::string& loopback_ipv4,
                                        uint32_t nhid)
{
    std::unique_lock lock(mutex_);
    const auto it = groups_.find(loopback_ipv4);
    if (it == groups_.end())
        return false;

    auto& members = it->second.members;
    const auto before = members.size();
    members.erase(
        std::remove_if(members.begin(),
                       members.end(),
                       [nhid](const EcmpMember& m) { return m.nhid == nhid; }),
        members.end());
    return members.size() < before;
}

void NexhopGroupManager::update_member(const std::string& loopback_ipv4,
                                        uint32_t nhid,
                                        uint8_t weight)
{
    if (nhid == 0)
        return;

    std::unique_lock lock(mutex_);
    auto& g = groups_[loopback_ipv4]; /* inserts if absent */
    if (g.loopback_ipv4.empty())
        g.loopback_ipv4 = loopback_ipv4;

    g.members.clear();
    g.members.push_back({nhid, weight});
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
NexhopGroupManager::effective_nhid(const std::string& loopback_ipv4) const
{
    std::shared_lock lock(mutex_);
    const auto it = groups_.find(loopback_ipv4);
    if (it == groups_.end())
        return 0;
    return it->second.effective_nhid();
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
