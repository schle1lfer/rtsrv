/**
 * @file common/src/log.cpp
 * @brief RFC 5424 logging module implementation.
 *
 * Thread-safety model
 * -------------------
 * Two atomics provide a lock-free fast path used by is_enabled() and the
 * entry guard of log():
 *
 *   g_ready   (atomic<bool>)    – true once init() completes successfully;
 *                                  cleared by shutdown() before joining the
 *                                  logging thread.
 *   g_min_sev (atomic<uint8_t>) – mirror of cfg.min_severity updated by init();
 *                                  read without the lock to filter cheap.
 *
 * Async logging thread
 * --------------------
 * log() enqueues a LogEntry (sev, msg_id, msg, sd) under g_queue_mutex and
 * wakes the logging thread via g_queue_cv.  The logging thread pops entries
 * in FIFO order and writes them to the configured sinks under g_mutex.
 *
 * Shutdown sequence (init() re-call or explicit shutdown()):
 *  1. g_ready.store(false)           – fast path stops accepting new entries.
 *  2. g_queue_active = false         – secondary gate under g_queue_mutex.
 *  3. g_thread_stop = true           – signals the thread to drain and exit.
 *  4. g_queue_cv.notify_one()        – wakes the thread.
 *  5. g_log_thread.join()            – waits for the thread to drain the queue.
 *  6. close_sinks() / reset State    – performed under g_mutex.
 *
 * Sink details:
 *  - Console  : atomic write(2) to STDERR_FILENO; no separate fd locking.
 *  - File     : std::ofstream with size-tracked rotation.
 *  - UDP      : connected SOCK_DGRAM; best-effort; max payload 65507 bytes.
 *  - TCP      : SOCK_STREAM; octet-count or newline framing; auto-reconnect.
 *  - Syslog   : POSIX openlog/syslog/closelog; ident pointer into g_state.
 *
 * @version 1.2
 */

#include "common/log.hpp"

#include <netdb.h>
#include <sys/socket.h>
#include <syslog.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <thread>

namespace rtsrv::log
{

namespace
{

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------

struct State
{
    bool initialised{false}; ///< True once init() succeeds; checked under g_mutex.
    Config cfg;
    std::string hostname;
    std::int32_t procid{0};

    // File sink
    std::ofstream file_stream;
    std::size_t file_bytes{0};

    // UDP sink
    int udp_fd{-1};

    // TCP sink
    int tcp_fd{-1};

    // openlog() ident pointer lives in cfg.app_name; tracked separately
    bool local_syslog_open{false};
};

// ---------------------------------------------------------------------------
// Fast-path atomics (no lock required for reads in is_enabled / log entry)
// ---------------------------------------------------------------------------

/// Set to true by init() after all sinks are open; cleared by shutdown().
std::atomic<bool> g_ready{false};

/// Mirror of cfg.min_severity updated by init(); allows lock-free severity
/// filtering before the mutex is taken.
std::atomic<std::uint8_t> g_min_sev{static_cast<std::uint8_t>(Severity::Debug)};

std::mutex g_mutex;
State g_state;

// ---------------------------------------------------------------------------
// Async logging thread state
// ---------------------------------------------------------------------------

/// A single log record queued for the logging thread.
struct LogEntry
{
    Severity sev;
    std::string msg_id;
    std::string msg;
    std::vector<SdElement> sd;
};

std::mutex g_queue_mutex;
std::condition_variable g_queue_cv;
std::queue<LogEntry> g_log_queue;
bool g_queue_active{false}; ///< True while the queue accepts new entries.
bool g_thread_stop{false};  ///< True when the thread should drain and exit.
std::thread g_log_thread;

// ---------------------------------------------------------------------------
// Timestamp
// ---------------------------------------------------------------------------

/// Returns the current UTC time formatted as "YYYY-MM-DDTHH:MM:SS.ffffffZ".
std::string current_timestamp()
{
    using namespace std::chrono;

    auto now = system_clock::now();
    auto now_us = time_point_cast<microseconds>(now);
    auto now_s = time_point_cast<seconds>(now);
    auto us = (now_us - now_s).count();

    std::time_t tt = system_clock::to_time_t(now);
    std::tm utc{};
    ::gmtime_r(&tt, &utc);

    return std::format("{:04d}-{:02d}-{:02d}T{:02d}:{:02d}:{:02d}.{:06d}Z",
                       utc.tm_year + 1900,
                       utc.tm_mon + 1,
                       utc.tm_mday,
                       utc.tm_hour,
                       utc.tm_min,
                       utc.tm_sec,
                       static_cast<int>(us));
}

// ---------------------------------------------------------------------------
// RFC 5424 field helpers
// ---------------------------------------------------------------------------

/// Truncates @p s to @p max_len and replaces characters outside the
/// PRINTUSASCII range (0x21–0x7E) with '_'.  Returns "-" on empty result.
std::string sanitize_printusascii(std::string_view s, std::size_t max_len)
{
    std::string out;
    out.reserve(std::min(s.size(), max_len));
    for (char c : s)
    {
        if (out.size() >= max_len)
            break;
        auto uc = static_cast<unsigned char>(c);
        out += (uc >= 0x21u && uc <= 0x7Eu) ? c : '_';
    }
    return out.empty() ? "-" : out;
}

/// Sanitizes an SD-NAME: same rules as sanitize_printusascii plus replaces
/// SP, '=', ']', and '"' with '_'.  Returns "_" on empty result.
std::string sanitize_sd_name(std::string_view s, std::size_t max_len = 32)
{
    std::string out;
    out.reserve(std::min(s.size(), max_len));
    for (char c : s)
    {
        if (out.size() >= max_len)
            break;
        if (c == ' ' || c == '=' || c == ']' || c == '"')
        {
            out += '_';
            continue;
        }
        auto uc = static_cast<unsigned char>(c);
        out += (uc >= 0x21u && uc <= 0x7Eu) ? c : '_';
    }
    return out.empty() ? "_" : out;
}

/// Escapes a PARAM-VALUE per RFC 5424 §6.3.3: prefixes '\\', '"', ']'
/// with a backslash.
std::string escape_sd_value(std::string_view v)
{
    std::string out;
    out.reserve(v.size() + 8);
    for (char c : v)
    {
        if (c == '\\' || c == '"' || c == ']')
            out += '\\';
        out += c;
    }
    return out;
}

/// Renders the STRUCTURED-DATA field (RFC 5424 §6.3).
/// Returns "-" (NILVALUE) when @p sd is empty.
std::string format_sd(const std::vector<SdElement>& sd)
{
    if (sd.empty())
        return "-";

    std::string out;
    for (const auto& elem : sd)
    {
        out += '[';
        out += sanitize_sd_name(elem.id, 32);
        for (const auto& p : elem.params)
        {
            out += ' ';
            out += sanitize_sd_name(p.name, 32);
            out += "=\"";
            out += escape_sd_value(p.value);
            out += '"';
        }
        out += ']';
    }
    return out;
}

// UTF-8 BOM prepended to MSG-UTF8 (RFC 5424 §6.4).
static constexpr std::string_view kBom{"\xEF\xBB\xBF", 3};

// ---------------------------------------------------------------------------
// Internal format_message (takes a pre-computed timestamp)
// ---------------------------------------------------------------------------

std::string build_rfc5424(Facility facility,
                          Severity severity,
                          std::string_view hostname,
                          std::string_view app_name,
                          std::int32_t procid,
                          std::string_view msg_id,
                          const std::vector<SdElement>& sd,
                          std::string_view msg,
                          const std::string& ts)
{
    auto prival =
        static_cast<unsigned>(facility) * 8u + static_cast<unsigned>(severity);

    std::string hn =
        hostname.empty() ? "-" : sanitize_printusascii(hostname, 255);
    std::string an =
        app_name.empty() ? "-" : sanitize_printusascii(app_name, 48);
    std::string mid = (msg_id.empty() || msg_id == "-")
                          ? "-"
                          : sanitize_printusascii(msg_id, 32);

    std::string out = std::format("<{}>1 {} {} {} {} {} {}",
                                  prival,
                                  ts,
                                  hn,
                                  an,
                                  procid,
                                  mid,
                                  format_sd(sd));
    if (!msg.empty())
    {
        out += ' ';
        out += kBom;
        out += msg;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Human-readable format
// ---------------------------------------------------------------------------

static constexpr std::array<std::string_view, 8> kSevLabel{{
    "EMERGENCY",
    "ALERT    ",
    "CRITICAL ",
    "ERROR    ",
    "WARNING  ",
    "NOTICE   ",
    "INFO     ",
    "DEBUG    ",
}};

std::string build_human(Severity sev,
                        std::string_view app_name,
                        std::string_view msg_id,
                        const std::vector<SdElement>& sd,
                        std::string_view msg,
                        const std::string& ts)
{
    auto idx = static_cast<std::uint8_t>(sev);
    std::string_view label =
        idx < kSevLabel.size() ? kSevLabel[idx] : "UNKNOWN  ";

    std::string line = std::format("{} {} [{}]", ts, label, app_name);

    if (!msg_id.empty() && msg_id != "-")
        line += std::format(" {}", msg_id);

    if (!msg.empty())
        line += std::format(" {}", msg);

    if (!sd.empty())
    {
        line += ' ';
        line += format_sd(sd);
    }
    return line;
}

// ---------------------------------------------------------------------------
// File sink – rotation
// ---------------------------------------------------------------------------

void rotate_file(State& s)
{
    s.file_stream.close();

    const std::string& base = s.cfg.file.path;
    const std::size_t max = s.cfg.file.max_files;

    if (max == 0)
    {
        // No archiving: truncate and reopen.
        s.file_stream.open(base, std::ios::out | std::ios::trunc);
        s.file_bytes = 0;
        return;
    }

    // Shift archives: .(max-1) → .max, ..., .1 → .2  (oldest is overwritten).
    for (std::size_t i = max; i >= 2; --i)
    {
        std::string src = std::format("{}.{}", base, i - 1);
        std::string dst = std::format("{}.{}", base, i);
        std::error_code ec;
        std::filesystem::rename(src, dst, ec); // no-op if src absent
    }

    // base → .1
    {
        std::error_code ec;
        std::filesystem::rename(base, base + ".1", ec);
    }

    s.file_stream.open(base, std::ios::out | std::ios::app);
    s.file_bytes = 0;
}

void file_write(State& s, const std::string& line)
{
    if (!s.file_stream.is_open())
        return;

    s.file_stream << line << '\n';
    s.file_stream.flush();
    s.file_bytes += line.size() + 1;

    if (s.file_bytes >= s.cfg.file.rotation_bytes)
        rotate_file(s);
}

// ---------------------------------------------------------------------------
// UDP sink
// ---------------------------------------------------------------------------

int udp_connect(const std::string& host, std::uint16_t port)
{
    addrinfo hints{};
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;

    addrinfo* res = nullptr;
    std::string port_str = std::to_string(port);

    if (::getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res) != 0)
        return -1;

    int fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0)
    {
        ::freeaddrinfo(res);
        return -1;
    }

    if (::connect(fd, res->ai_addr, res->ai_addrlen) != 0)
    {
        ::close(fd);
        ::freeaddrinfo(res);
        return -1;
    }

    ::freeaddrinfo(res);
    return fd;
}

void udp_write(State& s, const std::string& msg)
{
    if (s.udp_fd < 0)
        return;
    constexpr std::size_t kMaxPayload = 65507;
    auto len = std::min(msg.size(), kMaxPayload);
    ::send(s.udp_fd, msg.data(), len, MSG_NOSIGNAL);
}

// ---------------------------------------------------------------------------
// TCP sink
// ---------------------------------------------------------------------------

int tcp_connect(const std::string& host, std::uint16_t port)
{
    addrinfo hints{};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* res = nullptr;
    std::string port_str = std::to_string(port);

    if (::getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res) != 0)
        return -1;

    int fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0)
    {
        ::freeaddrinfo(res);
        return -1;
    }

    if (::connect(fd, res->ai_addr, res->ai_addrlen) != 0)
    {
        ::close(fd);
        ::freeaddrinfo(res);
        return -1;
    }

    ::freeaddrinfo(res);
    return fd;
}

void tcp_write(State& s, const std::string& rfc5424)
{
    // Frame per RFC 6587: octet-count "LEN SP MSG" or newline-delimited.
    std::string frame = s.cfg.tcp.octet_framing
                            ? std::format("{} {}", rfc5424.size(), rfc5424)
                            : rfc5424 + '\n';

    auto try_send = [&]() -> bool {
        ssize_t n = ::send(s.tcp_fd, frame.data(), frame.size(), MSG_NOSIGNAL);
        return n == static_cast<ssize_t>(frame.size());
    };

    auto reconnect = [&]() -> bool {
        if (s.tcp_fd >= 0)
        {
            ::close(s.tcp_fd);
            s.tcp_fd = -1;
        }
        s.tcp_fd = tcp_connect(s.cfg.tcp.host, s.cfg.tcp.port);
        return s.tcp_fd >= 0;
    };

    if (s.tcp_fd < 0 && !reconnect())
        return;

    if (!try_send())
    {
        if (reconnect())
            try_send();
    }
}

// ---------------------------------------------------------------------------
// Local syslog helpers
// ---------------------------------------------------------------------------

int to_posix_priority(Severity sev)
{
    switch (sev)
    {
    case Severity::Emergency:
        return LOG_EMERG;
    case Severity::Alert:
        return LOG_ALERT;
    case Severity::Critical:
        return LOG_CRIT;
    case Severity::Error:
        return LOG_ERR;
    case Severity::Warning:
        return LOG_WARNING;
    case Severity::Notice:
        return LOG_NOTICE;
    case Severity::Info:
        return LOG_INFO;
    case Severity::Debug:
        return LOG_DEBUG;
    }
    return LOG_INFO;
}

int to_posix_facility(Facility fac)
{
    switch (fac)
    {
    case Facility::Kern:
        return LOG_KERN;
    case Facility::User:
        return LOG_USER;
    case Facility::Mail:
        return LOG_MAIL;
    case Facility::Daemon:
        return LOG_DAEMON;
    case Facility::Auth:
        return LOG_AUTH;
    case Facility::Syslogd:
        return LOG_SYSLOG;
    case Facility::Lpr:
        return LOG_LPR;
    case Facility::News:
        return LOG_NEWS;
    case Facility::Uucp:
        return LOG_UUCP;
    case Facility::Cron:
        return LOG_CRON;
    case Facility::AuthPriv:
        return LOG_AUTHPRIV;
    case Facility::Ftp:
        return LOG_FTP;
    // Facilities 12-15 have no standard LOG_ constant; compute directly.
    case Facility::Ntp:
        return (12 << 3);
    case Facility::LogAudit:
        return (13 << 3);
    case Facility::LogAlert:
        return (14 << 3);
    case Facility::Clock:
        return (15 << 3);
    case Facility::Local0:
        return LOG_LOCAL0;
    case Facility::Local1:
        return LOG_LOCAL1;
    case Facility::Local2:
        return LOG_LOCAL2;
    case Facility::Local3:
        return LOG_LOCAL3;
    case Facility::Local4:
        return LOG_LOCAL4;
    case Facility::Local5:
        return LOG_LOCAL5;
    case Facility::Local6:
        return LOG_LOCAL6;
    case Facility::Local7:
        return LOG_LOCAL7;
    }
    return LOG_DAEMON;
}

// ---------------------------------------------------------------------------
// Hostname resolution
// ---------------------------------------------------------------------------

std::string resolve_hostname()
{
    char buf[256]{};
    if (::gethostname(buf, sizeof(buf) - 1) == 0)
        return std::string(buf);
    return "-";
}

// ---------------------------------------------------------------------------
// Teardown helper (called with g_mutex held)
// ---------------------------------------------------------------------------

void close_sinks(State& s)
{
    if (s.file_stream.is_open())
    {
        s.file_stream.flush();
        s.file_stream.close();
    }
    if (s.udp_fd >= 0)
    {
        ::close(s.udp_fd);
        s.udp_fd = -1;
    }
    if (s.tcp_fd >= 0)
    {
        ::close(s.tcp_fd);
        s.tcp_fd = -1;
    }
    if (s.local_syslog_open)
    {
        ::closelog();
        s.local_syslog_open = false;
    }
}

// ---------------------------------------------------------------------------
// Sink writer (called by logging thread with g_mutex held)
// ---------------------------------------------------------------------------

void write_entry_locked(Severity sev,
                        std::string_view msg_id,
                        std::string_view msg,
                        const std::vector<SdElement>& sd)
{
    const Config& cfg = g_state.cfg;
    std::string ts = current_timestamp();

    // Build the RFC 5424 string lazily (only if a raw-format sink is active).
    std::optional<std::string> rfc5424;
    auto get_rfc5424 = [&]() -> const std::string& {
        if (!rfc5424)
        {
            rfc5424 = build_rfc5424(cfg.facility,
                                    sev,
                                    g_state.hostname,
                                    cfg.app_name,
                                    g_state.procid,
                                    msg_id,
                                    sd,
                                    msg,
                                    ts);
        }
        return *rfc5424;
    };

    // Console sink
    if (cfg.console.enabled)
    {
        std::string line =
            cfg.console.human_readable
                ? build_human(sev, cfg.app_name, msg_id, sd, msg, ts)
                : get_rfc5424();
        line += '\n';
        // write(2) is signal-safe and avoids C++ stream buffering interleave.
        ::write(STDERR_FILENO, line.data(), line.size());
    }

    // File sink
    if (cfg.file.enabled && g_state.file_stream.is_open())
    {
        if (cfg.file.human_readable)
            file_write(g_state,
                       build_human(sev, cfg.app_name, msg_id, sd, msg, ts));
        else
            file_write(g_state, get_rfc5424());
    }

    // UDP sink
    if (cfg.udp.enabled && g_state.udp_fd >= 0)
        udp_write(g_state, get_rfc5424());

    // TCP sink
    if (cfg.tcp.enabled)
        tcp_write(g_state, get_rfc5424());

    // Local syslog sink – pass only the message text; syslogd adds its own
    // header fields.
    if (cfg.local_syslog.enabled && g_state.local_syslog_open)
    {
        std::string text;
        if (!msg_id.empty() && msg_id != "-")
            text = std::format("[{}] {}", msg_id, msg);
        else
            text = std::string(msg);
        ::syslog(to_posix_priority(sev), "%s", text.c_str());
    }
}

// ---------------------------------------------------------------------------
// Logging thread worker
// ---------------------------------------------------------------------------

void log_worker()
{
    while (true)
    {
        std::unique_lock qlock(g_queue_mutex);
        g_queue_cv.wait(qlock,
                        [] { return !g_log_queue.empty() || g_thread_stop; });

        // Drain all queued entries before checking stop.
        if (g_log_queue.empty())
            break; // g_thread_stop is true and no pending entries remain

        LogEntry entry = std::move(g_log_queue.front());
        g_log_queue.pop();
        qlock.unlock();

        // Write to sinks under the sink state lock.
        std::lock_guard slock(g_mutex);
        if (!g_state.initialised)
            continue;
        if (static_cast<std::uint8_t>(entry.sev) >
            static_cast<std::uint8_t>(g_state.cfg.min_severity))
            continue;
        write_entry_locked(entry.sev, entry.msg_id, entry.msg, entry.sd);
    }
}

// ---------------------------------------------------------------------------
// Stop and join the logging thread (called without holding any lock)
// ---------------------------------------------------------------------------

void stop_log_thread() noexcept
{
    {
        std::lock_guard qlock(g_queue_mutex);
        if (!g_queue_active && !g_log_thread.joinable())
            return; // already stopped
        g_queue_active = false;
        g_thread_stop = true;
    }
    g_queue_cv.notify_one();
    if (g_log_thread.joinable())
        g_log_thread.join();
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Public: format_message
// ---------------------------------------------------------------------------

std::string format_message(Facility facility,
                           Severity severity,
                           std::string_view hostname,
                           std::string_view app_name,
                           std::int32_t procid,
                           std::string_view msg_id,
                           const std::vector<SdElement>& sd,
                           std::string_view msg)
{
    return build_rfc5424(facility,
                         severity,
                         hostname,
                         app_name,
                         procid,
                         msg_id,
                         sd,
                         msg,
                         current_timestamp());
}

// ---------------------------------------------------------------------------
// Public: init
// ---------------------------------------------------------------------------

std::expected<void, std::string> init(const Config& cfg)
{
    // Stop any previously running logging thread before touching sink state.
    // This must be done without holding g_mutex to avoid deadlocking with the
    // worker thread that acquires g_mutex to write entries.
    stop_log_thread();

    std::lock_guard lock(g_mutex);

    if (g_state.initialised)
    {
        g_ready.store(false, std::memory_order_release);
        close_sinks(g_state);
        g_state = State{};
    }

    g_state.cfg = cfg;
    g_state.procid = static_cast<std::int32_t>(::getpid());
    g_state.hostname = cfg.hostname.empty() ? resolve_hostname() : cfg.hostname;

    if (g_state.cfg.app_name.size() > 48)
        g_state.cfg.app_name.resize(48);

    // File sink
    if (cfg.file.enabled)
    {
        g_state.file_stream.open(cfg.file.path, std::ios::out | std::ios::app);
        if (!g_state.file_stream.is_open())
        {
            return std::unexpected(
                std::format("log: cannot open log file '{}'", cfg.file.path));
        }
        std::error_code ec;
        auto sz = std::filesystem::file_size(cfg.file.path, ec);
        g_state.file_bytes = ec ? 0 : static_cast<std::size_t>(sz);
    }

    // UDP sink
    if (cfg.udp.enabled)
    {
        g_state.udp_fd = udp_connect(cfg.udp.host, cfg.udp.port);
        if (g_state.udp_fd < 0)
        {
            return std::unexpected(
                std::format("log: UDP connect to {}:{} failed: {}",
                            cfg.udp.host,
                            cfg.udp.port,
                            std::strerror(errno)));
        }
    }

    // TCP sink – connection failure is deferred to first write attempt.
    if (cfg.tcp.enabled)
        g_state.tcp_fd = tcp_connect(cfg.tcp.host, cfg.tcp.port);

    // Local syslog sink.
    // The ident pointer (app_name.c_str()) must remain valid until closelog();
    // it points into g_state.cfg.app_name which is never modified after this.
    if (cfg.local_syslog.enabled)
    {
        ::openlog(g_state.cfg.app_name.c_str(),
                  cfg.local_syslog.options | LOG_PID,
                  to_posix_facility(cfg.facility));
        g_state.local_syslog_open = true;
    }

    g_state.initialised = true;

    // Start the logging thread.
    {
        std::lock_guard qlock(g_queue_mutex);
        // Discard any leftover entries from a previous init cycle.
        g_log_queue = {};
        g_thread_stop = false;
        g_queue_active = true;
    }
    g_log_thread = std::thread(log_worker);

    // Publish the severity threshold and signal readiness.  The release store
    // on g_ready pairs with the acquire load in log() / is_enabled() so that
    // all sink state written above is visible to other threads before they
    // observe g_ready == true.
    g_min_sev.store(static_cast<std::uint8_t>(cfg.min_severity),
                    std::memory_order_relaxed);
    g_ready.store(true, std::memory_order_release);

    // Register shutdown() with atexit so the logging thread is always joined
    // on program exit, even if the caller never calls shutdown() explicitly.
    static std::once_flag atexit_once;
    std::call_once(atexit_once,
                   [] { std::atexit([] { rtsrv::log::shutdown(); }); });

    return {};
}

// ---------------------------------------------------------------------------
// Public: shutdown
// ---------------------------------------------------------------------------

void shutdown() noexcept
{
    // Prevent new entries from being enqueued (fast path).
    g_ready.store(false, std::memory_order_release);

    // Signal and join the logging thread so it drains remaining entries.
    stop_log_thread();

    // Now safe to close sinks – the thread is no longer writing.
    std::lock_guard lock(g_mutex);
    if (!g_state.initialised)
        return;
    close_sinks(g_state);
    g_state = State{};
}

// ---------------------------------------------------------------------------
// Public: log
// ---------------------------------------------------------------------------

void log(Severity sev,
         std::string_view msg_id,
         std::string_view msg,
         std::vector<SdElement> sd)
{
    // --- Lock-free fast path -------------------------------------------------
    // Avoids taking any lock when the logger is not ready or the severity is
    // filtered out.
    if (!g_ready.load(std::memory_order_acquire))
        return;
    if (static_cast<std::uint8_t>(sev) >
        g_min_sev.load(std::memory_order_relaxed))
        return;

    // --- Enqueue under queue lock --------------------------------------------
    {
        std::lock_guard qlock(g_queue_mutex);
        if (!g_queue_active)
            return;
        g_log_queue.push(
            {sev, std::string(msg_id), std::string(msg), std::move(sd)});
    }
    g_queue_cv.notify_one();
}

// ---------------------------------------------------------------------------
// Convenience wrappers
// ---------------------------------------------------------------------------

void emerg(std::string_view msg, std::string_view msg_id)
{
    log(Severity::Emergency, msg_id, msg);
}

void alert(std::string_view msg, std::string_view msg_id)
{
    log(Severity::Alert, msg_id, msg);
}

void crit(std::string_view msg, std::string_view msg_id)
{
    log(Severity::Critical, msg_id, msg);
}

void err(std::string_view msg, std::string_view msg_id)
{
    log(Severity::Error, msg_id, msg);
}

void warn(std::string_view msg, std::string_view msg_id)
{
    log(Severity::Warning, msg_id, msg);
}

void notice(std::string_view msg, std::string_view msg_id)
{
    log(Severity::Notice, msg_id, msg);
}

void info(std::string_view msg, std::string_view msg_id)
{
    log(Severity::Info, msg_id, msg);
}

void dbg(std::string_view msg, std::string_view msg_id)
{
    log(Severity::Debug, msg_id, msg);
}

// ---------------------------------------------------------------------------
// Query helpers
// ---------------------------------------------------------------------------

bool is_enabled(Severity sev) noexcept
{
    return g_ready.load(std::memory_order_acquire) &&
           static_cast<std::uint8_t>(sev) <=
               g_min_sev.load(std::memory_order_relaxed);
}

Severity min_severity() noexcept
{
    return g_ready.load(std::memory_order_acquire)
               ? static_cast<Severity>(
                     g_min_sev.load(std::memory_order_relaxed))
               : Severity::Debug;
}

std::string_view to_string(Severity sev) noexcept
{
    switch (sev)
    {
    case Severity::Emergency:
        return "emergency";
    case Severity::Alert:
        return "alert";
    case Severity::Critical:
        return "critical";
    case Severity::Error:
        return "error";
    case Severity::Warning:
        return "warning";
    case Severity::Notice:
        return "notice";
    case Severity::Info:
        return "info";
    case Severity::Debug:
        return "debug";
    }
    return "unknown";
}

std::string_view to_string(Facility fac) noexcept
{
    switch (fac)
    {
    case Facility::Kern:
        return "kern";
    case Facility::User:
        return "user";
    case Facility::Mail:
        return "mail";
    case Facility::Daemon:
        return "daemon";
    case Facility::Auth:
        return "auth";
    case Facility::Syslogd:
        return "syslog";
    case Facility::Lpr:
        return "lpr";
    case Facility::News:
        return "news";
    case Facility::Uucp:
        return "uucp";
    case Facility::Cron:
        return "cron";
    case Facility::AuthPriv:
        return "authpriv";
    case Facility::Ftp:
        return "ftp";
    case Facility::Ntp:
        return "ntp";
    case Facility::LogAudit:
        return "logaudit";
    case Facility::LogAlert:
        return "logalert";
    case Facility::Clock:
        return "clock";
    case Facility::Local0:
        return "local0";
    case Facility::Local1:
        return "local1";
    case Facility::Local2:
        return "local2";
    case Facility::Local3:
        return "local3";
    case Facility::Local4:
        return "local4";
    case Facility::Local5:
        return "local5";
    case Facility::Local6:
        return "local6";
    case Facility::Local7:
        return "local7";
    }
    return "unknown";
}

std::optional<Severity> parse_severity(std::string_view s) noexcept
{
    auto ci_eq = [&](std::string_view rhs) noexcept -> bool {
        if (s.size() != rhs.size())
            return false;
        for (std::size_t i = 0; i < s.size(); ++i)
        {
            if (std::tolower(static_cast<unsigned char>(s[i])) != rhs[i])
                return false;
        }
        return true;
    };

    if (ci_eq("emergency") || ci_eq("emerg"))
        return Severity::Emergency;
    if (ci_eq("alert"))
        return Severity::Alert;
    if (ci_eq("critical") || ci_eq("crit"))
        return Severity::Critical;
    if (ci_eq("error") || ci_eq("err"))
        return Severity::Error;
    if (ci_eq("warning") || ci_eq("warn"))
        return Severity::Warning;
    if (ci_eq("notice"))
        return Severity::Notice;
    if (ci_eq("info") || ci_eq("informational"))
        return Severity::Info;
    if (ci_eq("debug"))
        return Severity::Debug;
    return std::nullopt;
}

std::optional<Facility> parse_facility(std::string_view s) noexcept
{
    auto ci_eq = [&](std::string_view rhs) noexcept -> bool {
        if (s.size() != rhs.size())
            return false;
        for (std::size_t i = 0; i < s.size(); ++i)
        {
            if (std::tolower(static_cast<unsigned char>(s[i])) != rhs[i])
                return false;
        }
        return true;
    };

    if (ci_eq("kern"))
        return Facility::Kern;
    if (ci_eq("user"))
        return Facility::User;
    if (ci_eq("mail"))
        return Facility::Mail;
    if (ci_eq("daemon"))
        return Facility::Daemon;
    if (ci_eq("auth"))
        return Facility::Auth;
    if (ci_eq("syslog"))
        return Facility::Syslogd;
    if (ci_eq("lpr"))
        return Facility::Lpr;
    if (ci_eq("news"))
        return Facility::News;
    if (ci_eq("uucp"))
        return Facility::Uucp;
    if (ci_eq("cron"))
        return Facility::Cron;
    if (ci_eq("authpriv"))
        return Facility::AuthPriv;
    if (ci_eq("ftp"))
        return Facility::Ftp;
    if (ci_eq("ntp"))
        return Facility::Ntp;
    if (ci_eq("logaudit"))
        return Facility::LogAudit;
    if (ci_eq("logalert"))
        return Facility::LogAlert;
    if (ci_eq("clock"))
        return Facility::Clock;
    if (ci_eq("local0"))
        return Facility::Local0;
    if (ci_eq("local1"))
        return Facility::Local1;
    if (ci_eq("local2"))
        return Facility::Local2;
    if (ci_eq("local3"))
        return Facility::Local3;
    if (ci_eq("local4"))
        return Facility::Local4;
    if (ci_eq("local5"))
        return Facility::Local5;
    if (ci_eq("local6"))
        return Facility::Local6;
    if (ci_eq("local7"))
        return Facility::Local7;
    return std::nullopt;
}

} // namespace rtsrv::log
