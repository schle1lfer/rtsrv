/**
 * @file client/src/vrf_udp_client.cpp
 * @brief VRF UDP client thread implementation.
 *
 * Adapted from unix_domain/test/route_add_vrfs.cpp.  The original test's
 * main() is refactored into VrfUdpClient::run() which executes in its own
 * std::thread.
 */

#include "client/vrf_udp_client.hpp"

#include "lib/cmd_proto.hpp"
#include "lib/net.hpp"
#include "lib/route_proto.hpp"
#include "lib/ud_proto.hpp"

#include <array>
#include <cstdio>
#include <print>
#include <vector>

namespace sra
{

// ---------------------------------------------------------------------------
// Helpers (file-local)
// ---------------------------------------------------------------------------

namespace
{

/**
 * @brief Formats a 4-byte IPv4 address as a dotted-decimal string.
 * @param a  IPv4 address bytes in network byte order.
 * @return Human-readable string such as @c "192.168.1.1".
 */
static std::string fmt_ip(const cmdproto::Ipv4Addr& a)
{
    char buf[16];
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
 * @brief Copies a VRF name string into a fixed-width, null-padded array.
 * @param s  Source string.  Characters beyond @c VRFS_NAME_SIZE are truncated.
 * @return A @c VrfsName array zero-padded to @c cmdproto::VRFS_NAME_SIZE bytes.
 */
static cmdproto::VrfsName make_vrfs_name(const std::string& s)
{
    cmdproto::VrfsName name{};
    for (std::size_t i = 0; i < cmdproto::VRFS_NAME_SIZE && i < s.size(); ++i)
        name[i] = s[i];
    return name;
}

/**
 * @brief Reads bytes from @p fd until one complete udproto frame is available.
 *
 * Calls @c net::recv_data() in a loop, accumulating data in an internal
 * buffer, until @c udproto::find_frame() identifies a delimited frame.
 *
 * @param fd  Connected socket descriptor to read from.
 * @return The raw frame bytes (including 0x7E delimiters) on success,
 *         or an error code if the socket is closed or an I/O error occurs.
 */
static std::expected<std::vector<std::uint8_t>, std::error_code>
recv_frame(int fd)
{
    std::vector<std::uint8_t> buf;
    buf.reserve(256);
    std::array<std::uint8_t, 1024> tmp{};

    while (true)
    {
        auto n = net::recv_data(fd, tmp);
        if (!n)
            return std::unexpected(n.error());
        buf.insert(buf.end(), tmp.begin(), tmp.begin() + *n);

        std::size_t end = 0;
        auto span = udproto::find_frame(buf, end);
        if (!span)
            continue;
        return std::vector<std::uint8_t>{span->begin(), span->end()};
    }
}

} // namespace

// ---------------------------------------------------------------------------
// VrfUdpClient
// ---------------------------------------------------------------------------

VrfUdpClient::VrfUdpClient(VrfUdpClientConfig cfg) : cfg_(std::move(cfg))
{}

VrfUdpClient::~VrfUdpClient()
{
    stop();
}

void VrfUdpClient::start()
{
    if (running_.load())
        return;
    running_.store(true);
    thread_ = std::thread(&VrfUdpClient::run, this);
}

void VrfUdpClient::stop()
{
    running_.store(false);
    if (thread_.joinable())
        thread_.join();
}

bool VrfUdpClient::running() const noexcept
{
    return running_.load();
}

// ---------------------------------------------------------------------------
// run() — the thread body (one request/response cycle)
// ---------------------------------------------------------------------------

/**
 * @brief Thread body: performs one complete VRF route-add request/response.
 *
 * Executes the following pipeline:
 *  1. Build a @c VrfsRouteRequest from configuration.
 *  2. Encode it as a cmdproto command.
 *  3. Wrap it in a routeproto DATA message.
 *  4. Wrap it in a udproto frame.
 *  5. Open a UNIX-domain socket connection and send the frame.
 *  6. Receive and decode the server's response frame.
 *  7. Log the outcome and set @c running_ to @c false.
 *
 * The method is executed exactly once per @c start() call and exits when the
 * exchange completes or an error is encountered.
 */
void VrfUdpClient::run()
{
    static std::uint16_t s_msg_id = 1;
    const std::uint16_t msg_id = s_msg_id++;

    std::println("[vrf_udp_client] starting: socket={} vrf={} nexthop={}",
                 cfg_.socket_path,
                 cfg_.vrfs_name,
                 fmt_ip(cfg_.nexthop_addr));

    // ── [1] Build VrfsRouteRequest ──────────────────────────────────────────

    cmdproto::VrfsRouteRequest req;
    cmdproto::VrfsRequest vrf_req;
    vrf_req.vrfs_name = make_vrfs_name(cfg_.vrfs_name);
    vrf_req.nexthop_addr_ipv4 = cfg_.nexthop_addr;
    vrf_req.nexthop_id_ipv4 = cfg_.nexthop_id;
    vrf_req.prefixes = {
        cmdproto::PrefixIpv4{{10, 0, 1, 0}, 24},
        cmdproto::PrefixIpv4{{10, 0, 2, 0}, 24},
        cmdproto::PrefixIpv4{{10, 0, 3, 0}, 24},
        cmdproto::PrefixIpv4{{10, 0, 4, 0}, 24},
        cmdproto::PrefixIpv4{{10, 0, 5, 0}, 24},
        cmdproto::PrefixIpv4{{10, 0, 6, 0}, 24},
        cmdproto::PrefixIpv4{{10, 0, 7, 0}, 24},
        cmdproto::PrefixIpv4{{10, 0, 8, 0}, 24},
        cmdproto::PrefixIpv4{{10, 0, 9, 0}, 24},
        cmdproto::PrefixIpv4{{10, 0, 10, 0}, 24},
    };
    req.vrfs_requests.push_back(std::move(vrf_req));

    // ── [2] Encode cmdproto command ─────────────────────────────────────────

    auto cmd_bytes =
        cmdproto::encode_command(cmdproto::make_route_add_vrfs(req));
    if (!cmd_bytes)
    {
        std::println(stderr,
                     "[vrf_udp_client] encode_command: {}",
                     cmd_bytes.error().message());
        running_.store(false);
        return;
    }

    // ── [3] Wrap in routeproto DATA message ─────────────────────────────────

    routeproto::ExchangeData ed;
    ed.commands = *cmd_bytes;
    ed.num_commands = 1;

    auto exch_bytes = routeproto::encode_exchange(ed);
    if (!exch_bytes)
    {
        std::println(stderr,
                     "[vrf_udp_client] encode_exchange: {}",
                     exch_bytes.error().message());
        running_.store(false);
        return;
    }

    auto data_msg = routeproto::make_data(
        msg_id, std::span<const std::uint8_t>{*exch_bytes});
    auto msg_bytes = routeproto::encode_message(data_msg);
    if (!msg_bytes)
    {
        std::println(stderr,
                     "[vrf_udp_client] encode_message: {}",
                     msg_bytes.error().message());
        running_.store(false);
        return;
    }

    // ── [4] Wrap in udproto frame ────────────────────────────────────────────

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
                     "[vrf_udp_client] encode_packet: {}",
                     frame.error().message());
        running_.store(false);
        return;
    }

    // ── [5] Connect and send ─────────────────────────────────────────────────

    auto sock_result = net::create_socket();
    if (!sock_result)
    {
        std::println(stderr,
                     "[vrf_udp_client] create_socket: {}",
                     sock_result.error().message());
        running_.store(false);
        return;
    }
    net::Socket sock = std::move(*sock_result);

    if (auto r = net::connect_socket(sock.fd(), cfg_.socket_path); !r)
    {
        std::println(stderr,
                     "[vrf_udp_client] connect_socket({}): {}",
                     cfg_.socket_path,
                     r.error().message());
        running_.store(false);
        return;
    }

    std::println("[vrf_udp_client] connected; sending {} byte frame",
                 frame->size());

    if (auto r = net::send_all(sock.fd(), *frame); !r)
    {
        std::println(
            stderr, "[vrf_udp_client] send_all: {}", r.error().message());
        running_.store(false);
        return;
    }

    // ── [6] Receive and decode response ─────────────────────────────────────

    auto frame_recv = recv_frame(sock.fd());
    if (!frame_recv)
    {
        std::println(stderr,
                     "[vrf_udp_client] recv_frame: {}",
                     frame_recv.error().message());
        running_.store(false);
        return;
    }

    auto reply_pkt = udproto::decode_packet(*frame_recv);
    if (!reply_pkt)
    {
        std::println(stderr,
                     "[vrf_udp_client] decode_packet: {}",
                     reply_pkt.error().message());
        running_.store(false);
        return;
    }

    auto reply_msg = routeproto::decode_message(reply_pkt->data);
    if (!reply_msg)
    {
        std::println(stderr,
                     "[vrf_udp_client] decode_message: {}",
                     reply_msg.error().message());
        running_.store(false);
        return;
    }

    if (reply_msg->payload.empty())
    {
        std::println(stderr, "[vrf_udp_client] empty payload in response");
        running_.store(false);
        return;
    }

    const std::uint8_t ack_status = reply_msg->payload[0];

    if (reply_msg->payload.size() < 2)
    {
        std::println(stderr, "[vrf_udp_client] no binary response payload");
        running_.store(false);
        return;
    }

    auto vrf_resp = cmdproto::decode_vrfs_route_response(
        std::span<const std::uint8_t>{reply_msg->payload}.subspan(1));
    if (!vrf_resp)
    {
        std::println(stderr,
                     "[vrf_udp_client] decode_vrfs_route_response: {}",
                     vrf_resp.error().message());
        running_.store(false);
        return;
    }

    // ── [7] Report result ────────────────────────────────────────────────────

    std::println("[vrf_udp_client] response: msg_type=0x{:02x} ack=0x{:02x} "
                 "status=0x{:02x} ({})",
                 static_cast<unsigned>(reply_msg->msg_type),
                 static_cast<unsigned>(ack_status),
                 vrf_resp->status_code,
                 vrf_resp->status_code == 0x00 ? "all prefixes ok"
                                               : "one or more failures");

    for (const auto& ans : vrf_resp->answers)
    {
        std::println("[vrf_udp_client]   vrf='{}' prefix_status={}",
                     ans.vrfs_name.data(),
                     ans.prefix_status ? "ok" : "failed");
    }

    std::println("[vrf_udp_client] done: {}",
                 vrf_resp->status_code == 0x00 ? "PASS" : "FAIL");

    running_.store(false);
}

} // namespace sra
