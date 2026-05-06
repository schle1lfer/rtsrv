/**
 * @file server/include/server/netlink.h
 * @brief Netlink listener for IPv4 /32 route changes from FRR zebra.
 *
 * This module opens a NETLINK_ROUTE socket subscribed to the
 * RTMGRP_IPV4_ROUTE multicast group and delivers RTM_NEWROUTE /
 * RTM_DELROUTE events whose prefix length equals 32 to a caller-supplied
 * callback.  All other messages (wrong family, wrong prefix length,
 * non-unicast types, etc.) are silently discarded.
 *
 * The API is pure C so it can be used from both C and C++ translation units.
 * C++ callers must include this header; the extern "C" block ensures the
 * symbols are linked without name-mangling.
 *
 * Typical usage (C++ server thread):
 * @code
 *   int fd = netlink_init();
 *   if (fd < 0) { perror("netlink_init"); return; }
 *
 *   netlink_run(fd, [](netlink_event_t ev, const netlink_route32_t *r, void *)
 * { const char *label = (ev == NETLINK_ROUTE_ADDED)   ? "ADD" : (ev ==
 * NETLINK_ROUTE_CHANGED) ? "CHG" : "DEL"; printf("%s %s via %s dev %s metric
 * %u\n", label, inet_ntoa(r->dst), inet_ntoa(r->gateway), r->ifname,
 * r->metric);
 *   }, nullptr);
 *
 *   netlink_close(fd);
 * @endcode
 *
 * @version 1.0
 */

#pragma once

#ifdef __cplusplus
extern "C"
{
#endif

#include <net/if.h>     /* IF_NAMESIZE */
#include <netinet/in.h> /* struct in_addr */
#include <stdint.h>

    /* ---------------------------------------------------------------------------
     * Event type
     * -------------------------------------------------------------------------
     */

    /**
     * @brief Indicates whether a /32 route was installed, modified, or
     * withdrawn.
     *
     * Derived from the netlink message type and flags:
     *
     * - NETLINK_ROUTE_ADDED   — RTM_NEWROUTE without NLM_F_REPLACE.
     *                           Kernel received a brand-new route that did not
     *                           exist before (e.g. "ip route add ...").
     *
     * - NETLINK_ROUTE_CHANGED — RTM_NEWROUTE with NLM_F_REPLACE set.
     *                           An existing route was replaced in-place; the
     * new route descriptor carries the updated attributes (new gateway, new
     * metric, etc.). Triggered by "ip route replace/change ...". FRR zebra also
     * uses this form when it installs a replacement nexthop for a previously
     * announced prefix.
     *
     * - NETLINK_ROUTE_REMOVED — RTM_DELROUTE.
     *                           Route was withdrawn (e.g. "ip route del ...").
     */
    typedef enum
    {
        NETLINK_ROUTE_ADDED = 0,   /**< Route was newly installed. */
        NETLINK_ROUTE_REMOVED = 1, /**< Route was removed. */
        NETLINK_ROUTE_CHANGED =
            2, /**< Route attributes were updated in-place. */
    } netlink_event_t;

    /* ---------------------------------------------------------------------------
     * Route descriptor
     * -------------------------------------------------------------------------
     */

    /**
     * @brief Describes a single IPv4 /32 route event received from the kernel.
     *
     * Fields map directly to the struct rtmsg header members and the RTA_*
     * rtnetlink attributes extracted from the message.  Optional attributes
     * that were absent in the message are left zero-initialised.
     */
    typedef struct
    {
        /* ── From struct rtmsg ───────────────────────────────────────────────
         */
        uint8_t family;   /**< rtm_family: AF_INET (always 2 here). */
        uint8_t dst_len;  /**< rtm_dst_len: prefix length (always 32). */
        uint8_t tos;      /**< rtm_tos: type-of-service filter. */
        uint8_t scope;    /**< rtm_scope: RT_SCOPE_* (link/global/…). */
        uint8_t type;     /**< rtm_type: RTN_UNICAST (always here). */
        uint32_t flags;   /**< rtm_flags: RTM_F_* bitmask. */
        uint8_t protocol; /**< rtm_protocol: RTPROT_ZEBRA, RTPROT_OSPF, … */
        uint32_t table;   /**< rtm_table / RTA_TABLE override. */

        /* ── RTA_* attributes ────────────────────────────────────────────────
         */
        struct in_addr dst;     /**< RTA_DST: destination host address (/32). */
        struct in_addr gateway; /**< RTA_GATEWAY: next-hop (0.0.0.0 if none). */
        uint32_t ifindex; /**< RTA_OIF: output interface index (0 if absent). */
        char ifname[IF_NAMESIZE]; /**< Resolved interface name, NUL-terminated.
                                   */
        uint32_t metric; /**< RTA_PRIORITY: route metric / preference. */
        uint32_t nhid;   /**< RTA_NH_ID: nexthop object ID (0 if absent). */
    } netlink_route32_t;

    /* ---------------------------------------------------------------------------
     * Callback type
     * -------------------------------------------------------------------------
     */

    /**
     * @brief User callback invoked once per qualifying route event.
     *
     * The callback must not call netlink_close() on the fd that triggered it.
     * Blocking inside the callback stalls event processing.
     *
     * @param event      Whether the route was added or removed.
     * @param route      Pointer to the route descriptor; valid only for the
     *                   duration of this call.
     * @param user_data  Opaque pointer forwarded from netlink_process() /
     *                   netlink_run().
     */
    typedef void (*netlink_route_cb_t)(netlink_event_t event,
                                       const netlink_route32_t* route,
                                       void* user_data);

    /* ---------------------------------------------------------------------------
     * API
     * -------------------------------------------------------------------------
     */

    /**
     * @brief Opens and binds a NETLINK_ROUTE socket for IPv4 route multicast.
     *
     * Creates a SOCK_RAW|SOCK_CLOEXEC socket in the AF_NETLINK family and
     * subscribes it to the RTMGRP_IPV4_ROUTE group so the kernel delivers all
     * IPv4 routing table changes.
     *
     * @return A non-negative file descriptor on success, or -1 on error with
     *         errno set by socket(2) or bind(2).
     */
    int netlink_init(void);

    /**
     * @brief Drains all pending netlink messages without blocking.
     *
     * Reads as many messages as are currently available on @p fd using
     * MSG_DONTWAIT.  For each RTM_NEWROUTE or RTM_DELROUTE message whose
     * rtm_family is AF_INET and rtm_dst_len is 32, @p cb is invoked exactly
     * once.
     *
     * @param fd        File descriptor returned by netlink_init().
     * @param cb        Callback to invoke for each qualifying route event.
     * @param user_data Forwarded unchanged to @p cb.
     *
     * @return The number of qualifying route events dispatched (>= 0), or -1 on
     *         a fatal recv(2) error with errno set.
     */
    int netlink_process(int fd, netlink_route_cb_t cb, void* user_data);

    /**
     * @brief Blocking event loop — runs until the socket is closed or errors
     * out.
     *
     * Calls recv(2) in a loop (restarting on EINTR) and dispatches qualifying
     * route events to @p cb.  Returns only when recv returns 0 (peer closed) or
     * a non-EINTR error occurs.
     *
     * To stop the loop from another thread, close or shut down @p fd; recv will
     * then return an error and netlink_run() will return -1.
     *
     * @param fd        File descriptor returned by netlink_init().
     * @param cb        Callback to invoke for each qualifying route event.
     * @param user_data Forwarded unchanged to @p cb.
     *
     * @return 0 when the socket is cleanly closed, -1 on recv(2) error with
     *         errno set.
     */
    int netlink_run(int fd, netlink_route_cb_t cb, void* user_data);

    /**
     * @brief Closes the netlink socket.
     *
     * Wraps close(2).  Safe to call on fd values returned by netlink_init();
     * passing -1 is a no-op.
     *
     * @param fd  File descriptor to close.
     */
    void netlink_close(int fd);

#ifdef __cplusplus
} /* extern "C" */
#endif
