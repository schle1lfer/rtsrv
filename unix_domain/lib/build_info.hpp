/**
 * @file build_info.hpp
 * @brief Compile-time build metadata injected by the Makefile.
 *
 * The Makefile passes five preprocessor defines:
 *   - BUILD_NUMBER  — static integer that increments with each release
 *   - BUILD_DATE    — ISO-8601 UTC timestamp of the build (e.g.
 * 2026-04-16T12:00:00Z)
 *   - GIT_BRANCH    — branch name (git rev-parse --abbrev-ref HEAD)
 *   - GIT_COMMIT    — short commit hash (git rev-parse --short HEAD)
 *   - GIT_AUTHOR    — author of the last commit (git log -1 --format='%an')
 *
 * If the binary is built outside of the Makefile the macros fall back to
 * safe sentinel values so the header always compiles cleanly.
 */
#pragma once

#ifndef BUILD_NUMBER
#define BUILD_NUMBER 0
#endif
#ifndef BUILD_DATE
#define BUILD_DATE "unknown"
#endif
#ifndef GIT_BRANCH
#define GIT_BRANCH "unknown"
#endif
#ifndef GIT_COMMIT
#define GIT_COMMIT "unknown"
#endif
#ifndef GIT_AUTHOR
#define GIT_AUTHOR "unknown"
#endif

namespace build_info
{

inline constexpr int number = BUILD_NUMBER;
inline constexpr const char* date = BUILD_DATE;
inline constexpr const char* branch = GIT_BRANCH;
inline constexpr const char* commit = GIT_COMMIT;
inline constexpr const char* author = GIT_AUTHOR;

} // namespace build_info
