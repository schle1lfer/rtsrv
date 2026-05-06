/**
 * @file net.hpp
 * @brief Network API for UNIX domain sockets.
 *
 * Provides RAII socket handles, address reuse, non-blocking mode,
 * and send/receive helpers over AF_UNIX stream sockets.
 *
 *
 * @date    2026
 */

#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <system_error>
#include <vector>

namespace net
{

// ---------------------------------------------------------------------------
// Error category
// ---------------------------------------------------------------------------

/// @brief Error codes for network operations.
enum class NetError : int
{
    SocketCreateFailed = 1,
    BindFailed,
    ListenFailed,
    ConnectFailed,
    AcceptFailed,
    SendFailed,
    RecvFailed,
    SetOptFailed,
    SetNonBlockFailed,
    ConnectionClosed,
    WouldBlock, ///< Non-blocking operation would block (EAGAIN/EWOULDBLOCK)
};

/// @brief Returns the error category for NetError.
const std::error_category& net_error_category() noexcept;

/// @brief Creates a std::error_code from a NetError value.
std::error_code make_error_code(NetError e) noexcept;

} // namespace net

// Register NetError with the standard error_code infrastructure.
template <>
struct std::is_error_code_enum<net::NetError> : std::true_type
{};

namespace net
{

// ---------------------------------------------------------------------------
// RAII socket handle
// ---------------------------------------------------------------------------

/**
 * @brief RAII wrapper around a POSIX file descriptor used as a socket.
 *
 * The descriptor is closed automatically on destruction.
 * Move-only: cannot be copied.
 */
class Socket
{
public:
    /// @brief Constructs an invalid (closed) socket.
    Socket() noexcept = default;

    /// @brief Takes ownership of an existing descriptor.
    explicit Socket(int fd) noexcept : fd_{fd}
    {}

    ~Socket();

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;

    /// @brief Returns the underlying file descriptor (-1 if invalid).
    [[nodiscard]] int fd() const noexcept
    {
        return fd_;
    }

    /// @brief Returns true when the socket holds a valid descriptor.
    [[nodiscard]] bool valid() const noexcept
    {
        return fd_ >= 0;
    }

    /// @brief Closes the socket immediately.
    void close() noexcept;

    /**
     * @brief Releases ownership without closing.
     * @return The raw descriptor (caller is responsible for closing it).
     */
    int release() noexcept;

private:
    int fd_{-1};
};

// ---------------------------------------------------------------------------
// Socket creation and options
// ---------------------------------------------------------------------------

/**
 * @brief Creates a new AF_UNIX / SOCK_STREAM socket.
 * @return Socket on success, or an error code.
 */
[[nodiscard]] std::expected<Socket, std::error_code> create_socket();

/**
 * @brief Enables or disables SO_REUSEADDR on a socket.
 * @param fd     File descriptor of the socket.
 * @param enable true to enable (default), false to disable.
 * @return void on success, or an error code.
 */
[[nodiscard]] std::expected<void, std::error_code>
set_reuse_addr(int fd, bool enable = true) noexcept;

/**
 * @brief Switches a socket into (or out of) non-blocking mode.
 * @param fd     File descriptor of the socket.
 * @param enable true to enable non-blocking (default), false to disable.
 * @return void on success, or an error code.
 */
[[nodiscard]] std::expected<void, std::error_code>
set_nonblocking(int fd, bool enable = true) noexcept;

// ---------------------------------------------------------------------------
// Server-side helpers
// ---------------------------------------------------------------------------

/**
 * @brief Binds a socket to a UNIX-domain path.
 *
 * The path-file is removed before binding so that stale sockets do not
 * prevent restart.
 *
 * @param fd   File descriptor of the socket.
 * @param path Filesystem path for the socket file.
 * @return void on success, or an error code.
 */
[[nodiscard]] std::expected<void, std::error_code>
bind_socket(int fd, const std::string& path);

/**
 * @brief Starts listening for incoming connections.
 * @param fd      File descriptor of the socket.
 * @param backlog Maximum length of the pending connection queue.
 * @return void on success, or an error code.
 */
[[nodiscard]] std::expected<void, std::error_code>
listen_socket(int fd, int backlog = 10) noexcept;

/**
 * @brief Accepts one incoming connection (non-blocking-aware).
 *
 * On a non-blocking socket this returns NetError::WouldBlock when
 * no client is waiting.
 *
 * @param fd Listening socket descriptor.
 * @return A connected Socket on success, or an error code.
 */
[[nodiscard]] std::expected<Socket, std::error_code> accept_connection(int fd);

// ---------------------------------------------------------------------------
// Client-side helpers
// ---------------------------------------------------------------------------

/**
 * @brief Connects to a server at the given UNIX-domain path.
 * @param fd   File descriptor of the socket.
 * @param path Filesystem path of the server socket.
 * @return void on success, or an error code.
 */
[[nodiscard]] std::expected<void, std::error_code>
connect_socket(int fd, const std::string& path);

// ---------------------------------------------------------------------------
// I/O helpers
// ---------------------------------------------------------------------------

/**
 * @brief Sends up to @p data.size() bytes (single syscall).
 * @param fd   Connected socket descriptor.
 * @param data Bytes to transmit.
 * @return Number of bytes actually sent, or an error code.
 */
[[nodiscard]] std::expected<std::size_t, std::error_code>
send_data(int fd, std::span<const std::uint8_t> data) noexcept;

/**
 * @brief Receives up to @p buffer.size() bytes (single syscall).
 *
 * Returns NetError::ConnectionClosed when the peer has closed the connection.
 *
 * @param fd     Connected socket descriptor.
 * @param buffer Output buffer.
 * @return Number of bytes received, or an error code.
 */
[[nodiscard]] std::expected<std::size_t, std::error_code>
recv_data(int fd, std::span<std::uint8_t> buffer) noexcept;

/**
 * @brief Sends all @p data bytes, retrying until the buffer is exhausted.
 * @param fd   Connected socket descriptor.
 * @param data Bytes to transmit.
 * @return void on success, or an error code.
 */
[[nodiscard]] std::expected<void, std::error_code>
send_all(int fd, std::span<const std::uint8_t> data);

/**
 * @brief Receives exactly @p n bytes, retrying until the buffer is full.
 * @param fd Connected socket descriptor.
 * @param n  Number of bytes expected.
 * @return Vector of exactly @p n bytes on success, or an error code.
 */
[[nodiscard]] std::expected<std::vector<std::uint8_t>, std::error_code>
recv_exact(int fd, std::size_t n);

} // namespace net
