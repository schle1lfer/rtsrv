/**
 * @file client/include/client/vrf_udp_client.hpp
 * @brief VRF route-add client over a non-blocking UNIX-domain socket.
 *
 * VrfUdpClient runs in a dedicated background thread.  Callers submit
 * SingleRouteRequest values via submit(); the thread picks them up, encodes
 * and sends them over an AF_UNIX stream socket in non-blocking mode (using
 * poll() for all I/O readiness checks), receives the response frame, decodes
 * and logs the per-VRF status bitmask.
 *
 * The thread remains alive until stop() is called, processing one queued
 * command at a time.  The socket is reconnected automatically if the peer
 * closes the connection between commands.
 *
 * @version 1.0
 */

#pragma once

#include "lib/cmd_proto.hpp"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

namespace sra
{

/**
 * @brief Runs the VRF route-add exchange in a background thread.
 *
 * Non-blocking socket operation:
 *  - The AF_UNIX socket is created with O_NONBLOCK set via net::set_nonblocking().
 *  - All send / receive I/O uses poll() to wait for readiness before each
 *    syscall, with a configurable per-operation timeout.
 *  - connect() on AF_UNIX is typically instantaneous; EINPROGRESS is handled
 *    correctly if it occurs.
 *
 * Thread safety: submit() is safe to call from any thread.
 */
class VrfUdpClient
{
public:
    /**
     * @brief Constructs the client.
     * @param socketPath  Filesystem path of the UNIX-domain server socket.
     * @param ioTimeoutMs Per-operation poll() timeout in milliseconds.
     */
    explicit VrfUdpClient(std::string socketPath    = "/tmp/ud_server.sock",
                          int         ioTimeoutMs   = 5000);

    ~VrfUdpClient();

    VrfUdpClient(const VrfUdpClient&) = delete;
    VrfUdpClient& operator=(const VrfUdpClient&) = delete;

    /** @brief Launches the background thread (no-op if already running). */
    void start();

    /** @brief Signals the thread to stop and waits for it to join. */
    void stop();

    /** @brief Returns true while the worker thread is running. */
    [[nodiscard]] bool running() const noexcept;

    /**
     * @brief Enqueues a route-add request for async delivery.
     *
     * Thread-safe; may be called from any thread.  Returns immediately.
     *
     * @param req  SingleRouteRequest to deliver to ud_server.
     */
    void submit(cmdproto::SingleRouteRequest req);

private:
    void threadFunc();
    void processRequest(const cmdproto::SingleRouteRequest& req);

    std::string socketPath_;
    int         ioTimeoutMs_;

    std::queue<cmdproto::SingleRouteRequest> queue_;
    std::mutex                              queueMutex_;
    std::condition_variable                 queueCv_;

    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> running_{false};
    std::thread       thread_;
};

} // namespace sra
