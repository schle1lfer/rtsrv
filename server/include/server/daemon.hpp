/**
 * @file server/include/server/daemon.hpp
 * @brief Classic Linux daemon lifecycle manager for srmd.
 *
 * Implements the standard double-fork daemonisation sequence:
 *  1. fork()   – parent exits; shell regains the prompt.
 *  2. setsid() – child detaches from its controlling terminal.
 *  3. fork()   – grandchild can never reacquire a terminal.
 *  4. chdir()  – avoid blocking filesystem unmounts.
 *  5. umask(0) – full control over created-file permissions.
 *  6. dup2()   – redirect stdin/stdout/stderr → /dev/null.
 *  7. PID file – written by the surviving grandchild.
 *  8. Signals  – SIGTERM/SIGINT → stop; SIGHUP → reload; SIGPIPE → ignored.
 *
 * @note Requires a POSIX-compliant Linux kernel.
 *
 * @version 1.0
 */

#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <string>

namespace srmd
{

/**
 * @brief Manages the daemon process lifecycle for the Switch Route Manager.
 *
 * Typical usage:
 * @code
 *   srmd::Daemon daemon("/run/srmd/srmd.pid");
 *   daemon.daemonise();
 *   daemon.setSighupHandler([&]{ reloadConfig(); });
 *   while (!srmd::Daemon::shouldStop()) {
 *       if (daemon.shouldReload()) { ... }
 *       std::this_thread::sleep_for(500ms);
 *   }
 * @endcode
 */
class Daemon
{
public:
    /**
     * @brief Constructs the Daemon manager.
     *
     * @param pidFilePath  Absolute path where the PID file will be written.
     * @param workingDir   Directory for chdir() after fork (default: "/").
     */
    explicit Daemon(std::string pidFilePath, std::string workingDir = "/");

    /** @brief Destructor – removes the PID file if this process wrote it. */
    ~Daemon();

    Daemon(const Daemon&) = delete;
    Daemon& operator=(const Daemon&) = delete;
    Daemon(Daemon&&) = delete;
    Daemon& operator=(Daemon&&) = delete;

    // -----------------------------------------------------------------------
    // Daemonisation
    // -----------------------------------------------------------------------

    /**
     * @brief Executes the double-fork daemonisation sequence.
     *
     * After this call the calling process is the daemon grandchild.
     * stdio file descriptors are all redirected to /dev/null via dup2().
     *
     * @throws std::system_error on fork(), setsid(), open(), or dup2() failure.
     * @throws std::runtime_error if the PID file cannot be written.
     */
    void daemonise();

    // -----------------------------------------------------------------------
    // Signal handling
    // -----------------------------------------------------------------------

    /**
     * @brief Registers the callback invoked when SIGHUP is received.
     *
     * Called from shouldReload() on the main thread (not from the signal
     * handler itself).
     *
     * @param handler Zero-argument callable.
     */
    void setSighupHandler(std::function<void()> handler);

    /**
     * @brief Returns true once per SIGHUP receipt and invokes the handler.
     */
    [[nodiscard]] bool shouldReload() noexcept;

    /**
     * @brief Returns true when SIGTERM or SIGINT has been received.
     */
    [[nodiscard]] static bool shouldStop() noexcept;

    // -----------------------------------------------------------------------
    // PID file
    // -----------------------------------------------------------------------

    /**
     * @brief Writes the current process PID to pidFilePath_.
     *
     * Called automatically by daemonise().
     *
     * @throws std::runtime_error if the file cannot be created or written.
     */
    void writePidFile();

    /**
     * @brief Removes the PID file.  Safe to call multiple times.
     */
    void removePidFile() noexcept;

private:
    /**
     * @brief Redirects stdin/stdout/stderr to /dev/null via dup2().
     *
     * @throws std::system_error on open() or dup2() failure.
     */
    static void redirectStdio();

    /** @brief Installs SIGTERM, SIGINT, SIGHUP, and SIGPIPE handlers. */
    static void installSignalHandlers() noexcept;

    // -----------------------------------------------------------------------
    // Data members
    // -----------------------------------------------------------------------

    std::string pidFilePath_; ///< Path of the PID file.
    std::string workingDir_;  ///< chdir() target.
    bool pidWritten_{false};  ///< True after a successful writePidFile().

    std::function<void()> sighupHandler_; ///< User-supplied SIGHUP callback.

    static std::atomic<bool> stopRequested_;   ///< Set by SIGTERM/SIGINT.
    static std::atomic<bool> reloadRequested_; ///< Set by SIGHUP.
};

} // namespace srmd
