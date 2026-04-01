/**
 * @file client/include/client/route_client.hpp
 * @brief gRPC client for the SwitchRouteManager service (sra).
 *
 * RouteClient wraps a single gRPC channel and exposes typed C++ methods for
 * each RPC defined in proto/srmd.proto.  It is used by the sra CLI to send
 * requests to a running srmd instance and display results.
 *
 * All public methods are synchronous and return std::expected<T, std::string>
 * so the caller can handle errors without exceptions.
 *
 * @version 1.0
 */

#pragma once

#include "srmd.grpc.pb.h"
#include "srmd.pb.h"

#include <grpcpp/channel.h>

#include <expected>
#include <memory>
#include <string>
#include <vector>

namespace sra
{

/**
 * @brief Synchronous gRPC client for the Switch Route Manager.
 *
 * Constructed with a server address; call connect() before issuing RPCs.
 */
class RouteClient
{
public:
    /**
     * @brief Constructs the client.
     *
     * @param serverAddress  gRPC target string, e.g. "192.168.1.10:50051".
     * @param useTls         When true, attempts a TLS channel.
     * @param caCert         Path to PEM CA certificate (TLS only; empty =
     *                       system roots).
     * @param timeoutSeconds Default per-RPC deadline in seconds.
     * @param login          Optional login credential; sent as the
     *                       @c x-login gRPC call-metadata header on every RPC.
     * @param password       Optional password credential; sent as the
     *                       @c x-password gRPC call-metadata header on every
     *                       RPC.
     */
    explicit RouteClient(std::string serverAddress,
                         bool useTls = false,
                         std::string caCert = {},
                         int timeoutSeconds = 10,
                         std::string login = {},
                         std::string password = {});

    ~RouteClient() = default;

    RouteClient(const RouteClient&) = delete;
    RouteClient& operator=(const RouteClient&) = delete;
    RouteClient(RouteClient&&) = default;
    RouteClient& operator=(RouteClient&&) = default;

    // -----------------------------------------------------------------------
    // Test / keepalive
    // -----------------------------------------------------------------------

    /**
     * @brief Sends an Echo RPC and measures the round-trip.
     *
     * @param message  Arbitrary text payload to send.
     * @return Populated EchoResponse on success, or an error string.
     */
    [[nodiscard]] std::expected<srmd::v1::EchoResponse, std::string>
    echo(const std::string& message);

    /**
     * @brief Sends a Heartbeat RPC.
     *
     * @param sequence  Monotonically increasing sequence number.
     * @return Populated HeartbeatResponse, or an error string.
     */
    [[nodiscard]] std::expected<srmd::v1::HeartbeatResponse, std::string>
    heartbeat(uint64_t sequence);

    // -----------------------------------------------------------------------
    // Route CRUD
    // -----------------------------------------------------------------------

    /**
     * @brief Calls AddRoute and returns the created Route.
     *
     * @param destination   Destination prefix (CIDR or "default").
     * @param gateway       Next-hop gateway (may be empty).
     * @param interfaceName Outgoing interface (may be empty).
     * @param metric        Route metric (lower = preferred).
     * @param family        Address family (IPv4 / IPv6).
     * @param protocol      Origin protocol.
     * @return The server-created Route, or an error string.
     */
    [[nodiscard]] std::expected<srmd::v1::Route, std::string> addRoute(
        const std::string& destination,
        const std::string& gateway = {},
        const std::string& interfaceName = {},
        uint32_t metric = 0,
        srmd::v1::AddressFamily family = srmd::v1::ADDRESS_FAMILY_IPV4,
        srmd::v1::RouteProtocol protocol = srmd::v1::ROUTE_PROTOCOL_STATIC);

    /**
     * @brief Calls RemoveRoute for the given server-assigned ID.
     *
     * @param id  Route ID returned by addRoute().
     * @return void on success, or an error string.
     */
    [[nodiscard]] std::expected<void, std::string>
    removeRoute(const std::string& id);

    /**
     * @brief Calls GetRoute for the given ID.
     *
     * @param id  Route ID to retrieve.
     * @return The Route on success, or an error string.
     */
    [[nodiscard]] std::expected<srmd::v1::Route, std::string>
    getRoute(const std::string& id);

    /**
     * @brief Calls ListRoutes and returns all matching routes.
     *
     * @param activeOnly  When true, only active routes are returned.
     * @return Vector of routes, or an error string.
     */
    [[nodiscard]] std::expected<std::vector<srmd::v1::Route>, std::string>
    listRoutes(bool activeOnly = false);

    // -----------------------------------------------------------------------
    // Loopback
    // -----------------------------------------------------------------------

    /**
     * @brief Calls SetLoopback to store an address on the server.
     *
     * @param address  The loopback address string to store.
     * @return The stored address on success, or an error string.
     */
    [[nodiscard]] std::expected<std::string, std::string>
    setLoopback(const std::string& address);

    /**
     * @brief Calls GetLoopback to retrieve the address stored on the server.
     *
     * @return The stored loopback address, or an error string.
     */
    [[nodiscard]] std::expected<std::string, std::string> getLoopback();

private:
    /**
     * @brief Builds and returns a ClientContext with the configured deadline.
     */
    [[nodiscard]] std::unique_ptr<grpc::ClientContext> makeContext() const;

    /**
     * @brief Maps a gRPC Status to an error string for std::unexpected.
     */
    static std::string statusToError(const grpc::Status& status);

    std::string serverAddress_;              ///< gRPC target address.
    int timeoutSeconds_;                     ///< Per-RPC deadline.
    std::string login_;                      ///< Optional login credential.
    std::string password_;                   ///< Optional password credential.
    std::shared_ptr<grpc::Channel> channel_; ///< Underlying channel.
    std::unique_ptr<srmd::v1::SwitchRouteManager::Stub> stub_; ///< RPC stub.
};

} // namespace sra
