/**
 * @file grpc_client.hpp
 * @brief gRPC client for communication with a single remote RtService server.
 *
 * Each GrpcClient instance manages:
 *  - A gRPC channel to one remote server defined in ServerConfig.
 *  - Session establishment (Connect RPC) and teardown (Disconnect RPC).
 *  - A background heartbeat thread.
 *  - A background bidirectional stream thread for real-time event exchange.
 *  - Automatic reconnection with configurable backoff.
 *
 * Typical usage:
 * @code
 *   rtsrv::ServerConfig cfg = ...;
 *   rtsrv::GrpcClient client(cfg);
 *   client.start();
 *
 *   // from any thread
 *   client.publishEvents(events);
 *
 *   // on shutdown
 *   client.stop();
 * @endcode
 *
 * @version 1.0
 */

#pragma once

#include "config.hpp"

// Generated protobuf / gRPC headers (produced at build time from rtsrv.proto)
#include "rtsrv.grpc.pb.h"
#include "rtsrv.pb.h"

#include <grpcpp/grpcpp.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace rtsrv
{

/**
 * @brief Manages a gRPC client connection to one remote RtService endpoint.
 *
 * The client is thread-safe: publishEvents() may be called from any thread
 * while start() and stop() are running. start() and stop() themselves must
 * not be called concurrently.
 */
class GrpcClient
{
public:
    /**
     * @brief Constructs a client for the given server configuration.
     *
     * Does not open any connections; call start() to connect.
     *
     * @param config  Server connection parameters.
     * @param daemonId  UUID string identifying this daemon instance; included
     *                  in every Connect RPC.
     * @param hostname  Hostname of the local machine.
     * @param version   Daemon software version string.
     */
    explicit GrpcClient(const ServerConfig& config,
                        std::string daemonId,
                        std::string hostname,
                        std::string version);

    /**
     * @brief Destructor – calls stop() if still running.
     */
    ~GrpcClient();

    GrpcClient(const GrpcClient&) = delete;
    GrpcClient& operator=(const GrpcClient&) = delete;
    GrpcClient(GrpcClient&&) = delete;
    GrpcClient& operator=(GrpcClient&&) = delete;

    // -----------------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------------

    /**
     * @brief Starts the client: connects to the server, launches background
     *        threads for heartbeating and event streaming.
     *
     * If the initial connection fails the client enters the reconnect loop
     * and retries in the background.
     */
    void start();

    /**
     * @brief Gracefully stops the client.
     *
     * Sends a Disconnect RPC, cancels the stream, stops heartbeat and
     * reconnect threads, and waits for them to exit.
     */
    void stop();

    // -----------------------------------------------------------------------
    // Event publication
    // -----------------------------------------------------------------------

    /**
     * @brief Publishes a batch of events to the remote server.
     *
     * Thread-safe. If the session is not currently established the events are
     * silently dropped (the caller should queue-and-retry at a higher level).
     *
     * @param events  Vector of Event protobuf messages to publish.
     * @return True if the RPC succeeded; false otherwise.
     */
    bool publishEvents(const std::vector<rtsrv::v1::Event>& events);

    // -----------------------------------------------------------------------
    // Status query
    // -----------------------------------------------------------------------

    /**
     * @brief Returns true when a valid session is established with the server.
     */
    [[nodiscard]] bool isConnected() const noexcept;

    /**
     * @brief Returns the server identifier string (ServerConfig::id).
     */
    [[nodiscard]] const std::string& serverId() const noexcept;

private:
    // -----------------------------------------------------------------------
    // Connection helpers
    // -----------------------------------------------------------------------

    /**
     * @brief Creates (or re-creates) the gRPC channel according to the stored
     *        ServerConfig, honouring TLS settings.
     */
    void createChannel();

    /**
     * @brief Calls the Connect RPC and populates sessionId_.
     *
     * @return True on success.
     */
    bool connect();

    /**
     * @brief Calls the Disconnect RPC using the current sessionId_.
     */
    void disconnect() noexcept;

    // -----------------------------------------------------------------------
    // Background thread entry points
    // -----------------------------------------------------------------------

    /**
     * @brief Heartbeat thread: periodically calls Heartbeat RPC.
     *
     * Exits when stopFlag_ is set. Re-establishes the session if the server
     * reports session_valid=false.
     */
    void heartbeatLoop();

    /**
     * @brief Reconnect thread: retries connection on channel failure.
     */
    void reconnectLoop();

    /**
     * @brief Bidirectional stream thread: maintains a StreamEvents RPC.
     */
    void streamLoop();

    // -----------------------------------------------------------------------
    // Data members
    // -----------------------------------------------------------------------

    ServerConfig config_;  ///< Copy of the server configuration.
    std::string daemonId_; ///< UUID of this daemon instance.
    std::string hostname_; ///< Local machine hostname.
    std::string version_;  ///< Daemon software version.

    std::shared_ptr<grpc::Channel> channel_; ///< The underlying gRPC channel.
    std::unique_ptr<rtsrv::v1::RtService::Stub>
        stub_; ///< gRPC stub for RPC calls.

    std::atomic<bool> stopFlag_{false};  ///< Set to true by stop().
    std::atomic<bool> connected_{false}; ///< True while session is valid.

    mutable std::mutex sessionMutex_; ///< Guards sessionId_.
    std::string sessionId_;           ///< Current server-assigned session ID.

    int heartbeatIntervalS_{30}; ///< Effective heartbeat interval in seconds.
    uint64_t heartbeatSeq_{0}; ///< Monotonically increasing heartbeat counter.

    std::thread heartbeatThread_; ///< Background heartbeat thread.
    std::thread reconnectThread_; ///< Background reconnect thread.
    std::thread streamThread_;    ///< Background stream thread.
};

/**
 * @brief Manages a collection of GrpcClient instances, one per server.
 *
 * Provides a convenient single entry point for starting and stopping all
 * configured server connections, and for broadcasting events to all of them.
 */
class GrpcClientManager
{
public:
    /**
     * @brief Constructs the manager but does not start any clients.
     *
     * @param configs   List of server configurations.
     * @param daemonId  UUID string of this daemon instance.
     * @param hostname  Local machine hostname.
     * @param version   Daemon software version string.
     */
    explicit GrpcClientManager(const std::vector<ServerConfig>& configs,
                               std::string daemonId,
                               std::string hostname,
                               std::string version);

    /** @brief Stops all clients on destruction. */
    ~GrpcClientManager();

    GrpcClientManager(const GrpcClientManager&) = delete;
    GrpcClientManager& operator=(const GrpcClientManager&) = delete;
    GrpcClientManager(GrpcClientManager&&) = delete;
    GrpcClientManager& operator=(GrpcClientManager&&) = delete;

    /**
     * @brief Starts all configured gRPC clients.
     */
    void startAll();

    /**
     * @brief Stops all configured gRPC clients.
     */
    void stopAll();

    /**
     * @brief Broadcasts an event batch to all connected servers.
     *
     * @param events  Events to publish.
     */
    void broadcastEvents(const std::vector<rtsrv::v1::Event>& events);

private:
    std::vector<std::unique_ptr<GrpcClient>> clients_; ///< One per server.
};

} // namespace rtsrv
