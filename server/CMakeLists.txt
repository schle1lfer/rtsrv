##
# @file server/CMakeLists.txt
# @brief Builds srmd – the Switch Route Manager Daemon.
##

add_executable(
    srmd
    src/main.cpp
    src/daemon.cpp
    src/route_manager.cpp
    src/service_impl.cpp
    src/netlink.c
    ${PROTO_SRCS}
    ${GRPC_SRCS}
)

add_dependencies(srmd generate_proto increment_build_number)

target_include_directories(
    srmd
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${PROTO_GEN_DIR}"
        "${BUILD_INFO_GEN_DIR}"
)

target_link_libraries(
    srmd
    PRIVATE
        common_lib
        Threads::Threads
        protobuf::libprotobuf
        gRPC::grpc++
        gRPC::grpc++_reflection
        Boost::log
        Boost::log_setup
        Boost::program_options
        Boost::filesystem
        Boost::system
        Boost::thread
)

target_compile_definitions(
    srmd
    PRIVATE
        BOOST_LOG_DYN_LINK
        BOOST_ALL_DYN_LINK
)

# Post-build: create versioned snapshot
add_custom_command(
    TARGET srmd
    POST_BUILD
    COMMAND
        "${CMAKE_COMMAND}"
        "-DTARGET_FILE=$<TARGET_FILE:srmd>"
        "-DBUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}"
        "-DBINARY_NAME=srmd"
        -P "${CMAKE_SOURCE_DIR}/cmake/CreateVersionedBinary.cmake"
    COMMENT "Creating versioned srmd binary snapshot"
    VERBATIM
)
