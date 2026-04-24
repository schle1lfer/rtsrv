/**
 * @file client/include/client/udp_table_server.hpp
 * @brief UDP server that publishes NetLink table snapshots on fixed ports.
 *
 * Binds three UDP sockets and serves the most recent kernel table state to
 * any UDP client that queries a port.  Push notifications are sent to every
 * subscriber whenever the corresponding table is updated via setXxxData().
 *
 * Port assignments
 * ────────────────
 *  9001  – ARP/NDP neighbor table     (RTM_NEWNEIGH / RTM_DELNEIGH)
 *  9002  – Nexthop object table       (RTM_NEWNEXTHOP / RTM_DELNEXTHOP)
 *  9003  – IPv4 /32 routing table     (RTM_NEWROUTE / RTM_DELROUTE)
 *
 * Protocol (text, newline-delimited)
 * ───────────────────────────────────
 *  • Any incoming datagram → server replies immediately with the full table
 *    snapshot (request / response).
 *  • A datagram whose first 9 bytes are "SUBSCRIBE" additionally registers
 *    the sender's address for push updates.
 *  • After every setXxxData() call the new snapshot is automatically pushed
 *    to all registered subscribers on that port.
 *
 * Typical usage
 * ─────────────
 * @code
 *   sra::UdpTableServer udp;
 *
 *   udp.setNeighborData("NEIGHBORS 0\n");   // start with empty table
 *   udp.setNexthopData ("NEXTHOPS 0\n");
 *   udp.setRouteData   ("ROUTES 0\n");
 *
 *   udp.start();                            // opens sockets, starts threads
 *
 *   // … Netlink callback fires …
 *   udp.setNeighborData(serializeNeighborTable(ctx.neighbors));
 *   // → instantly pushed to all subscribers on port 9001
 *
 *   udp.stop();
 * @endcode
 *
 * The server is fully thread-safe: setXxxData() and stop() may be called from
 * any thread.
 */

#pragma once

#include <cstdint>
#include <mutex>
#include <netinet/in.h>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

namespace sra {

/** UDP port for the ARP/NDP neighbor table. */
inline constexpr uint16_t UDP_PORT_NEIGHBORS = 9001;

/** UDP port for the nexthop object table. */
inline constexpr uint16_t UDP_PORT_NEXTHOPS  = 9002;

/** UDP port for the IPv4 /32 routing table. */
inline constexpr uint16_t UDP_PORT_ROUTES    = 9003;

/**
 * @brief UDP server that exposes three kernel table snapshots on fixed ports.
 *
 * One lightweight listener thread runs per port.  The server is intentionally
 * connectionless: any UDP client can query a port at any time and receives an
 * up-to-date text snapshot in the reply datagram.
 */
class UdpTableServer
{
public:
    /**
     * @brief Constructs the server with the three well-known port numbers.
     *
     * @param portNeighbors Port for the ARP/NDP neighbor table (default 9001).
     * @param portNexthops  Port for the nexthop object table   (default 9002).
     * @param portRoutes    Port for the IPv4 /32 routing table (default 9003).
     */
    explicit UdpTableServer(uint16_t portNeighbors = UDP_PORT_NEIGHBORS,
                            uint16_t portNexthops  = UDP_PORT_NEXTHOPS,
                            uint16_t portRoutes    = UDP_PORT_ROUTES);

    ~UdpTableServer();

    UdpTableServer(const UdpTableServer&)            = delete;
    UdpTableServer& operator=(const UdpTableServer&) = delete;

    // ── Lifecycle ──────────────────────────────────────────────────────────

    /**
     * @brief Bind sockets and start one listener thread per port.
     *
     * Must be called once before any setXxxData() call.
     * @return true on success, false if any socket could not be opened.
     */
    bool start();

    /**
     * @brief Signal all listener threads to stop and join them.
     *
     * Safe to call even if start() was never invoked or already stopped.
     */
    void stop();

    // ── Table data setters (thread-safe) ───────────────────────────────────

    /**
     * @brief Replace the neighbor table snapshot and push it to subscribers.
     *
     * @param data Serialized neighbor table (text, newline-delimited).
     */
    void setNeighborData(std::string data);

    /**
     * @brief Replace the nexthop table snapshot and push it to subscribers.
     *
     * @param data Serialized nexthop table (text, newline-delimited).
     */
    void setNexthopData(std::string data);

    /**
     * @brief Replace the routing table snapshot and push it to subscribers.
     *
     * @param data Serialized routing table (text, newline-delimited).
     */
    void setRouteData(std::string data);

private:
    // ── Internal per-port state ────────────────────────────────────────────

    struct PortState
    {
        uint16_t port{0};
        int      fd{-1};
        int      stopPipe[2]{-1, -1};   ///< pipe[0]=read, pipe[1]=write

        mutable std::shared_mutex dataMutex;
        std::string               data;  ///< latest serialized snapshot

        std::mutex                subMutex;
        std::vector<sockaddr_in>  subscribers; ///< push-update recipients

        std::thread thread;
    };

    // ── Helpers ────────────────────────────────────────────────────────────

    /** Open and bind a UDP socket; returns fd or -1 on error. */
    static int openSocket(uint16_t port);

    /** Main loop executed by each listener thread. */
    void runListener(PortState& ps);

    /**
     * @brief Send @p payload to all registered subscribers of @p ps.
     *
     * Called while NOT holding ps.dataMutex (snapshot is passed in directly).
     */
    static void pushToSubscribers(PortState& ps, const std::string& payload);

    /** Shared implementation for setXxxData(). */
    void setData(PortState& ps, std::string data);

    // ── Per-port state objects ─────────────────────────────────────────────

    PortState neighbors_;
    PortState nexthops_;
    PortState routes_;

    bool running_{false};
};

} // namespace sra
