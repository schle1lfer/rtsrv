/**
 * @file client/src/sra_udp_client.cpp
 * @brief Route client — non-blocking UNIX-domain socket with command queue.
 *
 * The background thread waits for route commands (ROUTE_ADD, ROUTE_DEL,
 * ROUTE_LIST), connects to the UNIX-domain server (O_NONBLOCK socket + poll()
 * for every I/O step), sends the encoded frame, receives the response, and
 * decodes and logs the result.
 */

#include "client/sra_udp_client.hpp"

#include "lib/cmd_proto.hpp"
#include "lib/logger.hpp"
#include "lib/net.hpp"
#include "lib/route_proto.hpp"
#include "lib/ud_proto.hpp"

#include <arpa/inet.h>
#include <poll.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <format>
#include <print>
#include <vector>

namespace sra
{

// ---------------------------------------------------------------------------
// File-local helpers
// ---------------------------------------------------------------------------

namespace
{

/** Formats a 4-byte IPv4 address (network byte order) as dotted-decimal. */
static std::string fmt_ip(const cmdproto::Ipv4Addr& a)
{
    char buf[INET_ADDRSTRLEN];
    std::snprintf(buf,
                  sizeof(buf),
                  "%u.%u.%u.%u",
                  static_cast<unsigned>(a[0]),
                  static_cast<unsigned>(a[1]),
                  static_cast<unsigned>(a[2]),
                  static_cast<unsigned>(a[3]));
    return buf;
}

/**
 * @brief Sends all bytes of @p data over a non-blocking socket.
 *
 * Uses poll(POLLOUT) before each send_data() call.  Returns an error if
 * the poll times out or the socket becomes unwriteable.
 */
static std::expected<void, std::error_code>
nb_send_all(int fd, std::span<const std::uint8_t> data, int timeout_ms)
{
    std::size_t total = 0;
    while (total < data.size())
    {
        pollfd pfd{fd, POLLOUT, 0};
        int r = ::poll(&pfd, 1, timeout_ms);
        if (r < 0)
            return std::unexpected(
                std::error_code(errno, std::system_category()));
        if (r == 0)
            return std::unexpected(
                net::make_error_code(net::NetError::SendFailed)); // timeout

        auto n = net::send_data(fd, data.subspan(total));
        if (!n)
        {
            if (n.error() == net::make_error_code(net::NetError::WouldBlock))
                continue;
            return std::unexpected(n.error());
        }
        total += *n;
    }
    return {};
}

/**
 * @brief Receives bytes from a non-blocking socket until a complete
 *        udproto frame (delimited by 0x7E bytes) is available.
 *
 * Uses poll(POLLIN) before each recv_data() call.  Returns an error on
 * timeout, closed connection, or protocol framing failure.
 */
static std::expected<std::vector<std::uint8_t>, std::error_code>
nb_recv_frame(int fd, int timeout_ms)
{
    std::vector<std::uint8_t> buf;
    buf.reserve(512);
    std::array<std::uint8_t, 1024> tmp{};

    while (true)
    {
        pollfd pfd{fd, POLLIN, 0};
        int r = ::poll(&pfd, 1, timeout_ms);
        if (r < 0)
            return std::unexpected(
                std::error_code(errno, std::system_category()));
        if (r == 0)
            return std::unexpected(
                net::make_error_code(net::NetError::RecvFailed)); // timeout

        auto n = net::recv_data(fd, tmp);
        if (!n)
        {
            if (n.error() == net::make_error_code(net::NetError::WouldBlock))
                continue;
            return std::unexpected(n.error());
        }
        buf.insert(buf.end(),
                   tmp.begin(),
                   tmp.begin() + static_cast<std::ptrdiff_t>(*n));

        std::size_t end = 0;
        auto frame = udproto::find_frame(buf, end);
        if (frame)
            return std::vector<std::uint8_t>{frame->begin(), frame->end()};
    }
}

/**
 * @brief Connects a non-blocking AF_UNIX socket to @p path.
 *
 * AF_UNIX connect() is normally instantaneous, but EINPROGRESS is handled
 * with poll(POLLOUT) for portability.
 */
static std::expected<void, std::error_code>
nb_connect(int fd, const std::string& path, int timeout_ms)
{
    auto r = net::connect_socket(fd, path);
    if (r)
        return {};

    // connect_socket wraps errno == EINPROGRESS as ConnectFailed;
    // check the underlying errno to distinguish it.
    if (errno != EINPROGRESS)
        return std::unexpected(r.error());

    pollfd pfd{fd, POLLOUT, 0};
    int pr = ::poll(&pfd, 1, timeout_ms);
    if (pr <= 0)
        return std::unexpected(
            net::make_error_code(net::NetError::ConnectFailed));

    int err = 0;
    socklen_t len = sizeof(err);
    ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
    if (err != 0)
        return std::unexpected(std::error_code(err, std::system_category()));
    return {};
}

/**
 * @brief Creates a non-blocking AF_UNIX socket and connects to @p path.
 *
 * Combines socket creation, set_nonblocking, and nb_connect into one step
 * so the returned Socket is ready for immediate use.
 */
static std::expected<net::Socket, std::error_code>
openConnection(const std::string& path, int timeout_ms)
{
    auto sock_result = net::create_socket();
    if (!sock_result)
        return std::unexpected(sock_result.error());
    net::Socket sock = std::move(*sock_result);

    if (auto r = net::set_nonblocking(sock.fd(), true); !r)
        return std::unexpected(r.error());

    if (auto r = nb_connect(sock.fd(), path, timeout_ms); !r)
        return std::unexpected(r.error());

    return sock;
}

} // namespace

// ---------------------------------------------------------------------------
// SraUdpClient
// ---------------------------------------------------------------------------

SraUdpClient::SraUdpClient(std::string socketPath, int ioTimeoutMs)
    : socketPath_(std::move(socketPath)), ioTimeoutMs_(ioTimeoutMs)
{}

SraUdpClient::~SraUdpClient()
{
    stop();
}

void SraUdpClient::start()
{
    if (running_.load())
        return;
    stopRequested_.store(false);
    running_.store(true);
    thread_ = std::thread(&SraUdpClient::threadFunc, this);
}

void SraUdpClient::stop()
{
    stopRequested_.store(true);
    queueCv_.notify_all();
    if (thread_.joinable())
        thread_.join();
}

bool SraUdpClient::running() const noexcept
{
    return running_.load();
}

void SraUdpClient::submitAdd(cmdproto::SingleRouteRequest req)
{
    {
        std::lock_guard lock(queueMutex_);
        queue_.push(Request{std::move(req)});
    }
    queueCv_.notify_one();
}

void SraUdpClient::submitDelete(cmdproto::RouteDelParams params)
{
    {
        std::lock_guard lock(queueMutex_);
        queue_.push(Request{std::move(params)});
    }
    queueCv_.notify_one();
}

void SraUdpClient::submitList(std::string vrfs_name)
{
    {
        std::lock_guard lock(queueMutex_);
        queue_.push(Request{RouteListRequest{std::move(vrfs_name)}});
    }
    queueCv_.notify_one();
}

// ---------------------------------------------------------------------------
// threadFunc — main loop
// ---------------------------------------------------------------------------

void SraUdpClient::threadFunc()
{
    std::println("[SraUdpClient] thread started, socket='{}'", socketPath_);

    auto conn_result = openConnection(socketPath_, ioTimeoutMs_);
    if (!conn_result)
    {
        std::println(stderr,
                     "[SraUdpClient] connect({}): {}",
                     socketPath_,
                     conn_result.error().message());
        running_.store(false);
        std::println("[SraUdpClient] thread stopped");
        return;
    }
    net::Socket conn = std::move(*conn_result);
    std::println(
        "[SraUdpClient] connected fd={} path='{}'", conn.fd(), socketPath_);

    while (true)
    {
        Request req;

        {
            std::unique_lock lock(queueMutex_);
            queueCv_.wait(lock, [this] {
                return !queue_.empty() || stopRequested_.load();
            });
            if (stopRequested_.load() && queue_.empty())
                break;
            req = std::move(queue_.front());
            queue_.pop();
        }

        std::visit(
            [this, &conn](auto&& r) {
                using T = std::decay_t<decltype(r)>;
                if constexpr (std::is_same_v<T, cmdproto::SingleRouteRequest>)
                    processAddRequest(conn.fd(), r);
                // else if constexpr (std::is_same_v<T,
                // cmdproto::RouteDelParams>)
                //     processDeleteRequest(conn.fd(), r);
                // else if constexpr (std::is_same_v<T, RouteListRequest>)
                //     processListRequest(conn.fd(), r);
            },
            req);
    }

    running_.store(false);
    std::println("[SraUdpClient] thread stopped");
}

// ---------------------------------------------------------------------------
// processAddRequest — ROUTE_ADD binary payload
// ---------------------------------------------------------------------------

void SraUdpClient::processAddRequest(int fd,
                                     const cmdproto::SingleRouteRequest& req)
{
    static std::uint16_t s_msg_id = 1;
    const std::uint16_t msg_id = s_msg_id++;

    // Log what we're about to send
    std::println("[SraUdpClient] ROUTE_ADD msg_id={}: {} interface(s)",
                 msg_id,
                 req.interfaces.size());
    for (const auto& iface : req.interfaces)
    {
        std::println("[SraUdpClient]   iface='{}' nexthop={} nexthop_id={} "
                     "prefixes={}",
                     iface.iface_name.data(),
                     fmt_ip(iface.nexthop_addr_ipv4),
                     iface.nexthop_id_ipv4,
                     iface.prefixes.size());
        for (std::size_t i = 0; i < iface.prefixes.size(); ++i)
        {
            const auto& p = iface.prefixes[i];
            std::println("[SraUdpClient]     prefix[{}]: {}/{}",
                         i,
                         fmt_ip(p.addr),
                         static_cast<unsigned>(p.mask_len));
        }
    }

    // ── [1] Encode cmdproto ROUTE_ADD command ───────────────────────────────
    auto cmd_bytes =
        cmdproto::encode_command(cmdproto::make_route_add_binary(req));
    if (!cmd_bytes)
    {
        std::println(stderr,
                     "[SraUdpClient] encode_command: {}",
                     cmd_bytes.error().message());
        return;
    }
    logger::log(logger::DEBUG,
                "SraUdpClient",
                std::format("msg_id={} [1] cmdproto: {} byte(s)",
                            msg_id,
                            cmd_bytes->size()));

    // ── [2] Wrap in routeproto ExchangeData + DATA message ──────────────────
    routeproto::ExchangeData ed;
    ed.commands = *cmd_bytes;
    ed.num_commands = 1;

    auto exch_bytes = routeproto::encode_exchange(ed);
    if (!exch_bytes)
    {
        std::println(stderr,
                     "[SraUdpClient] encode_exchange: {}",
                     exch_bytes.error().message());
        return;
    }
    logger::log(logger::DEBUG,
                "SraUdpClient",
                std::format("msg_id={} [2] routeproto exchange: {} byte(s)",
                            msg_id,
                            exch_bytes->size()));

    auto data_msg = routeproto::make_data(
        msg_id, std::span<const std::uint8_t>{*exch_bytes});
    auto msg_bytes = routeproto::encode_message(data_msg);
    if (!msg_bytes)
    {
        std::println(stderr,
                     "[SraUdpClient] encode_message: {}",
                     msg_bytes.error().message());
        return;
    }
    logger::log(logger::DEBUG,
                "SraUdpClient",
                std::format("msg_id={} [2] routeproto message: {} byte(s)",
                            msg_id,
                            msg_bytes->size()));

    // ── [3] Wrap in udproto frame ────────────────────────────────────────────
    udproto::Packet pkt{
        .pkt_num = msg_id,
        .total_pkts = 1,
        .ctrl = 0x0000,
        .data = *msg_bytes,
    };
    auto frame = udproto::encode_packet(pkt);
    if (!frame)
    {
        std::println(stderr,
                     "[SraUdpClient] encode_packet: {}",
                     frame.error().message());
        return;
    }
    logger::log(logger::DEBUG,
                "SraUdpClient",
                std::format("msg_id={} [3] udproto frame: {} byte(s)",
                            msg_id,
                            frame->size()));

    // ── [4] Send frame (non-blocking with poll) ──────────────────────────────
    logger::log_hex("SraUdpClient", true, fd, *frame);
    if (auto r = nb_send_all(fd, *frame, ioTimeoutMs_); !r)
    {
        std::println(stderr, "[SraUdpClient] send: {}", r.error().message());
        return;
    }
    std::println("[SraUdpClient] frame sent fd={} msg_id={} {} byte(s)",
                 fd,
                 msg_id,
                 frame->size());

    // ── [5] Receive response frame (non-blocking with poll) ──────────────────
    auto frame_recv = nb_recv_frame(fd, ioTimeoutMs_);
    if (!frame_recv)
    {
        std::println(
            stderr, "[SraUdpClient] recv: {}", frame_recv.error().message());
        return;
    }
    logger::log_hex("SraUdpClient", false, fd, *frame_recv);

    // ── [8] Decode udproto packet ────────────────────────────────────────────
    auto reply_pkt = udproto::decode_packet(*frame_recv);
    if (!reply_pkt)
    {
        std::println(stderr,
                     "[SraUdpClient] decode_packet: {}",
                     reply_pkt.error().message());
        return;
    }
    logger::log(logger::DEBUG,
                "SraUdpClient",
                std::format("msg_id={} [8] udproto pkt: pkt_num={} "
                            "total_pkts={} ctrl={:#06x} data={} byte(s)",
                            msg_id,
                            reply_pkt->pkt_num,
                            reply_pkt->total_pkts,
                            reply_pkt->ctrl,
                            reply_pkt->data.size()));

    // ── [9] Decode routeproto message ────────────────────────────────────────
    auto reply_msg = routeproto::decode_message(reply_pkt->data);
    if (!reply_msg)
    {
        std::println(stderr,
                     "[SraUdpClient] decode_message: {}",
                     reply_msg.error().message());
        return;
    }
    logger::log(logger::DEBUG,
                "SraUdpClient",
                std::format("msg_id={} [9] routeproto msg: "
                            "type=0x{:02x} payload={} byte(s)",
                            msg_id,
                            static_cast<unsigned>(reply_msg->msg_type),
                            reply_msg->payload.size()));

    if (reply_msg->payload.size() < 2)
    {
        std::println(stderr,
                     "[SraUdpClient] response payload too short ({} bytes)",
                     reply_msg->payload.size());
        return;
    }

    const std::uint8_t ack_status = reply_msg->payload[0];

    // ── [10] Decode binary route-add response ────────────────────────────────
    auto bin_resp = cmdproto::decode_route_add_binary_response(
        std::span<const std::uint8_t>{reply_msg->payload}.subspan(1));
    if (!bin_resp)
    {
        std::println(stderr,
                     "[SraUdpClient] decode_route_add_binary_response: {}",
                     bin_resp.error().message());
        return;
    }

    // ── [11] Log per-prefix result bits ─────────────────────────────────────
    std::println("[SraUdpClient] response msg_id={}: msg_type=0x{:02x} "
                 "ack=0x{:02x} overall_status=0x{:02x} ({})",
                 msg_id,
                 static_cast<unsigned>(reply_msg->msg_type),
                 static_cast<unsigned>(ack_status),
                 static_cast<unsigned>(bin_resp->status_code),
                 bin_resp->status_code == 0x00 ? "all prefixes ok"
                                               : "one or more failures");

    for (std::size_t i = 0; i < bin_resp->prefix_status.size(); ++i)
    {
        std::println("[SraUdpClient]   prefix[{}]: {}",
                     i,
                     bin_resp->prefix_status[i] ? "ok" : "FAIL");
    }

    std::println("[SraUdpClient] exchange complete msg_id={}: {}",
                 msg_id,
                 bin_resp->status_code == 0x00 ? "PASS" : "FAIL");
}

// ---------------------------------------------------------------------------
// processDeleteRequest — ROUTE_DEL TLV
// ---------------------------------------------------------------------------

void SraUdpClient::processDeleteRequest(int fd,
                                        const cmdproto::RouteDelParams& params)
{
    static std::uint16_t s_msg_id =
        0x8000; // separate counter to distinguish add/del
    const std::uint16_t msg_id = s_msg_id++;

    std::println("[SraUdpClient] ROUTE_DEL msg_id={}: {}.{}.{}.{}/{}",
                 msg_id,
                 static_cast<unsigned>(params.dst_addr[0]),
                 static_cast<unsigned>(params.dst_addr[1]),
                 static_cast<unsigned>(params.dst_addr[2]),
                 static_cast<unsigned>(params.dst_addr[3]),
                 static_cast<unsigned>(params.prefix_len));

    // ── [1] Encode cmdproto ROUTE_DEL command ───────────────────────────────
    auto cmd_bytes = cmdproto::encode_command(cmdproto::make_route_del(params));
    if (!cmd_bytes)
    {
        std::println(stderr,
                     "[SraUdpClient] ROUTE_DEL encode_command: {}",
                     cmd_bytes.error().message());
        return;
    }

    // ── [2] Wrap in routeproto ExchangeData + DATA message ──────────────────
    routeproto::ExchangeData ed;
    ed.commands = *cmd_bytes;
    ed.num_commands = 1;

    auto exch_bytes = routeproto::encode_exchange(ed);
    if (!exch_bytes)
    {
        std::println(stderr,
                     "[SraUdpClient] ROUTE_DEL encode_exchange: {}",
                     exch_bytes.error().message());
        return;
    }

    auto data_msg = routeproto::make_data(
        msg_id, std::span<const std::uint8_t>{*exch_bytes});
    auto msg_bytes = routeproto::encode_message(data_msg);
    if (!msg_bytes)
    {
        std::println(stderr,
                     "[SraUdpClient] ROUTE_DEL encode_message: {}",
                     msg_bytes.error().message());
        return;
    }

    // ── [3] Wrap in udproto frame ────────────────────────────────────────────
    udproto::Packet pkt{
        .pkt_num = msg_id,
        .total_pkts = 1,
        .ctrl = 0x0000,
        .data = *msg_bytes,
    };
    auto frame = udproto::encode_packet(pkt);
    if (!frame)
    {
        std::println(stderr,
                     "[SraUdpClient] ROUTE_DEL encode_packet: {}",
                     frame.error().message());
        return;
    }

    // ── [4] Send frame ───────────────────────────────────────────────────────
    logger::log_hex("SraUdpClient", true, fd, *frame);
    if (auto r = nb_send_all(fd, *frame, ioTimeoutMs_); !r)
    {
        std::println(
            stderr, "[SraUdpClient] ROUTE_DEL send: {}", r.error().message());
        return;
    }
    std::println("[SraUdpClient] ROUTE_DEL frame sent msg_id={} {} byte(s)",
                 msg_id,
                 frame->size());

    // ── [5] Receive response frame ───────────────────────────────────────────
    auto frame_recv = nb_recv_frame(fd, ioTimeoutMs_);
    if (!frame_recv)
    {
        std::println(stderr,
                     "[SraUdpClient] ROUTE_DEL recv: {}",
                     frame_recv.error().message());
        return;
    }
    logger::log_hex("SraUdpClient", false, fd, *frame_recv);

    // ── [8] Decode udproto packet ────────────────────────────────────────────
    auto reply_pkt = udproto::decode_packet(*frame_recv);
    if (!reply_pkt)
    {
        std::println(stderr,
                     "[SraUdpClient] ROUTE_DEL decode_packet: {}",
                     reply_pkt.error().message());
        return;
    }

    // ── [9] Decode routeproto message ────────────────────────────────────────
    auto reply_msg = routeproto::decode_message(reply_pkt->data);
    if (!reply_msg)
    {
        std::println(stderr,
                     "[SraUdpClient] ROUTE_DEL decode_message: {}",
                     reply_msg.error().message());
        return;
    }

    if (reply_msg->payload.empty())
    {
        std::println(stderr, "[SraUdpClient] ROUTE_DEL response payload empty");
        return;
    }

    const std::uint8_t ack_status = reply_msg->payload[0];
    // ── [10] Log result ──────────────────────────────────────────────────────
    std::println(
        "[SraUdpClient] ROUTE_DEL response msg_id={}: ack=0x{:02x} ({})",
        msg_id,
        static_cast<unsigned>(ack_status),
        ack_status == 0x00 ? "ok" : "error");

    std::println("[SraUdpClient] ROUTE_DEL exchange complete msg_id={}: {}",
                 msg_id,
                 ack_status == 0x00 ? "PASS" : "FAIL");
}

// ---------------------------------------------------------------------------
// processListRequest — ROUTE_LIST
// ---------------------------------------------------------------------------

void SraUdpClient::processListRequest(int fd, const RouteListRequest& req)
{
    static std::uint16_t s_msg_id =
        0xC000; // separate counter for list commands
    const std::uint16_t msg_id = s_msg_id++;

    std::println(
        "[SraUdpClient] ROUTE_LIST msg_id={} vrf='{}'", msg_id, req.vrfs_name);

    // ── [1] Encode cmdproto ROUTE_LIST command ───────────────────────────────
    auto cmd_bytes =
        cmdproto::encode_command(cmdproto::make_route_list(req.vrfs_name));
    if (!cmd_bytes)
    {
        std::println(stderr,
                     "[SraUdpClient] ROUTE_LIST encode_command: {}",
                     cmd_bytes.error().message());
        return;
    }

    // ── [2] Wrap in routeproto ExchangeData + DATA message ──────────────────
    routeproto::ExchangeData ed;
    ed.commands = *cmd_bytes;
    ed.num_commands = 1;

    auto exch_bytes = routeproto::encode_exchange(ed);
    if (!exch_bytes)
    {
        std::println(stderr,
                     "[SraUdpClient] ROUTE_LIST encode_exchange: {}",
                     exch_bytes.error().message());
        return;
    }

    auto data_msg = routeproto::make_data(
        msg_id, std::span<const std::uint8_t>{*exch_bytes});
    auto msg_bytes = routeproto::encode_message(data_msg);
    if (!msg_bytes)
    {
        std::println(stderr,
                     "[SraUdpClient] ROUTE_LIST encode_message: {}",
                     msg_bytes.error().message());
        return;
    }

    // ── [3] Wrap in udproto frame ────────────────────────────────────────────
    udproto::Packet pkt{
        .pkt_num = msg_id,
        .total_pkts = 1,
        .ctrl = 0x0000,
        .data = *msg_bytes,
    };
    auto frame = udproto::encode_packet(pkt);
    if (!frame)
    {
        std::println(stderr,
                     "[SraUdpClient] ROUTE_LIST encode_packet: {}",
                     frame.error().message());
        return;
    }

    // ── [4] Send frame ───────────────────────────────────────────────────────
    logger::log_hex("SraUdpClient", true, fd, *frame);
    if (auto r = nb_send_all(fd, *frame, ioTimeoutMs_); !r)
    {
        std::println(
            stderr, "[SraUdpClient] ROUTE_LIST send: {}", r.error().message());
        return;
    }
    std::println("[SraUdpClient] ROUTE_LIST frame sent msg_id={} {} byte(s)",
                 msg_id,
                 frame->size());

    // ── [5] Receive response frame ───────────────────────────────────────────
    auto frame_recv = nb_recv_frame(fd, ioTimeoutMs_);
    if (!frame_recv)
    {
        std::println(stderr,
                     "[SraUdpClient] ROUTE_LIST recv: {}",
                     frame_recv.error().message());
        return;
    }
    logger::log_hex("SraUdpClient", false, fd, *frame_recv);

    // ── [8] Decode udproto packet ────────────────────────────────────────────
    auto reply_pkt = udproto::decode_packet(*frame_recv);
    if (!reply_pkt)
    {
        std::println(stderr,
                     "[SraUdpClient] ROUTE_LIST decode_packet: {}",
                     reply_pkt.error().message());
        return;
    }

    // ── [9] Decode routeproto message ────────────────────────────────────────
    auto reply_msg = routeproto::decode_message(reply_pkt->data);
    if (!reply_msg)
    {
        std::println(stderr,
                     "[SraUdpClient] ROUTE_LIST decode_message: {}",
                     reply_msg.error().message());
        return;
    }

    if (reply_msg->payload.empty())
    {
        std::println(stderr,
                     "[SraUdpClient] ROUTE_LIST response payload empty");
        return;
    }

    const std::uint8_t ack_status = reply_msg->payload[0];

    // ── [10] Decode route-list response ─────────────────────────────────────
    auto routes = cmdproto::decode_route_list_response(
        std::span<const std::uint8_t>{reply_msg->payload}.subspan(1));
    if (!routes)
    {
        std::println(stderr,
                     "[SraUdpClient] ROUTE_LIST decode_route_list_response: {}",
                     routes.error().message());
        return;
    }

    // ── [11] Log route table ─────────────────────────────────────────────────
    std::println("[SraUdpClient] ROUTE_LIST response msg_id={}: ack=0x{:02x} "
                 "{} route(s)",
                 msg_id,
                 static_cast<unsigned>(ack_status),
                 routes->size());

    for (std::size_t i = 0; i < routes->size(); ++i)
    {
        const auto& r = (*routes)[i];
        std::println("[SraUdpClient]   route[{}]: {}.{}.{}.{}/{} "
                     "via {}.{}.{}.{} dev {} metric {}",
                     i,
                     static_cast<unsigned>(r.dst_addr[0]),
                     static_cast<unsigned>(r.dst_addr[1]),
                     static_cast<unsigned>(r.dst_addr[2]),
                     static_cast<unsigned>(r.dst_addr[3]),
                     static_cast<unsigned>(r.prefix_len),
                     static_cast<unsigned>(r.gateway[0]),
                     static_cast<unsigned>(r.gateway[1]),
                     static_cast<unsigned>(r.gateway[2]),
                     static_cast<unsigned>(r.gateway[3]),
                     r.if_name,
                     static_cast<unsigned>(r.metric));
    }

    std::println("[SraUdpClient] ROUTE_LIST exchange complete msg_id={}: PASS",
                 msg_id);
}

} // namespace sra
