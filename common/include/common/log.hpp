/**
 * @file common/include/common/log.hpp
 * @brief RFC 5424 (Syslog Protocol) logging module shared by srmd and sra.
 *
 * ## Message format (RFC 5424 §6)
 * Every record is rendered as:
 * @code
 *   <PRI>1 TIMESTAMP HOSTNAME APP-NAME PROCID MSGID STRUCTURED-DATA MSG
 * @endcode
 *
 * - **PRI** = facility × 8 + severity  (e.g. @c <34> for Daemon/Critical).
 * - **TIMESTAMP** = RFC 3339 UTC, microsecond precision
 *   (e.g. @c 2026-05-12T14:30:00.123456Z).
 * - **STRUCTURED-DATA** = @c [SD-ID param="value" …] elements, or the
 *   NILVALUE @c "-" when none are present.
 * - **MSG** is UTF-8 and prefixed with the UTF-8 BOM (0xEF 0xBB 0xBF) as
 *   recommended by RFC 5424 §6.4.
 *
 * ## Supported sinks
 * | Sink            | Format                | Transport                     |
 * |-----------------|-----------------------|-------------------------------|
 * | ConsoleSink     | RFC 5424 or human     | stderr                        |
 * | FileSink        | RFC 5424 or human     | rotating file                 |
 * | UdpSink         | RFC 5424              | UDP datagram (RFC 5426)       |
 * | TcpSink         | RFC 5424              | TCP octet-count (RFC 6587)    |
 * | LocalSyslogSink | forwarded             | POSIX @c syslog(3) API        |
 *
 * Multiple sinks can be active simultaneously.
 *
 * ## Thread safety
 * All public functions are thread-safe.  A single mutex serialises access
 * to the shared sink state.
 *
 * ## Quick-start
 * @code
 *   rtsrv::log::Config cfg;
 *   cfg.app_name     = "srmd";
 *   cfg.facility     = rtsrv::log::Facility::Daemon;
 *   cfg.min_severity = rtsrv::log::Severity::Info;
 *   cfg.console.enabled = true;
 *
 *   if (auto r = rtsrv::log::init(cfg); !r)
 *       std::println(std::cerr, "log init failed: {}", r.error());
 *
 *   rtsrv::log::info("srmd started");
 *   rtsrv::log::warn("no route for 10.0.0.1", "ROUTE");
 *
 *   // With structured data:
 *   rtsrv::log::log(
 *       rtsrv::log::Severity::Error,
 *       "GRPC-ERR",
 *       "RPC failed",
 *       {{ "rpc@32473", {{ "code", "14" }, { "msg", "unavailable" }} }});
 * @endcode
 *
 * @version 1.0
 */

#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rtsrv::log
{

// ---------------------------------------------------------------------------
// RFC 5424 §6.2.1 – severity levels
// ---------------------------------------------------------------------------

/**
 * @brief RFC 5424 severity levels (§6.2.1).
 *
 * Numerically lower values indicate higher urgency.
 */
enum class Severity : std::uint8_t
{
    Emergency = 0, ///< System is unusable.
    Alert = 1,     ///< Action must be taken immediately.
    Critical = 2,  ///< Critical conditions.
    Error = 3,     ///< Error conditions.
    Warning = 4,   ///< Warning conditions.
    Notice = 5,    ///< Normal but significant condition.
    Info = 6,      ///< Informational messages.
    Debug = 7,     ///< Debug-level messages.
};

// ---------------------------------------------------------------------------
// RFC 5424 §6.2.1 – facility codes
// ---------------------------------------------------------------------------

/**
 * @brief RFC 5424 facility codes (§6.2.1).
 */
enum class Facility : std::uint8_t
{
    Kern = 0,      ///< Kernel messages.
    User = 1,      ///< User-level messages.
    Mail = 2,      ///< Mail system.
    Daemon = 3,    ///< System daemons.
    Auth = 4,      ///< Security / authorisation.
    Syslogd = 5,   ///< Messages generated internally by syslogd.
    Lpr = 6,       ///< Line printer subsystem.
    News = 7,      ///< Network news subsystem.
    Uucp = 8,      ///< UUCP subsystem.
    Cron = 9,      ///< Clock daemon.
    AuthPriv = 10, ///< Security / authorisation (private).
    Ftp = 11,      ///< FTP daemon.
    Ntp = 12,      ///< NTP subsystem.
    LogAudit = 13, ///< Log audit.
    LogAlert = 14, ///< Log alert.
    Clock = 15,    ///< Clock daemon (secondary).
    Local0 = 16,   ///< Local use 0.
    Local1 = 17,   ///< Local use 1.
    Local2 = 18,   ///< Local use 2.
    Local3 = 19,   ///< Local use 3.
    Local4 = 20,   ///< Local use 4.
    Local5 = 21,   ///< Local use 5.
    Local6 = 22,   ///< Local use 6.
    Local7 = 23,   ///< Local use 7.
};

// ---------------------------------------------------------------------------
// Structured Data (RFC 5424 §6.3)
// ---------------------------------------------------------------------------

/**
 * @brief A PARAM-NAME / PARAM-VALUE pair inside an SD-ELEMENT.
 *
 * @p value is automatically escaped on output: @c \\, @c ", and @c ] are
 * each prefixed with a @c \\ (RFC 5424 §6.3.3).
 */
struct SdParam
{
    std::string name;  ///< PARAM-NAME (≤32 printable US-ASCII; no SP = ] ")
    std::string value; ///< PARAM-VALUE (UTF-8; special chars escaped on output)
};

/**
 * @brief A complete SD-ELEMENT: @c "[SD-ID param-name=\"value\" ...]"
 *
 * Use IANA-registered SD-IDs (e.g. @c "timeQuality", @c "origin",
 * @c "meta") or private IDs with an enterprise-number suffix
 * (e.g. @c "srmd@32473").
 */
struct SdElement
{
    std::string id; ///< SD-ID (≤32 printable US-ASCII; no SP = ] ")
    std::vector<SdParam> params; ///< Parameter list for this element.
};

// ---------------------------------------------------------------------------
// Sink configuration
// ---------------------------------------------------------------------------

/**
 * @brief Configuration for the console (stderr) sink.
 */
struct ConsoleSink
{
    bool enabled{false};
    /// @c true → human-readable line; @c false → raw RFC 5424 string.
    bool human_readable{true};
};

/**
 * @brief Configuration for the rotating-file sink.
 */
struct FileSink
{
    bool enabled{false};
    std::string path;                               ///< Log file path.
    std::size_t rotation_bytes{10UL * 1024 * 1024}; ///< Rotate at this size.
    std::size_t max_files{
        5}; ///< Number of rotated archives to keep (0 = no archiving).
    /// @c false (default) → raw RFC 5424; @c true → human-readable.
    bool human_readable{false};
};

/**
 * @brief Configuration for the UDP syslog sink (RFC 5426).
 *
 * Each record is sent as a single UDP datagram.  The message is silently
 * truncated to 65507 bytes if it exceeds that limit.
 */
struct UdpSink
{
    bool enabled{false};
    std::string host;        ///< IPv4/IPv6 address or hostname.
    std::uint16_t port{514}; ///< Destination UDP port (RFC 5426 default: 514).
};

/**
 * @brief Configuration for the TCP syslog sink (RFC 6587).
 *
 * A persistent TCP connection is maintained and automatically re-established
 * on failure.  RFC 6587 octet-count framing (@c "LEN SP MSG") is used by
 * default; set @p octet_framing to @c false to use newline-delimited framing.
 */
struct TcpSink
{
    bool enabled{false};
    std::string host;        ///< IPv4/IPv6 address or hostname.
    std::uint16_t port{601}; ///< Destination TCP port.
    /// @c true (default) → RFC 6587 §3.4.1 octet-count; @c false → newline.
    bool octet_framing{true};
};

/**
 * @brief Configuration for the local POSIX @c syslog(3) sink.
 *
 * Calls @c openlog() with the @ref Config::app_name as @c ident and routes
 * each record through the local system logging daemon.
 */
struct LocalSyslogSink
{
    bool enabled{false};
    /// Flags passed to @c openlog() in addition to @c LOG_PID
    /// (e.g. @c LOG_NDELAY | @c LOG_CONS).
    int options{0};
};

// ---------------------------------------------------------------------------
// Top-level configuration
// ---------------------------------------------------------------------------

/**
 * @brief Complete logger configuration passed to @ref init().
 */
struct Config
{
    std::string app_name{"rtsrv"}; ///< APP-NAME (truncated to 48 chars).
    std::string hostname; ///< HOSTNAME override (empty → gethostname()).
    Facility facility{Facility::Daemon};   ///< Syslog facility for all records.
    Severity min_severity{Severity::Info}; ///< Records below this are dropped.

    ConsoleSink console;          ///< stderr console sink.
    FileSink file;                ///< Rotating log-file sink.
    UdpSink udp;                  ///< UDP remote syslog sink (RFC 5426).
    TcpSink tcp;                  ///< TCP remote syslog sink (RFC 6587).
    LocalSyslogSink local_syslog; ///< Local POSIX syslog(3) sink.
};

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

/**
 * @brief Initialises the logging subsystem and opens all enabled sinks.
 *
 * May be called more than once; each call closes existing sinks before
 * applying the new configuration.
 *
 * @param cfg  Logger configuration.
 * @return @c {} on success, or an error description string on failure.
 */
[[nodiscard]] std::expected<void, std::string> init(const Config& cfg);

/**
 * @brief Flushes and closes all sinks.
 *
 * Called automatically at process exit after @ref init() has been used.
 * Safe to call manually before that.
 */
void shutdown() noexcept;

// ---------------------------------------------------------------------------
// Formatting utility (stateless, exposed for testing)
// ---------------------------------------------------------------------------

/**
 * @brief Constructs a complete RFC 5424 syslog message string.
 *
 * This function is stateless and does not require @ref init() to have been
 * called.  It is exposed primarily for unit-testing and for forwarding
 * records to custom destinations.
 *
 * @param facility  RFC 5424 facility code.
 * @param severity  RFC 5424 severity level.
 * @param hostname  HOSTNAME field (max 255 chars; @c "-" for NILVALUE).
 * @param app_name  APP-NAME field (max 48 chars; @c "-" for NILVALUE).
 * @param procid    PROCID as a numeric process identifier.
 * @param msg_id    MSGID field (max 32 chars; @c "-" or empty → NILVALUE).
 * @param sd        Structured-data elements (empty → NILVALUE @c "-").
 * @param msg       Message body (UTF-8); prefixed with BOM when non-empty.
 * @return Fully formatted RFC 5424 syslog message string.
 */
[[nodiscard]] std::string format_message(Facility facility,
                                         Severity severity,
                                         std::string_view hostname,
                                         std::string_view app_name,
                                         std::int32_t procid,
                                         std::string_view msg_id,
                                         const std::vector<SdElement>& sd,
                                         std::string_view msg);

// ---------------------------------------------------------------------------
// Core logging function
// ---------------------------------------------------------------------------

/**
 * @brief Emits a log record to all configured sinks.
 *
 * Records whose @p sev is numerically greater than @c min_severity (i.e.
 * less urgent) are silently dropped.  Does nothing if @ref init() has not
 * been called.
 *
 * @param sev     Message severity.
 * @param msg_id  MSGID field (max 32 chars); @c "-" or empty → NILVALUE.
 * @param msg     Human-readable message body (UTF-8).
 * @param sd      Structured-data elements (may be empty).
 */
void log(Severity sev,
         std::string_view msg_id,
         std::string_view msg,
         std::vector<SdElement> sd = {});

// ---------------------------------------------------------------------------
// Severity-level convenience wrappers
// ---------------------------------------------------------------------------

/// @brief Emits an Emergency-severity record.
void emerg(std::string_view msg, std::string_view msg_id = "-");

/// @brief Emits an Alert-severity record.
void alert(std::string_view msg, std::string_view msg_id = "-");

/// @brief Emits a Critical-severity record.
void crit(std::string_view msg, std::string_view msg_id = "-");

/// @brief Emits an Error-severity record.
void err(std::string_view msg, std::string_view msg_id = "-");

/// @brief Emits a Warning-severity record.
void warn(std::string_view msg, std::string_view msg_id = "-");

/// @brief Emits a Notice-severity record.
void notice(std::string_view msg, std::string_view msg_id = "-");

/// @brief Emits an Info-severity record.
void info(std::string_view msg, std::string_view msg_id = "-");

/// @brief Emits a Debug-severity record.
void dbg(std::string_view msg, std::string_view msg_id = "-");

// ---------------------------------------------------------------------------
// Query helpers
// ---------------------------------------------------------------------------

/**
 * @brief Returns @c true when logging is initialised and @p sev passes the
 *        configured minimum-severity filter.
 */
[[nodiscard]] bool is_enabled(Severity sev) noexcept;

/// @brief Returns the currently configured minimum severity level.
[[nodiscard]] Severity min_severity() noexcept;

/// @brief Converts a severity to its RFC 5424 keyword (e.g. @c "warning").
[[nodiscard]] std::string_view to_string(Severity sev) noexcept;

/// @brief Converts a facility to its keyword (e.g. @c "daemon").
[[nodiscard]] std::string_view to_string(Facility fac) noexcept;

/**
 * @brief Parses a severity keyword (case-insensitive).
 *
 * Accepts both RFC 5424 names (@c "emergency", @c "alert", @c "critical",
 * @c "error", @c "warning", @c "notice", @c "informational", @c "debug")
 * and common abbreviations (@c "emerg", @c "crit", @c "err", @c "warn",
 * @c "info").
 *
 * @return Matching @ref Severity, or @c std::nullopt on unrecognised input.
 */
[[nodiscard]] std::optional<Severity>
parse_severity(std::string_view s) noexcept;

/**
 * @brief Parses a facility keyword (case-insensitive).
 * @return Matching @ref Facility, or @c std::nullopt on unrecognised input.
 */
[[nodiscard]] std::optional<Facility>
parse_facility(std::string_view s) noexcept;

} // namespace rtsrv::log
