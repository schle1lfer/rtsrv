##
# @file cmake/CreateVersionedBinary.cmake
# @brief POST-BUILD CMake script that copies the linked binary to a versioned
#        filename in the same directory.
#
# The versioned filename is derived from build_versioned_name.txt which was
# written by IncrementBuildNumber.cmake during the PRE-BUILD phase.
#
# Required -D arguments (passed from CMakeLists.txt POST_BUILD command):
#   TARGET_FILE  – Absolute path of the freshly linked binary
#                  (i.e. $<TARGET_FILE:rtsrv>).
#   BUILD_DIR    – CMAKE_CURRENT_BINARY_DIR of the top-level project.
#
# Side-effects:
#   Creates <binary-dir>/rtsrv-<padded_build>-<timestamp>  (hard copy)
#   Creates <binary-dir>/rtsrv-latest                       (copy alias)
#   Prints the versioned name to the build log.
##

cmake_minimum_required(VERSION 3.25)

# ---------------------------------------------------------------------------
# Validate required arguments
# ---------------------------------------------------------------------------
foreach(_req TARGET_FILE BUILD_DIR)
    if(NOT DEFINED ${_req})
        message(FATAL_ERROR
            "CreateVersionedBinary.cmake: missing required argument "
            "-D${_req}=..."
        )
    endif()
endforeach()

if(NOT EXISTS "${TARGET_FILE}")
    message(FATAL_ERROR
        "CreateVersionedBinary.cmake: TARGET_FILE does not exist: "
        "'${TARGET_FILE}'"
    )
endif()

# ---------------------------------------------------------------------------
# Read the versioned basename written by IncrementBuildNumber.cmake
# ---------------------------------------------------------------------------
set(_name_file "${BUILD_DIR}/build_versioned_name.txt")
if(NOT EXISTS "${_name_file}")
    message(FATAL_ERROR
        "CreateVersionedBinary.cmake: versioned name file not found: "
        "'${_name_file}'. "
        "Did IncrementBuildNumber.cmake run before this script?"
    )
endif()

file(READ "${_name_file}" VERSIONED_BASENAME)
string(STRIP "${VERSIONED_BASENAME}" VERSIONED_BASENAME)

if(VERSIONED_BASENAME STREQUAL "")
    message(FATAL_ERROR
        "CreateVersionedBinary.cmake: '${_name_file}' is empty."
    )
endif()

# ---------------------------------------------------------------------------
# Derive full paths
# ---------------------------------------------------------------------------
get_filename_component(_bin_dir "${TARGET_FILE}" DIRECTORY)

set(VERSIONED_FILE  "${_bin_dir}/${VERSIONED_BASENAME}")
set(LATEST_FILE     "${_bin_dir}/rtsrv-latest")

# ---------------------------------------------------------------------------
# Copy the binary to its versioned filename
# Prefer COPY_FILE (CMake ≥ 3.21) for an atomic replace on POSIX.
# ---------------------------------------------------------------------------
file(COPY_FILE "${TARGET_FILE}" "${VERSIONED_FILE}" ONLY_IF_DIFFERENT)

# ---------------------------------------------------------------------------
# Update the -latest alias (unconditional copy so mtime is always current)
# ---------------------------------------------------------------------------
file(COPY_FILE "${TARGET_FILE}" "${LATEST_FILE}")

# ---------------------------------------------------------------------------
# Report
# ---------------------------------------------------------------------------
message(STATUS
    "[rtsrv] Versioned binary → ${VERSIONED_FILE}"
)
message(STATUS
    "[rtsrv] Latest alias    → ${LATEST_FILE}"
)
