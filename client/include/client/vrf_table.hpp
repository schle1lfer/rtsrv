/**
 * @file client/include/client/vrf_table.hpp
 * @brief Thread-safe local cache of VRF route data received via GetAllRoutes.
 *
 * VrfTable stores every VrfRoute entry from a GetAllRoutesResponse and
 * maintains a secondary index keyed by nexthop gateway address so that
 * callers can quickly look up which VRF entries are associated with a
 * particular kernel nexthop.
 *
 * Typical usage:
 * @code
 *   sra::VrfTable table;
 *   table.load(getAllRoutesResponse);
 *
 *   // On a nexthop ADDED / CHANGED / REMOVED event:
 *   if (table.hasNexthop(nh.gateway)) {
 *       auto routes = table.findByNexthop(nh.gateway);
 *       // build and submit VrfsRouteRequest ...
 *   }
 * @endcode
 *
 * @version 1.0
 */

#pragma once

#include "srmd.pb.h"

#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace sra
{

/**
 * @brief Thread-safe local store of VRF route data from GetAllRoutes.
 *
 * All public methods are safe to call from multiple threads concurrently.
 * Reads (hasNexthop, findByNexthop, all, size, empty) take a shared lock;
 * writes (load, clear) take an exclusive lock.
 */
class VrfTable
{
public:
    VrfTable() = default;
    ~VrfTable() = default;

    VrfTable(const VrfTable&) = delete;
    VrfTable& operator=(const VrfTable&) = delete;
    VrfTable(VrfTable&&) = delete;
    VrfTable& operator=(VrfTable&&) = delete;

    // -----------------------------------------------------------------------
    // Write operations
    // -----------------------------------------------------------------------

    /**
     * @brief Replaces the table contents with the routes from @p resp.
     *
     * Clears any previous data and re-builds the nexthop index.
     * Only routes that carry a non-empty nexthop field are indexed.
     *
     * @param resp  Completed GetAllRoutesResponse from srmd.
     */
    void load(const srmd::v1::GetAllRoutesResponse& resp);

    /**
     * @brief Removes all stored entries and clears the nexthop index.
     */
    void clear();

    // -----------------------------------------------------------------------
    // Read operations
    // -----------------------------------------------------------------------

    /**
     * @brief Returns the total number of stored VrfRoute entries.
     */
    [[nodiscard]] std::size_t size() const;

    /**
     * @brief Returns @c true when the table holds no entries.
     */
    [[nodiscard]] bool empty() const;

    /**
     * @brief Returns @c true when at least one VrfRoute has @p gateway as
     *        its nexthop field.
     *
     * @param gateway  IPv4 address string (e.g. "192.168.1.1").
     */
    [[nodiscard]] bool hasNexthop(const std::string& gateway) const;

    /**
     * @brief Returns all VrfRoute entries whose nexthop == @p gateway.
     *
     * Returns an empty vector when no match is found.
     *
     * @param gateway  IPv4 address string to look up.
     */
    [[nodiscard]] std::vector<srmd::v1::VrfRoute>
    findByNexthop(const std::string& gateway) const;

    /**
     * @brief Returns all unique nexthop gateway addresses present in the table.
     */
    [[nodiscard]] std::vector<std::string> nexthops() const;

    /**
     * @brief Returns a snapshot of every stored VrfRoute entry.
     */
    [[nodiscard]] std::vector<srmd::v1::VrfRoute> all() const;

private:
    mutable std::shared_mutex mutex_;

    std::vector<srmd::v1::VrfRoute> routes_;

    // gateway string → indices into routes_
    std::unordered_map<std::string, std::vector<std::size_t>> byNexthop_;
};

} // namespace sra
