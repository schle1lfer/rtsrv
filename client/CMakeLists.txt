##
# @file client/CMakeLists.txt
# @brief Builds sra – the Switch Route Application (gRPC client).
##

add_executable(
    sra
    src/main.cpp
    src/route_client.cpp
    src/switch_config.cpp
    ${PROTO_SRCS}
    ${GRPC_SRCS}
)

add_dependencies(sra generate_proto increment_build_number)

target_include_directories(
    sra
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${PROTO_GEN_DIR}"
        "${BUILD_INFO_GEN_DIR}"
)

target_link_libraries(
    sra
    PRIVATE
        common_lib
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

# Post-build: create versioned snapshot
add_custom_command(
    TARGET sra
    POST_BUILD
    COMMAND
        "${CMAKE_COMMAND}"
        "-DTARGET_FILE=$<TARGET_FILE:sra>"
        "-DBUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}"
        "-DBINARY_NAME=sra"
        -P "${CMAKE_SOURCE_DIR}/cmake/CreateVersionedBinary.cmake"
    COMMENT "Creating versioned sra binary snapshot"
    VERBATIM
)
