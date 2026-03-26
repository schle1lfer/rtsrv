/**
 * @file server/include/server/route_manager.hpp
 * @brief In-memory route table for the Switch Route Manager daemon.
 *
 * RouteManager is the core data structure of srmd.  It stores all routing
 * table entries in a hash map keyed by server-assigned UUID, provides
 * thread-safe CRUD operations, and supports observer callbacks for the
 * WatchRoutes streaming RPC.
 *
 * All methods that modify state emit a RouteEvent to every registered
 * observer so that active WatchRoutes streams are notified immediately.
 *
 * @version 1.0
 */

#pragma once

#include "srmd.pb.h"

#include <expected>
#include <functional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace srmd
{

/**
 * @brief Callback type invoked whenever a route is added, updated, or removed.
 *
 * The observer is called with the assembled RouteEvent protobuf message.
 * Observers must not call back into RouteManager while the callback is
 * executing (would deadlock).
 */
using RouteObserver = std::function<void(const srmd::v1::RouteEvent&)>;

/**
 * @brief Thread-safe in-memory routing table.
 *
 * Internally uses a shared_mutex so concurrent reads (ListRoutes / GetRoute)
 * can proceed in parallel while writes (AddRoute / RemoveRoute) are exclusive.
 */
class RouteManager
{
public:
    RouteManager() = default;

    RouteManager(const RouteManager&) = delete;
    RouteManager& operator=(const RouteManager&) = delete;
    RouteManager(RouteManager&&) = delete;
    RouteManager& operator=(RouteManager&&) = delete;

    // -----------------------------------------------------------------------
    // CRUD
    // -----------------------------------------------------------------------

    /**
     * @brief Installs a new route from the supplied AddRouteRequest.
     *
     * Generates a UUID, sets created_at_us, and inserts the entry.
     *
     * @param req  Validated AddRouteRequest protobuf message.
     * @return The newly created Route on success, or an error string.
     */
    [[nodiscard]] std::expected<srmd::v1::Route, std::string>
    addRoute(const srmd::v1::AddRouteRequest& req);

    /**
     * @brief Removes the route with the given server-assigned ID.
     *
     * @param id  Route ID returned by addRoute().
     * @return void on success, or an error string when not found.
     */
    [[nodiscard]] std::expected<void, std::string>
    removeRoute(const std::string& id);

    /**
     * @brief Retrieves a single route by ID.
     *
     * @param id  Route ID to look up.
     * @return A copy of the Route on success, or an error string.
     */
    [[nodiscard]] std::expected<srmd::v1::Route, std::string>
    getRoute(const std::string& id) const;

    /**
     * @brief Returns a snapshot of all routes, optionally filtered.
     *
     * @param req  ListRoutesRequest carrying optional family/protocol filters.
     * @return Vector of matching Route copies.
     */
    [[nodiscard]] std::vector<srmd::v1::Route>
    listRoutes(const srmd::v1::ListRoutesRequest& req) const;

    // -----------------------------------------------------------------------
    // Observer registration
    // -----------------------------------------------------------------------

    /**
     * @brief Registers an observer that will be notified of route changes.
     *
     * @param observer  Callable accepting a const RouteEvent reference.
     * @return Opaque integer handle; pass to unregisterObserver() to remove.
     */
    int registerObserver(RouteObserver observer);

    /**
     * @brief Removes the observer with the given handle.
     *
     * Safe to call from inside the observer callback (removal is deferred
     * until after the current notification cycle completes).
     *
     * @param handle  Value returned by registerObserver().
     */
    void unregisterObserver(int handle);

    // -----------------------------------------------------------------------
    // Diagnostics
    // -----------------------------------------------------------------------

    /** @brief Returns the total number of routes currently stored. */
    [[nodiscard]] std::size_t size() const noexcept;

private:
    /**
     * @brief Notifies all registered observers with a RouteEvent.
     *
     * Must be called while *not* holding routesMutex_ in exclusive mode
     * (observers may call back into read-only RouteManager methods).
     *
     * @param event  Fully populated RouteEvent to broadcast.
     */
    void notifyObservers(const srmd::v1::RouteEvent& event);

    /**
     * @brief Generates a UUID v4 string for a new route entry.
     * @return 36-character UUID string.
     */
    static std::string generateId();

    /**
     * @brief Returns the current Unix epoch time in microseconds.
     */
    static int64_t nowUs() noexcept;

    // -----------------------------------------------------------------------
    // Data members
    // -----------------------------------------------------------------------

    mutable std::shared_mutex routesMutex_; ///< Guards routes_.
    std::unordered_map<std::string, srmd::v1::Route>
        routes_; ///< Route table keyed by ID.

    mutable std::mutex observersMutex_; ///< Guards observers_.
    int nextHandle_{0};                 ///< Counter for observer handles.
    std::unordered_map<int, RouteObserver>
        observers_; ///< Active route-change observers.
};

} // namespace srmd
