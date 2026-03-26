/**
 * @file server/src/daemon.cpp
 * @brief Daemon lifecycle implementation for srmd.
 *
 * @version 1.0
 */

#include "server/daemon.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <boost/log/trivial.hpp>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <stdexcept>
#include <system_error>

namespace srmd
{

// ---------------------------------------------------------------------------
// Static member definitions
// ---------------------------------------------------------------------------

std::atomic<bool> Daemon::stopRequested_{false};
std::atomic<bool> Daemon::reloadRequested_{false};

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

Daemon::Daemon(std::string pidFilePath, std::string workingDir)
    : pidFilePath_(std::move(pidFilePath)), workingDir_(std::move(workingDir))
{}

Daemon::~Daemon()
{
    removePidFile();
}

// ---------------------------------------------------------------------------
// Daemonisation
// ---------------------------------------------------------------------------

void Daemon::daemonise()
{
    // -----------------------------------------------------------------------
    // Step 1: First fork – parent exits so the shell regains the prompt.
    // -----------------------------------------------------------------------
    pid_t pid = ::fork();
    if (pid < 0)
    {
        throw std::system_error(
            errno, std::generic_category(), "First fork() failed");
    }
    if (pid > 0)
    {
        ::_exit(EXIT_SUCCESS); // parent
    }

    // -----------------------------------------------------------------------
    // Step 2: setsid() – become session leader, detach from terminal.
    // -----------------------------------------------------------------------
    if (::setsid() < 0)
    {
        throw std::system_error(
            errno, std::generic_category(), "setsid() failed");
    }

    // -----------------------------------------------------------------------
    // Step 3: Second fork – grandchild can never reacquire a terminal.
    // -----------------------------------------------------------------------
    pid = ::fork();
    if (pid < 0)
    {
        throw std::system_error(
            errno, std::generic_category(), "Second fork() failed");
    }
    if (pid > 0)
    {
        ::_exit(EXIT_SUCCESS); // first child
    }

    // -----------------------------------------------------------------------
    // Grandchild continues as the daemon process.
    // -----------------------------------------------------------------------

    // Step 4: Change working directory.
    if (::chdir(workingDir_.c_str()) < 0)
    {
        throw std::system_error(errno,
                                std::generic_category(),
                                std::format("chdir('{}') failed", workingDir_));
    }

    // Step 5: Reset umask so the daemon controls file permissions.
    ::umask(0);

    // Step 6: Redirect stdin / stdout / stderr → /dev/null via dup2().
    redirectStdio();

    // Step 7: Write the PID file.
    writePidFile();

    // Step 8: Install signal handlers.
    installSignalHandlers();
}

// ---------------------------------------------------------------------------
// Private static helpers
// ---------------------------------------------------------------------------

void Daemon::redirectStdio()
{
    const int devNull = ::open("/dev/null", O_RDWR | O_CLOEXEC);
    if (devNull < 0)
    {
        throw std::system_error(
            errno, std::generic_category(), "open('/dev/null') failed");
    }

    // stdin → /dev/null
    if (::dup2(devNull, STDIN_FILENO) < 0)
    {
        ::close(devNull);
        throw std::system_error(
            errno, std::generic_category(), "dup2(stdin) failed");
    }
    // stdout → /dev/null
    if (::dup2(devNull, STDOUT_FILENO) < 0)
    {
        ::close(devNull);
        throw std::system_error(
            errno, std::generic_category(), "dup2(stdout) failed");
    }
    // stderr → /dev/null
    if (::dup2(devNull, STDERR_FILENO) < 0)
    {
        ::close(devNull);
        throw std::system_error(
            errno, std::generic_category(), "dup2(stderr) failed");
    }

    if (devNull > STDERR_FILENO)
    {
        ::close(devNull);
    }
}

void Daemon::installSignalHandlers() noexcept
{
    struct sigaction sa
    {};
    ::sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;

    // SIGTERM / SIGINT → request orderly shutdown
    sa.sa_handler = [](int /*sig*/) noexcept {
        Daemon::stopRequested_.store(true, std::memory_order_relaxed);
    };
    ::sigaction(SIGTERM, &sa, nullptr);
    ::sigaction(SIGINT, &sa, nullptr);

    // SIGHUP → request configuration reload
    sa.sa_handler = [](int /*sig*/) noexcept {
        Daemon::reloadRequested_.store(true, std::memory_order_relaxed);
    };
    ::sigaction(SIGHUP, &sa, nullptr);

    // SIGPIPE → ignore (broken gRPC channel must not kill the daemon)
    sa.sa_handler = SIG_IGN;
    ::sigaction(SIGPIPE, &sa, nullptr);
}

// ---------------------------------------------------------------------------
// Signal query
// ---------------------------------------------------------------------------

bool Daemon::shouldStop() noexcept
{
    return stopRequested_.load(std::memory_order_relaxed);
}

bool Daemon::shouldReload() noexcept
{
    bool expected = true;
    if (reloadRequested_.compare_exchange_strong(
            expected, false, std::memory_order_acq_rel))
    {
        if (sighupHandler_)
        {
            sighupHandler_();
        }
        return true;
    }
    return false;
}

void Daemon::setSighupHandler(std::function<void()> handler)
{
    sighupHandler_ = std::move(handler);
}

// ---------------------------------------------------------------------------
// PID file
// ---------------------------------------------------------------------------

void Daemon::writePidFile()
{
    const std::filesystem::path pidPath(pidFilePath_);
    std::error_code ec;
    std::filesystem::create_directories(pidPath.parent_path(), ec);

    std::ofstream ofs(pidFilePath_, std::ios::out | std::ios::trunc);
    if (!ofs.is_open())
    {
        throw std::runtime_error(
            std::format("Cannot write PID file: '{}'", pidFilePath_));
    }
    ofs << ::getpid() << '\n';
    if (!ofs)
    {
        throw std::runtime_error(
            std::format("Write error on PID file: '{}'", pidFilePath_));
    }
    pidWritten_ = true;
}

void Daemon::removePidFile() noexcept
{
    if (!pidWritten_)
    {
        return;
    }
    std::error_code ec;
    std::filesystem::remove(pidFilePath_, ec);
    pidWritten_ = false;
}

} // namespace srmd
