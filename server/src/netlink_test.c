/**
 * @file server/src/netlink_test.c
 * @brief Manual smoke-test for the netlink /32 route listener.
 *
 * Starts a netlink listener and prints a human-readable log line to stdout
 * for every IPv4 /32 route event the kernel delivers.  Run it in one
 * terminal, then exercise it from another:
 *
 * @code
 *   # add a host route – you should see "ADDED 3.3.3.3/32 ..."
 *   sudo ip route add 3.3.3.3/32 via 192.168.0.1
 *
 *   # change the nexthop – you should see "ADDED 3.3.3.3/32 ..." again
 *   sudo ip route replace 3.3.3.3/32 via 192.168.0.2
 *
 *   # remove the route – you should see "REMOVED 3.3.3.3/32 ..."
 *   sudo ip route del 3.3.3.3/32
 * @endcode
 *
 * Press Ctrl-C to stop (SIGINT closes the netlink socket, causing
 * netlink_run() to return and the program to exit cleanly).
 *
 * Build:
 * @code
 *   cmake --build build --target netlink_test
 * @endcode
 *
 * @version 1.0
 */

#include "server/netlink.h"

#include <arpa/inet.h>   /* inet_ntop */
#include <linux/rtnetlink.h> /* RTPROT_* constants */
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* File-scope fd so the signal handler can close it. */
static volatile int g_nl_fd = -1;

/* ---------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */

/**
 * @brief Returns a short, human-readable name for an rtnetlink protocol value.
 *
 * Covers the protocols FRR zebra redistributes (Zebra itself, OSPF, BGP, ISIS,
 * RIP, EIGRP) plus common kernel-assigned values.  Falls back to a numeric
 * string for anything unrecognised.
 *
 * @param protocol  The rtm_protocol value from the route message.
 * @param buf       Caller-supplied scratch buffer for the numeric fallback.
 * @param buflen    Length of @p buf in bytes.
 * @return          A NUL-terminated string; either a literal constant or
 *                  the decimal value rendered into @p buf.
 */
static const char *protocol_name(uint8_t protocol, char *buf, size_t buflen)
{
    switch (protocol)
    {
    case RTPROT_UNSPEC:    return "unspec";
    case RTPROT_REDIRECT:  return "redirect";
    case RTPROT_KERNEL:    return "kernel";
    case RTPROT_BOOT:      return "boot";
    case RTPROT_STATIC:    return "static";
    case RTPROT_GATED:     return "gated";
    case RTPROT_RA:        return "ra";
    case RTPROT_MRT:       return "mrt";
    case RTPROT_ZEBRA:     return "zebra";
    case RTPROT_BIRD:      return "bird";
    case RTPROT_DNROUTED:  return "dnrouted";
    case RTPROT_XORP:      return "xorp";
    case RTPROT_NTK:       return "ntk";
    case RTPROT_DHCP:      return "dhcp";
    case RTPROT_MROUTED:   return "mrouted";
    case RTPROT_BABEL:     return "babel";
    case RTPROT_BGP:       return "bgp";
    case RTPROT_ISIS:      return "isis";
    case RTPROT_OSPF:      return "ospf";
    case RTPROT_RIP:       return "rip";
    case RTPROT_EIGRP:     return "eigrp";
    default:
        snprintf(buf, buflen, "%u", (unsigned)protocol);
        return buf;
    }
}

/**
 * @brief Returns a UTC timestamp string of the form "HH:MM:SS.mmm".
 *
 * @param buf     Caller-supplied buffer.
 * @param buflen  Length of @p buf.
 * @return        @p buf, always NUL-terminated.
 */
static const char *timestamp(char *buf, size_t buflen)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    struct tm tm_utc;
    gmtime_r(&ts.tv_sec, &tm_utc);

    snprintf(buf, buflen, "%02d:%02d:%02d.%03ld",
             tm_utc.tm_hour,
             tm_utc.tm_min,
             tm_utc.tm_sec,
             ts.tv_nsec / 1000000L);
    return buf;
}

/* ---------------------------------------------------------------------------
 * Route event callback
 * ------------------------------------------------------------------------- */

/**
 * @brief Prints one line per /32 route event to stdout.
 *
 * Output format (all on one line):
 * @code
 *   HH:MM:SS.mmm  [ADDED  |REMOVED]  3.3.3.3/32  via 192.168.0.1  dev eth0
 *                 metric 20  table 254  proto zebra
 * @endcode
 *
 * @param event      NETLINK_ROUTE_ADDED or NETLINK_ROUTE_REMOVED.
 * @param route      Pointer to the parsed route descriptor.
 * @param user_data  Unused; pass NULL.
 */
static void on_route_event(netlink_event_t          event,
                           const netlink_route32_t *route,
                           void                    *user_data)
{
    (void)user_data;

    /* Timestamp */
    char ts_buf[16];
    timestamp(ts_buf, sizeof(ts_buf));

    /* Destination */
    char dst_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &route->dst, dst_str, sizeof(dst_str));

    /* Gateway (optional – omit line segment when absent) */
    char gw_str[INET_ADDRSTRLEN];
    const char *gw_part = "";
    char gw_segment[INET_ADDRSTRLEN + 6]; /* "via <addr>" */
    gw_segment[0] = '\0';
    if (route->gateway.s_addr != 0)
    {
        inet_ntop(AF_INET, &route->gateway, gw_str, sizeof(gw_str));
        snprintf(gw_segment, sizeof(gw_segment), "via %s  ", gw_str);
        gw_part = gw_segment;
    }

    /* Interface (optional) */
    char dev_segment[IF_NAMESIZE + 6]; /* "dev <name>" */
    dev_segment[0] = '\0';
    if (route->ifname[0] != '\0')
    {
        snprintf(dev_segment, sizeof(dev_segment), "dev %s  ", route->ifname);
    }

    /* Protocol */
    char proto_num_buf[8];
    const char *proto = protocol_name(route->protocol,
                                       proto_num_buf,
                                       sizeof(proto_num_buf));

    printf("%s  %-7s  %s/32  %s%smetric %-5u  table %-5u  proto %s\n",
           ts_buf,
           event == NETLINK_ROUTE_ADDED ? "ADDED" : "REMOVED",
           dst_str,
           gw_part,
           dev_segment,
           route->metric,
           route->table,
           proto);

    fflush(stdout);
}

/* ---------------------------------------------------------------------------
 * Signal handling
 * ------------------------------------------------------------------------- */

/**
 * @brief SIGINT / SIGTERM handler: closes the netlink socket.
 *
 * Closing the fd causes netlink_run()'s recv() to return an error, which
 * makes the loop exit cleanly.
 */
static void sig_handler(int signo)
{
    (void)signo;
    if (g_nl_fd >= 0)
    {
        netlink_close(g_nl_fd);
        g_nl_fd = -1;
    }
}

/* ---------------------------------------------------------------------------
 * Entry point
 * ------------------------------------------------------------------------- */

int main(void)
{
    /* Register signal handlers before opening the socket. */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* Open netlink socket. */
    int fd = netlink_init();
    if (fd < 0)
    {
        perror("netlink_init");
        return 1;
    }
    g_nl_fd = fd;

    printf("Listening for IPv4 /32 route events (Ctrl-C to stop)...\n");
    printf("  Try: sudo ip route add 3.3.3.3/32 via 192.168.0.1\n");
    printf("       sudo ip route del 3.3.3.3/32\n\n");
    fflush(stdout);

    /* Block until socket closed or error. */
    int rc = netlink_run(fd, on_route_event, NULL);

    /* If we closed fd in the signal handler g_nl_fd is already -1. */
    netlink_close(g_nl_fd);

    if (rc < 0)
    {
        /* EBADF is the expected errno when we closed the fd ourselves. */
        if (g_nl_fd == -1)
        {
            printf("\nStopped.\n");
            return 0;
        }
        perror("netlink_run");
        return 1;
    }

    printf("\nStopped.\n");
    return 0;
}
