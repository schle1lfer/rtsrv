/**
 * @file server/src/route_manager.cpp
 * @brief In-memory route table implementation.
 *
 * @version 1.0
 */

#include "server/route_manager.hpp"

#include <boost/log/trivial.hpp>
#include <chrono>
#include <format>
#include <fstream>
#include <ranges>
#include <mutex>

namespace srmd
{

// ---------------------------------------------------------------------------
// Private static helpers
// ---------------------------------------------------------------------------

std::string RouteManager::generateId()
{
    // Prefer the kernel's UUID generator when available.
    std::ifstream uuidFile("/proc/sys/kernel/random/uuid");
    if (uuidFile.is_open())
    {
        std::string uuid;
        std::getline(uuidFile, uuid);
        if (!uuid.empty())
        {
            return uuid;
        }
    }
    // Fallback: timestamp-based pseudo-UUID.
    const auto ns = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::format("srmd-{:016x}-route", static_cast<uint64_t>(ns));
}

int64_t RouteManager::nowUs() noexcept
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// ---------------------------------------------------------------------------
// CRUD operations
// ---------------------------------------------------------------------------

std::expected<srmd::v1::Route, std::string>
RouteManager::addRoute(const srmd::v1::AddRouteRequest& req)
{
    if (req.destination().empty())
    {
        return std::unexpected("destination must not be empty");
    }

    const std::string id = generateId();
    const int64_t now = nowUs();

    srmd::v1::Route route;
    route.set_id(id);
    route.set_address_family(req.address_family());
    route.set_destination(req.destination());
    route.set_gateway(req.gateway());
    route.set_interface_name(req.interface_name());
    route.set_metric(req.metric());
    route.set_protocol(req.protocol());
    route.set_active(true);
    route.set_created_at_us(now);
    route.set_updated_at_us(now);
    for (const auto& [k, v] : req.tags())
    {
        (*route.mutable_tags())[k] = v;
    }

    {
        std::unique_lock lock(routesMutex_);
        routes_.emplace(id, route);
    }

    BOOST_LOG_TRIVIAL(info) << std::format(
        "[RouteManager] AddRoute id={} dest={} gw={} iface={} metric={}",
        id,
        req.destination(),
        req.gateway(),
        req.interface_name(),
        req.metric());

    // Notify observers (outside the write lock)
    srmd::v1::RouteEvent ev;
    ev.set_type(srmd::v1::ROUTE_EVENT_ADDED);
    *ev.mutable_route() = route;
    ev.set_event_ts_us(now);
    notifyObservers(ev);

    return route;
}

std::expected<void, std::string>
RouteManager::removeRoute(const std::string& id)
{
    srmd::v1::Route removed;
    {
        std::unique_lock lock(routesMutex_);
        auto it = routes_.find(id);
        if (it == routes_.end())
        {
            return std::unexpected(std::format("Route '{}' not found", id));
        }
        removed = it->second;
        routes_.erase(it);
    }

    BOOST_LOG_TRIVIAL(info) << std::format(
        "[RouteManager] RemoveRoute id={} dest={}", id, removed.destination());

    srmd::v1::RouteEvent ev;
    ev.set_type(srmd::v1::ROUTE_EVENT_REMOVED);
    *ev.mutable_route() = removed;
    ev.set_event_ts_us(nowUs());
    notifyObservers(ev);

    return {};
}

std::expected<srmd::v1::Route, std::string>
RouteManager::getRoute(const std::string& id) const
{
    std::shared_lock lock(routesMutex_);
    const auto it = routes_.find(id);
    if (it == routes_.end())
    {
        return std::unexpected(std::format("Route '{}' not found", id));
    }
    return it->second;
}

std::vector<srmd::v1::Route>
RouteManager::listRoutes(const srmd::v1::ListRoutesRequest& req) const
{
    std::vector<srmd::v1::Route> result;

    std::shared_lock lock(routesMutex_);
    result.reserve(routes_.size());

    for (const auto& [id, route] : routes_)
    {
        // Filter by address family
        if (req.address_family() != srmd::v1::ADDRESS_FAMILY_UNSPECIFIED &&
            route.address_family() != req.address_family())
        {
            continue;
        }
        // Filter by protocol
        if (req.protocol() != srmd::v1::ROUTE_PROTOCOL_UNSPECIFIED &&
            route.protocol() != req.protocol())
        {
            continue;
        }
        // Filter by active-only
        if (req.active_only() && !route.active())
        {
            continue;
        }
        result.push_back(route);
    }

    // Sort by metric then by ID for deterministic output
    std::ranges::sort(result, [](const auto& a, const auto& b) {
        if (a.metric() != b.metric())
        {
            return a.metric() < b.metric();
        }
        return a.id() < b.id();
    });

    return result;
}

// ---------------------------------------------------------------------------
// Observer management
// ---------------------------------------------------------------------------

int RouteManager::registerObserver(RouteObserver observer)
{
    std::lock_guard lock(observersMutex_);
    const int handle = nextHandle_++;
    observers_.emplace(handle, std::move(observer));
    return handle;
}

void RouteManager::unregisterObserver(int handle)
{
    std::lock_guard lock(observersMutex_);
    observers_.erase(handle);
}

void RouteManager::notifyObservers(const srmd::v1::RouteEvent& event)
{
    // Snapshot the map under the lock, then call outside to avoid deadlocks.
    std::vector<RouteObserver> snapshot;
    {
        std::lock_guard lock(observersMutex_);
        snapshot.reserve(observers_.size());
        for (const auto& [handle, obs] : observers_)
        {
            snapshot.push_back(obs);
        }
    }
    for (const auto& obs : snapshot)
    {
        obs(event);
    }
}

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

std::size_t RouteManager::size() const noexcept
{
    std::shared_lock lock(routesMutex_);
    return routes_.size();
}

} // namespace srmd
