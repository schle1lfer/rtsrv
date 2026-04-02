##
# @file cmake/IncrementBuildNumber.cmake
# @brief Pre-build CMake script that increments the persistent build counter
#        and generates build_info.hpp with the new number and UTC timestamp.
#
# This script is invoked via `cmake -P` as a PRE-BUILD step (before any C++
# translation unit is compiled).  It must NOT be included with include(); it
# is a standalone script.
#
# Required -D arguments (passed from CMakeLists.txt):
#   BUILD_DIR        - CMAKE_CURRENT_BINARY_DIR of the top-level project.
#   PROJECT_VERSION  - Semantic version string of the project (e.g. "1.0.0").
#
# Outputs written to BUILD_DIR:
#   build_number.txt          - Plain text file with the new build number.
#   build_timestamp.txt       - Plain text file with the compact UTC timestamp
#                               (YYYYMMDDTHHMMSSz).
#   build_versioned_name.txt  - Plain text file with the versioned binary
#                               basename (e.g. rtsrv-000042-20260324T143022z).
#   generated/build_info.hpp  - C++23 header consumed by src/main.cpp and
#                               any translation unit needing build metadata.
##

cmake_minimum_required(VERSION 3.25)

# ---------------------------------------------------------------------------
# Validate required arguments
# ---------------------------------------------------------------------------
foreach(_req BUILD_DIR PROJECT_VERSION)
    if(NOT DEFINED ${_req})
        message(
            FATAL_ERROR
            "IncrementBuildNumber.cmake: missing required argument -D${_req}=..."
        )
    endif()
endforeach()

# ---------------------------------------------------------------------------
# Read and increment the persistent build counter
# ---------------------------------------------------------------------------
set(_num_file "${BUILD_DIR}/build_number.txt")

if(EXISTS "${_num_file}")
    file(READ "${_num_file}" _stored_num)
    string(STRIP "${_stored_num}" _stored_num)
    if(_stored_num MATCHES "^[0-9]+$")
        math(EXPR BUILD_NUMBER "${_stored_num} + 1")
    else()
        # Corrupt file - reset to 1
        set(BUILD_NUMBER 1)
        message(
            WARNING
            "IncrementBuildNumber: '${_num_file}' contained non-numeric data; "
            "resetting build number to 1"
        )
    endif()
else()
    set(BUILD_NUMBER 1)
endif()

# Persist the new number
file(WRITE "${_num_file}" "${BUILD_NUMBER}\n")

# ---------------------------------------------------------------------------
# Capture the current UTC timestamp in two formats:
#   Compact (safe for filenames): YYYYMMDDTHHMMSSz
#   Human   (for display/logs):   YYYY-MM-DD HH:MM:SS UTC
# ---------------------------------------------------------------------------
string(TIMESTAMP BUILD_TIMESTAMP  "%Y%m%dT%H%M%Sz" UTC)
string(TIMESTAMP BUILD_DATE_HUMAN "%Y-%m-%d"        UTC)
string(TIMESTAMP BUILD_TIME_HUMAN "%H:%M:%S"        UTC)

# Persist the timestamp so the post-build script can read it without
# having to invoke cmake -P a second time
set(_ts_file "${BUILD_DIR}/build_timestamp.txt")
file(WRITE "${_ts_file}" "${BUILD_TIMESTAMP}\n")

# ---------------------------------------------------------------------------
# Compute the versioned binary basename
#   Format:  rtsrv-<build_number_zero_padded_6>-<timestamp>
#   Example: rtsrv-000042-20260324T143022z
# ---------------------------------------------------------------------------
# Zero-pad the build number to 6 digits for lexicographic sort stability
string(LENGTH "${BUILD_NUMBER}" _bn_len)
math(EXPR _pad_count "6 - ${_bn_len}")
if(_pad_count GREATER 0)
    string(REPEAT "0" ${_pad_count} _padding)
else()
    set(_padding "")
endif()
set(BUILD_NUMBER_PADDED "${_padding}${BUILD_NUMBER}")

set(VERSIONED_BASENAME "rtsrv-${BUILD_NUMBER_PADDED}-${BUILD_TIMESTAMP}")

# Persist the versioned basename so the post-build script can read it
file(WRITE "${BUILD_DIR}/build_versioned_name.txt" "${VERSIONED_BASENAME}\n")

# ---------------------------------------------------------------------------
# Generate build_info.hpp
# ---------------------------------------------------------------------------
set(_gen_dir "${BUILD_DIR}/generated")
file(MAKE_DIRECTORY "${_gen_dir}")

file(
    WRITE
    "${_gen_dir}/build_info.hpp"
    "/**\n"
    " * @file build_info.hpp\n"
    " * @brief Auto-generated build metadata - do NOT edit manually.\n"
    " *\n"
    " * This header is regenerated before each build by\n"
    " * cmake/IncrementBuildNumber.cmake via a CMake PRE-BUILD custom target.\n"
    " * Any manual changes will be overwritten on the next build.\n"
    " *\n"
    " * Build number : ${BUILD_NUMBER}\n"
    " * Timestamp    : ${BUILD_TIMESTAMP}\n"
    " * Version      : ${PROJECT_VERSION}\n"
    " */\n"
    "\n"
    "#pragma once\n"
    "\n"
    "#include <string_view>\n"
    "\n"
    "namespace rtsrv::build\n"
    "{\n"
    "\n"
    "/// Monotonically increasing build counter (incremented on every build).\n"
    "inline constexpr unsigned int kBuildNumber = ${BUILD_NUMBER}U;\n"
    "\n"
    "/// Zero-padded build number string (6 digits, e.g. \"000042\").\n"
    "inline constexpr std::string_view kBuildNumberPadded = \"${BUILD_NUMBER_PADDED}\";\n"
    "\n"
    "/// Compact UTC build timestamp in ISO-8601 basic form: YYYYMMDDTHHMMSSz.\n"
    "/// This value is safe to embed in file names.\n"
    "inline constexpr std::string_view kBuildTimestamp = \"${BUILD_TIMESTAMP}\";\n"
    "\n"
    "/// Human-readable UTC build date: YYYY-MM-DD.\n"
    "inline constexpr std::string_view kBuildDate = \"${BUILD_DATE_HUMAN}\";\n"
    "\n"
    "/// Human-readable UTC build time: HH:MM:SS.\n"
    "inline constexpr std::string_view kBuildTime = \"${BUILD_TIME_HUMAN}\";\n"
    "\n"
    "/// Base project version (SemVer, set in CMakeLists.txt).\n"
    "inline constexpr std::string_view kProjectVersion = \"${PROJECT_VERSION}\";\n"
    "\n"
    "/// Full version string including build number and timestamp.\n"
    "/// Format: <semver>+<build_number>.<timestamp>\n"
    "/// Example: 1.0.0+42.20260324T143022z\n"
    "inline constexpr std::string_view kFullVersion =\n"
    "    \"${PROJECT_VERSION}+${BUILD_NUMBER}.${BUILD_TIMESTAMP}\";\n"
    "\n"
    "/// Versioned binary basename (without directory or extension).\n"
    "/// Format: rtsrv-<padded_build_number>-<timestamp>\n"
    "/// Example: rtsrv-000042-20260324T143022z\n"
    "inline constexpr std::string_view kVersionedBinaryName =\n"
    "    \"${VERSIONED_BASENAME}\";\n"
    "\n"
    "} // namespace rtsrv::build\n"
)

message(
    STATUS
    "[rtsrv] Build #${BUILD_NUMBER}  "
    "Timestamp: ${BUILD_TIMESTAMP}  "
    "Binary: ${VERSIONED_BASENAME}"
)
