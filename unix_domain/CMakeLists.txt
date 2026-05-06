##
# @file unix_domain/CMakeLists.txt
# @brief Builds unix_domain_lib — a static library wrapping the unix_domain
#        protocol stack (logger, net, ud_proto, route_proto, cmd_proto, netlink).
##

add_library(
    unix_domain_lib STATIC
    lib/logger.cpp
    lib/net.cpp
    lib/ud_proto.cpp
    lib/route_proto.cpp
    lib/cmd_proto.cpp
    lib/netlink.cpp
)

target_include_directories(
    unix_domain_lib
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}"
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/lib"
)

target_compile_features(unix_domain_lib PUBLIC cxx_std_23)

target_compile_options(
    unix_domain_lib
    PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wcast-align
        -Wnon-virtual-dtor -Wold-style-cast -Woverloaded-virtual
)

# ---------------------------------------------------------------------------
# ud_server – UNIX-domain socket server for route-add commands
# ---------------------------------------------------------------------------

add_executable(
    ud_server
    ud_server.cpp
)

add_dependencies(ud_server increment_build_number)

target_include_directories(
    ud_server
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/lib"
        "${CMAKE_SOURCE_DIR}/common/include"
)

target_link_libraries(
    ud_server
    PRIVATE
        unix_domain_lib
        Threads::Threads
)

target_compile_features(ud_server PRIVATE cxx_std_23)

target_compile_options(
    ud_server
    PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wcast-align
        -Wnon-virtual-dtor -Wold-style-cast -Woverloaded-virtual
)

# ---------------------------------------------------------------------------
# Post-build: create versioned snapshot
add_custom_command(
    TARGET ud_server
    POST_BUILD
    COMMAND
        "${CMAKE_COMMAND}"
        "-DTARGET_FILE=$<TARGET_FILE:ud_server>"
        "-DBUILD_DIR=${CMAKE_BINARY_DIR}"
        "-DBINARY_NAME=ud_server"
        -P "${CMAKE_SOURCE_DIR}/cmake/CreateVersionedBinary.cmake"
    COMMENT "Creating versioned ud_server binary snapshot"
    VERBATIM
)
