/**
 * @file client/src/routing.cpp
 * @brief Linux kernel routing-table and network-interface manager
 * implementation.
 *
 * All netlink communication uses a single @c NETLINK_ROUTE socket kept open
 * for the lifetime of the RoutingManager object.  Requests are serialised with
 * an internal mutex; a monotonically increasing sequence number is used to
 * correlate requests with responses.
 *
 * Netlink message layout used throughout:
 * @code
 *   [struct nlmsghdr][payload struct (rtmsg / rtgenmsg)][rtattr …]
 * @endcode
 *
 * @version 1.0
 */

#include "client/routing.hpp"

#include <arpa/inet.h>
#include <asm/types.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <format>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace sra
{

// ---------------------------------------------------------------------------
// Module-private helpers
// ---------------------------------------------------------------------------

namespace
{

/// Size of the receive buffer used for all netlink dump operations.
constexpr std::size_t kNlBufSize = 32768;

/// Maximum size of the transmit buffer for a single route-mutation request.
constexpr std::size_t kNlReqSize = 1024;

/**
 * @brief Appends an @c rtattr to an in-progress netlink message.
 *
 * Extends the buffer beginning at @p nlh by one attribute of @p type carrying
 * @p alen bytes of @p data, updating @c nlh->nlmsg_len.
 *
 * @param nlh     Base netlink header whose @c nlmsg_len is updated.
 * @param maxLen  Total capacity of the buffer holding @p nlh.
 * @param type    Attribute type constant (e.g. @c RTA_DST).
 * @param data    Attribute payload (may be @c nullptr when @p alen is 0).
 * @param alen    Payload length in bytes.
 * @return Pointer to the new attribute on success, @c nullptr on overflow.
 */
rtattr* addRtAttr(
    nlmsghdr* nlh, std::size_t maxLen, int type, const void* data, int alen)
{
    int len = static_cast<int>(RTA_LENGTH(static_cast<unsigned>(alen)));
    if (NLMSG_ALIGN(nlh->nlmsg_len) + static_cast<unsigned>(RTA_ALIGN(len)) >
        maxLen)
    {
        return nullptr;
    }
    auto* rta = reinterpret_cast<rtattr*>(reinterpret_cast<char*>(nlh) +
                                          NLMSG_ALIGN(nlh->nlmsg_len));
    rta->rta_type = static_cast<unsigned short>(type);
    rta->rta_len = static_cast<unsigned short>(len);
    if (alen > 0 && data != nullptr)
    {
        std::memcpy(RTA_DATA(rta), data, static_cast<std::size_t>(alen));
    }
    nlh->nlmsg_len = static_cast<uint32_t>(
        NLMSG_ALIGN(nlh->nlmsg_len) + static_cast<unsigned>(RTA_ALIGN(len)));
    return rta;
}

/**
 * @brief Parses a CIDR string into a binary address and a prefix length.
 *
 * Accepts forms like @c "192.168.1.0/24", @c "2001:db8::/32", or a bare
 * address without a @c /N suffix (in which case the full host prefix is
 * assumed: 32 for IPv4, 128 for IPv6).
 *
 * @param cidr    Input CIDR string.
 * @param family  @c AF_INET or @c AF_INET6.
 * @return A pair of (16-byte address buffer, prefix length), or an error
 *         string when parsing fails.
 */
std::expected<std::pair<std::array<uint8_t, 16>, uint8_t>, std::string>
parseCidr(const std::string& cidr, int family)
{
    std::string addrStr = cidr;
    uint8_t prefixLen{0};
    const int maxPfx = (family == AF_INET6) ? 128 : 32;

    auto slashPos = cidr.find('/');
    if (slashPos != std::string::npos)
    {
        addrStr = cidr.substr(0, slashPos);
        try
        {
            int pl = std::stoi(cidr.substr(slashPos + 1));
            if (pl < 0 || pl > maxPfx)
            {
                return std::unexpected(std::format(
                    "prefix length {} out of range in '{}'", pl, cidr));
            }
            prefixLen = static_cast<uint8_t>(pl);
        }
        catch (...)
        {
            return std::unexpected(
                std::format("cannot parse prefix length in '{}'", cidr));
        }
    }
    else
    {
        prefixLen = static_cast<uint8_t>(maxPfx);
    }

    std::array<uint8_t, 16> buf{};
    if (inet_pton(family, addrStr.c_str(), buf.data()) != 1)
    {
        return std::unexpected(std::format("inet_pton('{}') failed", addrStr));
    }
    return std::make_pair(buf, prefixLen);
}

/**
 * @brief Converts a binary IP address to a text string.
 *
 * @param addr    Pointer to 4 (IPv4) or 16 (IPv6) bytes.
 * @param family  @c AF_INET or @c AF_INET6.
 * @return Dotted-decimal or colon-hex text; @c "<invalid>" on error.
 */
std::string addrToString(const void* addr, int family)
{
    char buf[INET6_ADDRSTRLEN]{};
    if (inet_ntop(family, addr, buf, sizeof(buf)) == nullptr)
    {
        return "<invalid>";
    }
    return {buf};
}

/**
 * @brief Formats 6 bytes of MAC address data as @c "xx:xx:xx:xx:xx:xx".
 *
 * @param data  Pointer to the raw MAC bytes.
 * @param len   Length of @p data; returns empty string when not 6.
 * @return Formatted MAC string or empty string.
 */
std::string macToString(const uint8_t* data, int len)
{
    if (len != 6)
    {
        return {};
    }
    return std::format("{:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}",
                       data[0],
                       data[1],
                       data[2],
                       data[3],
                       data[4],
                       data[5]);
}

/**
 * @brief Resolves a kernel interface index to its name string.
 *
 * @param index  Interface index.
 * @return Interface name, or empty string when the index is not found.
 */
std::string ifIndexToName(uint32_t index)
{
    char name[IF_NAMESIZE]{};
    if (if_indextoname(static_cast<unsigned>(index), name) == nullptr)
    {
        return {};
    }
    return {name};
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

RoutingManager::RoutingManager()
{
    fd_ = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd_ < 0)
    {
        throw std::system_error(
            errno, std::generic_category(), "RoutingManager: socket");
    }

    sockaddr_nl sa{};
    sa.nl_family = AF_NETLINK;
    if (bind(fd_, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) < 0)
    {
        const int saved = errno;
        close(fd_);
        fd_ = -1;
        throw std::system_error(
            saved, std::generic_category(), "RoutingManager: bind");
    }
}

RoutingManager::~RoutingManager()
{
    if (fd_ >= 0)
    {
        close(fd_);
    }
}

// ---------------------------------------------------------------------------
// Private: recvAll
// ---------------------------------------------------------------------------

std::expected<void, std::string>
RoutingManager::recvAll(uint32_t seq,
                        const std::function<void(nlmsghdr*)>& handler) const
{
    std::vector<char> buf(kNlBufSize);

    while (true)
    {
        ssize_t bytes = recv(fd_, buf.data(), buf.size(), 0);
        if (bytes < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return std::unexpected(
                std::format("recv: {}", std::strerror(errno)));
        }

        int n = static_cast<int>(bytes);
        auto* nlh = reinterpret_cast<nlmsghdr*>(buf.data());

        for (; NLMSG_OK(nlh, n); nlh = NLMSG_NEXT(nlh, n))
        {
            // Ignore stale responses from earlier requests.
            if (nlh->nlmsg_seq != seq)
            {
                continue;
            }

            if (nlh->nlmsg_type == NLMSG_DONE)
            {
                return {};
            }

            if (nlh->nlmsg_type == NLMSG_ERROR)
            {
                const auto* err =
                    reinterpret_cast<const nlmsgerr*>(NLMSG_DATA(nlh));
                if (err->error != 0)
                {
                    return std::unexpected(std::format(
                        "netlink error: {}", std::strerror(-err->error)));
                }
                // err->error == 0: this is a positive ACK.
                return {};
            }

            handler(nlh);
        }

        // Non-multi-part responses end after the first recv(); dumps set
        // NLM_F_MULTI and are terminated by NLMSG_DONE above.
        if (!(reinterpret_cast<nlmsghdr*>(buf.data())->nlmsg_flags &
              NLM_F_MULTI))
        {
            break;
        }
    }
    return {};
}

// ---------------------------------------------------------------------------
// Private: dumpLinks
// ---------------------------------------------------------------------------

std::expected<std::vector<NetworkInterface>, std::string>
RoutingManager::dumpLinks() const
{
    struct
    {
        nlmsghdr nlh;
        rtgenmsg gen;
    } req{};

    const uint32_t mySeq = seq_++;
    req.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(req.gen));
    req.nlh.nlmsg_type = RTM_GETLINK;
    req.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    req.nlh.nlmsg_seq = mySeq;
    req.gen.rtgen_family = AF_UNSPEC;

    if (send(fd_, &req, req.nlh.nlmsg_len, 0) < 0)
    {
        return std::unexpected(
            std::format("RTM_GETLINK send: {}", std::strerror(errno)));
    }

    std::vector<NetworkInterface> ifaces;

    auto result = recvAll(mySeq, [&](nlmsghdr* nlh) {
        if (nlh->nlmsg_type != RTM_NEWLINK)
        {
            return;
        }

        const auto* ifi = reinterpret_cast<const ifinfomsg*>(NLMSG_DATA(nlh));
        int attrLen = static_cast<int>(IFLA_PAYLOAD(nlh));
        const auto* rta = IFLA_RTA(ifi);

        NetworkInterface iface{};
        iface.index = static_cast<uint32_t>(ifi->ifi_index);
        iface.flags = static_cast<uint32_t>(ifi->ifi_flags);

        for (; RTA_OK(rta, attrLen); rta = RTA_NEXT(rta, attrLen))
        {
            switch (rta->rta_type)
            {
            case IFLA_IFNAME:
                iface.name = reinterpret_cast<const char*>(RTA_DATA(rta));
                break;

            case IFLA_MTU: {
                uint32_t mtu{};
                std::memcpy(&mtu, RTA_DATA(rta), sizeof(mtu));
                iface.mtu = mtu;
                break;
            }

            case IFLA_ADDRESS:
                iface.hwAddress =
                    macToString(reinterpret_cast<const uint8_t*>(RTA_DATA(rta)),
                                static_cast<int>(RTA_PAYLOAD(rta)));
                break;

            default:
                break;
            }
        }

        if (!iface.name.empty())
        {
            ifaces.push_back(std::move(iface));
        }
    });

    if (!result)
    {
        return std::unexpected(result.error());
    }
    return ifaces;
}

// ---------------------------------------------------------------------------
// Private: dumpAddrs
// ---------------------------------------------------------------------------

std::expected<void, std::string>
RoutingManager::dumpAddrs(std::vector<NetworkInterface>& ifaces) const
{
    struct
    {
        nlmsghdr nlh;
        rtgenmsg gen;
    } req{};

    const uint32_t mySeq = seq_++;
    req.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(req.gen));
    req.nlh.nlmsg_type = RTM_GETADDR;
    req.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    req.nlh.nlmsg_seq = mySeq;
    req.gen.rtgen_family = AF_UNSPEC;

    if (send(fd_, &req, req.nlh.nlmsg_len, 0) < 0)
    {
        return std::unexpected(
            std::format("RTM_GETADDR send: {}", std::strerror(errno)));
    }

    return recvAll(mySeq, [&](nlmsghdr* nlh) {
        if (nlh->nlmsg_type != RTM_NEWADDR)
        {
            return;
        }

        const auto* ifa = reinterpret_cast<const ifaddrmsg*>(NLMSG_DATA(nlh));
        if (ifa->ifa_family != AF_INET && ifa->ifa_family != AF_INET6)
        {
            return;
        }

        int attrLen = static_cast<int>(IFA_PAYLOAD(nlh));
        const auto* rta = IFA_RTA(ifa);

        // IFA_LOCAL holds the local address on point-to-point links;
        // IFA_ADDRESS holds it on broadcast links.  Prefer IFA_LOCAL.
        std::string localAddr;
        std::string peerAddr;

        for (; RTA_OK(rta, attrLen); rta = RTA_NEXT(rta, attrLen))
        {
            if (rta->rta_type == IFA_LOCAL)
            {
                localAddr = addrToString(RTA_DATA(rta),
                                         static_cast<int>(ifa->ifa_family));
            }
            else if (rta->rta_type == IFA_ADDRESS)
            {
                peerAddr = addrToString(RTA_DATA(rta),
                                        static_cast<int>(ifa->ifa_family));
            }
        }

        const std::string& chosen = !localAddr.empty() ? localAddr : peerAddr;
        if (chosen.empty())
        {
            return;
        }

        for (auto& iface : ifaces)
        {
            if (iface.index == ifa->ifa_index)
            {
                iface.addresses.push_back(
                    InterfaceAddress{chosen,
                                     ifa->ifa_prefixlen,
                                     static_cast<int>(ifa->ifa_family)});
                break;
            }
        }
    });
}

// ---------------------------------------------------------------------------
// Public: listInterfaces
// ---------------------------------------------------------------------------

std::expected<std::vector<NetworkInterface>, std::string>
RoutingManager::listInterfaces() const
{
    std::lock_guard lock(mtx_);

    auto ifaces = dumpLinks();
    if (!ifaces)
    {
        return std::unexpected(ifaces.error());
    }

    if (auto r = dumpAddrs(*ifaces); !r)
    {
        return std::unexpected(r.error());
    }

    return ifaces;
}

// ---------------------------------------------------------------------------
// Public: getInterface
// ---------------------------------------------------------------------------

std::expected<NetworkInterface, std::string>
RoutingManager::getInterface(const std::string& name) const
{
    auto ifaces = listInterfaces();
    if (!ifaces)
    {
        return std::unexpected(ifaces.error());
    }

    for (auto& iface : *ifaces)
    {
        if (iface.name == name)
        {
            return iface;
        }
    }
    return std::unexpected(std::format("interface '{}' not found", name));
}

// ---------------------------------------------------------------------------
// Public: isInterfaceUp
// ---------------------------------------------------------------------------

std::expected<bool, std::string>
RoutingManager::isInterfaceUp(const std::string& name) const
{
    auto iface = getInterface(name);
    if (!iface)
    {
        return std::unexpected(iface.error());
    }
    return iface->isUp();
}

// ---------------------------------------------------------------------------
// Public: listRoutes
// ---------------------------------------------------------------------------

std::expected<std::vector<KernelRoute>, std::string>
RoutingManager::listRoutes(int family, uint32_t table) const
{
    std::lock_guard lock(mtx_);

    struct
    {
        nlmsghdr nlh;
        rtmsg rt;
    } req{};

    const uint32_t mySeq = seq_++;
    req.nlh.nlmsg_len = NLMSG_LENGTH(sizeof(req.rt));
    req.nlh.nlmsg_type = RTM_GETROUTE;
    req.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    req.nlh.nlmsg_seq = mySeq;
    req.rt.rtm_family = static_cast<uint8_t>(family);

    if (send(fd_, &req, req.nlh.nlmsg_len, 0) < 0)
    {
        return std::unexpected(
            std::format("RTM_GETROUTE send: {}", std::strerror(errno)));
    }

    std::vector<KernelRoute> routes;

    auto result = recvAll(mySeq, [&](nlmsghdr* nlh) {
        if (nlh->nlmsg_type != RTM_NEWROUTE)
        {
            return;
        }

        const auto* rtm = reinterpret_cast<const rtmsg*>(NLMSG_DATA(nlh));
        int attrLen = static_cast<int>(RTM_PAYLOAD(nlh));
        const auto* rta = RTM_RTA(rtm);

        KernelRoute route{};
        route.family = static_cast<int>(rtm->rtm_family);
        route.prefixLen = rtm->rtm_dst_len;
        route.protocol = rtm->rtm_protocol;
        route.type = rtm->rtm_type;
        route.scope = rtm->rtm_scope;
        // rtm_table may be RT_TABLE_COMPAT (252) when the real ID > 255;
        // the actual value is then in RTA_TABLE and will overwrite this.
        route.table = rtm->rtm_table;

        bool hasDst{false};

        for (; RTA_OK(rta, attrLen); rta = RTA_NEXT(rta, attrLen))
        {
            switch (rta->rta_type)
            {
            case RTA_DST:
                route.destination = addrToString(RTA_DATA(rta), route.family) +
                                    "/" + std::to_string(route.prefixLen);
                hasDst = true;
                break;

            case RTA_GATEWAY:
                route.gateway = addrToString(RTA_DATA(rta), route.family);
                break;

            case RTA_OIF: {
                uint32_t oif{};
                std::memcpy(&oif, RTA_DATA(rta), sizeof(oif));
                route.interfaceIndex = oif;
                route.interfaceName = ifIndexToName(oif);
                break;
            }

            case RTA_PRIORITY: {
                uint32_t prio{};
                std::memcpy(&prio, RTA_DATA(rta), sizeof(prio));
                route.metric = prio;
                break;
            }

            case RTA_NH_ID: {
                uint32_t nhid{};
                std::memcpy(&nhid, RTA_DATA(rta), sizeof(nhid));
                route.nhid = nhid;
                break;
            }

            case RTA_TABLE: {
                uint32_t tbl{};
                std::memcpy(&tbl, RTA_DATA(rta), sizeof(tbl));
                route.table = tbl;
                break;
            }

            case RTA_MULTIPATH: {
                // Walk the rtnexthop list and collect one KernelRouteNexthop
                // per entry.  A single KernelRoute is emitted; callers see
                // all ECMP paths in route.nexthops without duplication.
                auto* rtnh = reinterpret_cast<rtnexthop*>(RTA_DATA(rta));
                int nhrem = static_cast<int>(RTA_PAYLOAD(rta));

                while (RTNH_OK(rtnh, nhrem))
                {
                    KernelRouteNexthop knh;
                    knh.interfaceIndex = static_cast<uint32_t>(rtnh->rtnh_ifindex);
                    knh.interfaceName  = ifIndexToName(knh.interfaceIndex);
                    // rtnh_hops is (weight - 1); 0 means equal-cost weight 1.
                    knh.weight = static_cast<uint8_t>(rtnh->rtnh_hops + 1);

                    auto* inner = reinterpret_cast<rtattr*>(RTNH_DATA(rtnh));
                    int ilen = static_cast<int>(rtnh->rtnh_len) -
                               static_cast<int>(sizeof(rtnexthop));
                    for (; RTA_OK(inner, ilen); inner = RTA_NEXT(inner, ilen))
                    {
                        if (inner->rta_type == RTA_GATEWAY)
                            knh.gateway = addrToString(RTA_DATA(inner), route.family);
                    }

                    route.nexthops.push_back(std::move(knh));

                    nhrem -= NLMSG_ALIGN(rtnh->rtnh_len);
                    rtnh = RTNH_NEXT(rtnh);
                }
                break;
            }

            default:
                break;
            }
        }

        // Synthesise a CIDR destination string for the default route, which
        // carries no RTA_DST attribute.
        if (!hasDst)
        {
            route.destination =
                (route.family == AF_INET6) ? "::/0" : "0.0.0.0/0";
        }

        if (table != RT_TABLE_UNSPEC && route.table != table)
            return;

        routes.push_back(std::move(route));
    });

    if (!result)
    {
        return std::unexpected(result.error());
    }
    return routes;
}

// ---------------------------------------------------------------------------
// Private: sendRouteRequest
// ---------------------------------------------------------------------------

std::expected<void, std::string> RoutingManager::sendRouteRequest(
    const RouteParams& params, uint16_t nlType, uint16_t flags)
{
    // -- Parse destination CIDR -----------------------------------------------
    auto cidr = parseCidr(params.destination, params.family);
    if (!cidr)
    {
        return std::unexpected(cidr.error());
    }
    auto& [dstAddr, prefixLen] = *cidr;

    // -- Optional gateway -----------------------------------------------------
    std::array<uint8_t, 16> gwAddr{};
    bool hasGw{false};
    if (!params.gateway.empty())
    {
        if (inet_pton(params.family, params.gateway.c_str(), gwAddr.data()) !=
            1)
        {
            return std::unexpected(
                std::format("invalid gateway address '{}'", params.gateway));
        }
        hasGw = true;
    }

    // -- Optional output interface --------------------------------------------
    uint32_t oif{0};
    if (!params.interfaceName.empty())
    {
        const unsigned idx = if_nametoindex(params.interfaceName.c_str());
        if (idx == 0)
        {
            return std::unexpected(std::format("if_nametoindex '{}': {}",
                                               params.interfaceName,
                                               std::strerror(errno)));
        }
        oif = static_cast<uint32_t>(idx);
    }

    // Automatically promote scope to LINK for on-link (no-gateway) routes.
    const uint8_t scope =
        (params.scope == RT_SCOPE_UNIVERSE && !hasGw && oif != 0)
            ? static_cast<uint8_t>(RT_SCOPE_LINK)
            : params.scope;

    // -- Build netlink message ------------------------------------------------
    std::array<char, kNlReqSize> buf{};
    auto* nlh = reinterpret_cast<nlmsghdr*>(buf.data());
    nlh->nlmsg_type = nlType;
    nlh->nlmsg_flags = static_cast<uint16_t>(NLM_F_REQUEST | NLM_F_ACK) | flags;
    const uint32_t mySeq = seq_++;
    nlh->nlmsg_seq = mySeq;
    nlh->nlmsg_len = NLMSG_LENGTH(sizeof(rtmsg));

    auto* rtm = reinterpret_cast<rtmsg*>(NLMSG_DATA(nlh));
    rtm->rtm_family = static_cast<uint8_t>(params.family);
    rtm->rtm_dst_len = prefixLen;
    rtm->rtm_src_len = 0;
    rtm->rtm_tos = 0;
    rtm->rtm_table = static_cast<uint8_t>(
        params.table <= 255 ? params.table
                            : static_cast<uint32_t>(RT_TABLE_COMPAT));
    rtm->rtm_protocol = params.protocol;
    rtm->rtm_scope = scope;
    rtm->rtm_type = params.type;
    rtm->rtm_flags = 0;

    const int addrLen = (params.family == AF_INET6) ? 16 : 4;

    // RTA_DST – destination network address
    if (addRtAttr(nlh, buf.size(), RTA_DST, dstAddr.data(), addrLen) == nullptr)
    {
        return std::unexpected("message buffer overflow: RTA_DST");
    }

    // RTA_GATEWAY – optional next-hop
    if (hasGw)
    {
        if (addRtAttr(nlh, buf.size(), RTA_GATEWAY, gwAddr.data(), addrLen) ==
            nullptr)
        {
            return std::unexpected("message buffer overflow: RTA_GATEWAY");
        }
    }

    // RTA_OIF – optional output interface
    if (oif != 0)
    {
        if (addRtAttr(nlh, buf.size(), RTA_OIF, &oif, sizeof(oif)) == nullptr)
        {
            return std::unexpected("message buffer overflow: RTA_OIF");
        }
    }

    // RTA_PRIORITY – only emitted when non-zero to preserve kernel defaults
    if (params.metric != 0)
    {
        if (addRtAttr(nlh,
                      buf.size(),
                      RTA_PRIORITY,
                      &params.metric,
                      sizeof(params.metric)) == nullptr)
        {
            return std::unexpected("message buffer overflow: RTA_PRIORITY");
        }
    }

    // RTA_TABLE – required when the table ID does not fit in 8 bits
    if (params.table > 255)
    {
        if (addRtAttr(nlh,
                      buf.size(),
                      RTA_TABLE,
                      &params.table,
                      sizeof(params.table)) == nullptr)
        {
            return std::unexpected("message buffer overflow: RTA_TABLE");
        }
    }

    if (send(fd_, buf.data(), nlh->nlmsg_len, 0) < 0)
    {
        return std::unexpected(std::format("send: {}", std::strerror(errno)));
    }

    // Wait for the kernel ACK / NACK (NLMSG_ERROR with err == 0 / err < 0).
    return recvAll(mySeq, [](nlmsghdr*) {});
}

// ---------------------------------------------------------------------------
// Public: addRoute
// ---------------------------------------------------------------------------

std::expected<void, std::string>
RoutingManager::addRoute(const RouteParams& params)
{
    std::lock_guard lock(mtx_);
    return sendRouteRequest(
        params, RTM_NEWROUTE, static_cast<uint16_t>(NLM_F_CREATE | NLM_F_EXCL));
}

// ---------------------------------------------------------------------------
// Public: removeRoute
// ---------------------------------------------------------------------------

std::expected<void, std::string>
RoutingManager::removeRoute(const RouteParams& params)
{
    std::lock_guard lock(mtx_);
    return sendRouteRequest(params, RTM_DELROUTE, 0);
}

// ---------------------------------------------------------------------------
// Public: replaceRoute
// ---------------------------------------------------------------------------

std::expected<void, std::string>
RoutingManager::replaceRoute(const RouteParams& params)
{
    std::lock_guard lock(mtx_);
    return sendRouteRequest(
        params,
        RTM_NEWROUTE,
        static_cast<uint16_t>(NLM_F_CREATE | NLM_F_REPLACE));
}

} // namespace sra
