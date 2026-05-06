/**
 * @file client/src/grpc_proc.cpp
 * @brief Implementation of the grpc_proc asynchronous gRPC processing thread.
 *
 * See client/include/client/grpc_proc.hpp for the full design description.
 *
 * @version 1.0
 */

#include "client/grpc_proc.hpp"

#include <print>
#include <stdexcept>

namespace sra
{

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

GrpcProc::GrpcProc(RouteClient& client, bool autoStart) : client_(client)
{
    if (autoStart)
    {
        start();
    }
}

GrpcProc::~GrpcProc()
{
    stop();
}

// ---------------------------------------------------------------------------
// Thread lifecycle
// ---------------------------------------------------------------------------

void GrpcProc::start()
{
    if (running_.exchange(true))
    {
        throw std::logic_error(
            "GrpcProc::start() called while already running");
    }
    thread_ = std::thread(&GrpcProc::threadFunc, this);
}

void GrpcProc::stop()
{
    if (!running_.exchange(false))
    {
        return; // already stopped or never started
    }
    requestCv_
        .notify_all(); // wake the thread so it can observe running_ == false
    if (thread_.joinable())
    {
        thread_.join();
    }
}

bool GrpcProc::isRunning() const noexcept
{
    return running_.load();
}

// ---------------------------------------------------------------------------
// Request submission
// ---------------------------------------------------------------------------

uint64_t GrpcProc::submit(RequestPayload payload)
{
    const uint64_t id = nextId_.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard lock(requestMutex_);
        requestQueue_.push(GrpcRequest{id, std::move(payload)});
    }
    requestCv_.notify_one();
    return id;
}

// ---------------------------------------------------------------------------
// Response retrieval
// ---------------------------------------------------------------------------

std::optional<GrpcResponse>
GrpcProc::waitForResponse(uint64_t id, std::chrono::milliseconds timeout)
{
    std::unique_lock lock(responseMutex_);
    const bool arrived = responseCv_.wait_for(lock, timeout, [&] {
        return responses_.contains(id);
    });

    if (!arrived)
    {
        return std::nullopt;
    }

    GrpcResponse resp = std::move(responses_.at(id));
    responses_.erase(id);
    return resp;
}

std::optional<GrpcResponse> GrpcProc::tryGetResponse(uint64_t id)
{
    std::lock_guard lock(responseMutex_);
    auto it = responses_.find(id);
    if (it == responses_.end())
    {
        return std::nullopt;
    }
    GrpcResponse resp = std::move(it->second);
    responses_.erase(it);
    return resp;
}

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

std::size_t GrpcProc::pendingCount() const
{
    std::lock_guard lock(requestMutex_);
    return requestQueue_.size();
}

std::size_t GrpcProc::storedResponseCount() const
{
    std::lock_guard lock(responseMutex_);
    return responses_.size();
}

// ---------------------------------------------------------------------------
// Internal – thread entry point
// ---------------------------------------------------------------------------

// Build an error ResponsePayload that matches the variant index of the
// request so callers can std::get<> the right type and check !result.
static ResponsePayload makeErrorPayload(const RequestPayload& req,
                                        const std::string& msg)
{
    return std::visit(
        [&](const auto& params) -> ResponsePayload {
            using P = std::decay_t<decltype(params)>;
            if constexpr (std::is_same_v<P, EchoParams>)
                return EchoResult(std::unexpected(msg));
            else if constexpr (std::is_same_v<P, HeartbeatParams>)
                return HeartbeatResult(std::unexpected(msg));
            else if constexpr (std::is_same_v<P, AddRouteParams>)
                return ResponsePayload(std::in_place_index<2>,
                                       AddRouteResult(std::unexpected(msg)));
            else if constexpr (std::is_same_v<P, RemoveRouteParams>)
                return RemoveRouteResult(std::unexpected(msg));
            else if constexpr (std::is_same_v<P, GetRouteParams>)
                return ResponsePayload(std::in_place_index<4>,
                                       GetRouteResult(std::unexpected(msg)));
            else if constexpr (std::is_same_v<P, ListRoutesParams>)
                return ListRoutesResult(std::unexpected(msg));
            else if constexpr (std::is_same_v<P, SetLoopbackParams>)
                return ResponsePayload(std::in_place_index<6>,
                                       SetLoopbackResult(std::unexpected(msg)));
            else if constexpr (std::is_same_v<P, GetLoopbackParams>)
                return ResponsePayload(std::in_place_index<7>,
                                       GetLoopbackResult(std::unexpected(msg)));
            else if constexpr (std::is_same_v<P, GetLoopbacksParams>)
                return GetLoopbacksResult(std::unexpected(msg));
            else if constexpr (std::is_same_v<P, RequestLoopbackParams>)
                return ResponsePayload(
                    std::in_place_index<9>,
                    RequestLoopbackResult(std::unexpected(msg)));
            else if constexpr (std::is_same_v<P, GetAllRoutesParams>)
                return ResponsePayload(
                    std::in_place_index<10>,
                    GetAllRoutesResult(std::unexpected(msg)));
        },
        req);
}

void GrpcProc::threadFunc()
{
    while (true)
    {
        GrpcRequest req;

        // ── Wait for a request ──────────────────────────────────────────────
        {
            std::unique_lock lock(requestMutex_);
            requestCv_.wait(lock, [this] {
                return !requestQueue_.empty() || !running_.load();
            });

            if (!running_.load() && requestQueue_.empty())
            {
                break;
            }

            req = std::move(requestQueue_.front());
            requestQueue_.pop();
        }

        // ── Execute the RPC ─────────────────────────────────────────────────
        ResponsePayload result;
        try
        {
            result = dispatch(req);
        }
        catch (const std::exception& ex)
        {
            std::println(std::cerr,
                         "[GrpcProc] RPC exception (id={}): {}",
                         req.id,
                         ex.what());
            result = makeErrorPayload(req.payload, ex.what());
        }
        catch (...)
        {
            std::println(
                std::cerr, "[GrpcProc] RPC unknown exception (id={})", req.id);
            result = makeErrorPayload(req.payload, "unknown exception");
        }

        // ── Store the response ──────────────────────────────────────────────
        {
            std::lock_guard lock(responseMutex_);
            responses_.emplace(req.id,
                               GrpcResponse{req.id,
                                            std::move(result),
                                            std::chrono::system_clock::now()});
        }
        responseCv_.notify_all();
    }
}

// ---------------------------------------------------------------------------
// Internal – RPC dispatch
// ---------------------------------------------------------------------------

/**
 * @brief Visitor that executes the appropriate RouteClient RPC for each
 *        request payload alternative and returns a matching ResponsePayload.
 */
ResponsePayload GrpcProc::dispatch(const GrpcRequest& req)
{
    return std::visit(
        [this](const auto& params) -> ResponsePayload {
            using T = std::decay_t<decltype(params)>;

            // Non-ambiguous types: implicit conversion to ResponsePayload is
            // fine.
            if constexpr (std::is_same_v<T, EchoParams>)
            {
                return client_.echo(params.message);
            }
            else if constexpr (std::is_same_v<T, HeartbeatParams>)
            {
                return client_.heartbeat(params.sequence);
            }
            else if constexpr (std::is_same_v<T, RemoveRouteParams>)
            {
                return client_.removeRoute(params.id);
            }
            else if constexpr (std::is_same_v<T, ListRoutesParams>)
            {
                return client_.listRoutes(params.activeOnly);
            }
            else if constexpr (std::is_same_v<T, GetLoopbacksParams>)
            {
                return client_.getLoopbacks(params.loopback);
            }
            // Ambiguous types: use in_place_index to select the correct
            // variant alternative explicitly (see index map in grpc_proc.hpp).
            else if constexpr (std::is_same_v<T, AddRouteParams>)
            {
                return ResponsePayload(std::in_place_index<2>,
                                       client_.addRoute(params.destination,
                                                        params.gateway,
                                                        params.interfaceName,
                                                        params.metric,
                                                        params.family,
                                                        params.protocol));
            }
            else if constexpr (std::is_same_v<T, GetRouteParams>)
            {
                return ResponsePayload(std::in_place_index<4>,
                                       client_.getRoute(params.id));
            }
            else if constexpr (std::is_same_v<T, SetLoopbackParams>)
            {
                return ResponsePayload(std::in_place_index<6>,
                                       client_.setLoopback(params.address));
            }
            else if constexpr (std::is_same_v<T, GetLoopbackParams>)
            {
                return ResponsePayload(std::in_place_index<7>,
                                       client_.getLoopback());
            }
            else if constexpr (std::is_same_v<T, RequestLoopbackParams>)
            {
                return ResponsePayload(std::in_place_index<9>,
                                       client_.requestLoopback());
            }
            else if constexpr (std::is_same_v<T, GetAllRoutesParams>)
            {
                return ResponsePayload(std::in_place_index<10>,
                                       client_.getAllRoutes());
            }
        },
        req.payload);
}

} // namespace sra
