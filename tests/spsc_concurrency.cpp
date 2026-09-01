// concurrency.cpp — one producer, one consumer, cross-thread SPSC stress.
// Compiled TWICE by the grader: once normally (correctness + throughput),
// once with -fsanitize=thread (data-race check).
// N via env SPSC_N (default 2,000,000). Emits RESULT|concurrent|... and prints
// "THROUGHPUT <ops_per_sec>" for reporting.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <thread>
#include "spsc_ring.hpp"

int main() {
    const char* env = std::getenv("SPSC_N");
    const std::uint64_t N = env ? std::strtoull(env, nullptr, 10) : 2000000ULL;
    SPSCRing r(1u << 16);

    std::atomic<bool> failed{false};
    auto t0 = std::chrono::steady_clock::now();
    const auto DL = t0 + std::chrono::seconds(5);   // wall-clock bailout for stuck/stub impls

    std::thread producer([&] {
        for (std::uint64_t i = 0; i < N; ++i) {
            long spins = 0;
            while (!r.push(i)) {
                if ((++spins & 0x3FFF) == 0 && std::chrono::steady_clock::now() > DL) { failed = true; return; }
            }
        }
    });
    std::thread consumer([&] {
        for (std::uint64_t i = 0; i < N; ++i) {
            std::uint64_t x = 0;
            long spins = 0;
            while (!r.pop(x)) {
                if ((++spins & 0x3FFF) == 0 && std::chrono::steady_clock::now() > DL) { failed = true; return; }
            }
            if (x != i) { failed = true; return; }   // FIFO / loss / dup
        }
    });
    producer.join();
    consumer.join();

    auto t1 = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(t1 - t0).count();
    double ops = secs > 0 ? static_cast<double>(N) / secs : 0.0;

    bool ok = !failed.load();
    std::cout << "RESULT|concurrent|" << (ok ? "pass" : "fail") << "|"
              << (ok ? ("moved " + std::to_string(N) + " items in FIFO order across threads")
                     : "lost/reordered items or deadlocked across threads") << "\n";
    std::cout << "THROUGHPUT " << static_cast<long long>(ops) << "\n";
    return ok ? 0 : 1;
}
