/**
 * @file server/src/netlink.c
 * @brief Netlink listener for IPv4 /32 route changes from FRR zebra.
 *
 * FRR zebra programmes host routes (/32) into the kernel routing table via
 * rtnetlink.  When a route is installed zebra sends RTM_NEWROUTE; when it
 * is withdrawn it sends RTM_DELROUTE.  This module subscribes to the
 * RTMGRP_IPV4_ROUTE multicast group, parses incoming messages, and fires
 * the caller-supplied callback for every unicast /32 route event.
 *
 * Message filtering applied before the callback is invoked:
 *   - nlmsg_type  must be RTM_NEWROUTE or RTM_DELROUTE
 *   - rtm_family  must be AF_INET  (IPv4)
 *   - rtm_dst_len must be 32       (host route)
 *   - rtm_type    must be RTN_UNICAST
 *
 * Attributes extracted (others are ignored):
 *   - RTA_DST      → netlink_route32_t.dst
 *   - RTA_GATEWAY  → netlink_route32_t.gateway
 *   - RTA_OIF      → netlink_route32_t.ifindex (+ if_indextoname lookup)
 *   - RTA_PRIORITY → netlink_route32_t.metric
 *   - RTA_TABLE    → netlink_route32_t.table   (overrides rtm_table for
 *                    tables > 255 as used by FRR VRFs)
 *
 * @version 1.0
 */

#include "server/netlink.h"

#include <arpa/inet.h>
#include <asm/types.h>
#include <errno.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* Receive buffer: large enough for a full-sized netlink datagram. */
#define NL_BUF_SIZE 8192

/* ---------------------------------------------------------------------------
 * Internal helpers
 * ------------------------------------------------------------------------- */

/**
 * @brief Logs a full OSPF netlink route message to stdout.
 *
 * Prints a separator, the rtmsg header fields, each rtattr in message order,
 * and a closing separator.  Called for every RTM_NEWROUTE / RTM_DELROUTE
 * message whose rtm_protocol equals RTPROT_OSPF, before the /32 and
 * RTN_UNICAST filters are applied so that all OSPF prefix lengths are visible.
 *
 * Output format (one line per attribute):
 * @code
 * --------------------------------------------------------------
 * NETLINK > RTM_NEWROUTE: { family: 2 (PF_INET); dst_len: 30; ... }
 * NETLINK > RTM_NEWROUTE: "RTA_TABLE"    : 254 (000000FE)
 * NETLINK > RTM_NEWROUTE: "RTA_DST"      : 10.42.42.4
 * ...
 * --------------------------------------------------------------
 * @endcode
 *
 * @param nlh  Pointer to the netlink message header.
 */
static void nl_log_ospf(const struct nlmsghdr *nlh)
{
    const struct rtmsg *rtm = NLMSG_DATA(nlh);

    if (rtm->rtm_protocol != RTPROT_OSPF)
    {
        return;
    }

    /* Message type name */
    const char *tn = (nlh->nlmsg_type == RTM_NEWROUTE) ? "RTM_NEWROUTE"
                                                        : "RTM_DELROUTE";

    /* Route type name */
    const char *rtype;
    switch (rtm->rtm_type)
    {
    case RTN_UNICAST:     rtype = "UNICAST";     break;
    case RTN_BLACKHOLE:   rtype = "BLACKHOLE";   break;
    case RTN_UNREACHABLE: rtype = "UNREACHABLE"; break;
    case RTN_PROHIBIT:    rtype = "PROHIBIT";    break;
    default:              rtype = "?";           break;
    }

    printf("--------------------------------------------------------------\n");
    printf("NETLINK > %s: { family: %u (PF_INET); dst_len: %u; src_len: %u;"
           " tos: %u; table: %u; protocol: %u (OSPF); scope: %u;"
           " type: %u (%s); flags: %02X }\n",
           tn,
           (unsigned)rtm->rtm_family,
           (unsigned)rtm->rtm_dst_len,
           (unsigned)rtm->rtm_src_len,
           (unsigned)rtm->rtm_tos,
           (unsigned)rtm->rtm_table,
           (unsigned)rtm->rtm_protocol,
           (unsigned)rtm->rtm_scope,
           (unsigned)rtm->rtm_type, rtype,
           (unsigned)rtm->rtm_flags);

    /* Attributes – printed in the order they appear in the message */
    char         addr_buf[INET_ADDRSTRLEN];
    uint32_t     u32;
    unsigned int attrlen = (unsigned int)RTM_PAYLOAD(nlh);

    for (const struct rtattr *rta = RTM_RTA(rtm);
         RTA_OK(rta, attrlen);
         rta = RTA_NEXT(rta, attrlen))
    {
        switch (rta->rta_type)
        {
        case RTA_TABLE:
            if (RTA_PAYLOAD(rta) >= sizeof(u32))
            {
                memcpy(&u32, RTA_DATA(rta), sizeof(u32));
                printf("NETLINK > %s: \"RTA_TABLE\"    : %u (%08X)\n",
                       tn, u32, u32);
            }
            break;

        case RTA_DST:
            if (RTA_PAYLOAD(rta) >= sizeof(struct in_addr))
            {
                inet_ntop(AF_INET, RTA_DATA(rta), addr_buf, sizeof(addr_buf));
                printf("NETLINK > %s: \"RTA_DST\"      : %s\n", tn, addr_buf);
            }
            break;

        case RTA_PRIORITY:
            if (RTA_PAYLOAD(rta) >= sizeof(u32))
            {
                memcpy(&u32, RTA_DATA(rta), sizeof(u32));
                printf("NETLINK > %s: \"RTA_PRIORITY\" : %u (%08X)\n",
                       tn, u32, u32);
            }
            break;

        case RTA_NH_ID:
            if (RTA_PAYLOAD(rta) >= sizeof(u32))
            {
                memcpy(&u32, RTA_DATA(rta), sizeof(u32));
                printf("NETLINK > %s: \"RTA_NH_ID\"    : %u (%08X)\n",
                       tn, u32, u32);
            }
            break;

        case RTA_GATEWAY:
            if (RTA_PAYLOAD(rta) >= sizeof(struct in_addr))
            {
                inet_ntop(AF_INET, RTA_DATA(rta), addr_buf, sizeof(addr_buf));
                printf("NETLINK > %s: \"RTA_GATEWAY\"  : %s\n", tn, addr_buf);
            }
            break;

        case RTA_OIF:
            if (RTA_PAYLOAD(rta) >= sizeof(u32))
            {
                char ifname[IF_NAMESIZE] = {0};
                memcpy(&u32, RTA_DATA(rta), sizeof(u32));
                if_indextoname(u32, ifname);
                printf("NETLINK > %s: \"RTA_OIF\"      : %s (%u)\n",
                       tn, ifname[0] ? ifname : "?", u32);
            }
            break;

        case RTA_MULTIPATH:
        {
            /* Log each nexthop entry in the multipath list. */
            const struct rtnexthop *rtnh =
                (const struct rtnexthop *)RTA_DATA(rta);
            int nhrem = (int)RTA_PAYLOAD(rta);
            int nhidx = 0;

            while (RTNH_OK(rtnh, nhrem))
            {
                char gw_buf[INET_ADDRSTRLEN] = "(none)";
                char ifname[IF_NAMESIZE]      = {0};

                if_indextoname((unsigned)rtnh->rtnh_ifindex, ifname);

                int ilen = (int)rtnh->rtnh_len -
                           (int)sizeof(struct rtnexthop);
                const struct rtattr *inner =
                    (const struct rtattr *)RTNH_DATA(rtnh);
                for (; RTA_OK(inner, ilen); inner = RTA_NEXT(inner, ilen))
                {
                    if (inner->rta_type == RTA_GATEWAY &&
                        RTA_PAYLOAD(inner) >= sizeof(struct in_addr))
                    {
                        inet_ntop(AF_INET, RTA_DATA(inner),
                                  gw_buf, sizeof(gw_buf));
                        break;
                    }
                }

                printf("NETLINK > %s: \"RTA_MULTIPATH\"[%d]: via %s dev %s\n",
                       tn, nhidx,
                       gw_buf, ifname[0] ? ifname : "?");

                nhrem -= (int)NLMSG_ALIGN(rtnh->rtnh_len);
                rtnh   = RTNH_NEXT(rtnh);
                ++nhidx;
            }
            break;
        }

        default:
            break;
        }
    }

    printf("--------------------------------------------------------------\n");
    fflush(stdout);
}

/**
 * @brief Parses one RTM_NEWROUTE / RTM_DELROUTE message and fires @p cb.
 *
 * Applies all filters described in the file header before calling the
 * callback.  Returns immediately (without calling @p cb) if any filter
 * condition is not met.
 *
 * For RTM_NEWROUTE the nlmsg_flags field is inspected to distinguish a
 * fresh install from an in-place update:
 *   - NLM_F_REPLACE not set → NETLINK_ROUTE_ADDED   ("ip route add")
 *   - NLM_F_REPLACE set     → NETLINK_ROUTE_CHANGED ("ip route replace/change",
 *                                                     FRR zebra nexthop update)
 *
 * @param nlh       Pointer to the netlink message header.
 * @param cb        Caller-supplied event callback.
 * @param user_data Forwarded to @p cb.
 */
static void nl_dispatch(const struct nlmsghdr *nlh,
                        netlink_route_cb_t     cb,
                        void                  *user_data)
{
    const struct rtmsg *rtm = NLMSG_DATA(nlh);

    /* --- Mandatory filters ------------------------------------------------ */

    if (rtm->rtm_family != AF_INET)
    {
        return; /* IPv6 or other; not of interest */
    }

    /* Log every OSPF route event before the /32 filter so that all OSPF
     * prefix lengths (not just host routes) appear in the output. */
    nl_log_ospf(nlh);

    if (rtm->rtm_dst_len != 32)
    {
        return; /* Not a host route */
    }
    if (rtm->rtm_type != RTN_UNICAST)
    {
        return; /* Blackhole, unreachable, multicast, etc. */
    }

    /* --- Map nlmsg_type + nlmsg_flags to event ---------------------------- */

    netlink_event_t event;
    if (nlh->nlmsg_type == RTM_NEWROUTE)
    {
        /*
         * NLM_F_REPLACE indicates that the kernel is replacing an existing
         * route entry rather than inserting a new one.  Both "ip route replace"
         * and "ip route change" set this flag, as does FRR zebra when it
         * updates a nexthop or metric for a previously announced prefix.
         */
        event = (nlh->nlmsg_flags & NLM_F_REPLACE)
                    ? NETLINK_ROUTE_CHANGED
                    : NETLINK_ROUTE_ADDED;
    }
    else if (nlh->nlmsg_type == RTM_DELROUTE)
    {
        event = NETLINK_ROUTE_REMOVED;
    }
    else
    {
        return; /* Should not happen given the caller's pre-filter */
    }

    /* --- Populate route descriptor from rtnetlink attributes -------------- */

    netlink_route32_t route;
    memset(&route, 0, sizeof(route));
    route.protocol = rtm->rtm_protocol;
    route.table    = rtm->rtm_table; /* May be overridden by RTA_TABLE below */

    unsigned int attrlen = (unsigned int)RTM_PAYLOAD(nlh);
    for (const struct rtattr *rta = RTM_RTA(rtm);
         RTA_OK(rta, attrlen);
         rta = RTA_NEXT(rta, attrlen))
    {
        switch (rta->rta_type)
        {
        case RTA_DST:
            if (RTA_PAYLOAD(rta) >= sizeof(route.dst))
            {
                memcpy(&route.dst, RTA_DATA(rta), sizeof(route.dst));
            }
            break;

        case RTA_GATEWAY:
            if (RTA_PAYLOAD(rta) >= sizeof(route.gateway))
            {
                memcpy(&route.gateway, RTA_DATA(rta), sizeof(route.gateway));
            }
            break;

        case RTA_OIF:
            if (RTA_PAYLOAD(rta) >= sizeof(route.ifindex))
            {
                memcpy(&route.ifindex, RTA_DATA(rta), sizeof(route.ifindex));
                /* Resolve the interface name; ignore failure (name stays ""). */
                if_indextoname(route.ifindex, route.ifname);
            }
            break;

        case RTA_PRIORITY:
            if (RTA_PAYLOAD(rta) >= sizeof(route.metric))
            {
                memcpy(&route.metric, RTA_DATA(rta), sizeof(route.metric));
            }
            break;

        case RTA_TABLE:
            /*
             * FRR uses RTA_TABLE for VRF-specific table IDs > 255.  When
             * present it supersedes the 8-bit rtm_table field.
             */
            if (RTA_PAYLOAD(rta) >= sizeof(route.table))
            {
                memcpy(&route.table, RTA_DATA(rta), sizeof(route.table));
            }
            break;

        default:
            break;
        }
    }

    cb(event, &route, user_data);
}

/**
 * @brief Processes one receive buffer worth of netlink messages.
 *
 * Iterates over all nlmsghdr records in @p buf (up to @p n bytes), calling
 * nl_dispatch() for every RTM_NEWROUTE / RTM_DELROUTE message found.
 *
 * @param buf       Buffer containing one or more netlink messages.
 * @param n         Number of valid bytes in @p buf.
 * @param cb        Caller-supplied event callback.
 * @param user_data Forwarded to nl_dispatch().
 *
 * @return Number of qualifying route events dispatched, or -1 if a
 *         NLMSG_ERROR record is encountered.
 */
static int nl_process_buf(const char        *buf,
                          ssize_t            n,
                          netlink_route_cb_t cb,
                          void              *user_data)
{
    int count = 0;

    for (const struct nlmsghdr *nlh = (const struct nlmsghdr *)buf;
         NLMSG_OK(nlh, (unsigned int)n);
         nlh = NLMSG_NEXT(nlh, n))
    {
        if (nlh->nlmsg_type == NLMSG_DONE)
        {
            break;
        }
        if (nlh->nlmsg_type == NLMSG_ERROR)
        {
            return -1;
        }

        if (nlh->nlmsg_type == RTM_NEWROUTE ||
            nlh->nlmsg_type == RTM_DELROUTE)
        {
            nl_dispatch(nlh, cb, user_data);
            ++count;
        }
    }

    return count;
}

/* ---------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

int netlink_init(void)
{
    int fd = socket(AF_NETLINK,
                    SOCK_RAW | SOCK_CLOEXEC,
                    NETLINK_ROUTE);
    if (fd < 0)
    {
        return -1;
    }

    struct sockaddr_nl addr;
    memset(&addr, 0, sizeof(addr));
    addr.nl_family = AF_NETLINK;
    addr.nl_groups = RTMGRP_IPV4_ROUTE;

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }

    return fd;
}

int netlink_process(int fd, netlink_route_cb_t cb, void *user_data)
{
    char    buf[NL_BUF_SIZE];
    int     total = 0;

    for (;;)
    {
        ssize_t n = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
        if (n < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                break; /* No more messages pending */
            }
            return -1;
        }

        int r = nl_process_buf(buf, n, cb, user_data);
        if (r < 0)
        {
            return -1;
        }
        total += r;
    }

    return total;
}

int netlink_run(int fd, netlink_route_cb_t cb, void *user_data)
{
    char buf[NL_BUF_SIZE];

    for (;;)
    {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue; /* Interrupted by signal; retry */
            }
            return -1;
        }
        if (n == 0)
        {
            return 0; /* Socket closed cleanly */
        }

        if (nl_process_buf(buf, n, cb, user_data) < 0)
        {
            return -1;
        }
    }
}

void netlink_close(int fd)
{
    if (fd >= 0)
    {
        close(fd);
    }
}
