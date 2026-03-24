/**
 * @file daemon.hpp
 * @brief Classic Linux daemon initialisation and lifecycle management.
 *
 * Provides the Daemon class which implements the classic double-fork
 * daemonisation sequence described in UNIX Network Programming and the
 * Linux Programming Interface:
 *
 *  1. Fork – the parent exits so the shell regains the prompt.
 *  2. setsid() – the child becomes the session and process group leader,
 *     detaching from any controlling terminal.
 *  3. Fork again – the grandchild can never reacquire a controlling terminal
 *     (it is not a session leader).
 *  4. chdir() – prevents the daemon from blocking unmount of the launch
 *     filesystem.
 *  5. umask(0) – gives the daemon full control over permissions of files it
 *     creates.
 *  6. dup2() – redirects stdin, stdout, and stderr to /dev/null so that
 *     accidental writes to standard streams do not produce errors.
 *  7. PID file – written by the surviving grandchild process.
 *  8. Signal handling – SIGTERM triggers an orderly shutdown; SIGHUP
 *     requests a configuration reload.
 *
 * @note This module requires a POSIX-compliant operating system (Linux).
 *
 * @version 1.0
 */

#pragma once

#include <atomic>
#include <csignal>
#include <filesystem>
#include <functional>
#include <string>

namespace rtsrv
{

/**
 * @brief Manages the daemon's process lifecycle.
 *
 * Usage pattern:
 * @code
 *   rtsrv::Daemon daemon("/run/rtsrv/rtsrv.pid", "/");
 *   daemon.daemonise();          // double-fork, dup2 stdio, write PID file
 *   daemon.setSighupHandler([]{ reload(); });
 *   // ... run the main event loop ...
 *   daemon.removePidFile();      // cleanup on exit
 * @endcode
 */
class Daemon
{
public:
    /**
     * @brief Constructs a Daemon instance.
     *
     * @param pidFilePath  Absolute path where the PID file will be written.
     *                     The parent directory must exist prior to calling
     *                     daemonise().
     * @param workingDir   Directory to chdir() into after daemonisation.
     *                     Defaults to "/" to avoid blocking unmounts.
     */
    explicit Daemon(std::string pidFilePath, std::string workingDir = "/");

    /** @brief Destructor – removes the PID file if it was written by this
     *         process. */
    ~Daemon();

    Daemon(const Daemon&) = delete;
    Daemon& operator=(const Daemon&) = delete;
    Daemon(Daemon&&) = delete;
    Daemon& operator=(Daemon&&) = delete;

    // -----------------------------------------------------------------------
    // Daemonisation
    // -----------------------------------------------------------------------

    /**
     * @brief Performs the classic double-fork daemonisation sequence.
     *
     * After this call the calling process has been replaced by the daemon
     * grandchild. The function performs the following steps in order:
     *  -# First fork – parent exits with EXIT_SUCCESS.
     *  -# setsid() in the child.
     *  -# Second fork – first child exits with EXIT_SUCCESS.
     *  -# chdir() to workingDir_.
     *  -# umask(0).
     *  -# Redirect stdin/stdout/stderr to /dev/null via dup2().
     *  -# Write the PID file.
     *  -# Install SIGTERM and SIGHUP handlers.
     *
     * @throws std::system_error if fork(), setsid(), open("/dev/null"), or
     *         dup2() fails.
     * @throws std::runtime_error if the PID file cannot be written.
     */
    void daemonise();

    // -----------------------------------------------------------------------
    // Signal handling
    // -----------------------------------------------------------------------

    /**
     * @brief Registers a callback invoked when SIGHUP is received.
     *
     * The callback is called from the daemon's main thread during the next
     * call to shouldReload(). It must be safe to call from signal context
     * (no heap allocation).
     *
     * @param handler Zero-argument callable.
     */
    void setSighupHandler(std::function<void()> handler);

    /**
     * @brief Returns true and clears the reload flag if SIGHUP was received.
     *
     * Invoke periodically from the main event loop to check whether a
     * configuration reload has been requested.
     *
     * @return True exactly once per SIGHUP receipt.
     */
    [[nodiscard]] bool shouldReload() noexcept;

    /**
     * @brief Returns true when the daemon should shut down.
     *
     * Set to true when SIGTERM or SIGINT is received.
     *
     * @return True if a shutdown signal has been delivered.
     */
    [[nodiscard]] static bool shouldStop() noexcept;

    // -----------------------------------------------------------------------
    // PID file
    // -----------------------------------------------------------------------

    /**
     * @brief Writes the current process PID to pidFilePath_.
     *
     * Called automatically by daemonise(). May also be called manually in
     * unit-test scenarios where daemonise() is not invoked.
     *
     * @throws std::runtime_error if the file cannot be created or written.
     */
    void writePidFile() const;

    /**
     * @brief Removes the PID file created by this instance.
     *
     * Safe to call multiple times. Does nothing if the PID file does not exist
     * or was not created by this process.
     */
    void removePidFile() noexcept;

private:
    /**
     * @brief Redirects stdin, stdout, and stderr to /dev/null using dup2().
     *
     * @throws std::system_error if open() or dup2() fails.
     */
    static void redirectStdio();

    /**
     * @brief Installs signal handlers for SIGTERM, SIGINT, and SIGHUP.
     */
    static void installSignalHandlers() noexcept;

    // -----------------------------------------------------------------------
    // Data members
    // -----------------------------------------------------------------------

    std::string pidFilePath_; ///< Path of the PID file to write.
    std::string workingDir_;  ///< Directory to chdir into after forking.
    bool pidWritten_{false};  ///< True once writePidFile() succeeds.

    std::function<void()> sighupHandler_; ///< User-supplied SIGHUP callback.

    // Static signal flags – set by SA_SIGACTION handlers; read from main loop.
    static std::atomic<bool> stopRequested_;   ///< Set on SIGTERM / SIGINT.
    static std::atomic<bool> reloadRequested_; ///< Set on SIGHUP.
};

} // namespace rtsrv
