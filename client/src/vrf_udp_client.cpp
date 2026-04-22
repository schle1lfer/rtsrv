/**
 * @file client/src/vrf_udp_client.cpp
 * @brief VRF UDP client — non-blocking UNIX-domain socket with command queue.
 *
 * The background thread waits for VrfsRouteRequest commands, connects to the
 * UNIX-domain server (O_NONBLOCK socket + poll() for every I/O step), sends
 * the encoded frame, receives the response, decodes and logs the per-VRF
 * status bitmask.
 */

#include "client/vrf_udp_client.hpp"

#include "lib/cmd_proto.hpp"
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
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
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
        buf.insert(buf.end(), tmp.begin(),
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
        return std::unexpected(
            std::error_code(err, std::system_category()));
    return {};
}

} // namespace

// ---------------------------------------------------------------------------
// VrfUdpClient
// ---------------------------------------------------------------------------

VrfUdpClient::VrfUdpClient(std::string socketPath, int ioTimeoutMs)
    : socketPath_(std::move(socketPath)), ioTimeoutMs_(ioTimeoutMs)
{}

VrfUdpClient::~VrfUdpClient()
{
    stop();
}

void VrfUdpClient::start()
{
    if (running_.load())
        return;
    stopRequested_.store(false);
    running_.store(true);
    thread_ = std::thread(&VrfUdpClient::threadFunc, this);
}

void VrfUdpClient::stop()
{
    stopRequested_.store(true);
    queueCv_.notify_all();
    if (thread_.joinable())
        thread_.join();
}

bool VrfUdpClient::running() const noexcept
{
    return running_.load();
}

void VrfUdpClient::submit(cmdproto::VrfsRouteRequest req)
{
    {
        std::lock_guard lock(queueMutex_);
        queue_.push(std::move(req));
    }
    queueCv_.notify_one();
}

// ---------------------------------------------------------------------------
// threadFunc — main loop
// ---------------------------------------------------------------------------

void VrfUdpClient::threadFunc()
{
    std::println("[VrfUdpClient] thread started, socket='{}'", socketPath_);

    while (!stopRequested_.load())
    {
        cmdproto::VrfsRouteRequest req;

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

        processRequest(req);
    }

    running_.store(false);
    std::println("[VrfUdpClient] thread stopped");
}

// ---------------------------------------------------------------------------
// processRequest — encode → connect (non-blocking) → send → recv → log
// ---------------------------------------------------------------------------

void VrfUdpClient::processRequest(const cmdproto::VrfsRouteRequest& req)
{
    static std::uint16_t s_msg_id = 1;
    const std::uint16_t  msg_id   = s_msg_id++;

    // Log what we're about to send
    std::println("[VrfUdpClient] processing request: {} VRF(s)", req.vrfs_requests.size());
    for (const auto& vr : req.vrfs_requests)
    {
        std::println("[VrfUdpClient]   vrf='{}' nexthop={} nexthop_id={} "
                     "prefixes={}",
                     vr.vrfs_name.data(),
                     fmt_ip(vr.nexthop_addr_ipv4),
                     vr.nexthop_id_ipv4,
                     vr.prefixes.size());
        for (std::size_t i = 0; i < vr.prefixes.size(); ++i)
        {
            const auto& p = vr.prefixes[i];
            std::println("[VrfUdpClient]     prefix[{}]: {}/{}", i,
                         fmt_ip(p.addr), static_cast<unsigned>(p.mask_len));
        }
    }

    // ── [1] Encode cmdproto ROUTE_ADD command ───────────────────────────────
    auto cmd_bytes =
        cmdproto::encode_command(cmdproto::make_route_add_vrfs(req));
    if (!cmd_bytes)
    {
        std::println(stderr, "[VrfUdpClient] encode_command: {}",
                     cmd_bytes.error().message());
        return;
    }

    // ── [2] Wrap in routeproto ExchangeData + DATA message ──────────────────
    routeproto::ExchangeData ed;
    ed.commands    = *cmd_bytes;
    ed.num_commands = 1;

    auto exch_bytes = routeproto::encode_exchange(ed);
    if (!exch_bytes)
    {
        std::println(stderr, "[VrfUdpClient] encode_exchange: {}",
                     exch_bytes.error().message());
        return;
    }

    auto data_msg  = routeproto::make_data(
        msg_id, std::span<const std::uint8_t>{*exch_bytes});
    auto msg_bytes = routeproto::encode_message(data_msg);
    if (!msg_bytes)
    {
        std::println(stderr, "[VrfUdpClient] encode_message: {}",
                     msg_bytes.error().message());
        return;
    }

    // ── [3] Wrap in udproto frame ────────────────────────────────────────────
    udproto::Packet pkt{
        .pkt_num    = msg_id,
        .total_pkts = 1,
        .ctrl       = 0x0000,
        .data       = *msg_bytes,
    };
    auto frame = udproto::encode_packet(pkt);
    if (!frame)
    {
        std::println(stderr, "[VrfUdpClient] encode_packet: {}",
                     frame.error().message());
        return;
    }

    // ── [4] Create NON-BLOCKING socket ──────────────────────────────────────
    auto sock_result = net::create_socket();
    if (!sock_result)
    {
        std::println(stderr, "[VrfUdpClient] create_socket: {}",
                     sock_result.error().message());
        return;
    }
    net::Socket sock = std::move(*sock_result);

    if (auto r = net::set_nonblocking(sock.fd(), true); !r)
    {
        std::println(stderr, "[VrfUdpClient] set_nonblocking: {}",
                     r.error().message());
        return;
    }

    // ── [5] Connect (non-blocking) ───────────────────────────────────────────
    if (auto r = nb_connect(sock.fd(), socketPath_, ioTimeoutMs_); !r)
    {
        std::println(stderr, "[VrfUdpClient] connect({}): {}",
                     socketPath_, r.error().message());
        return;
    }
    std::println("[VrfUdpClient] connected (non-blocking); "
                 "sending {} byte frame", frame->size());

    // ── [6] Send frame (non-blocking with poll) ──────────────────────────────
    if (auto r = nb_send_all(sock.fd(), *frame, ioTimeoutMs_); !r)
    {
        std::println(stderr, "[VrfUdpClient] send: {}", r.error().message());
        return;
    }
    std::println("[VrfUdpClient] frame sent ({} bytes)", frame->size());

    // ── [7] Receive response frame (non-blocking with poll) ──────────────────
    auto frame_recv = nb_recv_frame(sock.fd(), ioTimeoutMs_);
    if (!frame_recv)
    {
        std::println(stderr, "[VrfUdpClient] recv: {}",
                     frame_recv.error().message());
        return;
    }

    // ── [8] Decode udproto packet ────────────────────────────────────────────
    auto reply_pkt = udproto::decode_packet(*frame_recv);
    if (!reply_pkt)
    {
        std::println(stderr, "[VrfUdpClient] decode_packet: {}",
                     reply_pkt.error().message());
        return;
    }

    // ── [9] Decode routeproto message ────────────────────────────────────────
    auto reply_msg = routeproto::decode_message(reply_pkt->data);
    if (!reply_msg)
    {
        std::println(stderr, "[VrfUdpClient] decode_message: {}",
                     reply_msg.error().message());
        return;
    }

    if (reply_msg->payload.size() < 2)
    {
        std::println(stderr,
                     "[VrfUdpClient] response payload too short ({} bytes)",
                     reply_msg->payload.size());
        return;
    }

    const std::uint8_t ack_status = reply_msg->payload[0];

    // ── [10] Decode VRF response ─────────────────────────────────────────────
    auto vrf_resp = cmdproto::decode_vrfs_route_response(
        std::span<const std::uint8_t>{reply_msg->payload}.subspan(1));
    if (!vrf_resp)
    {
        std::println(stderr, "[VrfUdpClient] decode_vrfs_route_response: {}",
                     vrf_resp.error().message());
        return;
    }

    // ── [11] Log bitmask for each VRF prefix set ─────────────────────────────
    std::println("[VrfUdpClient] response: msg_type=0x{:02x} ack=0x{:02x} "
                 "overall_status=0x{:02x} ({})",
                 static_cast<unsigned>(reply_msg->msg_type),
                 static_cast<unsigned>(ack_status),
                 static_cast<unsigned>(vrf_resp->status_code),
                 vrf_resp->status_code == 0x00 ? "all VRFs ok"
                                               : "one or more failures");

    for (std::size_t i = 0; i < vrf_resp->answers.size(); ++i)
    {
        const auto& ans = vrf_resp->answers[i];
        // prefix_status is 1 byte on wire (bit 0 = success flag) — log as hex
        const std::uint8_t bitmask =
            ans.prefix_status ? static_cast<std::uint8_t>(0x01)
                              : static_cast<std::uint8_t>(0x00);
        std::println("[VrfUdpClient]   answer[{}]: vrf='{}' "
                     "prefix_status_bitmask=0x{:02x} ({})",
                     i,
                     ans.vrfs_name.data(),
                     static_cast<unsigned>(bitmask),
                     ans.prefix_status ? "installed ok" : "install failed");
    }

    std::println("[VrfUdpClient] exchange complete: {}",
                 vrf_resp->status_code == 0x00 ? "PASS" : "FAIL");
}

} // namespace sra
