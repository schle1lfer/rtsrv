/**
 * @file logger.hpp
 * @brief Global logging facility for the unix_domain protocol stack.
 *
 * ## Log levels
 * Levels follow the Linux kernel syslog numbering but with the scale
 * inverted so that level 1 is the most verbose (DEBUG) and level 7 is
 * the least verbose (EMERG):
 *
 * | Level | Name    | Typical use                              |
 * |-------|---------|------------------------------------------|
 * |   1   | DEBUG   | Raw socket bytes, per-frame trace        |
 * |   2   | INFO    | Decoded packet / message summaries       |
 * |   3   | NOTICE  | Connection events, start / stop          |
 * |   4   | WARNING | Non-fatal anomalies                      |
 * |   5   | ERR     | Protocol / decode errors                 |
 * |   6   | CRIT    | Critical failures                        |
 * |   7   | EMERG   | Unrecoverable / fatal errors             |
 *
 * ## Activation
 * Logging is **disabled by default**.  Call logger::init() (typically
 * right after parsing command-line arguments) to enable it:
 *
 * @code
 *   // Parse --logstream / --loglevel from argv.
 *   auto [logstream, loglevel, rest] = logger::parse_args(argc, argv);
 *   logger::init(logstream, loglevel);
 * @endcode
 *
 * A message is emitted when its level is **≥ the configured minimum level**:
 *  - @c --loglevel=1  →  emit everything (DEBUG through EMERG)
 *  - @c --loglevel=7  →  emit EMERG only
 *
 * 
 * @date    2026
 */

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace logger
{

// ---------------------------------------------------------------------------
// Level constants (1 = most verbose, 7 = least verbose)
// ---------------------------------------------------------------------------

/// @brief Most verbose level — raw bytes, per-field trace.
inline constexpr int DEBUG = 1;

/// @brief Decoded packet / message summaries.
inline constexpr int INFO = 2;

/// @brief Connection events, start/stop notices.
inline constexpr int NOTICE = 3;

/// @brief Non-fatal anomalies.
inline constexpr int WARNING = 4;

/// @brief Protocol / decode errors.
inline constexpr int ERR = 5;

/// @brief Critical failures.
inline constexpr int CRIT = 6;

/// @brief Unrecoverable / fatal errors.
inline constexpr int EMERG = 7;

// ---------------------------------------------------------------------------
// Command-line helpers
// ---------------------------------------------------------------------------

/**
 * @brief Result of parse_args(): extracted log options plus remaining argv.
 *
 * @c remaining holds all argv elements that were not consumed by
 * @c --logstream or @c --loglevel.  argv[0] (the program name) is always
 * included in @c remaining.
 */
struct ParseResult
{
    std::string logstream; ///< Destination (path, "stdout", or "stderr")
    int loglevel{1};       ///< Minimum level to emit (1–7)
    std::vector<std::string> remaining; ///< Non-log argv elements
};

/**
 * @brief Scans @p argv for @c --logstream and @c --loglevel options.
 *
 * Both options consume two tokens (the flag and its value).  All other
 * tokens are returned in ParseResult::remaining so that the caller can
 * process its own positional arguments unchanged.
 *
 * @param argc  Argument count (as received by main).
 * @param argv  Argument vector (as received by main).
 * @return Parsed log options and the residual argument list.
 */
ParseResult parse_args(int argc, char* argv[]);

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

/**
 * @brief Initialises the logging subsystem.
 *
 * May be called multiple times; each call replaces the previous
 * configuration and closes any previously opened log file.
 *
 * @param logstream  Output destination: a filesystem path, @c "stdout",
 *                   or @c "stderr".  Pass an empty string to disable
 *                   logging entirely.
 * @param loglevel   Minimum severity to emit (1–7).  Messages whose level
 *                   is below this value are silently dropped.
 */
void init(const std::string& logstream, int loglevel);

/**
 * @brief Flushes and closes the log stream (if a file was opened).
 *
 * Called automatically at program exit when init() was used with a file
 * path.  Safe to call manually before process termination.
 */
void shutdown() noexcept;

// ---------------------------------------------------------------------------
// Logging API
// ---------------------------------------------------------------------------

/**
 * @brief Returns true when logging is active and @p level would produce output.
 * @param level  Severity to test.
 */
[[nodiscard]] bool is_enabled(int level) noexcept;

/**
 * @brief Emits a log line if @p level ≥ the configured minimum level.
 *
 * Output format:
 * @code
 *   [WARNING] [udproto] CRC mismatch: expected=0x1234 computed=0x5678
 * @endcode
 *
 * @param level  Severity (use the DEBUG/INFO/… constants).
 * @param tag    Short module name, e.g. @c "net" or @c "udproto".
 * @param msg    Human-readable message.
 */
void log(int level, std::string_view tag, std::string_view msg);

/**
 * @brief Logs raw socket data as a hex byte dump at DEBUG level.
 *
 * Output format:
 * @code
 *   [  DEBUG] [net] TX fd=3 (47 bytes): [0x7e 0x00 0x13 ... 0x7e]
 * @endcode
 *
 * Frames longer than 64 bytes are truncated; the total byte count is
 * always appended.
 *
 * @param tag    Module identifier (e.g. @c "net").
 * @param is_tx  @c true for transmitted data, @c false for received data.
 * @param fd     File descriptor (used in the log message; -1 to omit).
 * @param data   Raw bytes to format.
 */
void log_hex(std::string_view tag,
             bool is_tx,
             int fd,
             std::span<const std::uint8_t> data);

} // namespace logger
