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
