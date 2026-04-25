/**
 * @file server/include/server/service_impl.hpp
 * @brief gRPC service implementation for the Switch Route Manager daemon.
 *
 * SwitchRouteManagerImpl inherits from the protobuf-generated
 * SwitchRouteManager::Service and implements all RPCs defined in
 * proto/srmd.proto.  It delegates route state management to RouteManager
 * and accepts an injected server identity string for Echo responses.
 *
 * @version 1.0
 */

#pragma once

#include "route_manager.hpp"
#include "server/sot_config.hpp"
#include "srmd.grpc.pb.h"
#include "srmd.pb.h"

#include <grpcpp/server_context.h>
#include <grpcpp/support/status.h>

#include <mutex>
#include <string>

namespace srmd
{

/**
 * @brief Concrete implementation of the SwitchRouteManager gRPC service.
 *
 * One instance is registered with the gRPC ServerBuilder and handles all
 * inbound RPCs for the lifetime of the server process.
 */
class SwitchRouteManagerImpl final
    : public srmd::v1::SwitchRouteManager::Service
{
public:
    /**
     * @brief Constructs the service with injectable dependencies.
     *
     * @param routeManager  Reference to the shared route table.
     * @param serverId      Human-readable server identity (hostname + build).
     * @param serverVersion Full version string of the srmd binary.
     * @param sotConfig     Parsed Source-of-Truth configuration used for
     *                      client authorisation and interface lookup.
     */
    explicit SwitchRouteManagerImpl(RouteManager& routeManager,
                                    std::string serverId,
                                    std::string serverVersion,
                                    SotConfig sotConfig);

    // -----------------------------------------------------------------------
    // Test / keepalive RPCs
    // -----------------------------------------------------------------------

    /**
     * @brief Echoes the request message back with server metadata.
     *
     * Used by `sra --test` to verify end-to-end gRPC connectivity and
     * measure round-trip latency.
     */
    grpc::Status Echo(grpc::ServerContext* ctx,
                      const srmd::v1::EchoRequest* req,
                      srmd::v1::EchoResponse* resp) override;

    /**
     * @brief Returns the server timestamp, confirming session liveness.
     */
    grpc::Status Heartbeat(grpc::ServerContext* ctx,
                           const srmd::v1::HeartbeatRequest* req,
                           srmd::v1::HeartbeatResponse* resp) override;

    // -----------------------------------------------------------------------
    // Route CRUD RPCs
    // -----------------------------------------------------------------------

    /**
     * @brief Installs a new route in the srmd route table.
     */
    grpc::Status AddRoute(grpc::ServerContext* ctx,
                          const srmd::v1::AddRouteRequest* req,
                          srmd::v1::RouteResponse* resp) override;

    /**
     * @brief Removes a route by its server-assigned ID.
     */
    grpc::Status RemoveRoute(grpc::ServerContext* ctx,
                             const srmd::v1::RemoveRouteRequest* req,
                             srmd::v1::StatusResponse* resp) override;

    /**
     * @brief Retrieves a single route by ID.
     */
    grpc::Status GetRoute(grpc::ServerContext* ctx,
                          const srmd::v1::GetRouteRequest* req,
                          srmd::v1::RouteResponse* resp) override;

    /**
     * @brief Returns a filtered snapshot of the route table.
     */
    grpc::Status ListRoutes(grpc::ServerContext* ctx,
                            const srmd::v1::ListRoutesRequest* req,
                            srmd::v1::ListRoutesResponse* resp) override;

    // -----------------------------------------------------------------------
    // Streaming RPCs
    // -----------------------------------------------------------------------

    /**
     * @brief Opens a server-side stream that delivers route change events.
     *
     * Optionally prefixes the stream with the current route table snapshot
     * (when WatchRoutesRequest.send_initial_state is true).  Stays open
     * until the client cancels or the server shuts down.
     */
    grpc::Status
    WatchRoutes(grpc::ServerContext* ctx,
                const srmd::v1::WatchRoutesRequest* req,
                grpc::ServerWriter<srmd::v1::RouteEvent>* writer) override;

    // -----------------------------------------------------------------------
    // Loopback RPCs
    // -----------------------------------------------------------------------

    /**
     * @brief Stores a loopback address string on the server.
     */
    grpc::Status SetLoopback(grpc::ServerContext* ctx,
                             const srmd::v1::SetLoopbackRequest* req,
                             srmd::v1::SetLoopbackResponse* resp) override;

    /**
     * @brief Returns the loopback address currently stored on the server.
     */
    grpc::Status GetLoopback(grpc::ServerContext* ctx,
                             const srmd::v1::GetLoopbackRequest* req,
                             srmd::v1::GetLoopbackResponse* resp) override;

    /**
     * @brief Returns the interface list for a given loopback address.
     *
     * Validates the client IP against the SOT nodes_by_loopback map,
     * matches the requested loopback to the node's IPv4 or IPv6 loopback,
     * and returns the corresponding VRF interface list.
     */
    grpc::Status GetLoopbacks(grpc::ServerContext* ctx,
                              const srmd::v1::GetLoopbacksRequest* req,
                              srmd::v1::GetLoopbacksResponse* resp) override;

    /**
     * @brief Returns the loopback IPv4 address for this client from the SOT.
     *
     * Extracts the client IP from the gRPC peer string, looks it up in the
     * SOT nodes_by_loopback map, and returns the node's loopback IPv4.
     * Returns STATUS_CODE_NOT_FOUND when the client IP is not in the SOT.
     */
    grpc::Status
    RequestLoopback(grpc::ServerContext* ctx,
                    const srmd::v1::RequestLoopbackRequest* req,
                    srmd::v1::RequestLoopbackResponse* resp) override;

    /**
     * @brief Returns all VRF routes for the calling client.
     *
     * Authorises by matching the client's peer IP against loopback IPv4
     * addresses in the SOT (client IP must equal a node's loopbacks.ipv4).
     * On success, returns every VRF → interface → prefix entry for the
     * matched node as a flat list of VrfRoute messages.
     * Returns STATUS_CODE_NOT_FOUND when the client IP is not in the SOT.
     */
    grpc::Status GetAllRoutes(grpc::ServerContext* ctx,
                              const srmd::v1::GetAllRoutesRequest* req,
                              srmd::v1::GetAllRoutesResponse* resp) override;

private:
    /**
     * @brief Returns the current Unix epoch time in microseconds.
     */
    static int64_t nowUs() noexcept;

    /**
     * @brief Extracts the bare IP address from a gRPC peer string.
     *
     * gRPC peer strings are formatted as @c "ipv4:A.B.C.D:PORT" or
     * @c "ipv6:[ADDR]:PORT".  This helper strips the scheme and port,
     * returning just the IP address string.
     *
     * @param peer  Value returned by @c grpc::ServerContext::peer().
     * @return Bare IP address string, or the original @p peer on parse failure.
     */
    static std::string extractClientIp(const std::string& peer);

    RouteManager& routeManager_; ///< Shared route table (injected).
    std::string serverId_;       ///< Server identity for Echo responses.
    std::string serverVersion_;  ///< Binary version for Echo responses.
    SotConfig sotConfig_;        ///< SOT configuration for authorisation.

    mutable std::mutex loopbackMutex_; ///< Guards loopbackAddress_.
    std::string loopbackAddress_;      ///< Loopback address set by clients.
};

} // namespace srmd
