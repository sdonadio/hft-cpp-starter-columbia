// shm_ring_test.cpp — Project Phase 4. Contract: shm_ring.hpp defines a POD ring
// that lives entirely inside a shared-memory region (no internal pointers), usable
// across processes:
//   struct ShmRing {
//     static constexpr uint32_t CAPACITY = <power of two>;
//     void init();                 // called once by the creator, before fork
//     bool push(uint64_t v);       // producer; false if full
//     bool pop(uint64_t& out);     // consumer; false if empty
//   };
// This driver mmaps a MAP_SHARED region, forks, and runs producer (child) /
// consumer (parent) through the student's ring — a real cross-process test.
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include "shm_ring.hpp"

static void R(const char* k, bool ok, const std::string& m) {
    std::cout << "RESULT|" << k << "|" << (ok ? "pass" : "fail") << "|" << m << "\n";
}

int main() {
    void* region = mmap(nullptr, sizeof(ShmRing), PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (region == MAP_FAILED) { R("shm_spsc", false, "mmap failed"); return 0; }
    ShmRing* r = reinterpret_cast<ShmRing*>(region);
    r->init();

    const uint64_t N = 500000;
    const auto DL = std::chrono::steady_clock::now() + std::chrono::seconds(5);  // bailout
    pid_t pid = fork();
    if (pid == 0) {                                   // child = producer
        for (uint64_t i = 0; i < N; ++i) {
            long spins = 0;
            while (!r->push(i)) { if ((++spins & 0x3FFF) == 0 && std::chrono::steady_clock::now() > DL) _exit(2); }
        }
        _exit(0);
    }
    bool ok = true;
    auto t0 = std::chrono::steady_clock::now();
    for (uint64_t i = 0; i < N; ++i) {                // parent = consumer
        uint64_t v = 0; long spins = 0;
        while (!r->pop(v)) { if ((++spins & 0x3FFF) == 0 && std::chrono::steady_clock::now() > DL) { ok = false; break; } }
        if (!ok || v != i) { ok = false; break; }     // FIFO / loss / dup across processes
    }
    auto t1 = std::chrono::steady_clock::now();
    int st = 0; waitpid(pid, &st, 0);
    ok = ok && WIFEXITED(st) && WEXITSTATUS(st) == 0;
    double secs = std::chrono::duration<double>(t1 - t0).count();
    R("shm_spsc", ok, ok ? "moved " + std::to_string(N) + " items across processes, FIFO"
                         : "cross-process ring lost/reordered items or child failed");
    if (secs > 0) std::cout << "METRIC|shm_ops_per_sec|" << (long long)(N / secs) << "\n";
    munmap(region, sizeof(ShmRing));
    return 0;
}
