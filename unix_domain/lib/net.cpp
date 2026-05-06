/**
 * @file net.cpp
 * @brief Implementation of the UNIX-domain socket network API.
 *
 * Compiled into both lib/net.so (shared) and lib/net.ar (static).
 *
 *
 * @date    2026
 */

#include "net.hpp"

#include "logger.hpp"

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <format>
#include <stdexcept>

namespace net
{

// ---------------------------------------------------------------------------
// Error category
// ---------------------------------------------------------------------------

namespace
{

/**
 * @brief Custom std::error_category for net::NetError codes.
 *
 * Registered as a singleton via net_error_category().
 */
class NetErrorCategory final : public std::error_category
{
public:
    /**
     * @brief Returns the category name.
     * @return The string "net".
     */
    [[nodiscard]] const char* name() const noexcept override
    {
        return "net";
    }

    /**
     * @brief Maps a NetError integer value to a human-readable message.
     * @param ev Integer representation of a NetError enumerator.
     * @return Descriptive error string.
     */
    [[nodiscard]] std::string message(int ev) const override
    {
        switch (static_cast<NetError>(ev))
        {
        case NetError::SocketCreateFailed:
            return "socket creation failed";
        case NetError::BindFailed:
            return "socket bind failed";
        case NetError::ListenFailed:
            return "socket listen failed";
        case NetError::ConnectFailed:
            return "socket connect failed";
        case NetError::AcceptFailed:
            return "socket accept failed";
        case NetError::SendFailed:
            return "socket send failed";
        case NetError::RecvFailed:
            return "socket recv failed";
        case NetError::SetOptFailed:
            return "setsockopt failed";
        case NetError::SetNonBlockFailed:
            return "set non-blocking failed";
        case NetError::ConnectionClosed:
            return "connection closed by peer";
        case NetError::WouldBlock:
            return "operation would block";
        default:
            return "unknown net error";
        }
    }
};

} // anonymous namespace

/**
 * @brief Returns the singleton NetErrorCategory instance.
 * @return Reference to the global NetError category.
 */
const std::error_category& net_error_category() noexcept
{
    static const NetErrorCategory instance;
    return instance;
}

/**
 * @brief Creates a std::error_code from a NetError enumerator.
 * @param e The NetError value to wrap.
 * @return Corresponding std::error_code in the net_error_category.
 */
std::error_code make_error_code(NetError e) noexcept
{
    return {static_cast<int>(e), net_error_category()};
}

// ---------------------------------------------------------------------------
// Socket RAII
// ---------------------------------------------------------------------------

/**
 * @brief Destroys the Socket, closing the underlying fd if still open.
 */
Socket::~Socket()
{
    close();
}

/**
 * @brief Move-constructs a Socket, transferring ownership of the fd.
 * @param other Source socket; its fd is set to -1 after the move.
 */
Socket::Socket(Socket&& other) noexcept : fd_{other.fd_}
{
    other.fd_ = -1;
}

/**
 * @brief Move-assigns a Socket, closing any currently held fd first.
 * @param other Source socket; its fd is set to -1 after the move.
 * @return Reference to this socket.
 */
Socket& Socket::operator=(Socket&& other) noexcept
{
    if (this != &other)
    {
        close();
        fd_ = other.fd_;
        other.fd_ = -1;
    }
    return *this;
}

/**
 * @brief Closes the underlying file descriptor and resets it to -1.
 *
 * Safe to call multiple times; a no-op when the fd is already invalid.
 */
void Socket::close() noexcept
{
    if (fd_ >= 0)
    {
        ::close(fd_);
        fd_ = -1;
    }
}

/**
 * @brief Releases ownership of the fd without closing it.
 * @return The raw file descriptor (caller takes ownership).
 */
int Socket::release() noexcept
{
    int tmp = fd_;
    fd_ = -1;
    return tmp;
}

// ---------------------------------------------------------------------------
// Socket creation and options
// ---------------------------------------------------------------------------

/**
 * @brief Creates a new AF_UNIX SOCK_STREAM socket.
 * @return A Socket wrapping the new fd, or NetError::SocketCreateFailed.
 */
std::expected<Socket, std::error_code> create_socket()
{
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
    {
        return std::unexpected(make_error_code(NetError::SocketCreateFailed));
    }
    return Socket{fd};
}

/**
 * @brief Enables or disables SO_REUSEADDR on a socket.
 * @param fd     File descriptor to configure.
 * @param enable true to enable, false to disable the option.
 * @return Empty expected on success, or NetError::SetOptFailed.
 */
std::expected<void, std::error_code> set_reuse_addr(int fd,
                                                    bool enable) noexcept
{
    const int opt = enable ? 1 : 0;
    if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
    {
        return std::unexpected(make_error_code(NetError::SetOptFailed));
    }
    return {};
}

/**
 * @brief Sets or clears the O_NONBLOCK flag on a file descriptor.
 * @param fd     File descriptor to configure.
 * @param enable true to set non-blocking mode, false to restore blocking mode.
 * @return Empty expected on success, or NetError::SetNonBlockFailed.
 */
std::expected<void, std::error_code> set_nonblocking(int fd,
                                                     bool enable) noexcept
{
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0)
    {
        return std::unexpected(make_error_code(NetError::SetNonBlockFailed));
    }
    if (enable)
    {
        flags |= O_NONBLOCK;
    }
    else
    {
        flags &= ~O_NONBLOCK;
    }
    if (::fcntl(fd, F_SETFL, flags) < 0)
    {
        return std::unexpected(make_error_code(NetError::SetNonBlockFailed));
    }
    return {};
}

// ---------------------------------------------------------------------------
// Server-side helpers
// ---------------------------------------------------------------------------

/**
 * @brief Binds a UNIX-domain socket to @p path, unlinking any stale file first.
 * @param fd   File descriptor of an unbound socket.
 * @param path Filesystem path for the socket file.
 * @return Empty expected on success, or NetError::BindFailed.
 */
std::expected<void, std::error_code> bind_socket(int fd,
                                                 const std::string& path)
{
    // Remove stale socket file so bind does not fail after a crash.
    ::unlink(path.c_str());

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path))
    {
        return std::unexpected(make_error_code(NetError::BindFailed));
    }
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    if (::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0)
    {
        return std::unexpected(make_error_code(NetError::BindFailed));
    }
    return {};
}

/**
 * @brief Puts a bound socket into the listening state.
 * @param fd      File descriptor of a bound socket.
 * @param backlog Maximum length of the pending-connections queue.
 * @return Empty expected on success, or NetError::ListenFailed.
 */
std::expected<void, std::error_code> listen_socket(int fd, int backlog) noexcept
{
    if (::listen(fd, backlog) < 0)
    {
        return std::unexpected(make_error_code(NetError::ListenFailed));
    }
    return {};
}

/**
 * @brief Accepts one pending connection on a listening socket.
 * @param fd File descriptor of a listening socket (may be non-blocking).
 * @return Socket for the new connection, NetError::WouldBlock if no connection
 *         is pending (EAGAIN/EWOULDBLOCK), or NetError::AcceptFailed on error.
 */
std::expected<Socket, std::error_code> accept_connection(int fd)
{
    const int client_fd = ::accept(fd, nullptr, nullptr);
    if (client_fd < 0)
    {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            return std::unexpected(make_error_code(NetError::WouldBlock));
        }
        return std::unexpected(make_error_code(NetError::AcceptFailed));
    }
    logger::log(logger::NOTICE,
                "net",
                std::format("accepted connection: new fd={}", client_fd));
    return Socket{client_fd};
}

// ---------------------------------------------------------------------------
// Client-side helpers
// ---------------------------------------------------------------------------

/**
 * @brief Connects a socket to a UNIX-domain server at @p path.
 * @param fd   Unconnected socket file descriptor.
 * @param path Filesystem path of the server socket.
 * @return Empty expected on success, or NetError::ConnectFailed.
 */
std::expected<void, std::error_code> connect_socket(int fd,
                                                    const std::string& path)
{
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path))
    {
        return std::unexpected(make_error_code(NetError::ConnectFailed));
    }
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) <
        0)
    {
        return std::unexpected(make_error_code(NetError::ConnectFailed));
    }
    logger::log(
        logger::NOTICE, "net", std::format("connected fd={} to {}", fd, path));
    return {};
}

// ---------------------------------------------------------------------------
// I/O helpers
// ---------------------------------------------------------------------------

/**
 * @brief Sends up to @p data.size() bytes on a socket (single syscall).
 * @param fd   Connected socket file descriptor.
 * @param data Bytes to transmit.
 * @return Number of bytes actually sent, NetError::WouldBlock if the send
 *         buffer is full, or NetError::SendFailed on error.
 * @note Uses MSG_NOSIGNAL to suppress SIGPIPE on broken connections.
 */
std::expected<std::size_t, std::error_code>
send_data(int fd, std::span<const std::uint8_t> data) noexcept
{
    const ssize_t n = ::send(fd, data.data(), data.size(), MSG_NOSIGNAL);
    if (n < 0)
    {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            return std::unexpected(make_error_code(NetError::WouldBlock));
        }
        logger::log(
            logger::ERR,
            "net",
            std::format("send_data fd={} failed: {}", fd, strerror(errno)));
        return std::unexpected(make_error_code(NetError::SendFailed));
    }
    return static_cast<std::size_t>(n);
}

/**
 * @brief Receives up to @p buffer.size() bytes from a socket (single syscall).
 * @param fd     Connected socket file descriptor.
 * @param buffer Destination span; must be non-empty.
 * @return Number of bytes received, NetError::WouldBlock if no data is
 *         available yet, NetError::ConnectionClosed if the peer closed the
 *         connection, or NetError::RecvFailed on error.
 */
std::expected<std::size_t, std::error_code>
recv_data(int fd, std::span<std::uint8_t> buffer) noexcept
{
    const ssize_t n = ::recv(fd, buffer.data(), buffer.size(), 0);
    if (n < 0)
    {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            return std::unexpected(make_error_code(NetError::WouldBlock));
        }
        logger::log(
            logger::ERR,
            "net",
            std::format("recv_data fd={} failed: {}", fd, strerror(errno)));
        return std::unexpected(make_error_code(NetError::RecvFailed));
    }
    if (n == 0)
    {
        logger::log(logger::NOTICE,
                    "net",
                    std::format("connection closed by peer fd={}", fd));
        return std::unexpected(make_error_code(NetError::ConnectionClosed));
    }
    logger::log_hex("net",
                    /*is_tx=*/false,
                    fd,
                    buffer.subspan(0, static_cast<std::size_t>(n)));
    return static_cast<std::size_t>(n);
}

/**
 * @brief Sends all bytes in @p data, retrying until the full span is written.
 * @param fd   Connected socket file descriptor.
 * @param data Bytes to transmit.
 * @return Empty expected on success, or the first send_data error encountered.
 */
std::expected<void, std::error_code>
send_all(int fd, std::span<const std::uint8_t> data)
{
    // Log the complete TX frame once before sending — send_data may split it
    // across multiple syscalls for large payloads so logging per-call would
    // produce partial fragments.
    logger::log_hex("net", /*is_tx=*/true, fd, data);

    std::size_t sent = 0;
    while (sent < data.size())
    {
        auto result = send_data(fd, data.subspan(sent));
        if (!result)
        {
            return std::unexpected(result.error());
        }
        sent += *result;
    }
    return {};
}

/**
 * @brief Reads exactly @p n bytes from a socket, blocking until all arrive.
 * @param fd File descriptor to read from.
 * @param n  Number of bytes to receive.
 * @return Vector of exactly @p n bytes on success, or the first recv_data
 *         error encountered.
 */
std::expected<std::vector<std::uint8_t>, std::error_code>
recv_exact(int fd, std::size_t n)
{
    std::vector<std::uint8_t> buf(n);
    std::size_t received = 0;
    while (received < n)
    {
        auto result = recv_data(fd, std::span{buf}.subspan(received));
        if (!result)
        {
            return std::unexpected(result.error());
        }
        received += *result;
    }
    return buf;
}

} // namespace net
