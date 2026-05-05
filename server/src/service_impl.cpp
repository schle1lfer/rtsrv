/**
 * @file server/src/service_impl.cpp
 * @brief gRPC service RPC implementations for srmd.
 *
 * @version 1.0
 */

#include "server/service_impl.hpp"

#include <boost/log/trivial.hpp>
#include <chrono>
#include <condition_variable>
#include <format>
#include <mutex>

namespace srmd
{

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

SwitchRouteManagerImpl::SwitchRouteManagerImpl(RouteManager& routeManager,
                                               std::string serverId,
                                               std::string serverVersion,
                                               SotConfig sotConfig)
    : routeManager_(routeManager), serverId_(std::move(serverId)),
      serverVersion_(std::move(serverVersion)), sotConfig_(std::move(sotConfig))
{}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

std::string SwitchRouteManagerImpl::extractClientIp(const std::string& peer)
{
    // "ipv4:A.B.C.D:PORT"  →  "A.B.C.D"
    if (peer.size() > 5 && peer.substr(0, 5) == "ipv4:")
    {
        const std::string rest = peer.substr(5);
        const auto colon = rest.rfind(':');
        return colon != std::string::npos ? rest.substr(0, colon) : rest;
    }
    // "ipv6:[ADDR]:PORT"  →  "ADDR"
    if (peer.size() > 5 && peer.substr(0, 5) == "ipv6:")
    {
        const std::string rest = peer.substr(5);
        if (!rest.empty() && rest[0] == '[')
        {
            const auto end = rest.find(']');
            if (end != std::string::npos)
            {
                return rest.substr(1, end - 1);
            }
        }
    }
    return peer;
}

int64_t SwitchRouteManagerImpl::nowUs() noexcept
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// ---------------------------------------------------------------------------
// Echo
// ---------------------------------------------------------------------------

grpc::Status SwitchRouteManagerImpl::Echo(grpc::ServerContext* /*ctx*/,
                                          const srmd::v1::EchoRequest* req,
                                          srmd::v1::EchoResponse* resp)
{
    BOOST_LOG_TRIVIAL(debug) << std::format(
        "[Echo] msg='{}' client_ts={}", req->message(), req->client_ts_us());

    resp->set_message(req->message());
    resp->set_client_ts_us(req->client_ts_us());
    resp->set_server_ts_us(nowUs());
    resp->set_server_id(serverId_);
    resp->set_server_version(serverVersion_);

    return grpc::Status::OK;
}

// ---------------------------------------------------------------------------
// Heartbeat
// ---------------------------------------------------------------------------

grpc::Status
SwitchRouteManagerImpl::Heartbeat(grpc::ServerContext* /*ctx*/,
                                  const srmd::v1::HeartbeatRequest* req,
                                  srmd::v1::HeartbeatResponse* resp)
{
    resp->set_sequence(req->sequence());
    resp->set_server_ts_us(nowUs());
    return grpc::Status::OK;
}

// ---------------------------------------------------------------------------
// AddRoute
// ---------------------------------------------------------------------------

grpc::Status
SwitchRouteManagerImpl::AddRoute(grpc::ServerContext* /*ctx*/,
                                 const srmd::v1::AddRouteRequest* req,
                                 srmd::v1::RouteResponse* resp)
{
    auto result = routeManager_.addRoute(*req);
    if (!result)
    {
        resp->set_code(srmd::v1::STATUS_CODE_INVALID_ARGUMENT);
        resp->set_message(result.error());
        BOOST_LOG_TRIVIAL(warning)
            << std::format("[AddRoute] rejected: {}", result.error());
        return grpc::Status::OK;
    }

    resp->set_code(srmd::v1::STATUS_CODE_OK);
    resp->set_message("Route added successfully");
    *resp->mutable_route() = std::move(*result);
    return grpc::Status::OK;
}

// ---------------------------------------------------------------------------
// RemoveRoute
// ---------------------------------------------------------------------------

grpc::Status
SwitchRouteManagerImpl::RemoveRoute(grpc::ServerContext* /*ctx*/,
                                    const srmd::v1::RemoveRouteRequest* req,
                                    srmd::v1::StatusResponse* resp)
{
    if (req->id().empty())
    {
        resp->set_code(srmd::v1::STATUS_CODE_INVALID_ARGUMENT);
        resp->set_message("id must not be empty");
        return grpc::Status::OK;
    }

    auto result = routeManager_.removeRoute(req->id());
    if (!result)
    {
        resp->set_code(srmd::v1::STATUS_CODE_NOT_FOUND);
        resp->set_message(result.error());
        return grpc::Status::OK;
    }

    resp->set_code(srmd::v1::STATUS_CODE_OK);
    resp->set_message("Route removed");
    return grpc::Status::OK;
}

// ---------------------------------------------------------------------------
// GetRoute
// ---------------------------------------------------------------------------

grpc::Status
SwitchRouteManagerImpl::GetRoute(grpc::ServerContext* /*ctx*/,
                                 const srmd::v1::GetRouteRequest* req,
                                 srmd::v1::RouteResponse* resp)
{
    if (req->id().empty())
    {
        resp->set_code(srmd::v1::STATUS_CODE_INVALID_ARGUMENT);
        resp->set_message("id must not be empty");
        return grpc::Status::OK;
    }

    auto result = routeManager_.getRoute(req->id());
    if (!result)
    {
        resp->set_code(srmd::v1::STATUS_CODE_NOT_FOUND);
        resp->set_message(result.error());
        return grpc::Status::OK;
    }

    resp->set_code(srmd::v1::STATUS_CODE_OK);
    resp->set_message("OK");
    *resp->mutable_route() = std::move(*result);
    return grpc::Status::OK;
}

// ---------------------------------------------------------------------------
// ListRoutes
// ---------------------------------------------------------------------------

grpc::Status
SwitchRouteManagerImpl::ListRoutes(grpc::ServerContext* /*ctx*/,
                                   const srmd::v1::ListRoutesRequest* req,
                                   srmd::v1::ListRoutesResponse* resp)
{
    const auto routes = routeManager_.listRoutes(*req);

    resp->set_code(srmd::v1::STATUS_CODE_OK);
    resp->set_snapshot_ts_us(nowUs());
    for (const auto& r : routes)
    {
        *resp->add_routes() = r;
    }

    BOOST_LOG_TRIVIAL(debug)
        << std::format("[ListRoutes] returning {} route(s)", routes.size());
    return grpc::Status::OK;
}

// ---------------------------------------------------------------------------
// WatchRoutes (server-side streaming)
// ---------------------------------------------------------------------------

grpc::Status SwitchRouteManagerImpl::WatchRoutes(
    grpc::ServerContext* ctx,
    const srmd::v1::WatchRoutesRequest* req,
    grpc::ServerWriter<srmd::v1::RouteEvent>* writer)
{
    BOOST_LOG_TRIVIAL(info) << "[WatchRoutes] stream opened";

    // Optionally send the current route snapshot first
    if (req->send_initial_state())
    {
        srmd::v1::ListRoutesRequest listReq;
        listReq.set_address_family(req->address_family());
        const auto snapshot = routeManager_.listRoutes(listReq);
        const int64_t ts = nowUs();
        for (const auto& route : snapshot)
        {
            srmd::v1::RouteEvent ev;
            ev.set_type(srmd::v1::ROUTE_EVENT_ADDED);
            *ev.mutable_route() = route;
            ev.set_event_ts_us(ts);
            if (!writer->Write(ev))
            {
                BOOST_LOG_TRIVIAL(info)
                    << "[WatchRoutes] client disconnected during snapshot";
                return grpc::Status::OK;
            }
        }
    }

    // Register an observer that forwards events to the stream
    std::mutex eventMutex;
    std::condition_variable eventCv;
    std::vector<srmd::v1::RouteEvent> pending;
    bool streamDone = false;

    const int handle =
        routeManager_.registerObserver([&](const srmd::v1::RouteEvent& ev) {
            std::lock_guard lock(eventMutex);
            pending.push_back(ev);
            eventCv.notify_one();
        });

    // Stream events until the client cancels or the server shuts down
    while (!ctx->IsCancelled())
    {
        std::vector<srmd::v1::RouteEvent> batch;
        {
            std::unique_lock lock(eventMutex);
            eventCv.wait_for(lock, std::chrono::milliseconds(200), [&] {
                return !pending.empty() || streamDone;
            });
            std::swap(batch, pending);
        }

        for (const auto& ev : batch)
        {
            // Apply address-family filter if set
            if (req->address_family() != srmd::v1::ADDRESS_FAMILY_UNSPECIFIED &&
                ev.route().address_family() != req->address_family())
            {
                continue;
            }
            if (!writer->Write(ev))
            {
                BOOST_LOG_TRIVIAL(info)
                    << "[WatchRoutes] client disconnected during stream";
                routeManager_.unregisterObserver(handle);
                return grpc::Status::OK;
            }
        }
    }

    routeManager_.unregisterObserver(handle);
    BOOST_LOG_TRIVIAL(info) << "[WatchRoutes] stream closed";
    return grpc::Status::OK;
}

// ---------------------------------------------------------------------------
// SetLoopback
// ---------------------------------------------------------------------------

grpc::Status
SwitchRouteManagerImpl::SetLoopback(grpc::ServerContext* /*ctx*/,
                                    const srmd::v1::SetLoopbackRequest* req,
                                    srmd::v1::SetLoopbackResponse* resp)
{
    if (req->address().empty())
    {
        resp->set_code(srmd::v1::STATUS_CODE_INVALID_ARGUMENT);
        resp->set_message("address must not be empty");
        return grpc::Status::OK;
    }

    {
        std::lock_guard lock(loopbackMutex_);
        loopbackAddress_ = req->address();
    }

    BOOST_LOG_TRIVIAL(info)
        << std::format("[SetLoopback] address='{}'", req->address());

    resp->set_code(srmd::v1::STATUS_CODE_OK);
    resp->set_message("Loopback address set");
    resp->set_address(req->address());
    return grpc::Status::OK;
}

// ---------------------------------------------------------------------------
// GetLoopback
// ---------------------------------------------------------------------------

grpc::Status
SwitchRouteManagerImpl::GetLoopback(grpc::ServerContext* /*ctx*/,
                                    const srmd::v1::GetLoopbackRequest* /*req*/,
                                    srmd::v1::GetLoopbackResponse* resp)
{
    std::string addr;
    {
        std::lock_guard lock(loopbackMutex_);
        addr = loopbackAddress_;
    }

    BOOST_LOG_TRIVIAL(debug)
        << std::format("[GetLoopback] returning address='{}'", addr);

    resp->set_code(srmd::v1::STATUS_CODE_OK);
    resp->set_message("OK");
    resp->set_address(addr);
    return grpc::Status::OK;
}

// ---------------------------------------------------------------------------
// GetLoopbacks
// ---------------------------------------------------------------------------

grpc::Status
SwitchRouteManagerImpl::GetLoopbacks(grpc::ServerContext* ctx,
                                     const srmd::v1::GetLoopbacksRequest* req,
                                     srmd::v1::GetLoopbacksResponse* resp)
{
    // 1. Extract client IP from peer string ("ipv4:A.B.C.D:PORT" etc.)
    const std::string clientIp = extractClientIp(ctx->peer());

    BOOST_LOG_TRIVIAL(debug)
        << std::format("[GetLoopbacks] peer='{}' clientIp='{}' loopback='{}'",
                       ctx->peer(),
                       clientIp,
                       req->loopback());

    // 2. Authorise: client IP must be present in nodes_by_loopback
    const SotNode* node = sotConfig_.findByManagementIp(clientIp);
    if (!node)
    {
        BOOST_LOG_TRIVIAL(warning) << std::format(
            "[GetLoopbacks] access denied for clientIp='{}'", clientIp);
        resp->set_code(srmd::v1::STATUS_CODE_PERMISSION_DENIED);
        resp->set_message(std::format(
            "Access denied: IP '{}' is not registered in the SOT", clientIp));
        return grpc::Status::OK;
    }

    // 3. Match requested loopback to this node's loopbacks
    const std::string& requestedLb = req->loopback();
    bool isIpv4Match = (node->loopbacks.ipv4 == requestedLb);
    bool isIpv6Match = (node->loopbacks.ipv6 == requestedLb);

    if (!isIpv4Match && !isIpv6Match)
    {
        BOOST_LOG_TRIVIAL(warning) << std::format(
            "[GetLoopbacks] loopback '{}' not found for node '{}' ({})",
            requestedLb,
            node->hostname,
            clientIp);
        resp->set_code(srmd::v1::STATUS_CODE_NOT_FOUND);
        resp->set_message(std::format(
            "Loopback '{}' does not match node '{}' (ipv4='{}' ipv6='{}')",
            requestedLb,
            node->hostname,
            node->loopbacks.ipv4,
            node->loopbacks.ipv6));
        return grpc::Status::OK;
    }

    // 4. Retrieve the interface list from vrfs["default"]
    const SotVrf* defaultVrf = node->findVrf("default");
    if (!defaultVrf)
    {
        resp->set_code(srmd::v1::STATUS_CODE_NOT_FOUND);
        resp->set_message(std::format("VRF 'default' not found for node '{}'",
                                      node->hostname));
        return grpc::Status::OK;
    }

    const std::vector<SotInterface>* interfaces = nullptr;
    if (isIpv4Match)
    {
        interfaces = &defaultVrf->ipv4.interfaces;
    }
    else
    {
        interfaces = &defaultVrf->ipv6.interfaces;
    }

    // 5. Build response
    resp->set_code(srmd::v1::STATUS_CODE_OK);
    resp->set_message(
        std::format("OK: node '{}' loopback '{}' ({}) – {} interface(s)",
                    node->hostname,
                    requestedLb,
                    isIpv4Match ? "ipv4" : "ipv6",
                    interfaces->size()));

    for (const auto& iface : *interfaces)
    {
        auto* pbIface = resp->add_interfaces();
        pbIface->set_name(iface.name);
        pbIface->set_type(iface.type);
        pbIface->set_local_address(iface.local_address);
        pbIface->set_nexthop(iface.nexthop);
        pbIface->set_weight(iface.weight);
        pbIface->set_description(iface.description);

        for (const auto& pfx : iface.prefixes)
        {
            auto* pbPfx = pbIface->add_prefixes();
            pbPfx->set_prefix(pfx.prefix);
            pbPfx->set_weight(pfx.weight);
            pbPfx->set_role(pfx.role);
            pbPfx->set_description(pfx.description);
        }
    }

    BOOST_LOG_TRIVIAL(info) << std::format(
        "[GetLoopbacks] node='{}' loopback='{}' ({}) → {} interface(s)",
        node->hostname,
        requestedLb,
        isIpv4Match ? "ipv4" : "ipv6",
        interfaces->size());

    return grpc::Status::OK;
}

// ---------------------------------------------------------------------------
// RequestLoopback
// ---------------------------------------------------------------------------

grpc::Status SwitchRouteManagerImpl::RequestLoopback(
    grpc::ServerContext* ctx,
    const srmd::v1::RequestLoopbackRequest* /*req*/,
    srmd::v1::RequestLoopbackResponse* resp)
{
    const std::string clientIp = extractClientIp(ctx->peer());

    BOOST_LOG_TRIVIAL(info) << std::format(
        "[RequestLoopback] peer='{}' clientIp='{}'", ctx->peer(), clientIp);

    const SotNode* node = sotConfig_.findByManagementIp(clientIp);
    if (!node)
    {
        BOOST_LOG_TRIVIAL(warning)
            << std::format("[RequestLoopback] clientIp='{}' not found in SOT — "
                           "closing connection",
                           clientIp);
        resp->set_code(srmd::v1::STATUS_CODE_PERMISSION_DENIED);
        resp->set_message(std::format(
            "Client IP '{}' is not registered in the SOT — connection rejected",
            clientIp));
        return grpc::Status(
            grpc::StatusCode::PERMISSION_DENIED,
            std::format("SRA IP '{}' not in SOT nodes_by_loopback", clientIp));
    }

    BOOST_LOG_TRIVIAL(info) << std::format(
        "[RequestLoopback] node='{}' clientIp='{}' loopback='{}'",
        node->hostname,
        clientIp,
        node->loopbacks.ipv4);

    resp->set_code(srmd::v1::STATUS_CODE_OK);
    resp->set_message(
        std::format("OK: node '{}' ({})", node->hostname, clientIp));
    resp->set_loopback(node->loopbacks.ipv4);
    return grpc::Status::OK;
}

// ---------------------------------------------------------------------------
// GetAllRoutes
// ---------------------------------------------------------------------------

grpc::Status SwitchRouteManagerImpl::GetAllRoutes(
    grpc::ServerContext* ctx,
    const srmd::v1::GetAllRoutesRequest* /*req*/,
    srmd::v1::GetAllRoutesResponse* resp)
{
    const std::string clientIp = extractClientIp(ctx->peer());

    BOOST_LOG_TRIVIAL(info) << std::format(
        "[GetAllRoutes] peer='{}' clientIp='{}'", ctx->peer(), clientIp);

    // Authorise: client IP must be registered as a management IP in the SOT.
    // Using the same lookup as RequestLoopback so that the SRA can call both
    // RPCs on the same gRPC channel (same source IP).
    const SotNode* node = sotConfig_.findByManagementIp(clientIp);
    if (!node)
    {
        BOOST_LOG_TRIVIAL(warning) << std::format(
            "[GetAllRoutes] clientIp='{}' not found in SOT", clientIp);
        resp->set_code(srmd::v1::STATUS_CODE_NOT_FOUND);
        resp->set_message(std::format(
            "Client IP '{}' is not registered in the SOT", clientIp));
        return grpc::Status::OK;
    }

    resp->set_hostname(node->hostname);
    resp->set_loopback_ipv4(node->loopbacks.ipv4);

    // Walk every VRF → every IPv4 interface → every prefix.
    std::size_t routeCount = 0;
    for (const auto& vrf : node->vrfs)
    {
        for (const auto& iface : vrf.ipv4.interfaces)
        {
            auto* pbRoute = resp->add_routes();
            pbRoute->set_vrf_name(vrf.name);
            pbRoute->set_interface_name(iface.name);
            pbRoute->set_interface_type(iface.type);
            pbRoute->set_local_address(iface.local_address);
            pbRoute->set_nexthop(iface.nexthop);
            pbRoute->set_weight(iface.weight);
            pbRoute->set_description(iface.description);

            for (const auto& pfx : iface.prefixes)
            {
                auto* pbPfx = pbRoute->add_prefixes();
                pbPfx->set_prefix(pfx.prefix);
                pbPfx->set_weight(pfx.weight);
                pbPfx->set_role(pfx.role);
                pbPfx->set_description(pfx.description);
            }
            ++routeCount;
        }
    }

    resp->set_code(srmd::v1::STATUS_CODE_OK);
    resp->set_message(
        std::format("OK: node '{}' loopback '{}' — {} VRF interface(s)",
                    node->hostname,
                    node->loopbacks.ipv4,
                    routeCount));

    BOOST_LOG_TRIVIAL(info) << std::format(
        "[GetAllRoutes] node='{}' loopback='{}' → {} VRF interface(s) across"
        " {} VRF(s)",
        node->hostname,
        node->loopbacks.ipv4,
        routeCount,
        node->vrfs.size());

    return grpc::Status::OK;
}

// ---------------------------------------------------------------------------
// GetRemainingNodes
// ---------------------------------------------------------------------------

grpc::Status SwitchRouteManagerImpl::GetRemainingNodes(
    grpc::ServerContext* ctx,
    const srmd::v1::GetRemainingNodesRequest* /*req*/,
    srmd::v1::GetRemainingNodesResponse* resp)
{
    const std::string clientIp = extractClientIp(ctx->peer());

    BOOST_LOG_TRIVIAL(info) << std::format(
        "[GetRemainingNodes] peer='{}' clientIp='{}'", ctx->peer(), clientIp);

    const SotNode* clientNode = sotConfig_.findByManagementIp(clientIp);
    if (!clientNode)
    {
        BOOST_LOG_TRIVIAL(warning) << std::format(
            "[GetRemainingNodes] clientIp='{}' not found in SOT", clientIp);
        resp->set_code(srmd::v1::STATUS_CODE_PERMISSION_DENIED);
        resp->set_message(std::format(
            "Client IP '{}' is not registered in the SOT", clientIp));
        return grpc::Status::OK;
    }

    std::size_t count = 0;
    for (const auto& node : sotConfig_.nodes)
    {
        if (node.management_ip == clientNode->management_ip)
            continue;

        auto* pbNode = resp->add_nodes();
        pbNode->set_management_ip(node.management_ip);
        pbNode->set_hostname(node.hostname);
        pbNode->set_loopback_ipv4(node.loopbacks.ipv4);
        pbNode->set_loopback_ipv6(node.loopbacks.ipv6);
        ++count;
    }

    resp->set_code(srmd::v1::STATUS_CODE_OK);
    resp->set_message(std::format(
        "OK: {} remaining node(s) (excluding '{}')",
        count,
        clientNode->hostname));

    BOOST_LOG_TRIVIAL(info) << std::format(
        "[GetRemainingNodes] node='{}' → {} remaining node(s)",
        clientNode->hostname,
        count);

    return grpc::Status::OK;
}

// ---------------------------------------------------------------------------
// GetLoopbacksByNodeIp
// ---------------------------------------------------------------------------

grpc::Status SwitchRouteManagerImpl::GetLoopbacksByNodeIp(
    grpc::ServerContext* ctx,
    const srmd::v1::GetLoopbacksByNodeIpRequest* req,
    srmd::v1::GetNodePrefixesResponse* resp)
{
    const std::string clientIp = extractClientIp(ctx->peer());

    BOOST_LOG_TRIVIAL(debug) << std::format(
        "[GetLoopbacksByNodeIp] peer='{}' clientIp='{}' node_ip='{}'",
        ctx->peer(), clientIp, req->node_ip());

    // Authorise: caller must be registered in the SOT.
    const SotNode* clientNode = sotConfig_.findByManagementIp(clientIp);
    if (!clientNode)
    {
        BOOST_LOG_TRIVIAL(warning) << std::format(
            "[GetLoopbacksByNodeIp] access denied for clientIp='{}'", clientIp);
        resp->set_code(srmd::v1::STATUS_CODE_PERMISSION_DENIED);
        resp->set_message(std::format(
            "Access denied: IP '{}' is not registered in the SOT", clientIp));
        return grpc::Status::OK;
    }

    // Look up the target node by its management IP.
    const SotNode* targetNode = sotConfig_.findByManagementIp(req->node_ip());
    if (!targetNode)
    {
        BOOST_LOG_TRIVIAL(warning) << std::format(
            "[GetLoopbacksByNodeIp] node_ip='{}' not found in SOT",
            req->node_ip());
        resp->set_code(srmd::v1::STATUS_CODE_NOT_FOUND);
        resp->set_message(std::format(
            "Node IP '{}' is not registered in the SOT", req->node_ip()));
        return grpc::Status::OK;
    }

    // Collect all prefixes from every interface (nni and uni) across all VRFs.
    std::size_t pfxCount = 0;
    for (const auto& vrf : targetNode->vrfs)
    {
        for (const auto& iface : vrf.ipv4.interfaces)
        {
            for (const auto& pfx : iface.prefixes)
            {
                auto* pbPfx = resp->add_prefixes();
                pbPfx->set_prefix(pfx.prefix);
                pbPfx->set_weight(pfx.weight);
                pbPfx->set_role(pfx.role);
                pbPfx->set_description(pfx.description);
                ++pfxCount;
            }
        }
        for (const auto& iface : vrf.ipv6.interfaces)
        {
            for (const auto& pfx : iface.prefixes)
            {
                auto* pbPfx = resp->add_prefixes();
                pbPfx->set_prefix(pfx.prefix);
                pbPfx->set_weight(pfx.weight);
                pbPfx->set_role(pfx.role);
                pbPfx->set_description(pfx.description);
                ++pfxCount;
            }
        }
    }

    resp->set_code(srmd::v1::STATUS_CODE_OK);
    resp->set_message(std::format("OK: node '{}' – {} prefix(es)",
                                  targetNode->hostname, pfxCount));

    BOOST_LOG_TRIVIAL(info) << std::format(
        "[GetLoopbacksByNodeIp] node='{}' ({}) → {} prefix(es)",
        targetNode->hostname, req->node_ip(), pfxCount);

    return grpc::Status::OK;
}

} // namespace srmd
