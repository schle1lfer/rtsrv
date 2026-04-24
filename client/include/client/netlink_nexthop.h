/**
 * @file client/include/client/netlink_nexthop.h
 * @brief Netlink listener for nexthop object changes (Linux 5.3+).
 *
 * Opens a NETLINK_ROUTE socket subscribed to the RTNLGRP_NEXTHOP multicast
 * group and delivers RTM_NEWNEXTHOP / RTM_DELNEXTHOP events to a
 * caller-supplied callback with all fields from the nhmsg header and every
 * recognised NHA_* attribute.
 *
 * An initial dump of existing nexthop objects can be obtained via
 * netlink_nexthop_dump(), which sends RTM_GETNEXTHOP with NLM_F_DUMP and
 * fires the callback once per object (all with event = NETLINK_NEXTHOP_ADDED).
 *
 * Nexthop objects (kernel 5.3+) are named nexthops independent of routes;
 * routes may reference them by ID.  Groups aggregate multiple nexthops for
 * ECMP or resilient hashing.
 *
 * Typical usage:
 * @code
 *   int fd = netlink_nexthop_init();
 *   netlink_nexthop_dump(fd, my_cb, ctx);  // populate from existing table
 *   netlink_nexthop_run(fd,  my_cb, ctx);  // block until socket closed
 *   netlink_nexthop_close(fd);
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
 * @brief Whether a nexthop object was created, deleted, or updated.
 *
 * - NETLINK_NEXTHOP_ADDED   – RTM_NEWNEXTHOP without NLM_F_REPLACE, or a dump
 *                             response.
 * - NETLINK_NEXTHOP_REMOVED – RTM_DELNEXTHOP.
 * - NETLINK_NEXTHOP_CHANGED – RTM_NEWNEXTHOP with NLM_F_REPLACE set.
 */
typedef enum
{
    NETLINK_NEXTHOP_ADDED   = 0,
    NETLINK_NEXTHOP_REMOVED = 1,
    NETLINK_NEXTHOP_CHANGED = 2,
} netlink_nexthop_event_t;

/* ---------------------------------------------------------------------------
 * Nexthop group member
 * ------------------------------------------------------------------------- */

/** Maximum number of group members stored per nexthop entry. */
#define NETLINK_NEXTHOP_MAX_GROUP 64

/**
 * @brief One member of a nexthop group (from NHA_GROUP attribute).
 *
 * Matches struct nexthop_grp from linux/nexthop.h.
 */
typedef struct
{
    uint32_t id;           /**< Nexthop ID of this group member              */
    uint8_t  weight;       /**< Weight stored by kernel (actual = weight + 1) */
    uint8_t  weight_high;  /**< High 8 bits of the weight (extended weight)  */
} netlink_nexthop_grp_t;

/* ---------------------------------------------------------------------------
 * Nexthop descriptor – carries all fields from nhmsg + NHA_* attributes
 * ------------------------------------------------------------------------- */

/**
 * @brief Describes a single nexthop object received from the kernel.
 *
 * Fields map directly to the nhmsg header and NHA_* rtnetlink attributes.
 * Optional attributes absent in the message are left zero-initialised.
 */
typedef struct
{
    /* ── From struct nhmsg ─────────────────────────────────────────────── */
    uint8_t  family;       /**< nh_family: AF_INET, AF_INET6, or AF_UNSPEC  */
    uint8_t  scope;        /**< nh_scope: RT_SCOPE_* (link/host/global/…)   */
    uint8_t  protocol;     /**< nh_protocol: RTPROT_* (static/ospf/bgp/…)  */
    uint32_t flags;        /**< nh_flags: RTNH_F_* bitmask                  */

    /* ── NHA_ID ────────────────────────────────────────────────────────── */
    uint32_t id;           /**< Unique nexthop identifier (>= 1)            */

    /* ── NHA_OIF ───────────────────────────────────────────────────────── */
    uint32_t oif;          /**< Output interface index (0 = absent)         */
    char     oif_name[IF_NAMESIZE]; /**< Resolved interface name            */

    /* ── NHA_GATEWAY ───────────────────────────────────────────────────── */
    char     gateway[INET6_ADDRSTRLEN]; /**< Gateway address string ("" if absent) */

    /* ── NHA_BLACKHOLE ─────────────────────────────────────────────────── */
    uint8_t  blackhole;    /**< 1 if this is a blackhole nexthop            */

    /* ── NHA_FDB ───────────────────────────────────────────────────────── */
    uint8_t  fdb;          /**< 1 if nexthop is offloaded to FDB            */

    /* ── NHA_MASTER ────────────────────────────────────────────────────── */
    uint32_t master;       /**< Master device index (0 = absent)            */
    char     master_name[IF_NAMESIZE]; /**< Resolved master device name     */

    /* ── NHA_GROUP ─────────────────────────────────────────────────────── */
    netlink_nexthop_grp_t group[NETLINK_NEXTHOP_MAX_GROUP]; /**< Group members */
    uint32_t              group_count; /**< Number of valid entries in group[] */

    /* ── NHA_GROUP_TYPE ────────────────────────────────────────────────── */
    uint16_t group_type;   /**< 0=mpath (ECMP), 1=resilient                */

    /* ── NHA_ENCAP_TYPE ────────────────────────────────────────────────── */
    uint16_t encap_type;   /**< Encapsulation type (LWTUNNEL_ENCAP_*)      */
} netlink_nexthop_t;

/* ---------------------------------------------------------------------------
 * Callback type
 * ------------------------------------------------------------------------- */

/**
 * @brief User callback invoked once per nexthop event.
 *
 * @param event      Created, deleted, or updated.
 * @param nh         Parsed nexthop descriptor; valid only for this call.
 * @param user_data  Opaque pointer forwarded from the API functions.
 */
typedef void (*netlink_nexthop_cb_t)(netlink_nexthop_event_t   event,
                                     const netlink_nexthop_t  *nh,
                                     void                     *user_data);

/* ---------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

/**
 * @brief Opens and binds an AF_NETLINK/NETLINK_ROUTE socket subscribed to the
 *        RTNLGRP_NEXTHOP multicast group via NETLINK_ADD_MEMBERSHIP.
 *
 * Nexthop objects were added in Linux 5.3.  On older kernels the socket will
 * open successfully but no RTM_NEWNEXTHOP / RTM_DELNEXTHOP events will arrive
 * and the dump will return 0 entries.
 *
 * @return Non-negative file descriptor on success, -1 on error (errno set).
 */
int netlink_nexthop_init(void);

/**
 * @brief Sends RTM_GETNEXTHOP dump request and dispatches all existing
 *        nexthop objects via @p cb with event = NETLINK_NEXTHOP_ADDED.
 *
 * Blocks until NLMSG_DONE is received from the kernel.
 *
 * @param fd        File descriptor from netlink_nexthop_init().
 * @param cb        Callback to invoke for each nexthop.
 * @param user_data Forwarded to @p cb.
 * @return Number of nexthops dispatched (>= 0), or -1 on error.
 */
int netlink_nexthop_dump(int fd, netlink_nexthop_cb_t cb, void *user_data);

/**
 * @brief Blocking event loop — runs until the socket is closed or errors out.
 *
 * Call after netlink_nexthop_dump() to receive ongoing nexthop change events.
 * Returns 0 on clean close, -1 on error.
 *
 * @param fd        File descriptor from netlink_nexthop_init().
 * @param cb        Callback to invoke for each event.
 * @param user_data Forwarded to @p cb.
 */
int netlink_nexthop_run(int fd, netlink_nexthop_cb_t cb, void *user_data);

/**
 * @brief Closes the netlink socket.  Safe to call with fd = -1 (no-op).
 */
void netlink_nexthop_close(int fd);

#ifdef __cplusplus
} /* extern "C" */
#endif
