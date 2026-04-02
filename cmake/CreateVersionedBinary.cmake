##
# @file cmake/CreateVersionedBinary.cmake
# @brief POST-BUILD CMake script that copies a linked binary to a versioned
#        filename alongside the primary output.
#
# Required -D arguments:
#   TARGET_FILE   – Absolute path of the freshly linked binary.
#   BUILD_DIR     – CMAKE_CURRENT_BINARY_DIR of the top-level project
#                   (contains build_versioned_name.txt).
#
# Optional -D arguments:
#   BINARY_NAME   – Base name for the versioned copy (e.g. "srmd" or "sra").
#                   Defaults to the stem of TARGET_FILE when omitted.
#
# Outputs (placed next to TARGET_FILE):
#   <BINARY_NAME>-<padded_build>-<timestamp>   versioned immutable snapshot
#   <BINARY_NAME>-latest                        stable alias to newest build
##

cmake_minimum_required(VERSION 3.25)

# ---------------------------------------------------------------------------
# Validate required arguments
# ---------------------------------------------------------------------------
foreach(_req TARGET_FILE BUILD_DIR)
    if(NOT DEFINED ${_req})
        message(
            FATAL_ERROR
            "CreateVersionedBinary.cmake: missing required argument "
            "-D${_req}=..."
        )
    endif()
endforeach()

if(NOT EXISTS "${TARGET_FILE}")
    message(
        FATAL_ERROR
        "CreateVersionedBinary.cmake: TARGET_FILE not found: '${TARGET_FILE}'"
    )
endif()

# ---------------------------------------------------------------------------
# Resolve binary base name
# ---------------------------------------------------------------------------
if(NOT DEFINED BINARY_NAME OR BINARY_NAME STREQUAL "")
    get_filename_component(BINARY_NAME "${TARGET_FILE}" NAME_WE)
endif()

# ---------------------------------------------------------------------------
# Read the padded build number and timestamp written by
# IncrementBuildNumber.cmake
# ---------------------------------------------------------------------------
set(_num_file "${BUILD_DIR}/build_number.txt")
set(_ts_file  "${BUILD_DIR}/build_timestamp.txt")

if(NOT EXISTS "${_num_file}" OR NOT EXISTS "${_ts_file}")
    message(
        FATAL_ERROR
        "CreateVersionedBinary.cmake: build_number.txt or "
        "build_timestamp.txt not found in '${BUILD_DIR}'. "
        "Did IncrementBuildNumber.cmake run before this script?"
    )
endif()

file(READ "${_num_file}" _raw_num)
string(STRIP "${_raw_num}" BUILD_NUMBER)

file(READ "${_ts_file}" _raw_ts)
string(STRIP "${_raw_ts}" BUILD_TIMESTAMP)

# Zero-pad build number to 6 digits
string(LENGTH "${BUILD_NUMBER}" _bn_len)
math(EXPR _pad_count "6 - ${_bn_len}")
if(_pad_count GREATER 0)
    string(REPEAT "0" ${_pad_count} _padding)
else()
    set(_padding "")
endif()
set(BUILD_NUMBER_PADDED "${_padding}${BUILD_NUMBER}")

# ---------------------------------------------------------------------------
# Derive full output paths
# ---------------------------------------------------------------------------
get_filename_component(_bin_dir "${TARGET_FILE}" DIRECTORY)

set(VERSIONED_FILE "${_bin_dir}/${BINARY_NAME}-${BUILD_NUMBER_PADDED}-${BUILD_TIMESTAMP}")
set(LATEST_FILE    "${_bin_dir}/${BINARY_NAME}-latest")

# ---------------------------------------------------------------------------
# Create the copies
# ---------------------------------------------------------------------------
file(COPY_FILE "${TARGET_FILE}" "${VERSIONED_FILE}" ONLY_IF_DIFFERENT)
file(COPY_FILE "${TARGET_FILE}" "${LATEST_FILE}")

# ---------------------------------------------------------------------------
# Report
# ---------------------------------------------------------------------------
message(STATUS
    "[${BINARY_NAME}] versioned → ${VERSIONED_FILE}"
)
message(STATUS
    "[${BINARY_NAME}] latest    → ${LATEST_FILE}"
)
