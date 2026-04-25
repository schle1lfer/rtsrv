/**
 * @file client/src/netlink_neigh.c
 * @brief Netlink listener for neighbor (ARP/NDP) table changes.
 *
 * Subscribes to RTMGRP_NEIGH (IPv4, IPv6, and bridge neighbors) and parses
 * RTM_NEWNEIGH / RTM_DELNEIGH messages into netlink_neigh_t descriptors.
 * An initial dump of existing entries can be requested via
 * netlink_neigh_dump().
 *
 * Fields extracted from each message:
 *   ndmsg header : family, ifindex, state (NUD_*), flags (NTF_*), type
 *   NDA_DST      : destination IP address (IPv4 or IPv6)
 *   NDA_LLADDR   : link-layer address (MAC)
 *   NDA_CACHEINFO: confirmed_ms, used_ms, updated_ms, refcnt
 *   NDA_PROBES   : ARP/NDP probe count
 *   NDA_VLAN     : VLAN ID (bridge)
 *   NDA_MASTER   : master interface index
 *   NDA_IFINDEX  : nexthop interface override
 *   NDA_PROTOCOL : routing protocol (kernel 5.2+)
 *
 * @version 1.0
 */

#include "client/netlink_neigh.h"

#include <arpa/inet.h>
#include <asm/types.h>
#include <errno.h>
#include <linux/neighbour.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* RTNLGRP_NEIGH multicast group (Linux 2.6+). */
#ifndef RTNLGRP_NEIGH
#define RTNLGRP_NEIGH 3
#endif

/* Receive buffer: large enough for a full-sized netlink datagram. */
#define NL_BUF_SIZE 32768

/* ── NDA_RTA / NDA_PAYLOAD are not always exported in user-space headers ─── */
#ifndef NDA_RTA
#define NDA_RTA(r)                                                             \
    ((struct rtattr*)(((char*)(r)) + NLMSG_ALIGN(sizeof(struct ndmsg))))
#define NDA_PAYLOAD(n) NLMSG_PAYLOAD((n), sizeof(struct ndmsg))
#endif

/* ── NDA_PROTOCOL may not be present in older kernel headers ─────────────── */
#ifndef NDA_PROTOCOL
#define NDA_PROTOCOL 12
#endif

/* ---------------------------------------------------------------------------
 * Internal helpers
 * ------------------------------------------------------------------------- */

/**
 * @brief Parses one RTM_NEWNEIGH / RTM_DELNEIGH message and fires @p cb.
 *
 * @param nlh       Pointer to the netlink message header.
 * @param cb        Caller-supplied event callback.
 * @param user_data Forwarded to @p cb.
 */
static void nl_neigh_dispatch(const struct nlmsghdr* nlh,
                              netlink_neigh_cb_t cb,
                              void* user_data)
{
    /* Sanity: message must be large enough to contain ndmsg. */
    if (nlh->nlmsg_len < NLMSG_LENGTH(sizeof(struct ndmsg)))
        return;

    const struct ndmsg* ndm = (const struct ndmsg*)NLMSG_DATA(nlh);

    /* Map nlmsg_type + nlmsg_flags to event type. */
    netlink_neigh_event_t event;
    if (nlh->nlmsg_type == RTM_NEWNEIGH)
    {
        event = (nlh->nlmsg_flags & NLM_F_REPLACE) ? NETLINK_NEIGH_CHANGED
                                                   : NETLINK_NEIGH_ADDED;
    }
    else if (nlh->nlmsg_type == RTM_DELNEIGH)
    {
        event = NETLINK_NEIGH_REMOVED;
    }
    else
    {
        return; /* Not a neighbor message */
    }

    /* Populate the descriptor from the ndmsg header. */
    netlink_neigh_t n;
    memset(&n, 0, sizeof(n));
    n.family = ndm->ndm_family;
    n.ifindex = ndm->ndm_ifindex;
    n.state = ndm->ndm_state;
    n.flags = ndm->ndm_flags;
    n.type = ndm->ndm_type;

    if (n.ifindex > 0)
        if_indextoname((unsigned)n.ifindex, n.ifname);

    /* Walk NDA_* attributes. */
    unsigned int attrlen = (unsigned int)NDA_PAYLOAD(nlh);
    for (const struct rtattr* rta = NDA_RTA(ndm); RTA_OK(rta, attrlen);
         rta = RTA_NEXT(rta, attrlen))
    {
        /* Strip nested/network-byte-order flags from the type. */
        const int rtype = rta->rta_type & 0x00ff;

        switch (rtype)
        {
        case NDA_DST:
            if (n.family == AF_INET &&
                RTA_PAYLOAD(rta) >= sizeof(struct in_addr))
            {
                inet_ntop(AF_INET, RTA_DATA(rta), n.dst, sizeof(n.dst));
            }
            else if (n.family == AF_INET6 &&
                     RTA_PAYLOAD(rta) >= sizeof(struct in6_addr))
            {
                inet_ntop(AF_INET6, RTA_DATA(rta), n.dst, sizeof(n.dst));
            }
            break;

        case NDA_LLADDR: {
            size_t ll = RTA_PAYLOAD(rta);
            if (ll > sizeof(n.lladdr))
                ll = sizeof(n.lladdr);
            memcpy(n.lladdr, RTA_DATA(rta), ll);
            n.lladdr_len = (uint8_t)ll;
            break;
        }

        case NDA_CACHEINFO:
            if (RTA_PAYLOAD(rta) >= sizeof(struct nda_cacheinfo))
            {
                struct nda_cacheinfo ci;
                memcpy(&ci, RTA_DATA(rta), sizeof(ci));
                n.confirmed_ms = ci.ndm_confirmed;
                n.used_ms = ci.ndm_used;
                n.updated_ms = ci.ndm_updated;
                n.refcnt = ci.ndm_refcnt;
            }
            break;

        case NDA_PROBES:
            if (RTA_PAYLOAD(rta) >= sizeof(uint32_t))
                memcpy(&n.probes, RTA_DATA(rta), sizeof(uint32_t));
            break;

        case NDA_VLAN:
            if (RTA_PAYLOAD(rta) >= sizeof(uint16_t))
                memcpy(&n.vlan, RTA_DATA(rta), sizeof(uint16_t));
            break;

        case NDA_MASTER:
            if (RTA_PAYLOAD(rta) >= sizeof(uint32_t))
            {
                memcpy(&n.master, RTA_DATA(rta), sizeof(uint32_t));
                if (n.master > 0)
                    if_indextoname(n.master, n.master_name);
            }
            break;

        case NDA_IFINDEX:
            if (RTA_PAYLOAD(rta) >= sizeof(uint32_t))
                memcpy(&n.nh_ifindex, RTA_DATA(rta), sizeof(uint32_t));
            break;

        case NDA_PROTOCOL:
            if (RTA_PAYLOAD(rta) >= sizeof(uint8_t))
                memcpy(&n.protocol, RTA_DATA(rta), sizeof(uint8_t));
            break;

        default:
            break;
        }
    }

    cb(event, &n, user_data);
}

/**
 * @brief Processes one receive-buffer's worth of netlink messages.
 *
 * @param buf       Buffer containing one or more netlink messages.
 * @param n         Number of valid bytes.
 * @param cb        Caller-supplied callback.
 * @param user_data Forwarded to nl_neigh_dispatch().
 * @param done      Set to 1 when NLMSG_DONE is encountered; may be NULL.
 * @return Number of neighbor events dispatched, or -1 on NLMSG_ERROR.
 */
static int nl_neigh_process_buf(const char* buf,
                                ssize_t n,
                                netlink_neigh_cb_t cb,
                                void* user_data,
                                int* done)
{
    int count = 0;

    for (const struct nlmsghdr* nlh = (const struct nlmsghdr*)buf;
         NLMSG_OK(nlh, (unsigned int)n);
         nlh = NLMSG_NEXT(nlh, n))
    {
        if (nlh->nlmsg_type == NLMSG_DONE)
        {
            if (done)
                *done = 1;
            break;
        }
        if (nlh->nlmsg_type == NLMSG_ERROR)
            return -1;

        if (nlh->nlmsg_type == RTM_NEWNEIGH || nlh->nlmsg_type == RTM_DELNEIGH)
        {
            nl_neigh_dispatch(nlh, cb, user_data);
            ++count;
        }
    }

    return count;
}

/* ---------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

int netlink_neigh_init(void)
{
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0)
        return -1;

    /* Bind with nl_groups=0; subscribe via NETLINK_ADD_MEMBERSHIP below so
     * that a failed subscription (e.g. inside a restricted container) does
     * not prevent the initial dump from working. */
    struct sockaddr_nl addr;
    memset(&addr, 0, sizeof(addr));
    addr.nl_family = AF_NETLINK;
    addr.nl_groups = 0;

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0)
    {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }

    /* Subscribe to the neighbor multicast group.  Non-fatal: in restricted
     * environments the dump still works and live events are simply absent. */
    int group = RTNLGRP_NEIGH;
    if (setsockopt(
            fd, SOL_NETLINK, NETLINK_ADD_MEMBERSHIP, &group, sizeof(group)) < 0)
    {
        fprintf(stderr,
                "netlink_neigh: multicast subscribe failed "
                "(live events disabled): %s\n",
                strerror(errno));
    }

    return fd;
}

int netlink_neigh_dump(int fd, netlink_neigh_cb_t cb, void* user_data)
{
    /* Build RTM_GETNEIGH dump request for all address families. */
    struct
    {
        struct nlmsghdr nlh;
        struct ndmsg ndm;
    } req;

    memset(&req, 0, sizeof(req));
    req.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(struct ndmsg));
    req.nlh.nlmsg_type = RTM_GETNEIGH;
    req.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    req.nlh.nlmsg_seq = 1;
    req.ndm.ndm_family = AF_UNSPEC; /* All families */

    if (send(fd, &req, req.nlh.nlmsg_len, 0) < 0)
        return -1;

    char buf[NL_BUF_SIZE];
    int total = 0;
    int done = 0;

    while (!done)
    {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            break;

        int r = nl_neigh_process_buf(buf, n, cb, user_data, &done);
        if (r < 0)
            return -1;
        total += r;
    }

    return total;
}

int netlink_neigh_run(int fd, netlink_neigh_cb_t cb, void* user_data)
{
    char buf[NL_BUF_SIZE];

    for (;;)
    {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return 0;

        if (nl_neigh_process_buf(buf, n, cb, user_data, NULL) < 0)
            return -1;
    }
}

void netlink_neigh_close(int fd)
{
    if (fd >= 0)
        close(fd);
}
