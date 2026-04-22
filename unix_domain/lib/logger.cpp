/**
 * @file logger.cpp
 * @brief Implementation of the global logging facility.
 *
 * Thread-safe: all output is serialised through a single mutex so that
 * interleaved messages from the server's receiver and processor threads
 * remain readable.
 *
 * @author  Generated
 * @date    2026
 */

#include "logger.hpp"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <format>
#include <mutex>
#include <string>

namespace logger
{

// ---------------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------------

namespace
{

/// @brief Guards all accesses to the logger state below.
std::mutex g_mutex;

/// @brief Destination stream; nullptr means logging is disabled.
FILE* g_stream{nullptr};

/// @brief True when g_stream was opened by us and must be closed on shutdown.
bool g_owns_stream{false};

/// @brief Minimum level to emit (messages below this are dropped).
int g_min_level{DEBUG};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// @brief Maps a level integer to its display name (right-aligned, 7 chars).
constexpr std::string_view level_name(int level) noexcept
{
    switch (level)
    {
    case DEBUG:
        return "  DEBUG";
    case INFO:
        return "   INFO";
    case NOTICE:
        return " NOTICE";
    case WARNING:
        return "WARNING";
    case ERR:
        return "    ERR";
    case CRIT:
        return "   CRIT";
    case EMERG:
        return "  EMERG";
    default:
        return "UNKNOWN";
    }
}

/// @brief Formats all bytes in @p data as space-separated hex tokens.
std::string hex_span(std::span<const std::uint8_t> data)
{
    std::string out;
    out.reserve(data.size() * 5);

    for (std::size_t i = 0; i < data.size(); ++i)
    {
        if (i > 0)
            out += ' ';
        out += std::format("0x{:02x}", data[i]);
    }
    return out;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// parse_args
// ---------------------------------------------------------------------------

ParseResult parse_args(int argc, char* argv[])
{
    ParseResult result;
    bool loglevel_explicit = false;

    for (int i = 0; i < argc; ++i)
    {
        std::string_view arg{argv[i]};
        if ((arg == "--logstream") && (i + 1 < argc))
        {
            result.logstream = argv[++i];
        }
        else if ((arg == "--loglevel") && (i + 1 < argc))
        {
            std::string_view val{argv[++i]};

            // Accept numeric levels (1–7).
            try
            {
                int v = std::stoi(std::string{val});
                if (v >= DEBUG && v <= EMERG)
                {
                    result.loglevel  = v;
                    loglevel_explicit = true;
                }
            }
            catch (...)
            {
                // Accept named levels (case-insensitive).
                auto ci_eq = [&](std::string_view a, std::string_view b) {
                    if (a.size() != b.size())
                        return false;
                    for (std::size_t k = 0; k < a.size(); ++k)
                        if (std::tolower(static_cast<unsigned char>(a[k])) !=
                            std::tolower(static_cast<unsigned char>(b[k])))
                            return false;
                    return true;
                };

                if      (ci_eq(val, "debug"))   { result.loglevel = DEBUG;   loglevel_explicit = true; }
                else if (ci_eq(val, "info"))    { result.loglevel = INFO;    loglevel_explicit = true; }
                else if (ci_eq(val, "notice"))  { result.loglevel = NOTICE;  loglevel_explicit = true; }
                else if (ci_eq(val, "warning")) { result.loglevel = WARNING; loglevel_explicit = true; }
                else if (ci_eq(val, "err"))     { result.loglevel = ERR;     loglevel_explicit = true; }
                else if (ci_eq(val, "crit"))    { result.loglevel = CRIT;    loglevel_explicit = true; }
                else if (ci_eq(val, "emerg"))   { result.loglevel = EMERG;   loglevel_explicit = true; }
            }
        }
        else
        {
            result.remaining.emplace_back(argv[i]);
        }
    }

    // If the caller requested a specific log level but did not name a
    // logstream, default to stderr so that --loglevel <N> works on its own.
    if (loglevel_explicit && result.logstream.empty())
        result.logstream = "stderr";

    return result;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void init(const std::string& logstream, int loglevel)
{
    std::scoped_lock lock{g_mutex};

    // Close any previously opened file.
    if (g_owns_stream && g_stream)
    {
        std::fclose(g_stream);
        g_stream = nullptr;
        g_owns_stream = false;
    }

    if (logstream.empty())
    {
        g_stream = nullptr;
        return;
    }

    if (logstream == "stdout")
    {
        g_stream = stdout;
        g_owns_stream = false;
    }
    else if (logstream == "stderr")
    {
        g_stream = stderr;
        g_owns_stream = false;
    }
    else
    {
        g_stream = std::fopen(logstream.c_str(), "a");
        g_owns_stream = (g_stream != nullptr);
        if (!g_stream)
        {
            // Fall back to stderr so the failure is visible.
            std::fprintf(stderr,
                         "[logger] cannot open log file '%s': %s\n",
                         logstream.c_str(),
                         std::strerror(errno));
            g_stream = stderr;
        }
    }

    g_min_level = (loglevel >= DEBUG && loglevel <= EMERG) ? loglevel : DEBUG;
}

void shutdown() noexcept
{
    std::scoped_lock lock{g_mutex};
    if (g_owns_stream && g_stream)
    {
        std::fflush(g_stream);
        std::fclose(g_stream);
        g_stream = nullptr;
        g_owns_stream = false;
    }
}

// ---------------------------------------------------------------------------
// Logging API
// ---------------------------------------------------------------------------

bool is_enabled(int level) noexcept
{
    // Intentionally racy read of g_stream/g_min_level to avoid locking in the
    // hot path; the worst case is a spurious or missed log line during init.
    return (g_stream != nullptr) && (level >= g_min_level);
}

void log(int level, std::string_view tag, std::string_view msg)
{
    if (!is_enabled(level))
        return;

    const auto line =
        std::format("[{}] [{}] {}\n", level_name(level), tag, msg);

    std::scoped_lock lock{g_mutex};
    if (g_stream)
        std::fputs(line.c_str(), g_stream);
}

void log_hex(std::string_view tag,
             bool is_tx,
             int fd,
             std::span<const std::uint8_t> data)
{
    if (!is_enabled(DEBUG))
        return;

    std::string hex = hex_span(data);

    std::string line;
    if (fd >= 0)
    {
        line = std::format("[  DEBUG] [{}] {} fd={} ({} bytes): [{}]\n",
                           tag,
                           is_tx ? "TX" : "RX",
                           fd,
                           data.size(),
                           hex);
    }
    else
    {
        line = std::format("[  DEBUG] [{}] {} ({} bytes): [{}]\n",
                           tag,
                           is_tx ? "TX" : "RX",
                           data.size(),
                           hex);
    }

    std::scoped_lock lock{g_mutex};
    if (g_stream)
        std::fputs(line.c_str(), g_stream);
}

} // namespace logger
