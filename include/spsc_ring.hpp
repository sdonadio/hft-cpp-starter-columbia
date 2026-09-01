#pragma once
#include <cstddef>
#include <cstdint>
// HW10 / Project Phase 3 — single-producer/single-consumer lock-free ring buffer.
struct SPSCRing {
    explicit SPSCRing(std::size_t capacity_pow2) { (void)capacity_pow2;
        // TODO(student): allocate a power-of-two buffer; atomic head/tail (acquire/release);
        // pad indices (alignas(64)) to avoid false sharing.
    }
    bool push(std::uint64_t v) { (void)v; return false;   // TODO(student): false if full
    }
    bool pop(std::uint64_t& out) { (void)out; return false; // TODO(student): false if empty
    }
    bool empty() const { return true;  /* TODO(student) */ }
    bool full()  const { return false; /* TODO(student) */ }
};
