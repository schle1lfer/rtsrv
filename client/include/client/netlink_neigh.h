/**
 * @file client/include/client/netlink_neigh.h
 * @brief Netlink listener for neighbor (ARP/NDP) table changes.
 *
 * Opens a NETLINK_ROUTE socket subscribed to the RTMGRP_NEIGH multicast
 * group and delivers RTM_NEWNEIGH / RTM_DELNEIGH events to a caller-supplied
 * callback with all fields from the kernel ndmsg header and every recognised
 * NDA_* attribute.
 *
 * An initial dump of the kernel neighbor table can be requested via
 * netlink_neigh_dump(), which sends RTM_GETNEIGH with NLM_F_DUMP and fires
 * the callback once per entry (all with event = NETLINK_NEIGH_ADDED).
 *
 * Typical usage:
 * @code
 *   int fd = netlink_neigh_init();
 *   netlink_neigh_dump(fd, my_cb, ctx);   // populate from existing table
 *   netlink_neigh_run(fd,  my_cb, ctx);   // block until socket closed
 *   netlink_neigh_close(fd);
 * @endcode
 *
 * The API is pure C so it can be called from C++ translation units.
 */

#pragma once

#ifdef __cplusplus
extern "C"
{
#endif

#include <net/if.h>      /* IF_NAMESIZE */
#include <netinet/in.h>  /* INET6_ADDRSTRLEN */
#include <stdint.h>

/* ---------------------------------------------------------------------------
 * Event type
 * ------------------------------------------------------------------------- */

/**
 * @brief Whether a neighbor entry was created, deleted, or updated.
 *
 * - NETLINK_NEIGH_ADDED   – RTM_NEWNEIGH without NLM_F_REPLACE, or a dump
 *                           response (RTM_GETNEIGH reply).
 * - NETLINK_NEIGH_REMOVED – RTM_DELNEIGH.
 * - NETLINK_NEIGH_CHANGED – RTM_NEWNEIGH with NLM_F_REPLACE set.
 */
typedef enum
{
    NETLINK_NEIGH_ADDED   = 0,
    NETLINK_NEIGH_REMOVED = 1,
    NETLINK_NEIGH_CHANGED = 2,
} netlink_neigh_event_t;

/* ---------------------------------------------------------------------------
 * Neighbor descriptor – carries all fields from ndmsg + NDA_* attributes
 * ------------------------------------------------------------------------- */

/** Maximum byte length stored for a link-layer address (Ethernet = 6). */
#define NETLINK_NEIGH_LLADDR_MAX 20

/**
 * @brief Describes a single neighbor cache entry received from the kernel.
 *
 * Fields map directly to the corresponding ndmsg header members and NDA_*
 * rtnetlink attributes.  Optional attributes that were absent in the message
 * are left zero-initialised.
 */
typedef struct
{
    /* ── From struct ndmsg ─────────────────────────────────────────────── */
    uint8_t  family;                    /**< ndm_family: AF_INET/AF_INET6/AF_BRIDGE */
    int      ifindex;                   /**< ndm_ifindex: outgoing interface index  */
    char     ifname[IF_NAMESIZE];       /**< Resolved interface name (if_indextoname) */
    uint16_t state;                     /**< ndm_state: NUD_* bitmask */
    uint8_t  flags;                     /**< ndm_flags: NTF_* bitmask */
    uint8_t  type;                      /**< ndm_type: RTN_* */

    /* ── NDA_DST ───────────────────────────────────────────────────────── */
    char     dst[INET6_ADDRSTRLEN];     /**< Destination IP string ("" if absent) */

    /* ── NDA_LLADDR ────────────────────────────────────────────────────── */
    uint8_t  lladdr[NETLINK_NEIGH_LLADDR_MAX]; /**< Raw link-layer address bytes  */
    uint8_t  lladdr_len;                /**< Number of valid bytes in lladdr[]    */

    /* ── NDA_CACHEINFO ─────────────────────────────────────────────────── */
    uint32_t confirmed_ms;              /**< Milliseconds since last confirmation  */
    uint32_t used_ms;                   /**< Milliseconds since last use           */
    uint32_t updated_ms;                /**< Milliseconds since last update        */
    uint32_t refcnt;                    /**< Reference count                       */

    /* ── NDA_PROBES ────────────────────────────────────────────────────── */
    uint32_t probes;                    /**< Number of ARP/NDP probes sent         */

    /* ── NDA_VLAN ──────────────────────────────────────────────────────── */
    uint16_t vlan;                      /**< VLAN ID (bridge entries; 0 = absent)  */

    /* ── NDA_MASTER ────────────────────────────────────────────────────── */
    uint32_t master;                    /**< Master interface index (0 = absent)   */
    char     master_name[IF_NAMESIZE];  /**< Resolved master interface name        */

    /* ── NDA_IFINDEX ───────────────────────────────────────────────────── */
    uint32_t nh_ifindex;                /**< Nexthop interface override (0 = absent) */

    /* ── NDA_PROTOCOL (kernel 5.2+) ────────────────────────────────────── */
    uint8_t  protocol;                  /**< Routing protocol that installed entry */
} netlink_neigh_t;

/* ---------------------------------------------------------------------------
 * Callback type
 * ------------------------------------------------------------------------- */

/**
 * @brief User callback invoked once per neighbor event.
 *
 * @param event      Created, deleted, or updated.
 * @param neigh      Parsed neighbor descriptor; valid only for this call.
 * @param user_data  Opaque pointer forwarded from the API functions.
 */
typedef void (*netlink_neigh_cb_t)(netlink_neigh_event_t     event,
                                   const netlink_neigh_t    *neigh,
                                   void                     *user_data);

/* ---------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

/**
 * @brief Opens and binds an AF_NETLINK/NETLINK_ROUTE socket subscribed to
 *        the RTMGRP_NEIGH multicast group (IPv4, IPv6, and bridge neighbors).
 *
 * @return Non-negative file descriptor on success, -1 on error (errno set).
 */
int netlink_neigh_init(void);

/**
 * @brief Sends RTM_GETNEIGH dump request and dispatches all existing entries.
 *
 * Each existing kernel neighbor entry fires the callback with event =
 * NETLINK_NEIGH_ADDED.  The function blocks until the kernel signals
 * NLMSG_DONE (end-of-dump).
 *
 * @param fd        File descriptor from netlink_neigh_init().
 * @param cb        Callback to invoke for each entry.
 * @param user_data Forwarded to @p cb.
 * @return Number of entries dispatched (>= 0), or -1 on error.
 */
int netlink_neigh_dump(int fd, netlink_neigh_cb_t cb, void *user_data);

/**
 * @brief Blocking event loop — runs until the socket is closed or errors out.
 *
 * Call after netlink_neigh_dump() to receive ongoing neighbor change events.
 * Returns 0 on clean close, -1 on error.
 *
 * @param fd        File descriptor from netlink_neigh_init().
 * @param cb        Callback to invoke for each event.
 * @param user_data Forwarded to @p cb.
 */
int netlink_neigh_run(int fd, netlink_neigh_cb_t cb, void *user_data);

/**
 * @brief Closes the netlink socket.  Safe to call with fd = -1 (no-op).
 */
void netlink_neigh_close(int fd);

#ifdef __cplusplus
} /* extern "C" */
#endif
