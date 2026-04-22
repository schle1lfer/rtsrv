/**
 * @file client/include/client/vrf_udp_client.hpp
 * @brief VRF UDP client thread: sends VrfsRouteRequest frames to a UNIX-domain
 *        socket server and logs the decoded response.
 *
 * The client runs in its own thread.  Start it with start() and request a
 * clean shutdown with stop(); the destructor calls stop() if the thread is
 * still running.
 */

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

namespace sra
{

/**
 * @brief Configuration for VrfUdpClient.
 */
struct VrfUdpClientConfig
{
    std::string socket_path{"/tmp/ud_server.sock"}; ///< UNIX socket path
    std::string vrfs_name{"default"};               ///< VRF name (max 15 chars)
    std::uint32_t nexthop_id{777};                  ///< Next-hop identifier
    std::array<std::uint8_t, 4> nexthop_addr{192, 168, 1, 1}; ///< Next-hop IPv4
};

/**
 * @brief Runs the route_add_vrfs UDP client in a background thread.
 *
 * Each start() call spawns a thread that connects to the UNIX-domain server,
 * sends one VrfsRouteRequest, waits for the response, prints the result, and
 * then exits.
 */
class VrfUdpClient
{
public:
    explicit VrfUdpClient(VrfUdpClientConfig cfg = {});
    ~VrfUdpClient();

    VrfUdpClient(const VrfUdpClient&) = delete;
    VrfUdpClient& operator=(const VrfUdpClient&) = delete;

    /** @brief Launches the background thread (no-op if already running). */
    void start();

    /** @brief Signals the thread to stop and waits for it to finish. */
    void stop();

    /** @brief Returns true while the worker thread is running. */
    [[nodiscard]] bool running() const noexcept;

private:
    void run();

    VrfUdpClientConfig cfg_;
    std::atomic<bool> running_{false};
    std::thread thread_;
};

} // namespace sra
