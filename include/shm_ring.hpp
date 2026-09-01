#pragma once
#include <atomic>
#include <cstdint>
// Project Phase 4 — POD ring living entirely in a shared-memory region (NO pointers),
// usable across processes. init() is called once by the creator before fork().
struct ShmRing {
    static constexpr uint32_t CAPACITY = 1024;   // power of two
    void init() {
        // TODO(student): initialize head/tail (atomics) to 0.
    }
    bool push(uint64_t v) { (void)v; return false;   // TODO(student): producer; false if full
    }
    bool pop(uint64_t& out) { (void)out; return false; // TODO(student): consumer; false if empty
    }
    std::atomic<uint32_t> head, tail; uint64_t buf[CAPACITY];
};
