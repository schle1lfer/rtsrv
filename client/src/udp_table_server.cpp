/**
 * @file client/src/udp_table_server.cpp
 * @brief UDP server that publishes NetLink table snapshots on fixed ports.
 *
 * Each of the three ports (9001/9002/9003) runs its own listener thread.
 * The thread uses poll(2) so it can be interrupted cleanly via a stop-pipe
 * without blocking indefinitely on recvfrom().
 *
 * Protocol (text):
 *   Client → any datagram          : server replies with latest snapshot.
 *   Client → datagram starting with "SUBSCRIBE"
 *                                  : reply + add sender to push list.
 *   Server → push (on setXxxData): snapshot sent to all subscribers.
 */

#include "client/udp_table_server.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iostream>
#include <print>

namespace sra
{

// ── Constructor / Destructor ─────────────────────────────────────────────────

UdpTableServer::UdpTableServer(uint16_t portNeighbors,
                               uint16_t portNexthops,
                               uint16_t portRoutes)
{
    neighbors_.port = portNeighbors;
    nexthops_.port = portNexthops;
    routes_.port = portRoutes;

    neighbors_.stopPipe[0] = neighbors_.stopPipe[1] = -1;
    nexthops_.stopPipe[0] = nexthops_.stopPipe[1] = -1;
    routes_.stopPipe[0] = routes_.stopPipe[1] = -1;
}

UdpTableServer::~UdpTableServer()
{
    stop();
}

// ── Lifecycle ────────────────────────────────────────────────────────────────

int UdpTableServer::openSocket(uint16_t port)
{
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
    {
        std::println(std::cerr,
                     "[UdpTableServer] socket() failed for port {}: {}",
                     port,
                     std::strerror(errno));
        return -1;
    }

    /* Allow immediate reuse after restart. */
    const int yes = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
    {
        std::println(std::cerr,
                     "[UdpTableServer] bind() failed for port {}: {}",
                     port,
                     std::strerror(errno));
        ::close(fd);
        return -1;
    }

    return fd;
}

bool UdpTableServer::start()
{
    if (running_)
        return true;

    /* Open all three sockets and stop-pipes before starting any thread so
     * that a partial failure can be cleaned up without leaving dangling
     * threads. */
    for (PortState* ps : {&neighbors_, &nexthops_, &routes_})
    {
        ps->fd = openSocket(ps->port);
        if (ps->fd < 0)
        {
            stop();
            return false;
        }

        if (::pipe(ps->stopPipe) < 0)
        {
            std::println(std::cerr,
                         "[UdpTableServer] pipe() failed for port {}: {}",
                         ps->port,
                         std::strerror(errno));
            stop();
            return false;
        }

        /* Make the write end non-blocking so the signal from stop() never
         * blocks even if the pipe buffer is somehow full. */
        ::fcntl(ps->stopPipe[1], F_SETFL, O_NONBLOCK);
    }

    running_ = true;

    neighbors_.thread = std::thread([this] {
        runListener(neighbors_);
    });
    nexthops_.thread = std::thread([this] {
        runListener(nexthops_);
    });
    routes_.thread = std::thread([this] {
        runListener(routes_);
    });

    std::println("[UdpTableServer] Listening on UDP ports {}/{}/{}",
                 neighbors_.port,
                 nexthops_.port,
                 routes_.port);
    return true;
}

void UdpTableServer::stop()
{
    running_ = false;

    /* Wake every listener thread by writing to its stop-pipe. */
    for (PortState* ps : {&neighbors_, &nexthops_, &routes_})
    {
        if (ps->stopPipe[1] >= 0)
        {
            const char wake = 1;
            (void)::write(ps->stopPipe[1], &wake, 1);
        }
    }

    for (PortState* ps : {&neighbors_, &nexthops_, &routes_})
    {
        if (ps->thread.joinable())
            ps->thread.join();

        if (ps->fd >= 0)
        {
            ::close(ps->fd);
            ps->fd = -1;
        }
        if (ps->stopPipe[0] >= 0)
        {
            ::close(ps->stopPipe[0]);
            ps->stopPipe[0] = -1;
        }
        if (ps->stopPipe[1] >= 0)
        {
            ::close(ps->stopPipe[1]);
            ps->stopPipe[1] = -1;
        }
    }
}

// ── Table data setters ───────────────────────────────────────────────────────

void UdpTableServer::setData(PortState& ps, std::string data)
{
    /* Update stored snapshot. */
    {
        std::unique_lock lock(ps.dataMutex);
        ps.data = data;
    }

    /* Push to subscribers (outside the data lock). */
    pushToSubscribers(ps, data);
}

void UdpTableServer::setNeighborData(std::string data)
{
    setData(neighbors_, std::move(data));
}

void UdpTableServer::setNexthopData(std::string data)
{
    setData(nexthops_, std::move(data));
}

void UdpTableServer::setRouteData(std::string data)
{
    setData(routes_, std::move(data));
}

// ── Push helpers ─────────────────────────────────────────────────────────────

void UdpTableServer::pushToSubscribers(PortState& ps,
                                       const std::string& payload)
{
    std::lock_guard lock(ps.subMutex);
    for (const auto& addr : ps.subscribers)
    {
        ::sendto(ps.fd,
                 payload.data(),
                 payload.size(),
                 0,
                 reinterpret_cast<const sockaddr*>(&addr),
                 sizeof(addr));
    }
}

// ── Listener thread ──────────────────────────────────────────────────────────

void UdpTableServer::runListener(PortState& ps)
{
    /* Two poll descriptors: [0] = UDP socket, [1] = stop-pipe read end. */
    pollfd fds[2];
    fds[0].fd = ps.fd;
    fds[0].events = POLLIN;
    fds[1].fd = ps.stopPipe[0];
    fds[1].events = POLLIN;

    constexpr std::size_t kBufLen = 256;
    char buf[kBufLen];

    while (running_)
    {
        fds[0].revents = 0;
        fds[1].revents = 0;

        const int ready = ::poll(fds, 2, -1 /* block indefinitely */);
        if (ready < 0)
        {
            if (errno == EINTR)
                continue;
            break;
        }

        /* Stop-pipe became readable → shutdown requested. */
        if (fds[1].revents & POLLIN)
            break;

        if (!(fds[0].revents & POLLIN))
            continue;

        /* Receive the incoming datagram. */
        sockaddr_in sender{};
        socklen_t senderLen = sizeof(sender);

        const ssize_t n = ::recvfrom(ps.fd,
                                     buf,
                                     kBufLen - 1,
                                     0,
                                     reinterpret_cast<sockaddr*>(&sender),
                                     &senderLen);
        if (n < 0)
            continue;

        buf[n] = '\0';

        /* Log the request. */
        char senderIp[INET_ADDRSTRLEN];
        ::inet_ntop(AF_INET, &sender.sin_addr, senderIp, sizeof(senderIp));

        /* Check for SUBSCRIBE request. */
        const bool subscribe =
            (n >= 9 && std::strncmp(buf, "SUBSCRIBE", 9) == 0);
        if (subscribe)
        {
            std::lock_guard lock(ps.subMutex);
            /* Deduplicate: only add if the address is not already present. */
            bool found = false;
            for (const auto& s : ps.subscribers)
            {
                if (s.sin_addr.s_addr == sender.sin_addr.s_addr &&
                    s.sin_port == sender.sin_port)
                {
                    found = true;
                    break;
                }
            }
            if (!found)
            {
                ps.subscribers.push_back(sender);
                std::println("[UdpTableServer] port {} subscriber added: {}:{}",
                             ps.port,
                             senderIp,
                             ntohs(sender.sin_port));
            }
        }

        /* Reply with the current snapshot. */
        std::string snapshot;
        {
            std::shared_lock lock(ps.dataMutex);
            snapshot = ps.data;
        }

        if (!snapshot.empty())
        {
            ::sendto(ps.fd,
                     snapshot.data(),
                     snapshot.size(),
                     0,
                     reinterpret_cast<const sockaddr*>(&sender),
                     senderLen);
        }
    }
}

} // namespace sra
