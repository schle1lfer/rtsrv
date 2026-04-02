/**
 * @file client/src/grpc_proc.cpp
 * @brief Implementation of the grpc_proc asynchronous gRPC processing thread.
 *
 * See client/include/client/grpc_proc.hpp for the full design description.
 *
 * @version 1.0
 */

#include "client/grpc_proc.hpp"

#include <stdexcept>

namespace sra
{

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

GrpcProc::GrpcProc(RouteClient& client, bool autoStart)
    : client_(client)
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
        throw std::logic_error("GrpcProc::start() called while already running");
    }
    thread_ = std::thread(&GrpcProc::threadFunc, this);
}

void GrpcProc::stop()
{
    if (!running_.exchange(false))
    {
        return; // already stopped or never started
    }
    requestCv_.notify_all(); // wake the thread so it can observe running_ == false
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
    const bool arrived = responseCv_.wait_for(lock, timeout, [&]
    {
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

void GrpcProc::threadFunc()
{
    while (true)
    {
        GrpcRequest req;

        // ── Wait for a request ──────────────────────────────────────────────
        {
            std::unique_lock lock(requestMutex_);
            requestCv_.wait(lock, [this]
            {
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
        ResponsePayload result = dispatch(req);

        // ── Store the response ──────────────────────────────────────────────
        {
            std::lock_guard lock(responseMutex_);
            responses_.emplace(req.id,
                               GrpcResponse{
                                   req.id,
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
        [this](const auto& params) -> ResponsePayload
        {
            using T = std::decay_t<decltype(params)>;

            if constexpr (std::is_same_v<T, EchoParams>)
            {
                return client_.echo(params.message);
            }
            else if constexpr (std::is_same_v<T, HeartbeatParams>)
            {
                return client_.heartbeat(params.sequence);
            }
            else if constexpr (std::is_same_v<T, AddRouteParams>)
            {
                return client_.addRoute(params.destination,
                                        params.gateway,
                                        params.interfaceName,
                                        params.metric,
                                        params.family,
                                        params.protocol);
            }
            else if constexpr (std::is_same_v<T, RemoveRouteParams>)
            {
                return client_.removeRoute(params.id);
            }
            else if constexpr (std::is_same_v<T, GetRouteParams>)
            {
                return client_.getRoute(params.id);
            }
            else if constexpr (std::is_same_v<T, ListRoutesParams>)
            {
                return client_.listRoutes(params.activeOnly);
            }
            else if constexpr (std::is_same_v<T, SetLoopbackParams>)
            {
                return client_.setLoopback(params.address);
            }
            else if constexpr (std::is_same_v<T, GetLoopbackParams>)
            {
                return client_.getLoopback();
            }
        },
        req.payload);
}

} // namespace sra
