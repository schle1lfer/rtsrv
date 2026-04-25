##
# @file client/CMakeLists.txt
# @brief Builds sra – the Switch Route Application (gRPC client).
##

add_executable(
    sra
    src/main.cpp
    src/route_client.cpp
    src/grpc_proc.cpp
    src/routing.cpp
    src/switch_config.cpp
    src/sot_config.cpp
    src/udp_table_server.cpp
    src/sra_udp_client.cpp
    src/vrf_table.cpp
    src/netlink_neigh.c
    src/netlink_nexthop.c
    "${CMAKE_SOURCE_DIR}/server/src/netlink.c"
    ${PROTO_SRCS}
    ${GRPC_SRCS}
)

add_dependencies(sra generate_proto increment_build_number)

target_include_directories(
    sra
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_SOURCE_DIR}/server/include"
        "${CMAKE_SOURCE_DIR}/unix_domain"
        "${PROTO_GEN_DIR}"
        "${BUILD_INFO_GEN_DIR}"
)

target_link_libraries(
    sra
    PRIVATE
        common_lib
        unix_domain_lib
        Threads::Threads
        protobuf::libprotobuf
        gRPC::grpc++
        Boost::json
        Boost::program_options
        Boost::system
)

target_compile_definitions(
    sra
    PRIVATE
        BOOST_ALL_DYN_LINK
)

# ---------------------------------------------------------------------------
# grpc_proc_example – demonstrates the GrpcProc asynchronous request queue
# ---------------------------------------------------------------------------

add_executable(
    grpc_proc_example
    src/grpc_proc_example.cpp
    src/grpc_proc.cpp
    src/route_client.cpp
    ${PROTO_SRCS}
    ${GRPC_SRCS}
)

add_dependencies(grpc_proc_example generate_proto)

target_include_directories(
    grpc_proc_example
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${PROTO_GEN_DIR}"
        "${BUILD_INFO_GEN_DIR}"
)

target_link_libraries(
    grpc_proc_example
    PRIVATE
        common_lib
        Threads::Threads
        protobuf::libprotobuf
        gRPC::grpc++
        Boost::system
)

target_compile_definitions(
    grpc_proc_example
    PRIVATE
        BOOST_ALL_DYN_LINK
)

# ---------------------------------------------------------------------------
# routing_test – smoke-test for RoutingManager (requires CAP_NET_ADMIN / root)
# ---------------------------------------------------------------------------

add_executable(
    routing_test
    src/routing_test.cpp
    src/routing.cpp
)

target_include_directories(
    routing_test
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(
    routing_test
    PRIVATE
        common_lib
        Threads::Threads
)

# No install rule – development / debug tool only.

# ---------------------------------------------------------------------------
# udp_table_server_example – demonstrates UdpTableServer on ports 9001/9002/9003
# ---------------------------------------------------------------------------

add_executable(
    udp_table_server_example
    src/udp_table_server_example.cpp
    src/udp_table_server.cpp
)

add_dependencies(udp_table_server_example generate_proto)

target_include_directories(
    udp_table_server_example
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${PROTO_GEN_DIR}"
        "${BUILD_INFO_GEN_DIR}"
)

target_link_libraries(
    udp_table_server_example
    PRIVATE
        common_lib
        Threads::Threads
)

target_compile_definitions(
    udp_table_server_example
    PRIVATE
        BOOST_ALL_DYN_LINK
)

# No install rule – example / development tool only.

# ---------------------------------------------------------------------------
# Post-build: create versioned snapshot
add_custom_command(
    TARGET sra
    POST_BUILD
    COMMAND
        "${CMAKE_COMMAND}"
        "-DTARGET_FILE=$<TARGET_FILE:sra>"
        "-DBUILD_DIR=${CMAKE_BINARY_DIR}"
        "-DBINARY_NAME=sra"
        -P "${CMAKE_SOURCE_DIR}/cmake/CreateVersionedBinary.cmake"
    COMMENT "Creating versioned sra binary snapshot"
    VERBATIM
)
