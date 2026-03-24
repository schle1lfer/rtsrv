##
# @file CMakeLists.txt
# @brief Top-level CMake build script for the rtsrv Linux daemon.
#
# Builds the rtsrv daemon with C++23, gRPC / Protobuf generated sources,
# and Boost libraries. Installs the binary, default configuration file, and
# the systemd unit file.
#
# Minimum required CMake version: 3.25 (for C++23 standard support and the
# modern FindProtobuf / gRPC integration).
#
# Usage:
# @code
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build -j$(nproc)
#   cmake --install build --prefix /usr/local
# @endcode
##

cmake_minimum_required(VERSION 3.25)

project(
    rtsrv
    VERSION 1.0.0
    DESCRIPTION "Real-Time Service Daemon – gRPC-connected Linux daemon"
    LANGUAGES CXX
)

# ---------------------------------------------------------------------------
# C++23 standard – required by the entire project
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Build-type defaults
# ---------------------------------------------------------------------------
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE
        "RelWithDebInfo"
        CACHE STRING "Build type" FORCE
    )
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
    -Wnon-virtual-dtor
    -Wold-style-cast
    -Wcast-align
    -Woverloaded-virtual
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

# --- Threads ---------------------------------------------------------------
find_package(Threads REQUIRED)

# --- Protobuf --------------------------------------------------------------
find_package(Protobuf REQUIRED)
message(STATUS "Found Protobuf: ${Protobuf_VERSION}")

# --- gRPC ------------------------------------------------------------------
find_package(gRPC CONFIG REQUIRED)
if(NOT gRPC_FOUND)
    # Fallback: locate via pkg-config
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GRPC REQUIRED grpc++)
endif()
message(STATUS "Found gRPC: ${gRPC_VERSION}")

# --- Boost -----------------------------------------------------------------
set(BOOST_MIN_VERSION "1.81")
find_package(
    Boost ${BOOST_MIN_VERSION}
    REQUIRED
    COMPONENTS
        filesystem
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
set(PROTO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/proto")
set(PROTO_FILE "${PROTO_DIR}/rtsrv.proto")

# Directories where generated sources are placed
set(PROTO_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/proto_gen")
file(MAKE_DIRECTORY "${PROTO_GEN_DIR}")

# Derived output file names
set(PROTO_SRCS "${PROTO_GEN_DIR}/rtsrv.pb.cc")
set(PROTO_HDRS "${PROTO_GEN_DIR}/rtsrv.pb.h")
set(GRPC_SRCS "${PROTO_GEN_DIR}/rtsrv.grpc.pb.cc")
set(GRPC_HDRS "${PROTO_GEN_DIR}/rtsrv.grpc.pb.h")

# Locate the gRPC C++ plugin
if(TARGET gRPC::grpc_cpp_plugin)
    get_target_property(GRPC_CPP_PLUGIN_EXECUTABLE gRPC::grpc_cpp_plugin
                        LOCATION
    )
else()
    find_program(GRPC_CPP_PLUGIN_EXECUTABLE grpc_cpp_plugin REQUIRED)
endif()

# Custom command: compile the .proto file
add_custom_command(
    OUTPUT
        "${PROTO_SRCS}"
        "${PROTO_HDRS}"
        "${GRPC_SRCS}"
        "${GRPC_HDRS}"
    COMMAND
        ${Protobuf_PROTOC_EXECUTABLE}
        ARGS
        "--proto_path=${PROTO_DIR}"
        "--cpp_out=${PROTO_GEN_DIR}"
        "--grpc_out=${PROTO_GEN_DIR}"
        "--plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN_EXECUTABLE}"
        "${PROTO_FILE}"
    DEPENDS "${PROTO_FILE}"
    COMMENT "Generating Protobuf / gRPC C++ sources from rtsrv.proto"
    VERBATIM
)

# Convenience target so IDEs pick up the generated headers
add_custom_target(generate_proto ALL DEPENDS "${PROTO_SRCS}" "${GRPC_SRCS}")

# ---------------------------------------------------------------------------
# rtsrv executable
# ---------------------------------------------------------------------------
add_executable(
    rtsrv
    src/main.cpp
    src/config.cpp
    src/daemon.cpp
    src/grpc_client.cpp
    ${PROTO_SRCS}
    ${GRPC_SRCS}
)

# The executable depends on proto generation
add_dependencies(rtsrv generate_proto)

# Include paths
target_include_directories(
    rtsrv
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${PROTO_GEN_DIR}"
        ${Boost_INCLUDE_DIRS}
)

# Link libraries
target_link_libraries(
    rtsrv
    PRIVATE
        Threads::Threads
        protobuf::libprotobuf
        gRPC::grpc++
        gRPC::grpc++_reflection
        ${Boost_LIBRARIES}
)

# Boost.Log requires these definitions
target_compile_definitions(
    rtsrv
    PRIVATE
        BOOST_LOG_DYN_LINK
        BOOST_ALL_DYN_LINK
)

# ---------------------------------------------------------------------------
# Installation
# ---------------------------------------------------------------------------
include(GNUInstallDirs)

install(TARGETS rtsrv RUNTIME DESTINATION "${CMAKE_INSTALL_SBINDIR}")

install(
    FILES "${CMAKE_CURRENT_SOURCE_DIR}/config/rtsrv.json"
    DESTINATION "${CMAKE_INSTALL_SYSCONFDIR}/rtsrv"
)

install(
    FILES "${CMAKE_CURRENT_SOURCE_DIR}/systemd/rtsrv.service"
    DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/systemd/system"
)

# ---------------------------------------------------------------------------
# Doxygen documentation (optional)
# ---------------------------------------------------------------------------
find_package(Doxygen OPTIONAL_COMPONENTS dot)
if(DOXYGEN_FOUND)
    set(DOXYGEN_PROJECT_NAME "rtsrv")
    set(DOXYGEN_PROJECT_BRIEF "Real-Time Service Daemon")
    set(DOXYGEN_PROJECT_VERSION "${PROJECT_VERSION}")
    set(DOXYGEN_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/docs")
    set(DOXYGEN_GENERATE_HTML YES)
    set(DOXYGEN_GENERATE_MAN NO)
    set(DOXYGEN_EXTRACT_ALL YES)
    set(DOXYGEN_RECURSIVE YES)
    set(DOXYGEN_USE_MDFILE_AS_MAINPAGE "README.md")

    doxygen_add_docs(
        docs
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/proto"
        COMMENT "Generating Doxygen documentation"
    )
endif()

# ---------------------------------------------------------------------------
# Git hook installation target
# ---------------------------------------------------------------------------
add_custom_target(
    install-hooks
    COMMAND
        "${CMAKE_CURRENT_SOURCE_DIR}/hooks/install-hooks.sh"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Installing git hooks from hooks/"
    VERBATIM
)

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
message(STATUS "")
message(STATUS "=== rtsrv build configuration ===")
message(STATUS "  Version       : ${PROJECT_VERSION}")
message(STATUS "  Build type    : ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ standard  : ${CMAKE_CXX_STANDARD}")
message(STATUS "  Install prefix: ${CMAKE_INSTALL_PREFIX}")
message(STATUS "")
