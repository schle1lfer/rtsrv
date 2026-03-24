/**
 * @file daemon.cpp
 * @brief Implementation of the classic Linux daemon lifecycle.
 *
 * @version 1.0
 */

#include "daemon.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <boost/log/trivial.hpp>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <stdexcept>
#include <system_error>

namespace rtsrv
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
    // Step 1: First fork
    // The parent exits immediately so the shell regains control. The child
    // continues and will later call setsid().
    // -----------------------------------------------------------------------
    pid_t pid = ::fork();
    if (pid < 0)
    {
        throw std::system_error(
            errno, std::generic_category(), "First fork failed");
    }
    if (pid > 0)
    {
        // Parent process – exit cleanly
        ::_exit(EXIT_SUCCESS);
    }

    // -----------------------------------------------------------------------
    // Step 2: Create a new session
    // The child becomes the session leader, detaching from any controlling
    // terminal.
    // -----------------------------------------------------------------------
    if (::setsid() < 0)
    {
        throw std::system_error(
            errno, std::generic_category(), "setsid() failed");
    }

    // -----------------------------------------------------------------------
    // Step 3: Second fork
    // The session leader forks again. The second child can never reacquire a
    // controlling terminal because it is no longer a session leader.
    // -----------------------------------------------------------------------
    pid = ::fork();
    if (pid < 0)
    {
        throw std::system_error(
            errno, std::generic_category(), "Second fork failed");
    }
    if (pid > 0)
    {
        // First child – exit cleanly
        ::_exit(EXIT_SUCCESS);
    }

    // -----------------------------------------------------------------------
    // From here: grandchild (the actual daemon process)
    // -----------------------------------------------------------------------

    // Step 4: Change working directory to avoid blocking unmounts
    if (::chdir(workingDir_.c_str()) < 0)
    {
        throw std::system_error(errno,
                                std::generic_category(),
                                std::format("chdir('{}') failed", workingDir_));
    }

    // Step 5: Reset file-creation mask so the daemon has full control
    ::umask(0);

    // Step 6: Redirect stdin / stdout / stderr to /dev/null via dup2()
    redirectStdio();

    // Step 7: Write the PID file
    writePidFile();

    // Step 8: Install signal handlers
    installSignalHandlers();
}

// ---------------------------------------------------------------------------
// Static helpers
// ---------------------------------------------------------------------------

void Daemon::redirectStdio()
{
    // Open /dev/null for reading (will replace stdin)
    const int devNull = ::open("/dev/null", O_RDWR | O_CLOEXEC);
    if (devNull < 0)
    {
        throw std::system_error(
            errno, std::generic_category(), "open('/dev/null') failed");
    }

    // Redirect stdin (fd 0) → /dev/null
    if (::dup2(devNull, STDIN_FILENO) < 0)
    {
        ::close(devNull);
        throw std::system_error(errno,
                                std::generic_category(),
                                "dup2(devNull, STDIN_FILENO) failed");
    }

    // Redirect stdout (fd 1) → /dev/null
    if (::dup2(devNull, STDOUT_FILENO) < 0)
    {
        ::close(devNull);
        throw std::system_error(errno,
                                std::generic_category(),
                                "dup2(devNull, STDOUT_FILENO) failed");
    }

    // Redirect stderr (fd 2) → /dev/null
    if (::dup2(devNull, STDERR_FILENO) < 0)
    {
        ::close(devNull);
        throw std::system_error(errno,
                                std::generic_category(),
                                "dup2(devNull, STDERR_FILENO) failed");
    }

    // If devNull is not already one of the standard file descriptors, close it
    if (devNull > STDERR_FILENO)
    {
        ::close(devNull);
    }
}

void Daemon::installSignalHandlers() noexcept
{
    // Handler for SIGTERM and SIGINT – request orderly shutdown
    struct sigaction sa
    {};
    ::sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sa.sa_handler = [](int /*sig*/) noexcept {
        Daemon::stopRequested_.store(true, std::memory_order_relaxed);
    };

    ::sigaction(SIGTERM, &sa, nullptr);
    ::sigaction(SIGINT, &sa, nullptr);

    // Handler for SIGHUP – request configuration reload
    sa.sa_handler = [](int /*sig*/) noexcept {
        Daemon::reloadRequested_.store(true, std::memory_order_relaxed);
    };
    ::sigaction(SIGHUP, &sa, nullptr);

    // Ignore SIGPIPE – broken pipe on a gRPC channel must not kill the daemon
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

void Daemon::writePidFile() const
{
    // Ensure the parent directory exists
    const std::filesystem::path pidPath(pidFilePath_);
    const std::filesystem::path pidDir = pidPath.parent_path();

    std::error_code ec;
    std::filesystem::create_directories(pidDir, ec);
    // Ignore errors – the directory may already exist; failure is caught below.

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

} // namespace rtsrv
