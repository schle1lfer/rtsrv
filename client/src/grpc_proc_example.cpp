/**
 * @file client/src/grpc_proc_example.cpp
 * @brief Usage example for the GrpcProc asynchronous processing thread.
 *
 * This standalone program demonstrates three usage patterns:
 *
 *  1. **Fire-and-collect** – submit several requests from the main thread,
 *     then collect all responses after the fact.
 *
 *  2. **Multi-threaded submission** – multiple threads each submit a request
 *     and wait for their own response concurrently.
 *
 *  3. **Non-blocking poll** – submit a request then spin-poll with
 *     @ref sra::GrpcProc::tryGetResponse until the answer arrives.
 *
 * Build and run (assuming srmd is listening on 127.0.0.1:50051):
 * @code
 *   cmake --build build --target grpc_proc_example
 *   ./build/client/grpc_proc_example [host:port]
 * @endcode
 *
 * @version 1.0
 */

#include "client/grpc_proc.hpp"
#include "client/route_client.hpp"

#include <atomic>
#include <chrono>
#include <format>
#include <iostream>
#include <print>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

// ---------------------------------------------------------------------------
// Helper: print a GrpcResponse to stdout
// ---------------------------------------------------------------------------

/**
 * @brief Formats and prints a @ref sra::GrpcResponse to stdout.
 *
 * Uses @c std::visit to handle every possible @ref sra::ResponsePayload
 * alternative and prints the result or the error string.
 *
 * @param resp  The response to display.
 */
static void printResponse(const sra::GrpcResponse& resp)
{
    std::print("  [id={}] ", resp.id);

    std::visit(
        [](const auto& result) {
            using T = std::decay_t<decltype(result)>;

            if constexpr (std::is_same_v<T, sra::EchoResult>)
            {
                if (result)
                {
                    std::println("Echo OK  → \"{}\"", result->message());
                }
                else
                {
                    std::println("Echo ERR → {}", result.error());
                }
            }
            else if constexpr (std::is_same_v<T, sra::HeartbeatResult>)
            {
                if (result)
                {
                    std::println("Heartbeat OK  → seq={}", result->sequence());
                }
                else
                {
                    std::println("Heartbeat ERR → {}", result.error());
                }
            }
            else if constexpr (std::is_same_v<T, sra::AddRouteResult>)
            {
                if (result)
                {
                    std::println("AddRoute OK  → id={} dest={}",
                                 result->id(),
                                 result->destination());
                }
                else
                {
                    std::println("AddRoute ERR → {}", result.error());
                }
            }
            else if constexpr (std::is_same_v<T, sra::RemoveRouteResult>)
            {
                if (result)
                {
                    std::println("RemoveRoute OK");
                }
                else
                {
                    std::println("RemoveRoute ERR → {}", result.error());
                }
            }
            else if constexpr (std::is_same_v<T, sra::GetRouteResult>)
            {
                if (result)
                {
                    std::println("GetRoute OK  → dest={}",
                                 result->destination());
                }
                else
                {
                    std::println("GetRoute ERR → {}", result.error());
                }
            }
            else if constexpr (std::is_same_v<T, sra::ListRoutesResult>)
            {
                if (result)
                {
                    std::println("ListRoutes OK  → {} route(s)",
                                 result->size());
                }
                else
                {
                    std::println("ListRoutes ERR → {}", result.error());
                }
            }
            else if constexpr (std::is_same_v<T, sra::SetLoopbackResult>)
            {
                if (result)
                {
                    std::println("SetLoopback OK  → \"{}\"", *result);
                }
                else
                {
                    std::println("SetLoopback ERR → {}", result.error());
                }
            }
            else if constexpr (std::is_same_v<T, sra::GetLoopbackResult>)
            {
                if (result)
                {
                    std::println("GetLoopback OK  → \"{}\"", *result);
                }
                else
                {
                    std::println("GetLoopback ERR → {}", result.error());
                }
            }
        },
        resp.payload);
}

// ---------------------------------------------------------------------------
// Example 1 – fire-and-collect
// ---------------------------------------------------------------------------

/**
 * @brief Demonstrates submitting multiple requests from one thread and
 *        collecting all responses after the fact.
 *
 * All requests are placed on the queue before any response is awaited,
 * showing that the grpc_proc thread processes them in submission order while
 * the caller is free to do other work.
 *
 * @param proc  Running @ref sra::GrpcProc instance to use.
 */
static void exampleFireAndCollect(sra::GrpcProc& proc)
{
    std::println("\n=== Example 1: fire-and-collect ===");

    // Submit requests without waiting for them
    const uint64_t idEcho =
        proc.submit(sra::EchoParams{"hello from grpc_proc"});
    const uint64_t idHb = proc.submit(sra::HeartbeatParams{42});
    const uint64_t idList = proc.submit(sra::ListRoutesParams{false});
    const uint64_t idAdd =
        proc.submit(sra::AddRouteParams{.destination = "192.168.100.0/24",
                                        .gateway = "10.0.0.1",
                                        .interfaceName = "eth0",
                                        .metric = 10});

    std::println("  Submitted 4 requests (ids {}, {}, {}, {}), now collecting…",
                 idEcho,
                 idHb,
                 idList,
                 idAdd);

    // Collect responses – order of retrieval is independent of submission order
    for (const uint64_t id : {idEcho, idHb, idList, idAdd})
    {
        auto resp = proc.waitForResponse(id, 15s);
        if (resp)
        {
            printResponse(*resp);
        }
        else
        {
            std::println("  [id={}] timed out!", id);
        }
    }
}

// ---------------------------------------------------------------------------
// Example 2 – multi-threaded submission
// ---------------------------------------------------------------------------

/**
 * @brief Demonstrates multiple threads each submitting one request and waiting
 *        for their own response.
 *
 * A pool of worker threads is created; each submits an Echo and blocks on
 * @ref sra::GrpcProc::waitForResponse.  The grpc_proc thread serialises
 * the RPCs while all callers wait concurrently.
 *
 * @param proc       Running @ref sra::GrpcProc instance to use.
 * @param numThreads Number of concurrent caller threads to spawn.
 */
static void exampleMultiThreaded(sra::GrpcProc& proc, int numThreads = 4)
{
    std::println("\n=== Example 2: multi-threaded submission ({} threads) ===",
                 numThreads);

    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(numThreads));

    for (int i = 0; i < numThreads; ++i)
    {
        workers.emplace_back([&proc, i] {
            // Each thread submits its own request
            const uint64_t id =
                proc.submit(sra::EchoParams{std::format("thread-{}", i)});

            // Each thread waits for its own response
            auto resp = proc.waitForResponse(id, 15s);
            if (resp)
            {
                printResponse(*resp);
            }
            else
            {
                std::println("  thread-{} [id={}] timed out!", i, id);
            }
        });
    }

    for (auto& t : workers)
    {
        t.join();
    }
}

// ---------------------------------------------------------------------------
// Example 3 – non-blocking poll
// ---------------------------------------------------------------------------

/**
 * @brief Demonstrates non-blocking response polling with
 *        @ref sra::GrpcProc::tryGetResponse.
 *
 * A ListRoutes request is submitted, and the calling thread repeatedly calls
 * @c tryGetResponse in a tight loop (with a small sleep to be friendly to the
 * scheduler) until the response appears.
 *
 * @param proc  Running @ref sra::GrpcProc instance to use.
 */
static void exampleNonBlockingPoll(sra::GrpcProc& proc)
{
    std::println("\n=== Example 3: non-blocking poll ===");

    const uint64_t id = proc.submit(sra::ListRoutesParams{true});
    std::println("  Submitted ListRoutes(activeOnly=true) as id={}", id);

    int polls = 0;
    while (true)
    {
        auto resp = proc.tryGetResponse(id);
        if (resp)
        {
            std::println("  Response arrived after {} poll(s):", polls);
            printResponse(*resp);
            break;
        }
        ++polls;
        std::this_thread::sleep_for(5ms);
    }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

/**
 * @brief Entry point for the grpc_proc usage example.
 *
 * Accepts an optional server address argument (default: "127.0.0.1:50051"),
 * creates a @ref sra::RouteClient, wraps it in a @ref sra::GrpcProc, then
 * runs the three example scenarios in sequence.
 *
 * @param argc  Argument count.
 * @param argv  Argument vector.  argv[1] may be "host:port".
 * @return 0 on success, non-zero on error.
 */
int main(int argc, char* argv[])
{
    const std::string serverAddr = (argc > 1) ? argv[1] : "127.0.0.1:50051";

    std::println("grpc_proc example – connecting to {}", serverAddr);

    // ── Create the gRPC client ───────────────────────────────────────────────
    sra::RouteClient client(serverAddr,
                            /*useTls=*/false,
                            /*caCert=*/"",
                            /*timeoutSeconds=*/10);

    // ── Create and start the grpc_proc thread ────────────────────────────────
    sra::GrpcProc proc(client, /*autoStart=*/true);

    std::println("grpc_proc thread started.");
    std::println("Pending: {}  Stored responses: {}",
                 proc.pendingCount(),
                 proc.storedResponseCount());

    // ── Run examples ─────────────────────────────────────────────────────────
    exampleFireAndCollect(proc);
    exampleMultiThreaded(proc, 4);
    exampleNonBlockingPoll(proc);

    // ── Graceful shutdown
    // ─────────────────────────────────────────────────────
    std::println("\nStopping grpc_proc thread…");
    proc.stop();
    std::println("Done.");

    return 0;
}
