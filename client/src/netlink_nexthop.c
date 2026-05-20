/**
 * @file client/src/netlink_nexthop.c
 * @brief Netlink listener for nexthop object changes (Linux 5.3+).
 *
 * Subscribes to the RTNLGRP_NEXTHOP multicast group via
 * NETLINK_ADD_MEMBERSHIP and parses RTM_NEWNEXTHOP / RTM_DELNEXTHOP messages
 * into netlink_nexthop_t descriptors.  An initial dump of existing nexthop
 * objects can be obtained via netlink_nexthop_dump().
 *
 * Nexthop message types (RTM_NEWNEXTHOP / RTM_DELNEXTHOP / RTM_GETNEXTHOP)
 * and the nhmsg header were introduced in Linux 5.3.  This module compiles
 * cleanly on older kernels using inline definitions; the dump and monitoring
 * will simply return 0 entries.
 *
 * Fields extracted from each message:
 *   nhmsg header : family, scope, protocol, flags
 *   NHA_ID        : unique nexthop ID
 *   NHA_OIF       : output interface index + name
 *   NHA_GATEWAY   : gateway address (IPv4 or IPv6)
 *   NHA_BLACKHOLE : blackhole flag
 *   NHA_FDB       : FDB offload flag
 *   NHA_MASTER    : master device index + name
 *   NHA_GROUP     : array of { id, weight } group members
 *   NHA_GROUP_TYPE: 0=mpath, 1=resilient
 *   NHA_ENCAP_TYPE: encapsulation type
 *
 * @version 1.0
 */

#include "client/netlink_nexthop.h"

#include <arpa/inet.h>
#include <asm/types.h>
#include <errno.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/socket.h>
#include <net/if.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* Receive buffer: large enough for a full-sized netlink datagram. */
#define NL_BUF_SIZE 32768

/* ── Nexthop kernel structures – define inline if not in system headers ─── */

/* Nexthop message header (struct nhmsg from linux/nexthop.h, Linux 5.3+). */
struct nl_nhmsg
{
    unsigned char nh_family;
    unsigned char nh_scope;
    unsigned char nh_protocol;
    unsigned char resvd;
    unsigned int nh_flags;
};

/* Nexthop group member (struct nexthop_grp from linux/nexthop.h). */
struct nl_nexthop_grp
{
    uint32_t id;
    uint8_t weight; /* actual_weight = weight + 1 */
    uint8_t weight_high;
    uint16_t resvd;
};

/* RTM_*NEXTHOP message types (Linux 5.3+). */
#ifndef RTM_NEWNEXTHOP
#define RTM_NEWNEXTHOP 104
#define RTM_DELNEXTHOP 105
#define RTM_GETNEXTHOP 106
#endif

/* RTNLGRP_NEXTHOP multicast group number (Linux 5.3+). */
#ifndef RTNLGRP_NEXTHOP
#define RTNLGRP_NEXTHOP 22
#endif

/* NHA_* attribute types (from linux/nexthop.h). */
#ifndef NHA_ID
enum
{
    NHA_UNSPEC,
    NHA_ID,         /* u32 */
    NHA_GROUP,      /* array of struct nl_nexthop_grp */
    NHA_GROUP_TYPE, /* u16: 0=mpath, 1=resilient */
    NHA_BLACKHOLE,  /* flag (no payload) */
    NHA_OIF,        /* u32 */
    NHA_GATEWAY,    /* in_addr or in6_addr */
    NHA_ENCAP_TYPE, /* u16 */
    NHA_ENCAP,      /* nested */
    NHA_GROUPS,     /* flag (dump groups) */
    NHA_MASTER,     /* u32 */
    NHA_FDB,        /* flag */
    __NHA_MAX
};
#define NHA_MAX (__NHA_MAX - 1)
#endif

/* Helper macros for nhmsg attribute access (analogous to RTM_RTA/RTM_PAYLOAD).
 */
#define NL_NHA_RTA(n)                                                          \
    ((struct rtattr*)(((char*)(n)) + NLMSG_ALIGN(sizeof(struct nl_nhmsg))))
#define NL_NHA_PAYLOAD(n) NLMSG_PAYLOAD((n), sizeof(struct nl_nhmsg))

/* ---------------------------------------------------------------------------
 * Internal helpers
 * ------------------------------------------------------------------------- */

/**
 * @brief Parses nhmsg header and NHA_* attributes into @p nh (must be zeroed).
 */
static void nl_nexthop_parse(const struct nlmsghdr* nlh, netlink_nexthop_t* nh)
{
    const struct nl_nhmsg* nhm = (const struct nl_nhmsg*)NLMSG_DATA(nlh);

    /* Copy fields from nhmsg header. */
    nh->family   = nhm->nh_family;
    nh->scope    = nhm->nh_scope;
    nh->protocol = nhm->nh_protocol;
    nh->flags    = nhm->nh_flags;

    /* Walk NHA_* attributes. */
    unsigned int attrlen = (unsigned int)NL_NHA_PAYLOAD(nlh);
    for (const struct rtattr* rta = NL_NHA_RTA(nhm); RTA_OK(rta, attrlen);
         rta = RTA_NEXT(rta, attrlen))
    {
        /* Use only the low byte to strip any nested/byteorder flags. */
        const int rtype = rta->rta_type & 0x00ff;

        switch (rtype)
        {
        case NHA_ID:
            if (RTA_PAYLOAD(rta) >= sizeof(uint32_t))
                memcpy(&nh->id, RTA_DATA(rta), sizeof(uint32_t));
            break;

        case NHA_OIF:
            if (RTA_PAYLOAD(rta) >= sizeof(uint32_t))
            {
                memcpy(&nh->oif, RTA_DATA(rta), sizeof(uint32_t));
                if (nh->oif > 0)
                    if_indextoname(nh->oif, nh->oif_name);
            }
            break;

        case NHA_GATEWAY:
            if (nh->family == AF_INET &&
                RTA_PAYLOAD(rta) >= sizeof(struct in_addr))
            {
                inet_ntop(
                    AF_INET, RTA_DATA(rta), nh->gateway, sizeof(nh->gateway));
            }
            else if (nh->family == AF_INET6 &&
                     RTA_PAYLOAD(rta) >= sizeof(struct in6_addr))
            {
                inet_ntop(
                    AF_INET6, RTA_DATA(rta), nh->gateway, sizeof(nh->gateway));
            }
            break;

        case NHA_BLACKHOLE:
            nh->blackhole = 1;
            break;

        case NHA_FDB:
            nh->fdb = 1;
            break;

        case NHA_MASTER:
            if (RTA_PAYLOAD(rta) >= sizeof(uint32_t))
            {
                memcpy(&nh->master, RTA_DATA(rta), sizeof(uint32_t));
                if (nh->master > 0)
                    if_indextoname(nh->master, nh->master_name);
            }
            break;

        case NHA_GROUP: {
            /* Array of struct nl_nexthop_grp. */
            size_t grp_sz = sizeof(struct nl_nexthop_grp);
            size_t count = RTA_PAYLOAD(rta) / grp_sz;
            if (count > NETLINK_NEXTHOP_MAX_GROUP)
                count = NETLINK_NEXTHOP_MAX_GROUP;

            const struct nl_nexthop_grp* grp =
                (const struct nl_nexthop_grp*)RTA_DATA(rta);
            for (size_t i = 0; i < count; ++i)
            {
                nh->group[i].id          = grp[i].id;
                nh->group[i].weight      = grp[i].weight;
                nh->group[i].weight_high = grp[i].weight_high;
            }
            nh->group_count = (uint32_t)count;
            break;
        }

        case NHA_GROUP_TYPE:
            if (RTA_PAYLOAD(rta) >= sizeof(uint16_t))
                memcpy(&nh->group_type, RTA_DATA(rta), sizeof(uint16_t));
            break;

        case NHA_ENCAP_TYPE:
            if (RTA_PAYLOAD(rta) >= sizeof(uint16_t))
                memcpy(&nh->encap_type, RTA_DATA(rta), sizeof(uint16_t));
            break;

        default:
            break;
        }
    }
}

/**
 * @brief Parses one RTM_NEWNEXTHOP / RTM_DELNEXTHOP message and fires @p cb.
 *
 * @param nlh       Pointer to the netlink message header.
 * @param cb        Caller-supplied event callback.
 * @param user_data Forwarded to @p cb.
 */
static void nl_nexthop_dispatch(const struct nlmsghdr* nlh,
                                netlink_nexthop_cb_t cb,
                                void* user_data)
{
    if (nlh->nlmsg_len < NLMSG_LENGTH(sizeof(struct nl_nhmsg)))
        return;
    netlink_nexthop_event_t event;
    if (nlh->nlmsg_type == RTM_NEWNEXTHOP)
        event = (nlh->nlmsg_flags & NLM_F_REPLACE) ? NETLINK_NEXTHOP_CHANGED
                                                   : NETLINK_NEXTHOP_ADDED;
    else if (nlh->nlmsg_type == RTM_DELNEXTHOP)
        event = NETLINK_NEXTHOP_REMOVED;
    else
        return;
    netlink_nexthop_t nh;
    memset(&nh, 0, sizeof(nh));
    nl_nexthop_parse(nlh, &nh);
    cb(event, &nh, user_data);
}

/**
 * @brief Processes one receive-buffer's worth of netlink messages.
 *
 * @param buf       Buffer containing one or more netlink messages.
 * @param n         Number of valid bytes.
 * @param cb        Caller-supplied callback.
 * @param user_data Forwarded to nl_nexthop_dispatch().
 * @param done      Set to 1 when NLMSG_DONE is encountered; may be NULL.
 * @return Number of nexthop events dispatched, or -1 on NLMSG_ERROR.
 */
static int nl_nexthop_process_buf(const char* buf,
                                  ssize_t n,
                                  netlink_nexthop_cb_t cb,
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

        if (nlh->nlmsg_type == RTM_NEWNEXTHOP ||
            nlh->nlmsg_type == RTM_DELNEXTHOP)
        {
            nl_nexthop_dispatch(nlh, cb, user_data);
            ++count;
        }
    }

    return count;
}

/* ---------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

int netlink_nexthop_init(void)
{
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0)
        return -1;

    /* Bind with no legacy groups (RTNLGRP_NEXTHOP = 22 is within the
     * 32-bit nl_groups field, but we use NETLINK_ADD_MEMBERSHIP for clarity
     * and forward-compatibility). */
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

    /* Subscribe to the nexthop multicast group.  Non-fatal: in restricted
     * environments the dump still works and live events are simply absent. */
    int group = RTNLGRP_NEXTHOP;
    if (setsockopt(
            fd, SOL_NETLINK, NETLINK_ADD_MEMBERSHIP, &group, sizeof(group)) < 0)
    {
        fprintf(stderr,
                "netlink_nexthop: multicast subscribe failed "
                "(live events disabled): %s\n",
                strerror(errno));
    }

    return fd;
}

int netlink_nexthop_dump(int fd, netlink_nexthop_cb_t cb, void* user_data)
{
    /* Build RTM_GETNEXTHOP dump request. */
    struct
    {
        struct nlmsghdr nlh;
        struct nl_nhmsg nhm;
    } req;

    memset(&req, 0, sizeof(req));
    req.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(struct nl_nhmsg));
    req.nlh.nlmsg_type = RTM_GETNEXTHOP;
    req.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    req.nlh.nlmsg_seq = 1;
    req.nhm.nh_family = AF_UNSPEC;

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

        int r = nl_nexthop_process_buf(buf, n, cb, user_data, &done);
        if (r < 0)
            return -1;
        total += r;
    }

    return total;
}

int netlink_nexthop_run(int fd, netlink_nexthop_cb_t cb, void* user_data)
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

        if (nl_nexthop_process_buf(buf, n, cb, user_data, NULL) < 0)
            return -1;
    }
}

void netlink_nexthop_close(int fd)
{
    if (fd >= 0)
        close(fd);
}

/* ===========================================================================
 * Nexthop group management — create / update / delete
 * =========================================================================*/

/* Maximum wire size for one nexthop group management request:
 *   nlmsghdr(16) + nl_nhmsg(8) + NHA_ID attr(8) +
 *   NHA_GROUP attr(4 + 64*8=516) + NHA_GROUP_TYPE attr(8) = 560 → 1024 safe */
#define NL_MGMT_BUF 1024

/* Monotonically increasing sequence number for management requests.
 * Start high to avoid colliding with the monitor socket's sequence. */
static uint32_t g_mgmt_seq = 0x8000;

/**
 * @brief Opens a temporary NETLINK_ROUTE socket for a single management
 *        request.  Caller must close() the returned fd.
 *
 * @return Non-negative fd on success, -1 on error (errno set).
 */
static int nl_mgmt_open(void)
{
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0)
        return -1;

    struct sockaddr_nl sa;
    memset(&sa, 0, sizeof(sa));
    sa.nl_family = AF_NETLINK;

    if (bind(fd, (struct sockaddr*)&sa, sizeof(sa)) < 0)
    {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}

/**
 * @brief Appends an rtattr carrying a uint32 to an in-progress nlmsghdr.
 *
 * @param nlh   Message header (nlmsg_len is updated in place).
 * @param cap   Total buffer capacity.
 * @param type  Attribute type (NHA_*).
 * @param val   Value to embed.
 * @return 0 on success, -1 when the buffer would overflow.
 */
static int nl_add_u32(struct nlmsghdr* nlh,
                      size_t cap,
                      unsigned short type,
                      uint32_t val)
{
    unsigned int alen = (unsigned int)RTA_LENGTH(sizeof(uint32_t));
    if (NLMSG_ALIGN(nlh->nlmsg_len) + RTA_ALIGN(alen) > cap)
        return -1;
    struct rtattr* rta =
        (struct rtattr*)((char*)nlh + NLMSG_ALIGN(nlh->nlmsg_len));
    rta->rta_type = type;
    rta->rta_len = (unsigned short)alen;
    memcpy(RTA_DATA(rta), &val, sizeof(val));
    nlh->nlmsg_len =
        (uint32_t)(NLMSG_ALIGN(nlh->nlmsg_len) + RTA_ALIGN(alen));
    return 0;
}

/**
 * @brief Appends an rtattr carrying a uint16 to an in-progress nlmsghdr.
 */
static int nl_add_u16(struct nlmsghdr* nlh,
                      size_t cap,
                      unsigned short type,
                      uint16_t val)
{
    unsigned int alen = (unsigned int)RTA_LENGTH(sizeof(uint16_t));
    if (NLMSG_ALIGN(nlh->nlmsg_len) + RTA_ALIGN(alen) > cap)
        return -1;
    struct rtattr* rta =
        (struct rtattr*)((char*)nlh + NLMSG_ALIGN(nlh->nlmsg_len));
    rta->rta_type = type;
    rta->rta_len = (unsigned short)alen;
    memcpy(RTA_DATA(rta), &val, sizeof(val));
    nlh->nlmsg_len =
        (uint32_t)(NLMSG_ALIGN(nlh->nlmsg_len) + RTA_ALIGN(alen));
    return 0;
}

/**
 * @brief Appends an rtattr carrying raw bytes to an in-progress nlmsghdr.
 */
static int nl_add_raw(struct nlmsghdr* nlh,
                      size_t cap,
                      unsigned short type,
                      const void* data,
                      size_t dlen)
{
    unsigned int alen = (unsigned int)RTA_LENGTH(dlen);
    if (NLMSG_ALIGN(nlh->nlmsg_len) + RTA_ALIGN(alen) > cap)
        return -1;
    struct rtattr* rta =
        (struct rtattr*)((char*)nlh + NLMSG_ALIGN(nlh->nlmsg_len));
    rta->rta_type = type;
    rta->rta_len = (unsigned short)alen;
    if (dlen > 0)
        memcpy(RTA_DATA(rta), data, dlen);
    nlh->nlmsg_len =
        (uint32_t)(NLMSG_ALIGN(nlh->nlmsg_len) + RTA_ALIGN(alen));
    return 0;
}

/**
 * @brief Reads the kernel ACK and (if present) an echoed RTM_NEWNEXTHOP.
 *
 * For RTM_NEWNEXTHOP requests with @c NLM_F_ECHO the kernel first sends the
 * newly created object (so we can recover the NHA_ID), then an NLMSG_ERROR
 * with @c error == 0 as a positive ACK.
 *
 * @param fd      Netlink socket.
 * @param seq     Expected sequence number.
 * @param out_id  If non-NULL, filled with the NHA_ID parsed from the echo.
 * @return 0 on success, -1 on error (errno is set to the kernel error code).
 */
static int nl_recv_ack(int fd, uint32_t seq, uint32_t* out_id)
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

        const struct nlmsghdr* nlh = (const struct nlmsghdr*)buf;
        for (; NLMSG_OK(nlh, (unsigned int)n); nlh = NLMSG_NEXT(nlh, n))
        {
            if (nlh->nlmsg_seq != seq)
                continue;

            if (nlh->nlmsg_type == NLMSG_ERROR)
            {
                const struct nlmsgerr* err =
                    (const struct nlmsgerr*)NLMSG_DATA(nlh);
                if (err->error != 0)
                {
                    errno = -err->error;
                    return -1;
                }
                return 0; /* positive ACK */
            }

            /* Echoed RTM_NEWNEXTHOP: extract NHA_ID. */
            if (nlh->nlmsg_type == RTM_NEWNEXTHOP && out_id != NULL)
            {
                if (nlh->nlmsg_len < NLMSG_LENGTH(sizeof(struct nl_nhmsg)))
                    continue;
                unsigned int attrlen =
                    (unsigned int)NL_NHA_PAYLOAD(nlh);
                const struct rtattr* rta =
                    NL_NHA_RTA((const struct nl_nhmsg*)NLMSG_DATA(nlh));
                for (; RTA_OK(rta, attrlen); rta = RTA_NEXT(rta, attrlen))
                {
                    if ((rta->rta_type & 0x00ff) == NHA_ID &&
                        RTA_PAYLOAD(rta) >= sizeof(uint32_t))
                    {
                        memcpy(out_id, RTA_DATA(rta), sizeof(uint32_t));
                        break;
                    }
                }
            }
        }
    }
}

/**
 * @brief Builds and populates the NHA_GROUP + NHA_GROUP_TYPE attributes in
 *        @p buf.
 *
 * @return 0 on success, -1 on buffer overflow.
 */
static int nl_build_group_attrs(struct nlmsghdr* nlh,
                                size_t cap,
                                const netlink_nexthop_grp_t* members,
                                uint32_t count)
{
    /* Build the kernel group array on the stack. */
    struct nl_nexthop_grp grp[NETLINK_NEXTHOP_MAX_GROUP];
    if (count > NETLINK_NEXTHOP_MAX_GROUP)
        count = NETLINK_NEXTHOP_MAX_GROUP;
    for (uint32_t i = 0; i < count; ++i)
    {
        grp[i].id = members[i].id;
        grp[i].weight = members[i].weight;
        grp[i].weight_high = members[i].weight_high;
        grp[i].resvd = 0;
    }

    if (nl_add_raw(nlh, cap, NHA_GROUP,
                   grp, count * sizeof(struct nl_nexthop_grp)) < 0)
        return -1;

    /* NHA_GROUP_TYPE = 0 → NEXTHOP_GRP_TYPE_MPATH (ECMP). */
    uint16_t gtype = 0;
    if (nl_add_u16(nlh, cap, NHA_GROUP_TYPE, gtype) < 0)
        return -1;

    return 0;
}

/* ---------------------------------------------------------------------------
 * Public: netlink_nexthop_group_create
 * ------------------------------------------------------------------------- */

uint32_t netlink_nexthop_group_create(const netlink_nexthop_grp_t* members,
                                      uint32_t count)
{
    if (!members || count == 0)
    {
        errno = EINVAL;
        return 0;
    }

    int fd = nl_mgmt_open();
    if (fd < 0)
        return 0;

    char buf[NL_MGMT_BUF];
    memset(buf, 0, sizeof(buf));

    const uint32_t seq = ++g_mgmt_seq;

    struct nlmsghdr* nlh = (struct nlmsghdr*)buf;
    nlh->nlmsg_type = RTM_NEWNEXTHOP;
    /* NLM_F_ECHO: kernel echoes the created object so we get the NHA_ID. */
    nlh->nlmsg_flags =
        NLM_F_REQUEST | NLM_F_CREATE | NLM_F_ECHO | NLM_F_ACK;
    nlh->nlmsg_seq = seq;
    nlh->nlmsg_len = NLMSG_LENGTH(sizeof(struct nl_nhmsg));

    /* nhmsg header — family AF_UNSPEC for a group (no gateway/oif). */
    struct nl_nhmsg* nhm = (struct nl_nhmsg*)NLMSG_DATA(nlh);
    nhm->nh_family = AF_UNSPEC;

    /* No NHA_ID: let the kernel assign one; we recover it from the echo. */

    if (nl_build_group_attrs(nlh, sizeof(buf), members, count) < 0)
    {
        close(fd);
        errno = ENOBUFS;
        return 0;
    }

    if (send(fd, buf, nlh->nlmsg_len, 0) < 0)
    {
        int saved = errno;
        close(fd);
        errno = saved;
        return 0;
    }

    uint32_t assigned_id = 0;
    if (nl_recv_ack(fd, seq, &assigned_id) < 0)
    {
        int saved = errno;
        close(fd);
        errno = saved;
        return 0;
    }

    close(fd);
    return assigned_id;
}

/* ---------------------------------------------------------------------------
 * Public: netlink_nexthop_group_update
 * ------------------------------------------------------------------------- */

int netlink_nexthop_group_update(uint32_t id,
                                 const netlink_nexthop_grp_t* members,
                                 uint32_t count)
{
    if (!members || count == 0 || id == 0)
    {
        errno = EINVAL;
        return -1;
    }

    int fd = nl_mgmt_open();
    if (fd < 0)
        return -1;

    char buf[NL_MGMT_BUF];
    memset(buf, 0, sizeof(buf));

    const uint32_t seq = ++g_mgmt_seq;

    struct nlmsghdr* nlh = (struct nlmsghdr*)buf;
    nlh->nlmsg_type = RTM_NEWNEXTHOP;
    nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_REPLACE | NLM_F_ACK;
    nlh->nlmsg_seq = seq;
    nlh->nlmsg_len = NLMSG_LENGTH(sizeof(struct nl_nhmsg));

    struct nl_nhmsg* nhm = (struct nl_nhmsg*)NLMSG_DATA(nlh);
    nhm->nh_family = AF_UNSPEC;

    /* Specify the existing group ID to replace. */
    if (nl_add_u32(nlh, sizeof(buf), NHA_ID, id) < 0 ||
        nl_build_group_attrs(nlh, sizeof(buf), members, count) < 0)
    {
        close(fd);
        errno = ENOBUFS;
        return -1;
    }

    if (send(fd, buf, nlh->nlmsg_len, 0) < 0)
    {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }

    int rc = nl_recv_ack(fd, seq, NULL);
    int saved = errno;
    close(fd);
    errno = saved;
    return rc;
}

/* ---------------------------------------------------------------------------
 * Public: netlink_nexthop_group_delete
 * ------------------------------------------------------------------------- */

int netlink_nexthop_group_delete(uint32_t id)
{
    if (id == 0)
    {
        errno = EINVAL;
        return -1;
    }

    int fd = nl_mgmt_open();
    if (fd < 0)
        return -1;

    char buf[NL_MGMT_BUF];
    memset(buf, 0, sizeof(buf));

    const uint32_t seq = ++g_mgmt_seq;

    struct nlmsghdr* nlh = (struct nlmsghdr*)buf;
    nlh->nlmsg_type = RTM_DELNEXTHOP;
    nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    nlh->nlmsg_seq = seq;
    nlh->nlmsg_len = NLMSG_LENGTH(sizeof(struct nl_nhmsg));

    struct nl_nhmsg* nhm = (struct nl_nhmsg*)NLMSG_DATA(nlh);
    nhm->nh_family = AF_UNSPEC;

    if (nl_add_u32(nlh, sizeof(buf), NHA_ID, id) < 0)
    {
        close(fd);
        errno = ENOBUFS;
        return -1;
    }

    if (send(fd, buf, nlh->nlmsg_len, 0) < 0)
    {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }

    int rc = nl_recv_ack(fd, seq, NULL);
    int saved = errno;
    close(fd);
    errno = saved;
    return rc;
}

/* ---------------------------------------------------------------------------
 * Public: netlink_nexthop_delete  (generic — works for single and group objects)
 * ------------------------------------------------------------------------- */

int netlink_nexthop_delete(uint32_t id)
{
    return netlink_nexthop_group_delete(id);
}

/* ---------------------------------------------------------------------------
 * Public: netlink_nexthop_single_create
 *
 * Creates a single-path (non-group) SRA-owned nexthop with the given gateway
 * address and output interface.  The kernel assigns a fresh NHA_ID which is
 * returned via NLM_F_ECHO.
 * ------------------------------------------------------------------------- */

uint32_t netlink_nexthop_single_create(uint8_t     family,
                                       const char* gateway,
                                       uint32_t    oif)
{
    if (!gateway || gateway[0] == '\0' || oif == 0)
    {
        errno = EINVAL;
        return 0;
    }

    int fd = nl_mgmt_open();
    if (fd < 0)
        return 0;

    char buf[NL_MGMT_BUF];
    memset(buf, 0, sizeof(buf));
    const uint32_t seq = ++g_mgmt_seq;

    struct nlmsghdr* nlh = (struct nlmsghdr*)buf;
    nlh->nlmsg_type  = RTM_NEWNEXTHOP;
    nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_CREATE | NLM_F_ECHO | NLM_F_ACK;
    nlh->nlmsg_seq   = seq;
    nlh->nlmsg_len   = NLMSG_LENGTH(sizeof(struct nl_nhmsg));

    struct nl_nhmsg* nhm2 = (struct nl_nhmsg*)NLMSG_DATA(nlh);
    nhm2->nh_family   = family;
    nhm2->nh_protocol = RTPROT_STATIC;

    if (nl_add_u32(nlh, sizeof(buf), NHA_OIF, oif) < 0)
    {
        close(fd); errno = ENOBUFS; return 0;
    }

    if (family == AF_INET)
    {
        struct in_addr gw;
        if (inet_pton(AF_INET, gateway, &gw) != 1)
        {
            close(fd); errno = EINVAL; return 0;
        }
        if (nl_add_raw(nlh, sizeof(buf), NHA_GATEWAY, &gw, sizeof(gw)) < 0)
        {
            close(fd); errno = ENOBUFS; return 0;
        }
    }
    else
    {
        struct in6_addr gw6;
        if (inet_pton(AF_INET6, gateway, &gw6) != 1)
        {
            close(fd); errno = EINVAL; return 0;
        }
        if (nl_add_raw(nlh, sizeof(buf), NHA_GATEWAY, &gw6, sizeof(gw6)) < 0)
        {
            close(fd); errno = ENOBUFS; return 0;
        }
    }

    if (send(fd, buf, nlh->nlmsg_len, 0) < 0)
    {
        int saved = errno; close(fd); errno = saved; return 0;
    }

    uint32_t assigned_id = 0;
    if (nl_recv_ack(fd, seq, &assigned_id) < 0)
    {
        int saved = errno; close(fd); errno = saved; return 0;
    }
    close(fd);
    return assigned_id;
}

/* ---------------------------------------------------------------------------
 * Public: netlink_nexthop_get_by_id
 *
 * Sends RTM_GETNEXTHOP for a specific nhid (no NLM_F_DUMP) and parses the
 * kernel reply into @p out.  Returns 0 on success, -1 on error.
 * ------------------------------------------------------------------------- */

int netlink_nexthop_get_by_id(uint32_t nhid, netlink_nexthop_t* out)
{
    if (nhid == 0 || !out)
    {
        errno = EINVAL;
        return -1;
    }

    int fd = nl_mgmt_open();
    if (fd < 0)
        return -1;

    char buf[NL_MGMT_BUF];
    memset(buf, 0, sizeof(buf));
    const uint32_t seq = ++g_mgmt_seq;

    struct nlmsghdr* nlh = (struct nlmsghdr*)buf;
    nlh->nlmsg_type  = RTM_GETNEXTHOP;
    nlh->nlmsg_flags = NLM_F_REQUEST;
    nlh->nlmsg_seq   = seq;
    nlh->nlmsg_len   = NLMSG_LENGTH(sizeof(struct nl_nhmsg));

    struct nl_nhmsg* nhm3 = (struct nl_nhmsg*)NLMSG_DATA(nlh);
    nhm3->nh_family = AF_UNSPEC;

    if (nl_add_u32(nlh, sizeof(buf), NHA_ID, nhid) < 0)
    {
        close(fd); errno = ENOBUFS; return -1;
    }

    if (send(fd, buf, nlh->nlmsg_len, 0) < 0)
    {
        int saved = errno; close(fd); errno = saved; return -1;
    }

    int rc = -1;
    errno  = ENOENT;

    for (;;)
    {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n < 0)
        {
            if (errno == EINTR) continue;
            break;
        }
        int done = 0;
        const struct nlmsghdr* rnlh = (const struct nlmsghdr*)buf;
        for (; NLMSG_OK(rnlh, (unsigned int)n) && !done;
               rnlh = NLMSG_NEXT(rnlh, n))
        {
            if (rnlh->nlmsg_seq != seq)
                continue;
            if (rnlh->nlmsg_type == RTM_NEWNEXTHOP &&
                rnlh->nlmsg_len >= NLMSG_LENGTH(sizeof(struct nl_nhmsg)))
            {
                memset(out, 0, sizeof(*out));
                nl_nexthop_parse(rnlh, out);
                rc = 0; done = 1;
            }
            else if (rnlh->nlmsg_type == NLMSG_ERROR)
            {
                const struct nlmsgerr* err =
                    (const struct nlmsgerr*)NLMSG_DATA(rnlh);
                errno = (err->error != 0) ? -err->error : ENOENT;
                done = 1;
            }
            else if (rnlh->nlmsg_type == NLMSG_DONE)
            {
                done = 1;
            }
        }
        break;
    }

    close(fd);
    return rc;
}
