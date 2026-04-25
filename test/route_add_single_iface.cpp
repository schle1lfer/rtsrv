/**
 * @file test/route_add_single_iface.cpp
 * @brief End-to-end test for the INTERFACE_ROUTE (type=2) command protocol.
 *
 * Connects to a running ud_server instance, sends a ROUTE_ADD command
 * encoded as a SingleRouteRequest (INTERFACE_ROUTE payload type), and
 * decodes the RouteAddBinaryResponse.  Exits 0 on success, 1 on any error.
 *
 * ## Protocol stack (outbound)
 *   SingleRouteRequest
 *     → cmdproto::make_route_add_binary  (INTERFACE_ROUTE, type=2)
 *     → encode_command
 *     → routeproto ExchangeData + DATA message
 *     → udproto frame
 *     → AF_UNIX socket
 *
 * ## Protocol stack (inbound)
 *   AF_UNIX socket
 *     → udproto frame
 *     → routeproto DATA_ACK
 *     → payload[0]       = outer ack_status
 *     → payload.subspan(1) = RouteAddBinaryResponse bytes
 *     → decode_route_add_binary_response
 *
 * ## Prerequisites
 *   A ud_server process must be listening on the target socket path.
 *   The server requires CAP_NET_ADMIN / root to install kernel routes;
 *   prefix_status bits will be false when run without sufficient privileges.
 *
 * ## Usage
 * @code
 *   ud_server &                        # start the server
 *   ./route_add_single_iface [socket]  # run this test
 *   # Default socket: /tmp/ud_server.sock
 * @endcode
 *
 * @par Expected output (success)
 * @code
 *   === route_add_single_iface test ===
 *   socket : /tmp/ud_server.sock
 *   iface  : eth0
 *   nexthop: 10.0.0.1
 *   nexthop_id: 42
 *   prefixes:
 *     [0]  192.168.100.0/24
 *     [1]  192.168.200.0/24
 *
 *   [1/6] encoding ROUTE_ADD INTERFACE_ROUTE command ...  OK (N bytes)
 *   [2/6] connecting to /tmp/ud_server.sock ...           OK
 *   [3/6] sending frame ...                               OK (N bytes)
 *   [4/6] receiving response frame ...                    OK (N bytes)
 *   [5/6] decoding response ...                           OK
 *   [6/6] verifying response ...
 *         ack_status   : 0x00  (ok)
 *         overall      : 0x00  (all prefixes ok)
 *         prefix[0]    : ok
 *         prefix[1]    : ok
 *
 *   === TEST PASSED ===
 * @endcode
 *
 * @version 1.0
 */

#include "lib/cmd_proto.hpp"
#include "lib/logger.hpp"
#include "lib/net.hpp"
#include "lib/route_proto.hpp"
#include "lib/ud_proto.hpp"

#include <arpa/inet.h>
#include <poll.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <print>
#include <string>
#include <string_view>
#include <vector>

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------

static constexpr std::string_view kIfaceName = "eth0";
static constexpr std::string_view kNexthopIp = "10.0.0.1";
static constexpr std::uint32_t kNexthopId = 42;

struct TestPrefix
{
    std::string_view cidr;
    std::string_view addr;
    std::uint8_t mask;
};

static constexpr std::array<TestPrefix, 2> kPrefixes{{
    {"192.168.100.0/24", "192.168.100.0", 24},
    {"192.168.200.0/24", "192.168.200.0", 24},
}};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/**
 * @brief Parses a dotted-decimal IPv4 string into a 4-byte network-order array.
 * @return true on success.
 */
static bool parse_ipv4(std::string_view s, cmdproto::Ipv4Addr& out)
{
    struct in_addr a
    {};
    if (::inet_pton(AF_INET, std::string(s).c_str(), &a) != 1)
        return false;
    const auto* b = reinterpret_cast<const std::uint8_t*>(&a.s_addr);
    out = {b[0], b[1], b[2], b[3]};
    return true;
}

/**
 * @brief Sends all bytes over a non-blocking socket using poll(POLLOUT).
 */
static std::expected<void, std::error_code>
nb_send_all(int fd, std::span<const std::uint8_t> data, int timeout_ms = 5000)
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
                net::make_error_code(net::NetError::SendFailed));

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
 * @brief Receives bytes until a complete udproto frame is buffered.
 */
static std::expected<std::vector<std::uint8_t>, std::error_code>
nb_recv_frame(int fd, int timeout_ms = 5000)
{
    std::vector<std::uint8_t> buf;
    buf.reserve(1024);
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
                net::make_error_code(net::NetError::RecvFailed));

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

// ---------------------------------------------------------------------------
// Test phases
// ---------------------------------------------------------------------------

/**
 * @brief Phase 1 – builds and encodes the INTERFACE_ROUTE ROUTE_ADD command.
 *
 * Constructs a SingleRouteRequest with one interface entry (kIfaceName,
 * kNexthopIp, kNexthopId) and the kPrefixes prefix list, then encodes the
 * full protocol stack: cmdproto → ExchangeData → routeproto DATA → udproto.
 */
static std::expected<std::vector<std::uint8_t>, std::error_code> phase_encode()
{
    std::print("[1/6] encoding ROUTE_ADD INTERFACE_ROUTE command ... ");

    // Build the interface entry.
    cmdproto::Interface iface{};

    // Null-padded 32-byte interface name.
    for (std::size_t i = 0;
         i < cmdproto::IFACE_NAME_SIZE && i < kIfaceName.size();
         ++i)
        iface.iface_name[i] = kIfaceName[i];

    if (!parse_ipv4(kNexthopIp, iface.nexthop_addr_ipv4))
    {
        std::println("FAIL (bad nexthop address)");
        return std::unexpected(
            cmdproto::make_error_code(cmdproto::CmdError::InvalidField));
    }
    iface.nexthop_id_ipv4 = kNexthopId;

    for (const auto& p : kPrefixes)
    {
        cmdproto::PrefixIpv4 pfx{};
        if (!parse_ipv4(p.addr, pfx.addr))
        {
            std::println("FAIL (bad prefix address {})", p.addr);
            return std::unexpected(
                cmdproto::make_error_code(cmdproto::CmdError::InvalidField));
        }
        pfx.mask_len = p.mask;
        iface.prefixes.push_back(pfx);
    }

    cmdproto::SingleRouteRequest req;
    req.interfaces.push_back(std::move(iface));

    // cmdproto: ROUTE_ADD with INTERFACE_ROUTE binary payload.
    auto cmd = cmdproto::make_route_add_binary(req);
    auto cmd_bytes = cmdproto::encode_command(cmd);
    if (!cmd_bytes)
    {
        std::println("FAIL (encode_command: {})", cmd_bytes.error().message());
        return std::unexpected(cmd_bytes.error());
    }

    // routeproto: wrap in ExchangeData + DATA message.
    routeproto::ExchangeData ed;
    ed.commands = *cmd_bytes;
    ed.num_commands = 1;

    auto exch_bytes = routeproto::encode_exchange(ed);
    if (!exch_bytes)
    {
        std::println("FAIL (encode_exchange: {})",
                     exch_bytes.error().message());
        return std::unexpected(exch_bytes.error());
    }

    static std::uint16_t s_msg_id = 1;
    const std::uint16_t msg_id = s_msg_id++;

    auto data_msg = routeproto::make_data(msg_id, *exch_bytes);
    auto msg_bytes = routeproto::encode_message(data_msg);
    if (!msg_bytes)
    {
        std::println("FAIL (encode_message: {})", msg_bytes.error().message());
        return std::unexpected(msg_bytes.error());
    }

    // udproto: wrap in a framed packet.
    udproto::Packet pkt{
        .pkt_num = msg_id,
        .total_pkts = 1,
        .ctrl = 0x0000,
        .data = *msg_bytes,
    };
    auto frame = udproto::encode_packet(pkt);
    if (!frame)
    {
        std::println("FAIL (encode_packet: {})", frame.error().message());
        return std::unexpected(frame.error());
    }

    std::println("OK ({} bytes)", frame->size());
    return *frame;
}

/**
 * @brief Phase 2 – connects a non-blocking socket to the ud_server.
 */
static std::expected<net::Socket, std::error_code>
phase_connect(const std::string& sock_path)
{
    std::print("[2/6] connecting to {} ... ", sock_path);

    auto sock_result = net::create_socket();
    if (!sock_result)
    {
        std::println("FAIL (create_socket: {})", sock_result.error().message());
        return std::unexpected(sock_result.error());
    }

    if (auto r = net::set_nonblocking(sock_result->fd(), true); !r)
    {
        std::println("FAIL (set_nonblocking: {})", r.error().message());
        return std::unexpected(r.error());
    }

    // AF_UNIX connect is typically instantaneous; handle EINPROGRESS.
    if (auto r = net::connect_socket(sock_result->fd(), sock_path); !r)
    {
        if (errno != EINPROGRESS)
        {
            std::println("FAIL (connect: {})", r.error().message());
            return std::unexpected(r.error());
        }
        pollfd pfd{sock_result->fd(), POLLOUT, 0};
        if (::poll(&pfd, 1, 5000) <= 0)
        {
            std::println("FAIL (connect timeout)");
            return std::unexpected(
                net::make_error_code(net::NetError::ConnectFailed));
        }
        int err = 0;
        socklen_t len = sizeof(err);
        ::getsockopt(sock_result->fd(), SOL_SOCKET, SO_ERROR, &err, &len);
        if (err != 0)
        {
            std::println("FAIL (SO_ERROR: {})", std::strerror(err));
            return std::unexpected(
                std::error_code(err, std::system_category()));
        }
    }

    std::println("OK");
    return std::move(*sock_result);
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

/**
 * @brief Runs all test phases and returns EXIT_SUCCESS / EXIT_FAILURE.
 */
int main(int argc, char* argv[])
{
    // Parse log options (--logstream / --loglevel).
    auto [logstream, loglevel, remaining] = logger::parse_args(argc, argv);
    logger::init(logstream, loglevel);

    const std::string sock_path =
        (remaining.size() > 1) ? remaining[1] : "/tmp/ud_server.sock";

    // ── Print test fixture ──────────────────────────────────────────────────
    std::println("=== route_add_single_iface test ===");
    std::println("socket    : {}", sock_path);
    std::println("iface     : {}", kIfaceName);
    std::println("nexthop   : {}", kNexthopIp);
    std::println("nexthop_id: {}", kNexthopId);
    std::println("prefixes  :");
    for (std::size_t i = 0; i < kPrefixes.size(); ++i)
        std::println("  [{}]  {}", i, kPrefixes[i].cidr);
    std::println("");

    // ── Phase 1: encode ─────────────────────────────────────────────────────
    auto frame = phase_encode();
    if (!frame)
        return EXIT_FAILURE;

    // ── Phase 2: connect ────────────────────────────────────────────────────
    auto sock = phase_connect(sock_path);
    if (!sock)
        return EXIT_FAILURE;

    // ── Phase 3: send ───────────────────────────────────────────────────────
    std::print("[3/6] sending frame ... ");
    logger::log_hex("route_add_single_iface", true, sock->fd(), *frame);
    if (auto r = nb_send_all(sock->fd(), *frame); !r)
    {
        std::println("FAIL ({})", r.error().message());
        return EXIT_FAILURE;
    }
    std::println("OK ({} bytes)", frame->size());

    // ── Phase 4: receive ────────────────────────────────────────────────────
    std::print("[4/6] receiving response frame ... ");
    auto recv_frame_data = nb_recv_frame(sock->fd());
    if (!recv_frame_data)
    {
        std::println("FAIL ({})", recv_frame_data.error().message());
        return EXIT_FAILURE;
    }
    logger::log_hex(
        "route_add_single_iface", false, sock->fd(), *recv_frame_data);
    std::println("OK ({} bytes)", recv_frame_data->size());

    // ── Phase 5: decode ─────────────────────────────────────────────────────
    std::print("[5/6] decoding response ... ");

    auto reply_pkt = udproto::decode_packet(*recv_frame_data);
    if (!reply_pkt)
    {
        std::println("FAIL (decode_packet: {})", reply_pkt.error().message());
        return EXIT_FAILURE;
    }

    auto reply_msg = routeproto::decode_message(reply_pkt->data);
    if (!reply_msg)
    {
        std::println("FAIL (decode_message: {})", reply_msg.error().message());
        return EXIT_FAILURE;
    }

    if (reply_msg->payload.size() < 2)
    {
        std::println("FAIL (response payload too short: {} byte(s))",
                     reply_msg->payload.size());
        return EXIT_FAILURE;
    }

    const std::uint8_t ack_status = reply_msg->payload[0];

    auto bin_resp = cmdproto::decode_route_add_binary_response(
        std::span<const std::uint8_t>{reply_msg->payload}.subspan(1));
    if (!bin_resp)
    {
        std::println("FAIL (decode_route_add_binary_response: {})",
                     bin_resp.error().message());
        return EXIT_FAILURE;
    }

    std::println("OK");

    // ── Phase 6: verify ─────────────────────────────────────────────────────
    std::println("[6/6] verifying response ...");
    std::println("      ack_status : 0x{:02x}  ({})",
                 static_cast<unsigned>(ack_status),
                 ack_status == 0x00 ? "ok" : "error");
    std::println("      overall    : 0x{:02x}  ({})",
                 static_cast<unsigned>(bin_resp->status_code),
                 bin_resp->status_code == 0x00 ? "all prefixes ok"
                                               : "one or more failures");

    bool all_ok = (ack_status == 0x00) && (bin_resp->status_code == 0x00);

    for (std::size_t i = 0; i < bin_resp->prefix_status.size(); ++i)
    {
        const bool ok = bin_resp->prefix_status[i];
        std::println("      prefix[{}]  : {}", i, ok ? "ok" : "FAIL");
        if (!ok)
            all_ok = false;
    }

    // Warn if the server returned fewer prefix results than we sent.
    if (bin_resp->prefix_status.size() < kPrefixes.size())
    {
        std::println("      WARNING: expected {} prefix result(s), got {}",
                     kPrefixes.size(),
                     bin_resp->prefix_status.size());
    }

    std::println("");
    if (all_ok)
    {
        std::println("=== TEST PASSED ===");
        return EXIT_SUCCESS;
    }

    std::println("=== TEST FAILED ===");
    return EXIT_FAILURE;
}
