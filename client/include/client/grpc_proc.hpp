/**
 * @file client/include/client/grpc_proc.hpp
 * @brief Asynchronous gRPC processing thread and request/response queue.
 *
 * This module provides a dedicated background thread (@c grpc_proc) that
 * serialises all outbound gRPC calls to a single srmd server.  Callers on
 * any thread submit a typed @ref sra::GrpcRequest to the request queue and
 * later retrieve the matching @ref sra::GrpcResponse by the unique request ID
 * returned at submission time.
 *
 * ### Design overview
 *
 * ```
 *  caller thread A ──┐
 *  caller thread B ──┼──► request queue ──► grpc_proc thread ──► srmd server
 *  caller thread C ──┘                                │
 *                                                     ▼
 *  any thread  ◄──────────────────────── response store (keyed by ID)
 * ```
 *
 * - **Request queue** – a FIFO `std::queue` protected by a mutex and a
 *   condition variable.  Multiple threads may call @ref sra::GrpcProc::submit
 *   concurrently.
 * - **grpc_proc thread** – dequeues requests one at a time and executes the
 *   corresponding synchronous RouteClient RPC.
 * - **Response store** – an `std::unordered_map` protected by a mutex and a
 *   condition variable.  Any thread may call
 *   @ref sra::GrpcProc::waitForResponse or @ref sra::GrpcProc::tryGetResponse
 *   to fetch a completed response by its ID.
 *
 * ### Request / response type mapping
 *
 * Each concrete request carries a payload variant member that selects the RPC:
 *
 * | Payload type            | RPC called           | Response payload type     |
 * |-------------------------|----------------------|---------------------------|
 * | @ref sra::EchoParams    | Echo                 | @ref sra::EchoResult      |
 * | @ref sra::HeartbeatParams | Heartbeat          | @ref sra::HeartbeatResult |
 * | @ref sra::AddRouteParams | AddRoute            | @ref sra::AddRouteResult  |
 * | @ref sra::RemoveRouteParams | RemoveRoute      | @ref sra::RemoveRouteResult |
 * | @ref sra::GetRouteParams | GetRoute            | @ref sra::GetRouteResult  |
 * | @ref sra::ListRoutesParams | ListRoutes        | @ref sra::ListRoutesResult |
 * | @ref sra::SetLoopbackParams | SetLoopback      | @ref sra::SetLoopbackResult |
 * | @ref sra::GetLoopbackParams | GetLoopback      | @ref sra::GetLoopbackResult |
 * | @ref sra::GetLoopbacksParams | GetLoopbacks    | @ref sra::GetLoopbacksResult |
 *
 * @version 1.0
 */

#pragma once

#include "client/route_client.hpp"
#include "srmd.pb.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <expected>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <variant>
#include <vector>

namespace sra
{

// ---------------------------------------------------------------------------
// Request payload types
// ---------------------------------------------------------------------------

/**
 * @brief Payload for an Echo RPC request.
 */
struct EchoParams
{
    std::string message; ///< Arbitrary text payload to send.
};

/**
 * @brief Payload for a Heartbeat RPC request.
 */
struct HeartbeatParams
{
    uint64_t sequence; ///< Monotonically increasing sequence counter.
};

/**
 * @brief Payload for an AddRoute RPC request.
 */
struct AddRouteParams
{
    std::string destination;  ///< Destination prefix (CIDR or "default").
    std::string gateway;      ///< Next-hop gateway (may be empty).
    std::string interfaceName; ///< Outgoing interface (may be empty).
    uint32_t metric{0};       ///< Route metric (lower = preferred).
    srmd::v1::AddressFamily family{srmd::v1::ADDRESS_FAMILY_IPV4}; ///< Address family.
    srmd::v1::RouteProtocol protocol{srmd::v1::ROUTE_PROTOCOL_STATIC}; ///< Origin protocol.
};

/**
 * @brief Payload for a RemoveRoute RPC request.
 */
struct RemoveRouteParams
{
    std::string id; ///< Route ID returned by AddRoute.
};

/**
 * @brief Payload for a GetRoute RPC request.
 */
struct GetRouteParams
{
    std::string id; ///< Route ID to look up.
};

/**
 * @brief Payload for a ListRoutes RPC request.
 */
struct ListRoutesParams
{
    bool activeOnly{false}; ///< When true, only active routes are returned.
};

/**
 * @brief Payload for a SetLoopback RPC request.
 */
struct SetLoopbackParams
{
    std::string address; ///< Loopback address to store on the server.
};

/**
 * @brief Payload for a GetLoopback RPC request (no parameters).
 */
struct GetLoopbackParams {};

/**
 * @brief Payload for a GetLoopbacks RPC request.
 */
struct GetLoopbacksParams
{
    std::string loopback; ///< Loopback address to query (IPv4 or IPv6 string).
};

/**
 * @brief Discriminated union of all possible request payloads.
 *
 * The active alternative determines which RPC the grpc_proc thread will call.
 */
using RequestPayload = std::variant<
    EchoParams,
    HeartbeatParams,
    AddRouteParams,
    RemoveRouteParams,
    GetRouteParams,
    ListRoutesParams,
    SetLoopbackParams,
    GetLoopbackParams,
    GetLoopbacksParams>;

// ---------------------------------------------------------------------------
// Response result types
// ---------------------------------------------------------------------------

/** @brief Result type for a completed Echo RPC. */
using EchoResult = std::expected<srmd::v1::EchoResponse, std::string>;

/** @brief Result type for a completed Heartbeat RPC. */
using HeartbeatResult = std::expected<srmd::v1::HeartbeatResponse, std::string>;

/** @brief Result type for a completed AddRoute RPC. */
using AddRouteResult = std::expected<srmd::v1::Route, std::string>;

/** @brief Result type for a completed RemoveRoute RPC. */
using RemoveRouteResult = std::expected<void, std::string>;

/** @brief Result type for a completed GetRoute RPC. */
using GetRouteResult = std::expected<srmd::v1::Route, std::string>;

/** @brief Result type for a completed ListRoutes RPC. */
using ListRoutesResult = std::expected<std::vector<srmd::v1::Route>, std::string>;

/** @brief Result type for a completed SetLoopback RPC. */
using SetLoopbackResult = std::expected<std::string, std::string>;

/** @brief Result type for a completed GetLoopback RPC. */
using GetLoopbackResult = std::expected<std::string, std::string>;

/** @brief Result type for a completed GetLoopbacks RPC. */
using GetLoopbacksResult =
    std::expected<srmd::v1::GetLoopbacksResponse, std::string>;

/**
 * @brief Discriminated union of all possible response payloads.
 *
 * The active alternative matches the @ref RequestPayload alternative of the
 * corresponding request.
 *
 * @note Some result types are identical (e.g. AddRouteResult and GetRouteResult
 *       are both expected<Route, string>; SetLoopbackResult and GetLoopbackResult
 *       are both expected<string, string>).  The dispatch() function therefore
 *       uses std::in_place_index to construct these alternatives unambiguously.
 *       Index mapping (0-based):
 *         0 EchoResult, 1 HeartbeatResult, 2 AddRouteResult,
 *         3 RemoveRouteResult, 4 GetRouteResult, 5 ListRoutesResult,
 *         6 SetLoopbackResult, 7 GetLoopbackResult, 8 GetLoopbacksResult
 */
using ResponsePayload = std::variant<
    EchoResult,          // 0
    HeartbeatResult,     // 1
    AddRouteResult,      // 2  (= expected<Route, string>)
    RemoveRouteResult,   // 3
    GetRouteResult,      // 4  (= expected<Route, string>)
    ListRoutesResult,    // 5
    SetLoopbackResult,   // 6  (= expected<string, string>)
    GetLoopbackResult,   // 7  (= expected<string, string>)
    GetLoopbacksResult>; // 8

// ---------------------------------------------------------------------------
// Request structure
// ---------------------------------------------------------------------------

/**
 * @brief A single pending gRPC request placed on the request queue.
 *
 * Every request receives a unique, monotonically increasing @c id assigned by
 * @ref GrpcProc::submit.  The caller stores that ID and uses it later to
 * retrieve the matching @ref GrpcResponse from the response store.
 */
struct GrpcRequest
{
    uint64_t       id;      ///< Unique request identifier assigned at submission.
    RequestPayload payload; ///< RPC-specific parameters.
};

// ---------------------------------------------------------------------------
// Response structure
// ---------------------------------------------------------------------------

/**
 * @brief A completed gRPC response stored in the response map.
 *
 * The @c id matches the @ref GrpcRequest::id of the originating request,
 * allowing any thread to correlate requests and responses.
 *
 * @c completedAt records when the grpc_proc thread received the server reply.
 */
struct GrpcResponse
{
    uint64_t         id;          ///< Matches the originating GrpcRequest::id.
    ResponsePayload  payload;     ///< RPC-specific result (success or error).
    std::chrono::system_clock::time_point completedAt; ///< Completion timestamp.
};

// ---------------------------------------------------------------------------
// GrpcProc – the processing thread and queue manager
// ---------------------------------------------------------------------------

/**
 * @brief Manages the @c grpc_proc background thread and the associated
 *        request/response queues.
 *
 * ### Thread safety
 * All public methods are thread-safe.  Multiple threads may call @c submit,
 * @c waitForResponse, and @c tryGetResponse concurrently.
 *
 * ### Lifecycle
 * @code
 *   sra::RouteClient client("127.0.0.1:50051");
 *   sra::GrpcProc proc(client);
 *   proc.start();
 *
 *   uint64_t id = proc.submit(sra::EchoParams{"hello"});
 *   auto resp   = proc.waitForResponse(id);
 *
 *   proc.stop();
 * @endcode
 *
 * Alternatively, construct with @c autoStart = true to skip the explicit
 * @c start() call.
 */
class GrpcProc
{
public:
    /**
     * @brief Constructs the processor.
     *
     * @param client     Reference to a configured RouteClient.  Must outlive
     *                   this GrpcProc instance.
     * @param autoStart  When @c true, the grpc_proc thread is started
     *                   immediately in the constructor.
     */
    explicit GrpcProc(RouteClient& client, bool autoStart = false);

    /**
     * @brief Destructor – stops the grpc_proc thread if it is still running.
     */
    ~GrpcProc();

    GrpcProc(const GrpcProc&) = delete;
    GrpcProc& operator=(const GrpcProc&) = delete;
    GrpcProc(GrpcProc&&) = delete;
    GrpcProc& operator=(GrpcProc&&) = delete;

    // -----------------------------------------------------------------------
    // Thread lifecycle
    // -----------------------------------------------------------------------

    /**
     * @brief Starts the grpc_proc background thread.
     *
     * @note Must not be called when the thread is already running.
     */
    void start();

    /**
     * @brief Signals the grpc_proc thread to stop and joins it.
     *
     * Pending requests that have not yet been dispatched are abandoned.
     * Already-stored responses remain available in the response map.
     *
     * @note Safe to call even if @c start() was never called.
     */
    void stop();

    /**
     * @brief Returns @c true if the grpc_proc thread is currently running.
     */
    [[nodiscard]] bool isRunning() const noexcept;

    // -----------------------------------------------------------------------
    // Request submission
    // -----------------------------------------------------------------------

    /**
     * @brief Enqueues a request for execution by the grpc_proc thread.
     *
     * Assigns a unique ID to the request, places it on the FIFO request
     * queue, and returns the ID.  The caller may use that ID with
     * @ref waitForResponse or @ref tryGetResponse to retrieve the result.
     *
     * @param payload  RPC-specific parameters (any alternative of
     *                 @ref RequestPayload).
     * @return The unique request ID.
     *
     * @note Thread-safe; may be called from any thread while the processor
     *       is running.
     */
    uint64_t submit(RequestPayload payload);

    // -----------------------------------------------------------------------
    // Response retrieval
    // -----------------------------------------------------------------------

    /**
     * @brief Blocks until the response for @p id is available or @p timeout
     *        elapses.
     *
     * @param id       Request ID returned by @ref submit.
     * @param timeout  Maximum time to wait.  Pass
     *                 @c std::chrono::milliseconds::max() to wait forever.
     * @return The @ref GrpcResponse if it arrived before the timeout, or
     *         @c std::nullopt on timeout.
     *
     * @note Thread-safe; multiple threads may wait on different IDs
     *       simultaneously.
     * @note Retrieving a response removes it from the internal map.
     */
    [[nodiscard]] std::optional<GrpcResponse>
    waitForResponse(uint64_t id,
                    std::chrono::milliseconds timeout =
                        std::chrono::seconds(30));

    /**
     * @brief Non-blocking check for a completed response.
     *
     * @param id  Request ID returned by @ref submit.
     * @return The @ref GrpcResponse if already complete, or @c std::nullopt
     *         if the grpc_proc thread has not yet finished the RPC.
     *
     * @note Thread-safe.
     * @note Retrieving a response removes it from the internal map.
     */
    [[nodiscard]] std::optional<GrpcResponse> tryGetResponse(uint64_t id);

    // -----------------------------------------------------------------------
    // Diagnostics
    // -----------------------------------------------------------------------

    /**
     * @brief Returns the current number of requests waiting in the queue.
     */
    [[nodiscard]] std::size_t pendingCount() const;

    /**
     * @brief Returns the current number of responses stored but not yet
     *        retrieved by a caller.
     */
    [[nodiscard]] std::size_t storedResponseCount() const;

private:
    /**
     * @brief Entry point for the grpc_proc thread.
     *
     * Loops: waits for a request, dequeues it, executes the RPC via
     * @c client_, stores the response, notifies waiters.
     */
    void threadFunc();

    /**
     * @brief Dispatches a single @ref GrpcRequest and returns the
     *        corresponding @ref ResponsePayload.
     *
     * @param req  The request to execute.
     * @return The RPC result wrapped in the appropriate result type.
     */
    ResponsePayload dispatch(const GrpcRequest& req);

    // -----------------------------------------------------------------------
    // Members
    // -----------------------------------------------------------------------

    RouteClient& client_; ///< Underlying synchronous gRPC client.

    // Request queue
    std::queue<GrpcRequest>   requestQueue_; ///< Pending requests (FIFO).
    mutable std::mutex        requestMutex_; ///< Guards requestQueue_.
    std::condition_variable   requestCv_;    ///< Notified on new request or stop.

    // Response store
    std::unordered_map<uint64_t, GrpcResponse> responses_; ///< Completed responses keyed by ID.
    mutable std::mutex                          responseMutex_; ///< Guards responses_.
    std::condition_variable                     responseCv_;    ///< Notified when a response is stored.

    std::atomic<uint64_t> nextId_{1}; ///< Monotonic request ID counter.
    std::atomic<bool>     running_{false}; ///< Set to false to stop the thread.
    std::thread           thread_;         ///< The grpc_proc thread.
};

} // namespace sra
