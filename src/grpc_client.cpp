/**
 * @file grpc_client.cpp
 * @brief gRPC client implementation for RtService communication.
 *
 * @version 1.0
 */

#include "grpc_client.hpp"

#include <grpcpp/channel.h>
#include <grpcpp/client_context.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>

#include <boost/log/trivial.hpp>
#include <chrono>
#include <format>
#include <stdexcept>
#include <thread>

namespace rtsrv
{

using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// GrpcClient – construction / destruction
// ---------------------------------------------------------------------------

GrpcClient::GrpcClient(const ServerConfig& config,
                       std::string daemonId,
                       std::string hostname,
                       std::string version)
    : config_(config), daemonId_(std::move(daemonId)),
      hostname_(std::move(hostname)), version_(std::move(version))
{}

GrpcClient::~GrpcClient()
{
    stop();
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void GrpcClient::start()
{
    BOOST_LOG_TRIVIAL(info) << std::format(
        "[{}] Starting gRPC client → {}", config_.id, config_.target());

    createChannel();

    // Attempt initial connection; if it fails, reconnectLoop will retry.
    if (!connect())
    {
        BOOST_LOG_TRIVIAL(warning) << std::format(
            "[{}] Initial connection failed; entering reconnect loop",
            config_.id);
    }

    heartbeatThread_ = std::thread(&GrpcClient::heartbeatLoop, this);
    reconnectThread_ = std::thread(&GrpcClient::reconnectLoop, this);
    streamThread_ = std::thread(&GrpcClient::streamLoop, this);
}

void GrpcClient::stop()
{
    stopFlag_.store(true, std::memory_order_release);

    // Send a Disconnect RPC while the channel is still open
    if (connected_.load(std::memory_order_acquire))
    {
        disconnect();
    }

    if (heartbeatThread_.joinable())
    {
        heartbeatThread_.join();
    }
    if (reconnectThread_.joinable())
    {
        reconnectThread_.join();
    }
    if (streamThread_.joinable())
    {
        streamThread_.join();
    }

    connected_.store(false, std::memory_order_release);
    BOOST_LOG_TRIVIAL(info)
        << std::format("[{}] gRPC client stopped", config_.id);
}

// ---------------------------------------------------------------------------
// Channel creation
// ---------------------------------------------------------------------------

void GrpcClient::createChannel()
{
    grpc::ChannelArguments args;
    args.SetInt(GRPC_ARG_KEEPALIVE_TIME_MS, 20'000);
    args.SetInt(GRPC_ARG_KEEPALIVE_TIMEOUT_MS, 10'000);
    args.SetInt(GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS, 1);
    args.SetInt(GRPC_ARG_HTTP2_MAX_PINGS_WITHOUT_DATA, 0);

    if (config_.tls_enabled)
    {
        grpc::SslCredentialsOptions sslOpts;

        // Load CA certificate if specified
        if (!config_.tls_ca_cert.empty())
        {
            std::ifstream caFile(config_.tls_ca_cert);
            if (!caFile.is_open())
            {
                throw std::runtime_error(
                    std::format("[{}] Cannot open CA cert: '{}'",
                                config_.id,
                                config_.tls_ca_cert));
            }
            sslOpts.pem_root_certs =
                std::string(std::istreambuf_iterator<char>(caFile), {});
        }

        // Mutual TLS – load client cert and key if specified
        if (!config_.tls_client_cert.empty() && !config_.tls_client_key.empty())
        {
            std::ifstream certFile(config_.tls_client_cert);
            std::ifstream keyFile(config_.tls_client_key);
            if (certFile.is_open() && keyFile.is_open())
            {
                sslOpts.pem_cert_chain =
                    std::string(std::istreambuf_iterator<char>(certFile), {});
                sslOpts.pem_private_key =
                    std::string(std::istreambuf_iterator<char>(keyFile), {});
            }
        }

        channel_ = grpc::CreateCustomChannel(
            config_.target(), grpc::SslCredentials(sslOpts), args);
    }
    else
    {
        channel_ = grpc::CreateCustomChannel(
            config_.target(), grpc::InsecureChannelCredentials(), args);
    }

    stub_ = rtsrv::v1::RtService::NewStub(channel_);
    BOOST_LOG_TRIVIAL(debug)
        << std::format("[{}] Channel created → {} (TLS={})",
                       config_.id,
                       config_.target(),
                       config_.tls_enabled);
}

// ---------------------------------------------------------------------------
// Connect / Disconnect RPCs
// ---------------------------------------------------------------------------

bool GrpcClient::connect()
{
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() +
                     std::chrono::seconds(config_.connect_timeout_s));

    rtsrv::v1::ConnectRequest req;
    auto* identity = req.mutable_identity();
    identity->set_daemon_id(daemonId_);
    identity->set_hostname(hostname_);
    identity->set_version(version_);
    identity->set_start_time_us(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());

    req.set_auth_token(config_.auth_token);
    req.add_capabilities("events");
    req.add_capabilities("heartbeat");
    req.add_capabilities("stream");

    rtsrv::v1::ConnectResponse resp;
    const grpc::Status status = stub_->Connect(&ctx, req, &resp);

    if (!status.ok())
    {
        BOOST_LOG_TRIVIAL(error)
            << std::format("[{}] Connect RPC failed: {} ({})",
                           config_.id,
                           status.error_message(),
                           status.error_code());
        return false;
    }

    if (!resp.accepted())
    {
        BOOST_LOG_TRIVIAL(warning) << std::format(
            "[{}] Server rejected connection: {}", config_.id, resp.message());
        return false;
    }

    {
        std::lock_guard lock(sessionMutex_);
        sessionId_ = resp.session_id();
    }

    heartbeatIntervalS_ = (resp.heartbeat_interval_s() > 0)
                              ? resp.heartbeat_interval_s()
                              : config_.heartbeat_interval_s;

    connected_.store(true, std::memory_order_release);

    BOOST_LOG_TRIVIAL(info)
        << std::format("[{}] Connected – session={} heartbeat={}s",
                       config_.id,
                       resp.session_id(),
                       heartbeatIntervalS_);
    return true;
}

void GrpcClient::disconnect() noexcept
{
    try
    {
        grpc::ClientContext ctx;
        ctx.set_deadline(std::chrono::system_clock::now() + 5s);

        rtsrv::v1::DisconnectRequest req;
        {
            std::lock_guard lock(sessionMutex_);
            req.set_session_id(sessionId_);
        }
        req.set_reason("daemon shutdown");

        rtsrv::v1::DisconnectResponse resp;
        stub_->Disconnect(&ctx, req, &resp);

        connected_.store(false, std::memory_order_release);
        BOOST_LOG_TRIVIAL(info)
            << std::format("[{}] Disconnected from server", config_.id);
    }
    catch (const std::exception& ex)
    {
        BOOST_LOG_TRIVIAL(warning) << std::format(
            "[{}] Disconnect RPC threw: {}", config_.id, ex.what());
    }
}

// ---------------------------------------------------------------------------
// Heartbeat loop
// ---------------------------------------------------------------------------

void GrpcClient::heartbeatLoop()
{
    BOOST_LOG_TRIVIAL(debug)
        << std::format("[{}] Heartbeat thread started", config_.id);

    while (!stopFlag_.load(std::memory_order_acquire))
    {
        const int interval = heartbeatIntervalS_;
        // Sleep in 1-second increments so we can react to stop quickly
        for (int i = 0; i < interval; ++i)
        {
            if (stopFlag_.load(std::memory_order_acquire))
            {
                return;
            }
            std::this_thread::sleep_for(1s);
        }

        if (!connected_.load(std::memory_order_acquire))
        {
            continue;
        }

        grpc::ClientContext ctx;
        ctx.set_deadline(std::chrono::system_clock::now() + 10s);

        rtsrv::v1::HeartbeatRequest req;
        {
            std::lock_guard lock(sessionMutex_);
            req.set_session_id(sessionId_);
        }
        req.set_timestamp_us(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count());
        req.set_sequence_number(++heartbeatSeq_);

        rtsrv::v1::HeartbeatResponse resp;
        const grpc::Status status = stub_->Heartbeat(&ctx, req, &resp);

        if (!status.ok())
        {
            BOOST_LOG_TRIVIAL(warning)
                << std::format("[{}] Heartbeat failed: {}",
                               config_.id,
                               status.error_message());
            connected_.store(false, std::memory_order_release);
        }
        else if (!resp.session_valid())
        {
            BOOST_LOG_TRIVIAL(warning) << std::format(
                "[{}] Server invalidated session; will reconnect", config_.id);
            connected_.store(false, std::memory_order_release);
        }
        else
        {
            BOOST_LOG_TRIVIAL(debug) << std::format(
                "[{}] Heartbeat OK seq={}", config_.id, resp.sequence_number());
        }
    }

    BOOST_LOG_TRIVIAL(debug)
        << std::format("[{}] Heartbeat thread exiting", config_.id);
}

// ---------------------------------------------------------------------------
// Reconnect loop
// ---------------------------------------------------------------------------

void GrpcClient::reconnectLoop()
{
    BOOST_LOG_TRIVIAL(debug)
        << std::format("[{}] Reconnect thread started", config_.id);

    int attempts = 0;

    while (!stopFlag_.load(std::memory_order_acquire))
    {
        if (connected_.load(std::memory_order_acquire))
        {
            // Already connected – poll every second
            std::this_thread::sleep_for(1s);
            continue;
        }

        const int maxAttempts = config_.max_reconnect_attempts;
        if (maxAttempts > 0 && attempts >= maxAttempts)
        {
            BOOST_LOG_TRIVIAL(error) << std::format(
                "[{}] Max reconnect attempts ({}) reached; giving up",
                config_.id,
                maxAttempts);
            break;
        }

        ++attempts;
        BOOST_LOG_TRIVIAL(info)
            << std::format("[{}] Reconnect attempt {} (delay={}s)",
                           config_.id,
                           attempts,
                           config_.reconnect_delay_s);

        // Wait before attempting
        for (int i = 0; i < config_.reconnect_delay_s; ++i)
        {
            if (stopFlag_.load(std::memory_order_acquire))
            {
                return;
            }
            std::this_thread::sleep_for(1s);
        }

        // Re-create channel and attempt connect
        try
        {
            createChannel();
        }
        catch (const std::exception& ex)
        {
            BOOST_LOG_TRIVIAL(error) << std::format(
                "[{}] createChannel() threw: {}", config_.id, ex.what());
            continue;
        }

        if (connect())
        {
            attempts = 0; // Reset counter on success
        }
    }

    BOOST_LOG_TRIVIAL(debug)
        << std::format("[{}] Reconnect thread exiting", config_.id);
}

// ---------------------------------------------------------------------------
// Bidirectional stream loop
// ---------------------------------------------------------------------------

void GrpcClient::streamLoop()
{
    BOOST_LOG_TRIVIAL(debug)
        << std::format("[{}] Stream thread started", config_.id);

    while (!stopFlag_.load(std::memory_order_acquire))
    {
        if (!connected_.load(std::memory_order_acquire))
        {
            std::this_thread::sleep_for(1s);
            continue;
        }

        grpc::ClientContext ctx;
        auto stream = stub_->StreamEvents(&ctx);

        // Send initial StreamEventsRequest in-band via the first message
        rtsrv::v1::StreamEventsMessage initMsg;
        {
            auto* ping = initMsg.mutable_ping();
            std::lock_guard lock(sessionMutex_);
            ping->set_session_id(sessionId_);
        }
        if (!stream->Write(initMsg))
        {
            BOOST_LOG_TRIVIAL(warning) << std::format(
                "[{}] StreamEvents initial Write failed", config_.id);
            std::this_thread::sleep_for(2s);
            continue;
        }

        BOOST_LOG_TRIVIAL(info)
            << std::format("[{}] StreamEvents open", config_.id);

        // Read messages from the server
        rtsrv::v1::StreamEventsMessage inMsg;
        while (!stopFlag_.load(std::memory_order_acquire) &&
               stream->Read(&inMsg))
        {
            if (inMsg.has_event())
            {
                const auto& ev = inMsg.event();
                BOOST_LOG_TRIVIAL(info)
                    << std::format("[{}] Received event id={} src={} msg={}",
                                   config_.id,
                                   ev.event_id(),
                                   ev.source(),
                                   ev.message());
            }
            else if (inMsg.has_pong())
            {
                BOOST_LOG_TRIVIAL(debug)
                    << std::format("[{}] Stream pong seq={}",
                                   config_.id,
                                   inMsg.pong().sequence_number());
            }
        }

        stream->WritesDone();
        const grpc::Status status = stream->Finish();
        if (!status.ok() && !stopFlag_.load(std::memory_order_acquire))
        {
            BOOST_LOG_TRIVIAL(warning)
                << std::format("[{}] StreamEvents ended: {}",
                               config_.id,
                               status.error_message());
        }
    }

    BOOST_LOG_TRIVIAL(debug)
        << std::format("[{}] Stream thread exiting", config_.id);
}

// ---------------------------------------------------------------------------
// Event publication
// ---------------------------------------------------------------------------

bool GrpcClient::publishEvents(const std::vector<rtsrv::v1::Event>& events)
{
    if (events.empty())
    {
        return true;
    }
    if (!connected_.load(std::memory_order_acquire))
    {
        BOOST_LOG_TRIVIAL(warning) << std::format(
            "[{}] publishEvents: not connected; dropping {} event(s)",
            config_.id,
            events.size());
        return false;
    }

    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + 15s);

    rtsrv::v1::PublishEventsRequest req;
    for (const auto& ev : events)
    {
        *req.add_events() = ev;
    }

    rtsrv::v1::PublishEventsResponse resp;
    const grpc::Status status = stub_->PublishEvents(&ctx, req, &resp);

    if (!status.ok())
    {
        BOOST_LOG_TRIVIAL(error) << std::format("[{}] PublishEvents failed: {}",
                                                config_.id,
                                                status.error_message());
        return false;
    }

    BOOST_LOG_TRIVIAL(debug)
        << std::format("[{}] PublishEvents: accepted={} rejected={}",
                       config_.id,
                       resp.accepted_count(),
                       resp.rejected_count());
    return resp.rejected_count() == 0;
}

// ---------------------------------------------------------------------------
// Status
// ---------------------------------------------------------------------------

bool GrpcClient::isConnected() const noexcept
{
    return connected_.load(std::memory_order_acquire);
}

const std::string& GrpcClient::serverId() const noexcept
{
    return config_.id;
}

// ===========================================================================
// GrpcClientManager
// ===========================================================================

GrpcClientManager::GrpcClientManager(const std::vector<ServerConfig>& configs,
                                     std::string daemonId,
                                     std::string hostname,
                                     std::string version)
{
    clients_.reserve(configs.size());
    for (const auto& cfg : configs)
    {
        clients_.emplace_back(
            std::make_unique<GrpcClient>(cfg, daemonId, hostname, version));
    }
}

GrpcClientManager::~GrpcClientManager()
{
    stopAll();
}

void GrpcClientManager::startAll()
{
    for (auto& client : clients_)
    {
        client->start();
    }
}

void GrpcClientManager::stopAll()
{
    for (auto& client : clients_)
    {
        client->stop();
    }
}

void GrpcClientManager::broadcastEvents(
    const std::vector<rtsrv::v1::Event>& events)
{
    for (auto& client : clients_)
    {
        client->publishEvents(events);
    }
}

} // namespace rtsrv
