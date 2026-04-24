/**
 * @file unix_domain/ud_server.cpp
 * @brief UNIX-domain socket server for route-add commands.
 *
 * Listens on a configurable AF_UNIX stream socket and handles incoming
 * route-add requests encoded in the udproto/routeproto/cmdproto stack.
 *
 * ## Supported ROUTE_ADD payload types
 * | Type            | Value | Format          | Handler                    |
 * |-----------------|-------|-----------------|----------------------------|
 * | SINGLE_ROUTE    |   1   | VrfsRouteRequest  | handle_vrfs_route_request  |
 * | INTERFACE_ROUTE |   2   | SingleRouteRequest| handle_route_add_payload   |
 *
 * ## Usage
 * @code
 *   ud_server [socket_path]
 *   Default socket path: /tmp/ud_server.sock
 * @endcode
 *
 * ## Protocol stack (inbound)
 *   udproto frame → routeproto DATA message → ExchangeData → cmdproto commands
 *
 * ## Protocol stack (outbound)
 *   [ack_status(1)][response_bytes] → routeproto DATA_ACK → udproto frame
 *
 * @version 1.0
 */

#include "lib/cmd_proto.hpp"
#include "lib/logger.hpp"
#include "lib/net.hpp"
#include "lib/route_proto.hpp"
#include "lib/ud_proto.hpp"

#include <array>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <print>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Signal handling
// ---------------------------------------------------------------------------

static volatile sig_atomic_t g_stop = 0;

static void sig_handler(int) noexcept
{
    g_stop = 1;
}

// ---------------------------------------------------------------------------
// I/O helpers
// ---------------------------------------------------------------------------

/**
 * @brief Receives bytes from @p fd until a complete udproto frame is found.
 *
 * Accumulates data into an internal buffer and calls udproto::find_frame()
 * after every read.  Blocking I/O; returns when a frame is complete or the
 * connection is closed.
 */
static std::expected<std::vector<std::uint8_t>, std::error_code>
recv_frame(int fd)
{
    std::vector<std::uint8_t> buf;
    buf.reserve(2048);
    std::array<std::uint8_t, 1024> tmp{};

    while (true)
    {
        auto n = net::recv_data(fd, tmp);
        if (!n)
            return std::unexpected(n.error());

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
 * @brief Wraps @p payload in a routeproto DATA_ACK + udproto frame and sends
 *        it over @p fd.
 *
 * The payload is prepended with @p ack_status before encoding so that the
 * client can read payload[0] as the outer acknowledgement byte and
 * payload.subspan(1) as the command-specific response data.
 *
 * @param fd         Connected socket descriptor.
 * @param msg_id     Matches the inbound request msg_id.
 * @param pkt_num    Matches the inbound udproto pkt_num.
 * @param ack_status 0x00 = all commands processed ok, 0x01 = one or more errors.
 * @param resp_bytes Command-specific response payload (may be empty).
 */
static std::expected<void, std::error_code>
send_response(int fd,
              std::uint16_t msg_id,
              std::uint16_t pkt_num,
              std::uint8_t  ack_status,
              const std::vector<std::uint8_t>& resp_bytes)
{
    // Build DATA_ACK payload: [ack_status][response_bytes...]
    std::vector<std::uint8_t> payload;
    payload.reserve(1 + resp_bytes.size());
    payload.push_back(ack_status);
    payload.insert(payload.end(), resp_bytes.begin(), resp_bytes.end());

    routeproto::Message ack_msg{
        .msg_type = routeproto::MsgType::DATA_ACK,
        .flags    = routeproto::FLAG_NONE,
        .msg_id   = msg_id,
        .payload  = std::move(payload),
    };

    auto ack_bytes = routeproto::encode_message(ack_msg);
    if (!ack_bytes)
        return std::unexpected(ack_bytes.error());

    udproto::Packet rsp_pkt{
        .pkt_num    = pkt_num,
        .total_pkts = 1,
        .ctrl       = 0x0000,
        .data       = std::move(*ack_bytes),
    };

    auto rsp_frame = udproto::encode_packet(rsp_pkt);
    if (!rsp_frame)
        return std::unexpected(rsp_frame.error());

    logger::log_hex("ud_server", true, fd, *rsp_frame);
    return net::send_all(fd, *rsp_frame);
}

// ---------------------------------------------------------------------------
// Command dispatch
// ---------------------------------------------------------------------------

/**
 * @brief Dispatches a single ROUTE_ADD command by payload type and returns
 *        the encoded response bytes.
 *
 * Inspects raw_payload[0] to determine whether the payload uses the
 * SINGLE_ROUTE (VRF, type=1) or INTERFACE_ROUTE (single-iface, type=2)
 * binary format.  Falls back to the legacy TLV format when raw_payload is
 * empty.
 *
 * @param cmd  Decoded ROUTE_ADD command.
 * @return Encoded response bytes, or an error code.
 */
static std::expected<std::vector<std::uint8_t>, std::error_code>
dispatch_route_add(const cmdproto::Command& cmd)
{
    if (cmd.raw_payload.empty())
    {
        // Legacy TLV format — parse fields and install directly.
        auto params = cmdproto::parse_route_add(cmd);
        if (!params)
            return std::unexpected(params.error());

        auto result = cmdproto::handle_route_add(*params);
        if (!result)
            return std::unexpected(result.error());

        // No binary response for TLV format; return an empty success byte.
        return std::vector<std::uint8_t>{0x00};
    }

    const auto type_byte = cmd.raw_payload[0];

    if (type_byte == static_cast<std::uint8_t>(cmdproto::RouteAddPayloadType::SINGLE_ROUTE))
    {
        auto req = cmdproto::decode_vrfs_route_request(cmd.raw_payload);
        if (!req)
            return std::unexpected(req.error());

        std::println("[ud_server] ROUTE_ADD SINGLE_ROUTE: {} VRF(s)",
                     req->vrfs_requests.size());

        auto resp = cmdproto::handle_vrfs_route_request(*req);
        if (!resp)
            return std::unexpected(resp.error());

        return cmdproto::encode_vrfs_route_response(*resp);
    }

    if (type_byte == static_cast<std::uint8_t>(cmdproto::RouteAddPayloadType::INTERFACE_ROUTE))
    {
        auto req = cmdproto::decode_route_add_payload(cmd.raw_payload);
        if (!req)
            return std::unexpected(req.error());

        std::println("[ud_server] ROUTE_ADD INTERFACE_ROUTE: {} interface(s)",
                     req->interfaces.size());

        auto resp = cmdproto::handle_route_add_payload(*req);
        if (!resp)
            return std::unexpected(resp.error());

        return cmdproto::encode_route_add_binary_response(*resp);
    }

    std::println(stderr, "[ud_server] ROUTE_ADD: unknown payload type=0x{:02x}",
                 static_cast<unsigned>(type_byte));
    return std::unexpected(
        cmdproto::make_error_code(cmdproto::CmdError::InvalidCommand));
}

// ---------------------------------------------------------------------------
// Connection handler
// ---------------------------------------------------------------------------

/**
 * @brief Processes one client connection until it closes or an error occurs.
 *
 * Receive loop:
 *   1. Receive bytes until a complete udproto frame is buffered.
 *   2. Decode udproto → routeproto.
 *   3. PING  → reply with PONG.
 *   4. DATA  → decode ExchangeData → dispatch each command → send DATA_ACK.
 *   5. Other → log and skip.
 *
 * @param conn  Connected client socket (takes ownership).
 */
static void handle_connection(net::Socket conn)
{
    std::println("[ud_server] client connected fd={}", conn.fd());

    while (true)
    {
        // ── [1] Receive a complete udproto frame ────────────────────────────
        auto frame_data = recv_frame(conn.fd());
        if (!frame_data)
        {
            if (frame_data.error() ==
                net::make_error_code(net::NetError::ConnectionClosed))
                std::println("[ud_server] client disconnected fd={}", conn.fd());
            else
                std::println(stderr, "[ud_server] recv error: {}",
                             frame_data.error().message());
            return;
        }
        logger::log_hex("ud_server", false, conn.fd(), *frame_data);

        // ── [2] Decode udproto packet ───────────────────────────────────────
        auto pkt = udproto::decode_packet(*frame_data);
        if (!pkt)
        {
            std::println(stderr, "[ud_server] decode_packet: {}",
                         pkt.error().message());
            return;
        }

        // ── [3] Decode routeproto message ───────────────────────────────────
        auto msg = routeproto::decode_message(pkt->data);
        if (!msg)
        {
            std::println(stderr, "[ud_server] decode_message: {}",
                         msg.error().message());
            return;
        }

        std::println("[ud_server] msg_id={} type=0x{:02x} payload={} byte(s)",
                     msg->msg_id,
                     static_cast<unsigned>(msg->msg_type),
                     msg->payload.size());

        // ── [4] Handle PING ─────────────────────────────────────────────────
        if (msg->msg_type == routeproto::MsgType::PING)
        {
            auto pong     = routeproto::make_pong(msg->msg_id);
            auto pong_enc = routeproto::encode_message(pong);
            if (!pong_enc)
                return;

            udproto::Packet rsp{
                .pkt_num    = pkt->pkt_num,
                .total_pkts = 1,
                .ctrl       = 0x0000,
                .data       = std::move(*pong_enc),
            };
            auto rsp_frame = udproto::encode_packet(rsp);
            if (!rsp_frame)
                return;
            net::send_all(conn.fd(), *rsp_frame);
            std::println("[ud_server] PING → PONG sent");
            continue;
        }

        // ── [5] Handle DATA ─────────────────────────────────────────────────
        if (msg->msg_type != routeproto::MsgType::DATA)
        {
            std::println("[ud_server] ignoring msg type=0x{:02x}",
                         static_cast<unsigned>(msg->msg_type));
            continue;
        }

        auto ed = routeproto::decode_exchange(msg->payload);
        if (!ed)
        {
            std::println(stderr, "[ud_server] decode_exchange: {}",
                         ed.error().message());
            send_response(conn.fd(), msg->msg_id, pkt->pkt_num, 0x01, {});
            continue;
        }

        auto cmds = cmdproto::decode_commands(ed->commands);
        if (!cmds)
        {
            std::println(stderr, "[ud_server] decode_commands: {}",
                         cmds.error().message());
            send_response(conn.fd(), msg->msg_id, pkt->pkt_num, 0x01, {});
            continue;
        }

        std::println("[ud_server] {} command(s) in exchange", cmds->size());

        // Process commands and build the response payload.
        std::uint8_t              ack_status = 0x00;
        std::vector<std::uint8_t> resp_bytes;

        for (const auto& cmd : *cmds)
        {
            if (cmd.cmd_id != cmdproto::CmdId::ROUTE_ADD)
            {
                std::println("[ud_server] unsupported cmd_id=0x{:02x}",
                             static_cast<unsigned>(cmd.cmd_id));
                ack_status = 0x01;
                continue;
            }

            auto result = dispatch_route_add(cmd);
            if (!result)
            {
                std::println(stderr, "[ud_server] dispatch_route_add: {}",
                             result.error().message());
                ack_status = 0x01;
                continue;
            }

            resp_bytes.insert(resp_bytes.end(),
                              result->begin(), result->end());
        }

        if (auto r = send_response(conn.fd(), msg->msg_id, pkt->pkt_num,
                                   ack_status, resp_bytes); !r)
        {
            std::println(stderr, "[ud_server] send_response: {}",
                         r.error().message());
            return;
        }

        std::println("[ud_server] response sent: ack=0x{:02x} payload={} byte(s)",
                     static_cast<unsigned>(ack_status),
                     1 + resp_bytes.size());
    }
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int main(int argc, char* argv[])
{
    // Parse log options from argv (--logstream / --loglevel).
    auto [logstream, loglevel, remaining] = logger::parse_args(argc, argv);
    if (logstream.empty())
        logstream = "stderr";
    logger::init(logstream, loglevel);

    const std::string sock_path =
        (remaining.size() > 1) ? remaining[1] : "/tmp/ud_server.sock";

    // Install signal handlers for clean shutdown.
    struct sigaction sa{};
    sa.sa_handler = sig_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    // Create and bind the listening socket.
    auto srv_result = net::create_socket();
    if (!srv_result)
    {
        std::println(stderr, "[ud_server] create_socket: {}",
                     srv_result.error().message());
        return EXIT_FAILURE;
    }
    net::Socket srv = std::move(*srv_result);

    if (auto r = net::set_reuse_addr(srv.fd()); !r)
    {
        std::println(stderr, "[ud_server] set_reuse_addr: {}",
                     r.error().message());
        return EXIT_FAILURE;
    }

    if (auto r = net::bind_socket(srv.fd(), sock_path); !r)
    {
        std::println(stderr, "[ud_server] bind('{}'): {}",
                     sock_path, r.error().message());
        return EXIT_FAILURE;
    }

    if (auto r = net::listen_socket(srv.fd()); !r)
    {
        std::println(stderr, "[ud_server] listen: {}", r.error().message());
        return EXIT_FAILURE;
    }

    std::println("[ud_server] listening on '{}'  (Ctrl-C to stop)", sock_path);

    // Main accept loop.
    while (!g_stop)
    {
        auto conn_result = net::accept_connection(srv.fd());
        if (!conn_result)
        {
            if (g_stop)
                break;
            std::println(stderr, "[ud_server] accept: {}",
                         conn_result.error().message());
            continue;
        }
        handle_connection(std::move(*conn_result));
    }

    std::println("[ud_server] shutting down");
    logger::shutdown();
    return EXIT_SUCCESS;
}
