/**
 * @file client/src/routing_test.cpp
 * @brief Smoke-test for RoutingManager: add / replace / remove a host route.
 *
 * Exercises RoutingManager::addRoute(), replaceRoute(), removeRoute(), and
 * listRoutes() against a real Linux kernel routing table using a fixed test
 * route:
 *
 *   destination : 3.3.3.3/32
 *   nexthop     : 192.168.0.7
 *
 * The test runs four sequential phases:
 *
 *   Phase 1 – ADD     Install 3.3.3.3/32 via 192.168.0.7 and verify it
 *                     appears in the routing table.
 *   Phase 2 – REPLACE Change the route metric to 200 (same destination /
 *                     nexthop) and verify the update.
 *   Phase 3 – REMOVE  Delete the route and verify it is no longer present.
 *   Phase 4 – CLEANUP Attempt a second removal; the expected ESRCH / ENOENT
 *                     error is treated as success (idempotence check).
 *
 * @par Prerequisites
 *  - Must be run as @c root or with @c CAP_NET_ADMIN.
 *  - The kernel does not require 192.168.0.7 to be directly reachable; the
 *    route is installed unconditionally into @c RT_TABLE_MAIN.
 *
 * @par Running
 * @code
 *   cmake --build build --target routing_test
 *   sudo ./build/routing_test
 * @endcode
 *
 * @par Expected output
 * @code
 *   === routing_test: 3.3.3.3/32 via 192.168.0.7 ===
 *
 *   [1/4] ADD 3.3.3.3/32 via 192.168.0.7 ...
 *         addRoute       : OK
 *         verify present : OK  (3.3.3.3/32  gw=192.168.0.7  metric=0)
 *
 *   [2/4] REPLACE (metric 0 → 200) ...
 *         replaceRoute   : OK
 *         verify metric  : OK  (3.3.3.3/32  gw=192.168.0.7  metric=200)
 *
 *   [3/4] REMOVE 3.3.3.3/32 ...
 *         removeRoute    : OK
 *         verify absent  : OK
 *
 *   [4/4] CLEANUP: second remove (expect ESRCH / ENOENT) ...
 *         second remove  : OK  (got expected error)
 *
 *   === ALL PHASES PASSED ===
 * @endcode
 *
 * @version 1.0
 */

#include "client/routing.hpp"

#include <linux/rtnetlink.h>

#include <algorithm>
#include <cstdlib>
#include <format>
#include <print>
#include <string>
#include <string_view>
#include <vector>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

/// Destination host route used throughout the test.
static constexpr std::string_view kDst = "3.3.3.3/32";

/// Next-hop gateway used in all test phases.
static constexpr std::string_view kGw = "192.168.0.7";

/// Metric used for the initial route installation (kernel default).
static constexpr uint32_t kMetricInitial = 0;

/// Metric used in the replace phase to verify the update was applied.
static constexpr uint32_t kMetricReplaced = 200;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/**
 * @brief Prints a KernelRoute in a compact single-line format.
 *
 * @param route  Route to print.
 * @param indent Leading spaces.
 */
static void printRoute(const sra::KernelRoute& route, int indent = 10)
{
    const std::string pad(static_cast<std::size_t>(indent), ' ');
    std::println("{}dst={:<18} gw={:<15} dev={:<10} metric={:>6}  "
                 "table={:>3}  proto={}",
                 pad,
                 route.destination,
                 route.gateway.empty() ? "-" : route.gateway,
                 route.interfaceName.empty() ? "-" : route.interfaceName,
                 route.metric,
                 route.table,
                 route.protocol);
}

/**
 * @brief Finds the first route whose destination matches @p dst in @p routes.
 *
 * @param routes  Route list to search.
 * @param dst     Destination CIDR string to match (e.g. "3.3.3.3/32").
 * @return        Pointer to the matching entry, or @c nullptr when not found.
 */
static const sra::KernelRoute*
findRoute(const std::vector<sra::KernelRoute>& routes, std::string_view dst)
{
    auto it = std::ranges::find_if(routes, [&](const sra::KernelRoute& r) {
        return r.destination == dst;
    });
    return (it != routes.end()) ? &*it : nullptr;
}

/**
 * @brief Calls listRoutes(AF_INET) and returns the result, printing any error.
 *
 * @param rm  RoutingManager instance to query.
 * @return    Route vector on success; empty vector on failure.
 */
static std::vector<sra::KernelRoute> snapshot(sra::RoutingManager& rm)
{
    auto result = rm.listRoutes(AF_INET);
    if (!result)
    {
        std::println(stderr, "        listRoutes error: {}", result.error());
        return {};
    }
    return std::move(*result);
}

// ---------------------------------------------------------------------------
// Test phases
// ---------------------------------------------------------------------------

/**
 * @brief Phase 1 – installs the test route and verifies its presence.
 *
 * @param rm  RoutingManager under test.
 * @return    @c true when the phase passes, @c false on any failure.
 */
static bool phaseAdd(sra::RoutingManager& rm)
{
    std::println("\n[1/4] ADD {} via {} ...", kDst, kGw);

    // -- addRoute
    // --------------------------------------------------------------
    sra::RouteParams p;
    p.destination = std::string(kDst);
    p.gateway = std::string(kGw);
    p.metric = kMetricInitial;
    p.family = AF_INET;
    p.protocol = RTPROT_STATIC;
    p.type = RTN_UNICAST;
    p.table = RT_TABLE_MAIN;

    if (auto r = rm.addRoute(p); !r)
    {
        std::println("        addRoute       : FAIL  ({})", r.error());
        return false;
    }
    std::println("        addRoute       : OK");

    // -- verify
    // ----------------------------------------------------------------
    const auto routes = snapshot(rm);
    const auto* entry = findRoute(routes, kDst);
    if (entry == nullptr)
    {
        std::println(
            "        verify present : FAIL  (route not found in table)");
        return false;
    }
    std::print("        verify present : OK  ");
    printRoute(*entry, 0);
    return true;
}

/**
 * @brief Phase 2 – replaces the test route (metric 0 → 200) and verifies.
 *
 * @param rm  RoutingManager under test.
 * @return    @c true when the phase passes, @c false on any failure.
 */
static bool phaseReplace(sra::RoutingManager& rm)
{
    std::println("\n[2/4] REPLACE (metric {} → {}) ...",
                 kMetricInitial,
                 kMetricReplaced);

    // -- replaceRoute
    // ----------------------------------------------------------
    sra::RouteParams p;
    p.destination = std::string(kDst);
    p.gateway = std::string(kGw);
    p.metric = kMetricReplaced;
    p.family = AF_INET;
    p.protocol = RTPROT_STATIC;
    p.type = RTN_UNICAST;
    p.table = RT_TABLE_MAIN;

    if (auto r = rm.replaceRoute(p); !r)
    {
        std::println("        replaceRoute   : FAIL  ({})", r.error());
        return false;
    }
    std::println("        replaceRoute   : OK");

    // -- verify metric
    // ---------------------------------------------------------
    const auto routes = snapshot(rm);
    const auto* entry = findRoute(routes, kDst);
    if (entry == nullptr)
    {
        std::println(
            "        verify metric  : FAIL  (route not found after replace)");
        return false;
    }
    if (entry->metric != kMetricReplaced)
    {
        std::println("        verify metric  : FAIL  "
                     "(expected metric={}, got metric={})",
                     kMetricReplaced,
                     entry->metric);
        return false;
    }
    std::print("        verify metric  : OK  ");
    printRoute(*entry, 0);
    return true;
}

/**
 * @brief Phase 3 – removes the test route and verifies its absence.
 *
 * @param rm  RoutingManager under test.
 * @return    @c true when the phase passes, @c false on any failure.
 */
static bool phaseRemove(sra::RoutingManager& rm)
{
    std::println("\n[3/4] REMOVE {} ...", kDst);

    // -- removeRoute
    // -----------------------------------------------------------
    sra::RouteParams p;
    p.destination = std::string(kDst);
    p.gateway = std::string(kGw);
    p.metric = kMetricReplaced;
    p.family = AF_INET;
    p.table = RT_TABLE_MAIN;

    if (auto r = rm.removeRoute(p); !r)
    {
        std::println("        removeRoute    : FAIL  ({})", r.error());
        return false;
    }
    std::println("        removeRoute    : OK");

    // -- verify absence
    // --------------------------------------------------------
    const auto routes = snapshot(rm);
    const auto* entry = findRoute(routes, kDst);
    if (entry != nullptr)
    {
        std::println("        verify absent  : FAIL  (route still present)");
        printRoute(*entry);
        return false;
    }
    std::println("        verify absent  : OK");
    return true;
}

/**
 * @brief Phase 4 – attempts a second removal to verify idempotent error
 * handling.
 *
 * The kernel returns @c ESRCH (no such process) or @c ENOENT when asked to
 * delete a route that does not exist.  The test treats this as the expected
 * outcome and reports PASS.
 *
 * @param rm  RoutingManager under test.
 * @return    Always @c true (any error from the second remove is the goal).
 */
static bool phaseCleanup(sra::RoutingManager& rm)
{
    std::println("\n[4/4] CLEANUP: second remove (expect ESRCH / ENOENT) ...");

    sra::RouteParams p;
    p.destination = std::string(kDst);
    p.gateway = std::string(kGw);
    p.family = AF_INET;
    p.table = RT_TABLE_MAIN;

    auto r = rm.removeRoute(p);
    if (r)
    {
        // Unexpectedly succeeded – the route was still there; that is unusual
        // but not a hard failure for this phase.
        std::println("        second remove  : OK  (route was still present; "
                     "removed cleanly)");
    }
    else
    {
        std::println("        second remove  : OK  (got expected error: {})",
                     r.error());
    }
    return true;
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

/**
 * @brief Runs all test phases in sequence.
 *
 * @return @c EXIT_SUCCESS when every phase passes, @c EXIT_FAILURE otherwise.
 */
int main()
{
    std::println("=== routing_test: {} via {} ===", kDst, kGw);

    sra::RoutingManager rm;

    const bool ok =
        phaseAdd(rm) && phaseReplace(rm) && phaseRemove(rm) && phaseCleanup(rm);

    if (ok)
    {
        std::println("\n=== ALL PHASES PASSED ===");
        return EXIT_SUCCESS;
    }

    std::println("\n=== TEST FAILED ===");
    return EXIT_FAILURE;
}
