##
# @file test/CMakeLists.txt
# @brief Builds the logging-module thread-safety stress test.
##

add_executable(log_thread_test log_thread_test.cpp)

target_link_libraries(
    log_thread_test
    PRIVATE
        common_lib
        Threads::Threads
)
