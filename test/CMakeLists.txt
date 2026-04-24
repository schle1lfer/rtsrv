##
# @file test/CMakeLists.txt
# @brief Builds unix_domain protocol integration tests.
##

# ---------------------------------------------------------------------------
# route_add_single_iface – end-to-end test for INTERFACE_ROUTE (type=2)
# ---------------------------------------------------------------------------

add_executable(
    route_add_single_iface
    route_add_single_iface.cpp
)

target_include_directories(
    route_add_single_iface
    PRIVATE
        "${CMAKE_SOURCE_DIR}/unix_domain"
        "${CMAKE_SOURCE_DIR}/unix_domain/lib"
)

target_link_libraries(
    route_add_single_iface
    PRIVATE
        unix_domain_lib
        Threads::Threads
)

target_compile_features(route_add_single_iface PRIVATE cxx_std_23)

# No install rule – development / integration test only.
