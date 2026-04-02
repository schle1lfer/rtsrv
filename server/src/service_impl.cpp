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
                                               std::string serverVersion)
    : routeManager_(routeManager), serverId_(std::move(serverId)),
      serverVersion_(std::move(serverVersion))
{}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

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

} // namespace srmd
