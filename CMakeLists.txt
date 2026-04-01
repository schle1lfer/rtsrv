##
# @file CMakeLists.txt
# @brief Top-level CMake build script for the srmd / sra project.
#
# Builds two executables:
#   srmd  – Switch Route Manager Daemon  (server, daemonises on Linux)
#   sra   – Switch Route Application     (CLI client)
#
# Both targets share:
#   - C++26 standard
#   - Proto / gRPC generated sources (compiled once into a shared OBJECT lib)
#   - common_lib (Boost.JSON config loader)
#   - Auto-incrementing build number baked into build_info.hpp
#
# Build numbering
# ---------------
# Every `cmake --build` increments <build-dir>/build_number.txt and bakes
# the counter + UTC timestamp into <build-dir>/generated/build_info.hpp.
# After linking each target, a versioned copy is placed alongside it:
#   srmd-<NNNNNN>-<YYYYMMDDTHHMMSSz>
#   sra-<NNNNNN>-<YYYYMMDDTHHMMSSz>
#
# Usage:
# @code
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build -j$(nproc)
#   cmake --install build --prefix /usr/local
#
#   # Quick smoke-test (two terminals):
#   build/server/srmd --foreground --config config/srmd.json
#   build/client/sra test
# @endcode
##

cmake_minimum_required(VERSION 3.25)

project(
    srmd_sra
    VERSION 1.0.0
    DESCRIPTION "Switch Route Manager Daemon + Switch Route Application"
    LANGUAGES C CXX
)

# ---------------------------------------------------------------------------
# C++ standard
# C++23 is required for std::expected, std::format, and std::ranges.
# std::print / std::println (<print>) landed in the standard with C++23 but
# libstdc++ only ships the header from GCC 14 onwards.  A compatibility shim
# lives in common/include/print and is found via the propagated include path
# of common_lib; it also adds the C++26 std::ostream overloads used by the
# std::println(std::cerr, ...) calls throughout the codebase.
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Build type default
# ---------------------------------------------------------------------------
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# Compiler warnings
# ---------------------------------------------------------------------------
add_compile_options(
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion
    -Wshadow
    -Wcast-align
    # C++-only warnings – guard against accidental application to C sources
    $<$<COMPILE_LANGUAGE:CXX>:-Wnon-virtual-dtor>
    $<$<COMPILE_LANGUAGE:CXX>:-Wold-style-cast>
    $<$<COMPILE_LANGUAGE:CXX>:-Woverloaded-virtual>
    $<$<CONFIG:Debug>:-g3>
    $<$<CONFIG:Debug>:-O0>
    $<$<CONFIG:Release>:-O3>
    $<$<CONFIG:RelWithDebInfo>:-O2>
    $<$<CONFIG:RelWithDebInfo>:-g>
)

# ---------------------------------------------------------------------------
# CMake module path
# ---------------------------------------------------------------------------
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------

find_package(Threads REQUIRED)

# --- Protobuf ---------------------------------------------------------------
find_package(Protobuf REQUIRED)
message(STATUS "Found Protobuf: ${Protobuf_VERSION}")

# --- gRPC -------------------------------------------------------------------
find_package(gRPC CONFIG REQUIRED)
if(NOT gRPC_FOUND)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GRPC REQUIRED grpc++)
endif()
message(STATUS "Found gRPC: ${gRPC_VERSION}")

# --- Boost ------------------------------------------------------------------
set(BOOST_MIN_VERSION "1.81")
find_package(
    Boost ${BOOST_MIN_VERSION}
    REQUIRED
    COMPONENTS
        filesystem
        json
        log
        log_setup
        program_options
        system
        thread
)
message(STATUS "Found Boost: ${Boost_VERSION}")

# ---------------------------------------------------------------------------
# Proto / gRPC code generation
# ---------------------------------------------------------------------------
set(PROTO_DIR     "${CMAKE_CURRENT_SOURCE_DIR}/proto")
set(PROTO_FILE    "${PROTO_DIR}/srmd.proto")
set(PROTO_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/proto_gen")
file(MAKE_DIRECTORY "${PROTO_GEN_DIR}")

set(PROTO_SRCS "${PROTO_GEN_DIR}/srmd.pb.cc")
set(PROTO_HDRS "${PROTO_GEN_DIR}/srmd.pb.h")
set(GRPC_SRCS  "${PROTO_GEN_DIR}/srmd.grpc.pb.cc")
set(GRPC_HDRS  "${PROTO_GEN_DIR}/srmd.grpc.pb.h")

# Locate the gRPC C++ plugin
if(TARGET gRPC::grpc_cpp_plugin)
    get_target_property(GRPC_CPP_PLUGIN_EXECUTABLE
                        gRPC::grpc_cpp_plugin LOCATION)
else()
    find_program(GRPC_CPP_PLUGIN_EXECUTABLE grpc_cpp_plugin REQUIRED)
endif()

add_custom_command(
    OUTPUT  "${PROTO_SRCS}" "${PROTO_HDRS}" "${GRPC_SRCS}" "${GRPC_HDRS}"
    COMMAND ${Protobuf_PROTOC_EXECUTABLE}
    ARGS
        "--proto_path=${PROTO_DIR}"
        "--cpp_out=${PROTO_GEN_DIR}"
        "--grpc_out=${PROTO_GEN_DIR}"
        "--plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN_EXECUTABLE}"
        "${PROTO_FILE}"
    DEPENDS "${PROTO_FILE}"
    COMMENT "Generating Protobuf / gRPC C++ sources from srmd.proto"
    VERBATIM
)

add_custom_target(generate_proto ALL DEPENDS "${PROTO_SRCS}" "${GRPC_SRCS}")

# PROTO_SRCS and GRPC_SRCS are already in the top-level scope;
# subdirectories inherit them automatically.

# ---------------------------------------------------------------------------
# Build number and timestamp (auto-incremented on every build)
# ---------------------------------------------------------------------------
set(BUILD_INFO_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${BUILD_INFO_GEN_DIR}")

add_custom_target(
    increment_build_number
    ALL
    COMMAND
        "${CMAKE_COMMAND}"
        "-DBUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}"
        "-DPROJECT_VERSION=${PROJECT_VERSION}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/IncrementBuildNumber.cmake"
    BYPRODUCTS
        "${CMAKE_CURRENT_BINARY_DIR}/build_number.txt"
        "${CMAKE_CURRENT_BINARY_DIR}/build_timestamp.txt"
        "${CMAKE_CURRENT_BINARY_DIR}/build_versioned_name.txt"
        "${BUILD_INFO_GEN_DIR}/build_info.hpp"
    COMMENT "Incrementing build number and generating build_info.hpp"
    VERBATIM
)

# ---------------------------------------------------------------------------
# Subdirectories
# ---------------------------------------------------------------------------
add_subdirectory(common)
add_subdirectory(server)
add_subdirectory(client)

# ---------------------------------------------------------------------------
# Installation
# ---------------------------------------------------------------------------
include(GNUInstallDirs)

install(TARGETS srmd RUNTIME DESTINATION "${CMAKE_INSTALL_SBINDIR}")
install(TARGETS sra  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

install(
    FILES "${CMAKE_CURRENT_SOURCE_DIR}/config/srmd.json"
    DESTINATION "${CMAKE_INSTALL_SYSCONFDIR}/srmd"
)

install(
    FILES "${CMAKE_CURRENT_SOURCE_DIR}/systemd/srmd.service"
    DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/systemd/system"
)

# ---------------------------------------------------------------------------
# Doxygen documentation (optional)
# ---------------------------------------------------------------------------
find_package(Doxygen OPTIONAL_COMPONENTS dot)
if(DOXYGEN_FOUND)
    set(DOXYGEN_PROJECT_NAME    "srmd / sra")
    set(DOXYGEN_PROJECT_BRIEF   "Switch Route Manager Daemon and Application")
    set(DOXYGEN_PROJECT_VERSION "${PROJECT_VERSION}")
    set(DOXYGEN_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/docs")
    set(DOXYGEN_GENERATE_HTML   YES)
    set(DOXYGEN_GENERATE_MAN    NO)
    set(DOXYGEN_EXTRACT_ALL     YES)
    set(DOXYGEN_RECURSIVE       YES)

    doxygen_add_docs(
        docs
        "${CMAKE_CURRENT_SOURCE_DIR}/common"
        "${CMAKE_CURRENT_SOURCE_DIR}/server"
        "${CMAKE_CURRENT_SOURCE_DIR}/client"
        "${CMAKE_CURRENT_SOURCE_DIR}/proto"
        COMMENT "Generating Doxygen documentation"
    )
endif()

# ---------------------------------------------------------------------------
# Git hook installation target
# ---------------------------------------------------------------------------
add_custom_target(
    install-hooks
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/hooks/install-hooks.sh"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Installing git hooks from hooks/"
    VERBATIM
)

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
message(STATUS "")
message(STATUS "=== srmd / sra build configuration ===")
message(STATUS "  Version         : ${PROJECT_VERSION}")
message(STATUS "  Build type      : ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ standard    : ${CMAKE_CXX_STANDARD}")
message(STATUS "  Install prefix  : ${CMAKE_INSTALL_PREFIX}")
message(STATUS "  Targets         : srmd (server)  sra (client)")
message(STATUS "  Build number    : auto-incremented each build")
message(STATUS "  Versioned names : srmd-<N>-<ts>  sra-<N>-<ts>")
message(STATUS "")
