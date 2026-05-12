##
# @file common/CMakeLists.txt
# @brief Builds the shared configuration and logging library used by srmd and sra.
##

add_library(
    common_lib STATIC
    src/config.cpp
    src/log.cpp
)

target_include_directories(
    common_lib
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(
    common_lib
    PUBLIC
        Boost::json
        Threads::Threads
)

target_compile_definitions(
    common_lib
    PUBLIC
        BOOST_ALL_DYN_LINK
)
