/**
 * @file test/log_thread_test.cpp
 * @brief Multi-thread stress test for the RFC 5424 logging module.
 *
 * Spawns N_THREADS writer threads, each emitting N_MSGS log records to a
 * temporary file sink.  A separate "reinit" thread repeatedly calls
 * rtsrv::log::init() to verify that concurrent reconfiguration is safe.
 *
 * The test is intentionally free of external test frameworks so it can be
 * run without additional dependencies.  A non-zero exit code signals failure.
 */

#include "common/log.hpp"

#include <atomic>
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <format>
#include <string>
#include <thread>
#include <vector>

namespace
{

constexpr int N_THREADS = 8;
constexpr int N_MSGS = 500;

rtsrv::log::Config make_cfg(const std::string& path)
{
    rtsrv::log::Config cfg;
    cfg.app_name = "log_thread_test";
    cfg.facility = rtsrv::log::Facility::Daemon;
    cfg.min_severity = rtsrv::log::Severity::Debug;
    cfg.file.enabled = true;
    cfg.file.path = path;
    cfg.file.human_readable = false;
    return cfg;
}

} // anonymous namespace

int main()
{
    const std::string log_path = "/tmp/log_thread_test_XXXXXX.log";

    // Use mkstemp-style unique path to avoid collisions in parallel test runs.
    std::string unique_path =
        std::format("/tmp/log_thread_test_{}.log",
                    std::hash<std::thread::id>{}(std::this_thread::get_id()));

    // --- Phase 1: Concurrent writers -----------------------------------------
    {
        auto r = rtsrv::log::init(make_cfg(unique_path));
        if (!r)
        {
            std::fprintf(stderr, "init failed: %s\n", r.error().c_str());
            return 1;
        }

        std::atomic<int> ready_count{0};
        std::atomic<bool> go{false};

        auto writer = [&](int id) {
            ++ready_count;
            while (!go.load(std::memory_order_acquire))
                ; // spin until all threads are ready

            for (int i = 0; i < N_MSGS; ++i)
            {
                auto msg = std::format("thread {} message {}", id, i);
                rtsrv::log::info(msg, "TEST");

                // Exercise every severity level.
                switch (i % 8)
                {
                case 0:
                    rtsrv::log::dbg(msg);
                    break;
                case 1:
                    rtsrv::log::info(msg);
                    break;
                case 2:
                    rtsrv::log::notice(msg);
                    break;
                case 3:
                    rtsrv::log::warn(msg);
                    break;
                case 4:
                    rtsrv::log::err(msg);
                    break;
                case 5:
                    rtsrv::log::crit(msg);
                    break;
                case 6:
                    rtsrv::log::alert(msg);
                    break;
                case 7:
                    rtsrv::log::emerg(msg);
                    break;
                default:
                    break;
                }

                // Exercise is_enabled() from multiple threads.
                (void)rtsrv::log::is_enabled(rtsrv::log::Severity::Debug);
                (void)rtsrv::log::min_severity();
            }
        };

        std::vector<std::thread> threads;
        threads.reserve(N_THREADS);
        for (int i = 0; i < N_THREADS; ++i)
            threads.emplace_back(writer, i);

        // Wait for all threads to be ready, then release them simultaneously.
        while (ready_count.load(std::memory_order_acquire) < N_THREADS)
            std::this_thread::yield();
        go.store(true, std::memory_order_release);

        for (auto& t : threads)
            t.join();

        rtsrv::log::shutdown();
    }

    // --- Phase 2: Concurrent init / log / shutdown ---------------------------
    {
        std::atomic<bool> stop{false};

        // Reinit thread: repeatedly reconfigures the logger.
        std::thread reinit_thread([&] {
            int i = 0;
            while (!stop.load(std::memory_order_relaxed))
            {
                (void)rtsrv::log::init(make_cfg(unique_path));
                rtsrv::log::info(std::format("reinit #{}", i++), "REINIT");
                rtsrv::log::shutdown();
            }
        });

        // Writers race against the reinit thread.
        auto racer = [&](int id) {
            for (int i = 0; i < N_MSGS; ++i)
            {
                rtsrv::log::info(std::format("racer {} msg {}", id, i), "RACE");
                (void)rtsrv::log::is_enabled(rtsrv::log::Severity::Info);
            }
        };

        std::vector<std::thread> racers;
        racers.reserve(N_THREADS);
        for (int i = 0; i < N_THREADS; ++i)
            racers.emplace_back(racer, i);

        for (auto& t : racers)
            t.join();

        stop.store(true, std::memory_order_relaxed);
        reinit_thread.join();
        rtsrv::log::shutdown();
    }

    // --- Phase 3: format_message() is lock-free and always safe --------------
    {
        std::atomic<bool> stop2{false};

        auto fmt_thread = [&] {
            while (!stop2.load(std::memory_order_relaxed))
            {
                auto s =
                    rtsrv::log::format_message(rtsrv::log::Facility::Daemon,
                                               rtsrv::log::Severity::Info,
                                               "host",
                                               "app",
                                               42,
                                               "MSGID",
                                               {},
                                               "hello");
                assert(!s.empty());
            }
        };

        std::vector<std::thread> fmts;
        fmts.reserve(4);
        for (int i = 0; i < 4; ++i)
            fmts.emplace_back(fmt_thread);

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        stop2.store(true, std::memory_order_relaxed);
        for (auto& t : fmts)
            t.join();
    }

    // Clean up the temporary log file.
    std::error_code ec;
    std::filesystem::remove(unique_path, ec);

    std::puts("log_thread_test PASSED");
    return 0;
}
