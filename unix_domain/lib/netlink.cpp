/**
 * @file netlink.cpp
 * @brief Linux netlink-based route and network interface management
 * implementation.
 *
 * Implements the public API declared in routing.hpp using AF_NETLINK /
 * NETLINK_ROUTE sockets.  The implementation is self-contained: it opens a
 * fresh netlink socket per public call, which keeps each call independent and
 * thread-safe without the need for explicit locking.
 *
 * Compiled into both lib/netlink.so (shared) and lib/netlink.ar (static).
 *
 *
 * @date    2026
 */

#include "routing.hpp"

#include <arpa/inet.h>
#include <linux/netlink.h>
#include <linux/nexthop.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <unordered_map>

namespace netlink
{

// ---------------------------------------------------------------------------
// Error category
// ---------------------------------------------------------------------------

namespace
{

/**
 * @brief Custom std::error_category for netlink::NetlinkError codes.
 *
 * Registered as a singleton via netlink_error_category().
 */
class NetlinkErrorCategory final : public std::error_category
{
public:
    /**
     * @brief Returns the category name.
     * @return The string "netlink".
     */
    [[nodiscard]] const char* name() const noexcept override
    {
        return "netlink";
    }

    /**
     * @brief Maps a NetlinkError integer value to a human-readable message.
     * @param ev Integer representation of a NetlinkError enumerator.
     * @return Descriptive error string.
     */
    [[nodiscard]] std::string message(int ev) const override
    {
        switch (static_cast<NetlinkError>(ev))
        {
        case NetlinkError::SocketCreateFailed:
            return "failed to create netlink socket";
        case NetlinkError::BindFailed:
            return "failed to bind netlink socket";
        case NetlinkError::SendFailed:
            return "failed to send netlink message";
        case NetlinkError::RecvFailed:
            return "failed to receive netlink response";
        case NetlinkError::KernelError:
            return "kernel returned a netlink error";
        case NetlinkError::ParseError:
            return "malformed or truncated netlink message";
        case NetlinkError::NotFound:
            return "requested object not found";
        case NetlinkError::AddressError:
            return "invalid or unparseable IP address";
        case NetlinkError::InterfaceNotFound:
            return "network interface not found";
        default:
            return "unknown netlink error";
        }
    }
};

} // anonymous namespace

/**
 * @brief Returns the singleton NetlinkErrorCategory instance.
 * @return Reference to the global netlink error category.
 */
const std::error_category& netlink_error_category() noexcept
{
    static const NetlinkErrorCategory instance;
    return instance;
}

/**
 * @brief Creates a std::error_code from a NetlinkError enumerator.
 * @param e The NetlinkError value to wrap.
 * @return Corresponding std::error_code in the netlink_error_category.
 */
std::error_code make_error_code(NetlinkError e) noexcept
{
    return {static_cast<int>(e), netlink_error_category()};
}

// ---------------------------------------------------------------------------
// Internal implementation
// ---------------------------------------------------------------------------

namespace
{

// ── Sequence counter ──────────────────────────────────────────────────────

/// @brief Monotonically increasing netlink sequence number.
std::atomic<std::uint32_t> g_seq{1};

[[nodiscard]] std::uint32_t next_seq() noexcept
{
    return g_seq.fetch_add(1, std::memory_order_relaxed);
}

// ── RAII netlink socket ────────────────────────────────────────────────────

/**
 * @brief Move-only RAII wrapper for an AF_NETLINK / NETLINK_ROUTE socket.
 *
 * Each public API call opens its own socket so that concurrent calls from
 * multiple threads remain independent.
 */
class NlSocket
{
public:
    /// @brief Constructs an invalid (closed) socket.
    NlSocket() noexcept = default;

    /// @brief Closes the socket file descriptor if still open.
    ~NlSocket() noexcept
    {
        if (fd_ >= 0)
            ::close(fd_);
    }

    NlSocket(const NlSocket&) = delete;
    NlSocket& operator=(const NlSocket&) = delete;

    /**
     * @brief Move-constructs from @p o, transferring ownership of the fd.
     * @param o Source socket; its fd is set to -1 after the move.
     */
    NlSocket(NlSocket&& o) noexcept : fd_(o.fd_)
    {
        o.fd_ = -1;
    }

    /**
     * @brief Move-assigns from @p o, closing any currently held fd first.
     * @param o Source socket; its fd is set to -1 after the move.
     * @return Reference to this socket.
     */
    NlSocket& operator=(NlSocket&& o) noexcept
    {
        if (this != &o)
        {
            if (fd_ >= 0)
                ::close(fd_);
            fd_ = o.fd_;
            o.fd_ = -1;
        }
        return *this;
    }

    /// @brief Returns true when the socket holds a valid file descriptor.
    [[nodiscard]] bool valid() const noexcept
    {
        return fd_ >= 0;
    }

    /// @brief Returns the underlying file descriptor (-1 if invalid).
    [[nodiscard]] int fd() const noexcept
    {
        return fd_;
    }

    /**
     * @brief Opens and binds a NETLINK_ROUTE socket.
     * @return NlSocket on success, or NetlinkError::SocketCreateFailed /
     *         NetlinkError::BindFailed on failure.
     */
    [[nodiscard]] static std::expected<NlSocket, std::error_code>
    open() noexcept
    {
        NlSocket s;
        s.fd_ = ::socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
        if (s.fd_ < 0)
        {
            return std::unexpected(
                make_error_code(NetlinkError::SocketCreateFailed));
        }

        struct sockaddr_nl sa
        {};
        sa.nl_family = AF_NETLINK;
        if (::bind(s.fd_,
                   reinterpret_cast<const struct sockaddr*>(&sa),
                   sizeof(sa)) < 0)
        {
            return std::unexpected(make_error_code(NetlinkError::BindFailed));
        }

        return s;
    }

private:
    int fd_{-1};
};

// ── Message builder ────────────────────────────────────────────────────────

/**
 * @brief Fixed-size netlink message builder with inline attribute-append
 *        helpers.
 *
 * The 4096-byte buffer is large enough for all route and interface operations
 * performed by this module.
 */
struct NlMsg
{
    static constexpr std::size_t BUF_CAP = 4096;

    std::array<std::uint8_t, BUF_CAP> buf{};

    /**
     * @brief Initialises the nlmsghdr and zeroes the rest of the buffer.
     * @param type   Message type (e.g. RTM_NEWROUTE).
     * @param flags  Extra flags ORed with NLM_F_REQUEST.
     */
    void init(std::uint16_t type, std::uint16_t flags) noexcept
    {
        buf.fill(0);
        auto* h = hdr();
        h->nlmsg_type = type;
        h->nlmsg_flags = NLM_F_REQUEST | flags;
        h->nlmsg_seq = next_seq();
        h->nlmsg_pid = 0;
        h->nlmsg_len = NLMSG_HDRLEN;
    }

    /**
     * @brief Appends a plain struct body (rtmsg, ifinfomsg, …) after the
     *        current message end and updates nlmsg_len.
     * @tparam T  POD struct type.
     * @param body  Value to copy into the message.
     * @return Pointer to the body inside the buffer.
     */
    template <typename T>
    T* put_body(const T& body) noexcept
    {
        auto* p = buf.data() + hdr()->nlmsg_len;
        std::memcpy(p, &body, sizeof(T));
        hdr()->nlmsg_len += static_cast<std::uint32_t>(NLMSG_ALIGN(sizeof(T)));
        return reinterpret_cast<T*>(p);
    }

    /**
     * @brief Appends an rtattr with arbitrary data and updates nlmsg_len.
     * @param type  Attribute type (RTA_*, IFLA_*, IFA_*, …).
     * @param data  Pointer to attribute payload (may be nullptr when len == 0).
     * @param len   Byte length of @p data.
     */
    void
    add_attr(std::uint16_t type, const void* data, std::size_t len) noexcept
    {
        auto* rta =
            reinterpret_cast<struct rtattr*>(buf.data() + hdr()->nlmsg_len);
        rta->rta_type = type;
        rta->rta_len = static_cast<std::uint16_t>(RTA_LENGTH(len));
        if (data && len > 0)
        {
            std::memcpy(RTA_DATA(rta), data, len);
        }
        hdr()->nlmsg_len += static_cast<std::uint32_t>(RTA_SPACE(len));
    }

    /// @brief Convenience overload: appends a uint32_t attribute.
    void add_attr_u32(std::uint16_t type, std::uint32_t val) noexcept
    {
        add_attr(type, &val, sizeof(val));
    }

    /// @brief Returns a mutable pointer to the nlmsghdr at the start of the
    /// buffer.
    [[nodiscard]] struct nlmsghdr* hdr() noexcept
    {
        return reinterpret_cast<struct nlmsghdr*>(buf.data());
    }

    /// @brief Returns a const pointer to the nlmsghdr at the start of the
    /// buffer.
    [[nodiscard]] const struct nlmsghdr* hdr() const noexcept
    {
        return reinterpret_cast<const struct nlmsghdr*>(buf.data());
    }

    /// @brief Returns the current total byte length of the message.
    [[nodiscard]] std::uint32_t size() const noexcept
    {
        return hdr()->nlmsg_len;
    }
};

// ── Send / receive helpers ─────────────────────────────────────────────────

/**
 * @brief Transmits a single netlink message to the kernel.
 * @param fd   Open NETLINK_ROUTE socket.
 * @param msg  Message to send.
 * @return void on success, or NetlinkError::SendFailed.
 */
[[nodiscard]] std::expected<void, std::error_code>
nl_send(int fd, const NlMsg& msg) noexcept
{
    struct sockaddr_nl dst
    {};
    dst.nl_family = AF_NETLINK;

    const ssize_t n = ::sendto(fd,
                               msg.buf.data(),
                               msg.size(),
                               0,
                               reinterpret_cast<const struct sockaddr*>(&dst),
                               sizeof(dst));
    if (n < 0)
    {
        return std::unexpected(make_error_code(NetlinkError::SendFailed));
    }
    return {};
}

/**
 * @brief Sends a modify request (RTM_NEWROUTE, RTM_DELROUTE, …) that carries
 *        NLM_F_ACK and waits for the kernel's ACK or error reply.
 *
 * @param fd   Open NETLINK_ROUTE socket.
 * @param msg  Request message (must include NLM_F_ACK in its flags).
 * @return void on success, or a system / netlink error code.
 */
[[nodiscard]] std::expected<void, std::error_code> nl_transact(int fd,
                                                               const NlMsg& msg)
{
    if (auto r = nl_send(fd, msg); !r)
        return r;

    // Receive ACK or NLMSG_ERROR.
    std::array<std::uint8_t, 4096> buf{};
    const ssize_t n = ::recv(fd, buf.data(), buf.size(), 0);
    if (n < 0)
    {
        return std::unexpected(make_error_code(NetlinkError::RecvFailed));
    }

    const auto* nlh = reinterpret_cast<const struct nlmsghdr*>(buf.data());
    auto len = static_cast<int>(n);

    for (; NLMSG_OK(nlh, static_cast<unsigned>(len));
         nlh = NLMSG_NEXT(nlh, len))
    {
        if (nlh->nlmsg_type == NLMSG_ERROR)
        {
            const auto* err =
                reinterpret_cast<const struct nlmsgerr*>(NLMSG_DATA(nlh));
            if (err->error != 0)
            {
                // Kernel encodes errno as a negative value.
                return std::unexpected(
                    std::error_code(-err->error, std::system_category()));
            }
            return {}; // error == 0 → ACK
        }
    }
    return {}; // Unexpected: no explicit ACK received; treat as success.
}

/**
 * @brief Sends a dump request (NLM_F_DUMP) and collects the full multipart
 *        response into a contiguous byte buffer.
 *
 * Dump responses consist of zero or more RTM_NEW* messages followed by a
 * NLMSG_DONE terminator.  All messages are appended to the returned vector
 * in wire order so that they can be iterated with the standard NLMSG_OK /
 * NLMSG_NEXT macros.
 *
 * @param fd   Open NETLINK_ROUTE socket.
 * @param req  Dump request (must carry NLM_F_DUMP).
 * @return Byte vector containing all response messages, or an error code.
 */
[[nodiscard]] std::expected<std::vector<std::uint8_t>, std::error_code>
nl_dump(int fd, const NlMsg& req)
{
    if (auto r = nl_send(fd, req); !r)
        return std::unexpected(r.error());

    std::vector<std::uint8_t> result;
    std::array<std::uint8_t, 32768> buf{};

    while (true)
    {
        const ssize_t n = ::recv(fd, buf.data(), buf.size(), 0);
        if (n < 0)
        {
            return std::unexpected(make_error_code(NetlinkError::RecvFailed));
        }

        const auto* nlh = reinterpret_cast<const struct nlmsghdr*>(buf.data());
        auto len = static_cast<int>(n);
        bool done = false;

        for (; NLMSG_OK(nlh, static_cast<unsigned>(len));
             nlh = NLMSG_NEXT(nlh, len))
        {
            if (nlh->nlmsg_type == NLMSG_DONE)
            {
                done = true;
                break;
            }
            if (nlh->nlmsg_type == NLMSG_ERROR)
            {
                const auto* err =
                    reinterpret_cast<const struct nlmsgerr*>(NLMSG_DATA(nlh));
                if (err->error != 0)
                {
                    return std::unexpected(
                        std::error_code(-err->error, std::system_category()));
                }
                done = true;
                break;
            }
            // Append this message (aligned) to the result buffer.
            const auto* raw = reinterpret_cast<const std::uint8_t*>(nlh);
            result.insert(result.end(), raw, raw + NLMSG_ALIGN(nlh->nlmsg_len));
        }
        if (done)
            break;
    }
    return result;
}

// ── Attribute parser ───────────────────────────────────────────────────────

/**
 * @brief Map type alias used to hold parsed rtattr entries indexed by type.
 *
 * The spans point into the caller's buffer; the map must not outlive it.
 */
using AttrMap = std::unordered_map<int, std::pair<const void*, std::size_t>>;

/**
 * @brief Iterates over the rtattr chain starting at @p rta and populates
 *        @p out with one entry per attribute type (last-write wins on
 *        duplicates).
 *
 * @param rta  Pointer to the first rtattr in the payload region.
 * @param len  Byte length of the payload region.
 * @param out  Map to populate (cleared on entry).
 */
void parse_attrs(const struct rtattr* rta, int len, AttrMap& out) noexcept
{
    out.clear();
    for (; RTA_OK(rta, len); rta = RTA_NEXT(rta, len))
    {
        out[rta->rta_type] = {RTA_DATA(rta), RTA_PAYLOAD(rta)};
    }
}

/**
 * @brief Returns a pointer to the raw attribute data for @p type, or nullptr
 *        if the attribute is absent or has the wrong size.
 *
 * @param attrs    Populated AttrMap.
 * @param type     Attribute type to look up.
 * @param expected Expected payload size in bytes (0 = accept any size).
 */
[[nodiscard]] const void*
attr_data(const AttrMap& attrs, int type, std::size_t expected = 0) noexcept
{
    auto it = attrs.find(type);
    if (it == attrs.end())
        return nullptr;
    if (expected != 0 && it->second.second != expected)
        return nullptr;
    return it->second.first;
}

// ── Interface-name ↔ index helpers ────────────────────────────────────────

/**
 * @brief Converts a kernel interface index to its name via
 *        @c if_indextoname(3).
 * @param idx  Interface index (positive).
 * @return Interface name string, or an empty string on failure.
 */
[[nodiscard]] std::string index_to_name(int idx)
{
    if (idx <= 0)
        return {};
    char buf[IF_NAMESIZE]{};
    if (::if_indextoname(static_cast<unsigned>(idx), buf) == nullptr)
    {
        return {};
    }
    return buf;
}

/**
 * @brief Converts an interface name to its kernel index via
 *        @c if_nametoindex(3).
 * @param name  Interface name string.
 * @return Interface index on success, or NetlinkError::InterfaceNotFound when
 *         @p name is non-empty but not found.  Returns 0 for an empty name.
 */
[[nodiscard]] std::expected<int, std::error_code>
name_to_index(const std::string& name) noexcept
{
    if (name.empty())
        return 0;
    const unsigned idx = ::if_nametoindex(name.c_str());
    if (idx == 0)
    {
        return std::unexpected(
            make_error_code(NetlinkError::InterfaceNotFound));
    }
    return static_cast<int>(idx);
}

// ── Shared RTM_NEWROUTE builder ────────────────────────────────────────────

/**
 * @brief Builds and sends a RTM_NEWROUTE message with caller-supplied
 *        creation flags (NLM_F_CREATE | NLM_F_EXCL or NLM_F_CREATE |
 *        NLM_F_REPLACE).
 *
 * @param params       Route parameters.
 * @param extra_flags  NLM_F_* flags beyond NLM_F_REQUEST | NLM_F_ACK.
 * @return void on success, or an error code.
 */
[[nodiscard]] std::expected<void, std::error_code>
send_route_new(const RouteAddParams& params, std::uint16_t extra_flags)
{
    auto sock = NlSocket::open();
    if (!sock)
        return std::unexpected(sock.error());

    NlMsg msg;
    msg.init(RTM_NEWROUTE, NLM_F_ACK | extra_flags);

    // Build rtmsg body.
    struct rtmsg rt
    {};
    rt.rtm_family = AF_INET;
    rt.rtm_dst_len = params.prefix_len;

    // For table IDs that fit in uint8_t, store directly; larger IDs go in
    // the RTA_TABLE attribute added below.
    const auto tbl_u32 = static_cast<std::uint32_t>(params.table);
    rt.rtm_table = (tbl_u32 <= 255)
                       ? static_cast<std::uint8_t>(tbl_u32)
                       : static_cast<std::uint8_t>(RT_TABLE_UNSPEC);
    rt.rtm_protocol = static_cast<std::uint8_t>(params.protocol);
    rt.rtm_scope = static_cast<std::uint8_t>(params.scope);
    rt.rtm_type = static_cast<std::uint8_t>(params.type);

    msg.put_body(rt);

    // RTA_DST — destination network prefix.
    msg.add_attr(RTA_DST, params.dst.data(), params.dst.size());

    // RTA_GATEWAY — next-hop gateway (only when explicitly set).
    if (params.has_gateway)
    {
        msg.add_attr(RTA_GATEWAY, params.gateway.data(), params.gateway.size());
    }

    // RTA_OIF — output interface index.
    if (!params.if_name.empty())
    {
        auto idx = name_to_index(params.if_name);
        if (!idx)
            return std::unexpected(idx.error());
        msg.add_attr_u32(RTA_OIF, static_cast<std::uint32_t>(*idx));
    }

    // RTA_PRIORITY — metric (omit 0 to let the kernel choose).
    if (params.metric != 0)
    {
        msg.add_attr_u32(RTA_PRIORITY, params.metric);
    }

    // RTA_TABLE — 32-bit table ID for extended routing tables.
    if (tbl_u32 > 255)
    {
        msg.add_attr_u32(RTA_TABLE, tbl_u32);
    }

    return nl_transact(sock->fd(), msg);
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Utility / formatting helpers
// ---------------------------------------------------------------------------

/// @brief @copybrief netlink::format_ipv4
std::string format_ipv4(const Ipv4Addr& addr)
{
    char buf[INET_ADDRSTRLEN]{};
    ::inet_ntop(AF_INET, addr.data(), buf, sizeof(buf));
    return buf;
}

/// @brief @copybrief netlink::parse_ipv4
std::expected<Ipv4Addr, std::error_code> parse_ipv4(const std::string& s)
{
    Ipv4Addr out{};
    if (::inet_pton(AF_INET, s.c_str(), out.data()) != 1)
    {
        return std::unexpected(make_error_code(NetlinkError::AddressError));
    }
    return out;
}

/// @brief @copybrief netlink::format_cidr
std::string format_cidr(const Ipv4Addr& addr, std::uint8_t prefix_len)
{
    return format_ipv4(addr) + "/" + std::to_string(prefix_len);
}

// ---------------------------------------------------------------------------
// Route management
// ---------------------------------------------------------------------------

/// @brief @copybrief netlink::add_route
std::expected<void, std::error_code> add_route(const RouteAddParams& params)
{
    return send_route_new(params, NLM_F_CREATE | NLM_F_EXCL);
}

/// @brief @copybrief netlink::replace_route
std::expected<void, std::error_code> replace_route(const RouteAddParams& params)
{
    return send_route_new(params, NLM_F_CREATE | NLM_F_REPLACE);
}

/// @brief @copybrief netlink::delete_route
std::expected<void, std::error_code> delete_route(const RouteDelParams& params)
{
    auto sock = NlSocket::open();
    if (!sock)
        return std::unexpected(sock.error());

    NlMsg msg;
    msg.init(RTM_DELROUTE, NLM_F_ACK);

    struct rtmsg rt
    {};
    rt.rtm_family = AF_INET;
    rt.rtm_dst_len = params.prefix_len;
    // RT_SCOPE_NOWHERE (255) is the kernel's wildcard: it disables scope
    // filtering in fib_table_delete so both link-scope and universe-scope
    // routes are matched.  Leaving rtm_scope=0 (RT_SCOPE_UNIVERSE) would
    // only match universe-scope routes and produce ESRCH for link-scope ones.
    rt.rtm_scope = static_cast<std::uint8_t>(RouteScope::Nowhere);

    const auto tbl_u32 = static_cast<std::uint32_t>(params.table);
    rt.rtm_table = (tbl_u32 <= 255)
                       ? static_cast<std::uint8_t>(tbl_u32)
                       : static_cast<std::uint8_t>(RT_TABLE_UNSPEC);

    msg.put_body(rt);

    // RTA_DST — destination to match.
    msg.add_attr(RTA_DST, params.dst.data(), params.dst.size());

    // RTA_GATEWAY — narrow the match to a specific gateway (optional).
    if (params.has_gateway)
    {
        msg.add_attr(RTA_GATEWAY, params.gateway.data(), params.gateway.size());
    }

    // RTA_OIF — narrow the match to a specific interface (optional).
    if (!params.if_name.empty())
    {
        auto idx = name_to_index(params.if_name);
        if (!idx)
            return std::unexpected(idx.error());
        msg.add_attr_u32(RTA_OIF, static_cast<std::uint32_t>(*idx));
    }

    if (tbl_u32 > 255)
    {
        msg.add_attr_u32(RTA_TABLE, tbl_u32);
    }

    return nl_transact(sock->fd(), msg);
}

// ---------------------------------------------------------------------------
// Nexthop object management (Linux 5.3+)
// ---------------------------------------------------------------------------

/// @brief @copybrief netlink::add_nexthop
std::expected<void, std::error_code> add_nexthop(const NexthopAddParams& params)
{
    auto sock = NlSocket::open();
    if (!sock)
        return std::unexpected(sock.error());

    NlMsg msg;
    msg.init(RTM_NEWNEXTHOP, NLM_F_ACK | NLM_F_CREATE | NLM_F_EXCL);

    struct nhmsg nh
    {};
    nh.nh_family = params.has_gateway ? AF_INET : AF_UNSPEC;
    nh.nh_protocol = static_cast<std::uint8_t>(params.protocol);
    msg.put_body(nh);

    // NHA_ID — nexthop identifier.
    msg.add_attr_u32(NHA_ID, params.id);

    // NHA_OIF — output interface index.
    if (!params.if_name.empty())
    {
        auto idx = name_to_index(params.if_name);
        if (!idx)
            return std::unexpected(idx.error());
        msg.add_attr_u32(NHA_OIF, static_cast<std::uint32_t>(*idx));
    }

    // NHA_GATEWAY — next-hop gateway address.
    if (params.has_gateway)
    {
        msg.add_attr(NHA_GATEWAY, params.gateway.data(), params.gateway.size());
    }

    return nl_transact(sock->fd(), msg);
}

/// @brief @copybrief netlink::delete_nexthop
std::expected<void, std::error_code>
delete_nexthop(const NexthopDelParams& params)
{
    auto sock = NlSocket::open();
    if (!sock)
        return std::unexpected(sock.error());

    NlMsg msg;
    msg.init(RTM_DELNEXTHOP, NLM_F_ACK);

    struct nhmsg nh
    {};
    nh.nh_family = AF_UNSPEC;
    msg.put_body(nh);

    // NHA_ID — identifies the nexthop object to remove.
    msg.add_attr_u32(NHA_ID, params.id);

    return nl_transact(sock->fd(), msg);
}

/// @brief @copybrief netlink::list_routes
std::expected<std::vector<RouteEntry>, std::error_code>
list_routes(RouteTable table)
{
    auto sock = NlSocket::open();
    if (!sock)
        return std::unexpected(sock.error());

    NlMsg req;
    req.init(RTM_GETROUTE, NLM_F_DUMP);

    // Request only IPv4 routes from the kernel.
    struct rtmsg rt
    {};
    rt.rtm_family = AF_INET;
    req.put_body(rt);

    auto data = nl_dump(sock->fd(), req);
    if (!data)
        return std::unexpected(data.error());

    std::vector<RouteEntry> routes;
    const auto* nlh = reinterpret_cast<const struct nlmsghdr*>(data->data());
    auto remaining = static_cast<int>(data->size());

    for (; NLMSG_OK(nlh, static_cast<unsigned>(remaining));
         nlh = NLMSG_NEXT(nlh, remaining))
    {
        if (nlh->nlmsg_type != RTM_NEWROUTE)
            continue;

        const auto* rtm =
            reinterpret_cast<const struct rtmsg*>(NLMSG_DATA(nlh));
        if (rtm->rtm_family != AF_INET)
            continue;

        AttrMap attrs;
        parse_attrs(RTM_RTA(rtm), static_cast<int>(RTM_PAYLOAD(nlh)), attrs);

        RouteEntry e;
        e.prefix_len = rtm->rtm_dst_len;
        e.table = static_cast<RouteTable>(rtm->rtm_table);
        e.protocol = static_cast<RouteProtocol>(rtm->rtm_protocol);
        e.scope = static_cast<RouteScope>(rtm->rtm_scope);
        e.type = static_cast<RouteType>(rtm->rtm_type);

        // RTA_TABLE carries a 32-bit ID in newer kernels; prefer it over the
        // 8-bit rtm_table field.
        if (const auto* p = attr_data(attrs, RTA_TABLE, sizeof(std::uint32_t)))
        {
            std::uint32_t tbl{};
            std::memcpy(&tbl, p, sizeof(tbl));
            e.table = static_cast<RouteTable>(tbl);
        }

        // RTA_DST — destination prefix (absent for the default route 0/0).
        if (const auto* p = attr_data(attrs, RTA_DST, 4))
        {
            std::memcpy(e.dst.data(), p, 4);
        }

        // RTA_GATEWAY — next-hop gateway.
        if (const auto* p = attr_data(attrs, RTA_GATEWAY, 4))
        {
            std::memcpy(e.gateway.data(), p, 4);
            e.has_gateway = true;
        }

        // RTA_OIF — output interface index.
        if (const auto* p = attr_data(attrs, RTA_OIF, sizeof(std::uint32_t)))
        {
            std::uint32_t idx{};
            std::memcpy(&idx, p, sizeof(idx));
            e.if_index = static_cast<int>(idx);
            e.if_name = index_to_name(e.if_index);
        }

        // RTA_PRIORITY — route metric.
        if (const auto* p =
                attr_data(attrs, RTA_PRIORITY, sizeof(std::uint32_t)))
        {
            std::memcpy(&e.metric, p, sizeof(e.metric));
        }

        // Apply routing-table filter: RouteTable::Unspec means accept all.
        if (table != RouteTable::Unspec && e.table != table)
            continue;

        routes.push_back(std::move(e));
    }

    return routes;
}

// ---------------------------------------------------------------------------
// Interface management
// ---------------------------------------------------------------------------

/// @brief @copybrief netlink::list_interfaces
std::expected<std::vector<InterfaceInfo>, std::error_code> list_interfaces()
{
    auto sock = NlSocket::open();
    if (!sock)
        return std::unexpected(sock.error());

    NlMsg req;
    req.init(RTM_GETLINK, NLM_F_DUMP);

    struct ifinfomsg ifi
    {};
    ifi.ifi_family = AF_UNSPEC; // all address families
    req.put_body(ifi);

    auto data = nl_dump(sock->fd(), req);
    if (!data)
        return std::unexpected(data.error());

    std::vector<InterfaceInfo> ifaces;
    const auto* nlh = reinterpret_cast<const struct nlmsghdr*>(data->data());
    auto remaining = static_cast<int>(data->size());

    for (; NLMSG_OK(nlh, static_cast<unsigned>(remaining));
         nlh = NLMSG_NEXT(nlh, remaining))
    {
        if (nlh->nlmsg_type != RTM_NEWLINK)
            continue;

        const auto* ifi_msg =
            reinterpret_cast<const struct ifinfomsg*>(NLMSG_DATA(nlh));

        AttrMap attrs;
        parse_attrs(
            IFLA_RTA(ifi_msg), static_cast<int>(IFLA_PAYLOAD(nlh)), attrs);

        InterfaceInfo info;
        info.index = ifi_msg->ifi_index;
        info.flags = static_cast<IfFlags>(ifi_msg->ifi_flags);
        info.link_type = static_cast<std::uint32_t>(ifi_msg->ifi_type);

        // IFLA_IFNAME — NUL-terminated interface name.
        if (auto it = attrs.find(IFLA_IFNAME); it != attrs.end())
        {
            const auto* str = static_cast<const char*>(it->second.first);
            const auto len = it->second.second;
            // strnlen guards against a missing NUL terminator.
            info.name.assign(str, ::strnlen(str, len));
        }

        // IFLA_MTU — 32-bit MTU value.
        if (const auto* p = attr_data(attrs, IFLA_MTU, sizeof(std::uint32_t)))
        {
            std::memcpy(&info.mtu, p, sizeof(info.mtu));
        }

        // IFLA_ADDRESS — hardware (MAC) address (6 bytes for Ethernet).
        if (auto it = attrs.find(IFLA_ADDRESS);
            it != attrs.end() && it->second.second == 6)
        {
            std::memcpy(info.hwaddr.data(), it->second.first, 6);
            info.has_hwaddr = true;
        }

        ifaces.push_back(std::move(info));
    }

    return ifaces;
}

/// @brief @copybrief netlink::get_interface
std::expected<InterfaceInfo, std::error_code>
get_interface(const std::string& name)
{
    auto ifaces = list_interfaces();
    if (!ifaces)
        return std::unexpected(ifaces.error());

    for (auto& iface : *ifaces)
    {
        if (iface.name == name)
            return iface;
    }
    return std::unexpected(make_error_code(NetlinkError::InterfaceNotFound));
}

/// @brief @copybrief netlink::get_interface_by_index
std::expected<InterfaceInfo, std::error_code> get_interface_by_index(int index)
{
    auto ifaces = list_interfaces();
    if (!ifaces)
        return std::unexpected(ifaces.error());

    for (auto& iface : *ifaces)
    {
        if (iface.index == index)
            return iface;
    }
    return std::unexpected(make_error_code(NetlinkError::InterfaceNotFound));
}

/// @brief @copybrief netlink::get_interface_index
std::expected<int, std::error_code> get_interface_index(const std::string& name)
{
    return name_to_index(name);
}

/// @brief @copybrief netlink::list_interface_addresses
std::expected<std::vector<IfAddrEntry>, std::error_code>
list_interface_addresses(const std::string& if_name)
{
    auto sock = NlSocket::open();
    if (!sock)
        return std::unexpected(sock.error());

    NlMsg req;
    req.init(RTM_GETADDR, NLM_F_DUMP);

    struct ifaddrmsg ifa
    {};
    ifa.ifa_family = AF_INET; // IPv4 addresses only
    req.put_body(ifa);

    auto data = nl_dump(sock->fd(), req);
    if (!data)
        return std::unexpected(data.error());

    // Resolve the optional interface filter to an index.
    int filter_idx = 0;
    if (!if_name.empty())
    {
        auto idx = name_to_index(if_name);
        if (!idx)
            return std::unexpected(idx.error());
        filter_idx = *idx;
    }

    std::vector<IfAddrEntry> addrs;
    const auto* nlh = reinterpret_cast<const struct nlmsghdr*>(data->data());
    auto remaining = static_cast<int>(data->size());

    for (; NLMSG_OK(nlh, static_cast<unsigned>(remaining));
         nlh = NLMSG_NEXT(nlh, remaining))
    {
        if (nlh->nlmsg_type != RTM_NEWADDR)
            continue;

        const auto* ifa_msg =
            reinterpret_cast<const struct ifaddrmsg*>(NLMSG_DATA(nlh));
        if (ifa_msg->ifa_family != AF_INET)
            continue;

        if (filter_idx != 0 &&
            static_cast<int>(ifa_msg->ifa_index) != filter_idx)
        {
            continue;
        }

        AttrMap attrs;
        parse_attrs(
            IFA_RTA(ifa_msg), static_cast<int>(IFA_PAYLOAD(nlh)), attrs);

        IfAddrEntry entry;
        entry.prefix_len = ifa_msg->ifa_prefixlen;
        entry.if_index = static_cast<int>(ifa_msg->ifa_index);
        entry.if_name = index_to_name(entry.if_index);

        // For point-to-point links, IFA_LOCAL is the local address and
        // IFA_ADDRESS is the peer address.  For regular interfaces both
        // attributes carry the same address; prefer IFA_LOCAL when present.
        const void* addr_src = attr_data(attrs, IFA_LOCAL, 4);
        if (!addr_src)
            addr_src = attr_data(attrs, IFA_ADDRESS, 4);
        if (addr_src)
        {
            std::memcpy(entry.addr.data(), addr_src, 4);
        }

        // IFA_BROADCAST — broadcast address (only meaningful on Ethernet-like
        // interfaces).
        if (const auto* p = attr_data(attrs, IFA_BROADCAST, 4))
        {
            std::memcpy(entry.broadcast.data(), p, 4);
            entry.has_broadcast = true;
        }

        addrs.push_back(std::move(entry));
    }

    return addrs;
}

} // namespace netlink
