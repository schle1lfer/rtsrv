/**
 * @file client/include/client/sra_udp_client.hpp
 * @brief VRF route client over a non-blocking UNIX-domain socket.
 *
 * SraUdpClient runs in a dedicated background thread.  Callers submit
 * commands (ROUTE_ADD, ROUTE_DEL, ROUTE_LIST) via submitAdd(), submitDelete(),
 * or submitList(); the thread picks them up, encodes and sends them over an
 * AF_UNIX stream socket in non-blocking mode (using poll() for all I/O
 * readiness checks), receives the response frame, and decodes and logs the
 * result.
 *
 * The thread remains alive until stop() is called, processing one queued
 * command at a time.  The socket is reconnected automatically if the peer
 * closes the connection between commands.
 *
 * @version 2.0
 */

#pragma once

#include "lib/cmd_proto.hpp"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <variant>

namespace sra
{

/// @brief Tag type for a ROUTE_LIST request (carries no parameters).
struct RouteListRequest
{};

/**
 * @brief Runs route exchanges (add / delete / list) in a background thread.
 *
 * Non-blocking socket operation:
 *  - The AF_UNIX socket is created with O_NONBLOCK set via
 * net::set_nonblocking().
 *  - All send / receive I/O uses poll() to wait for readiness before each
 *    syscall, with a configurable per-operation timeout.
 *  - connect() on AF_UNIX is typically instantaneous; EINPROGRESS is handled
 *    correctly if it occurs.
 *
 * Thread safety: submitAdd(), submitDelete(), and submitList() are safe to call
 * from any thread.
 */
class SraUdpClient
{
public:
    /// @brief Discriminated union of all supported command types.
    using Request =
        std::variant<cmdproto::SingleRouteRequest, ///< ROUTE_ADD (binary
                                                   ///< payload)
                     cmdproto::RouteDelParams,     ///< ROUTE_DEL (TLV)
                     RouteListRequest              ///< ROUTE_LIST (no fields)
                     >;

    /**
     * @brief Constructs the client.
     * @param socketPath  Filesystem path of the UNIX-domain server socket.
     * @param ioTimeoutMs Per-operation poll() timeout in milliseconds.
     */
    explicit SraUdpClient(std::string socketPath = "/tmp/ud_server.sock",
                          int ioTimeoutMs = 5000);

    ~SraUdpClient();

    SraUdpClient(const SraUdpClient&) = delete;
    SraUdpClient& operator=(const SraUdpClient&) = delete;

    /** @brief Launches the background thread (no-op if already running). */
    void start();

    /** @brief Signals the thread to stop and waits for it to join. */
    void stop();

    /** @brief Returns true while the worker thread is running. */
    [[nodiscard]] bool running() const noexcept;

    /**
     * @brief Enqueues a ROUTE_ADD request for async delivery.
     *
     * Thread-safe; may be called from any thread.  Returns immediately.
     *
     * @param req  SingleRouteRequest to deliver to ud_server.
     */
    void submitAdd(cmdproto::SingleRouteRequest req);

    /**
     * @brief Enqueues a ROUTE_DEL request for async delivery.
     *
     * Thread-safe; may be called from any thread.  Returns immediately.
     *
     * @param params  Route delete parameters (dst_addr, prefix_len, gateway).
     */
    void submitDelete(cmdproto::RouteDelParams params);

    /**
     * @brief Enqueues a ROUTE_LIST request for async delivery.
     *
     * Thread-safe; may be called from any thread.  Returns immediately.
     */
    void submitList();

private:
    void threadFunc();
    void processAddRequest(int fd, const cmdproto::SingleRouteRequest& req);
    void processDeleteRequest(int fd, const cmdproto::RouteDelParams& params);
    void processListRequest(int fd);

    std::string socketPath_;
    int ioTimeoutMs_;

    std::queue<Request> queue_;
    std::mutex queueMutex_;
    std::condition_variable queueCv_;

    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> running_{false};
    std::thread thread_;
};

} // namespace sra
