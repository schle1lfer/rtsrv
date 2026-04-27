/**
 * @file client/src/vrf_table.cpp
 * @brief VrfTable — thread-safe local cache of GetAllRoutes data.
 */

#include "client/vrf_table.hpp"

#include <mutex>
#include <print>

namespace sra
{

void VrfTable::load(const srmd::v1::GetAllRoutesResponse& resp)
{
    std::unique_lock lock(mutex_);
    routes_.clear();
    byNexthop_.clear();
    routes_.reserve(static_cast<std::size_t>(resp.routes_size()));
    for (const auto& r : resp.routes())
    {
        const std::size_t idx = routes_.size();
        routes_.push_back(r);
        if (!r.nexthop().empty())
            byNexthop_[r.nexthop()].push_back(idx);
    }

    std::println("[VrfTable] loaded {} route(s) from GetAllRoutes response "
                 "({} unique nexthop(s))",
                 routes_.size(),
                 byNexthop_.size());

    for (const auto& r : routes_)
    {
        std::println("[VrfTable]   vrf='{}' iface='{}' type={} nexthop={} "
                     "prefixes={}",
                     r.vrf_name(),
                     r.interface_name(),
                     r.interface_type(),
                     r.nexthop().empty() ? "(none)" : r.nexthop(),
                     r.prefixes_size());
    }
}

void VrfTable::clear()
{
    std::unique_lock lock(mutex_);
    const std::size_t prev = routes_.size();
    routes_.clear();
    byNexthop_.clear();
    std::println("[VrfTable] cleared ({} entry/entries removed)", prev);
}

std::size_t VrfTable::size() const
{
    std::shared_lock lock(mutex_);
    return routes_.size();
}

bool VrfTable::empty() const
{
    std::shared_lock lock(mutex_);
    return routes_.empty();
}

bool VrfTable::hasNexthop(const std::string& gateway) const
{
    std::shared_lock lock(mutex_);
    return byNexthop_.count(gateway) > 0;
}

std::vector<srmd::v1::VrfRoute>
VrfTable::findByNexthop(const std::string& gateway) const
{
    std::shared_lock lock(mutex_);
    auto it = byNexthop_.find(gateway);
    if (it == byNexthop_.end())
        return {};
    std::vector<srmd::v1::VrfRoute> result;
    result.reserve(it->second.size());
    for (std::size_t idx : it->second)
        result.push_back(routes_[idx]);
    return result;
}

std::vector<std::string> VrfTable::nexthops() const
{
    std::shared_lock lock(mutex_);
    std::vector<std::string> result;
    result.reserve(byNexthop_.size());
    for (const auto& [gw, _] : byNexthop_)
        result.push_back(gw);
    return result;
}

std::vector<srmd::v1::VrfRoute> VrfTable::all() const
{
    std::shared_lock lock(mutex_);
    return routes_;
}

} // namespace sra
