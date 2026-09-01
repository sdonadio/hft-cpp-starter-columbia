// correctness.cpp — single-threaded behavioural tests for a student SPSC ring.
// Submission contract (spsc_ring.hpp must provide):
//   struct SPSCRing {
//     explicit SPSCRing(std::size_t capacity_pow2);
//     bool push(std::uint64_t v);   // false if full
//     bool pop(std::uint64_t& out); // false if empty
//     bool empty() const;
//     bool full()  const;
//   };
// Emits lines: RESULT|<key>|pass|fail|<message>
#include <cstdint>
#include <cstddef>
#include <iostream>
#include "spsc_ring.hpp"

static void result(const char* key, bool ok, const std::string& msg) {
    std::cout << "RESULT|" << key << "|" << (ok ? "pass" : "fail")
              << "|" << msg << "\n";
}

int main() {
    // basic push/pop + empty semantics
    {
        SPSCRing r(1024);
        bool ok = r.empty();
        ok = ok && r.push(42);
        ok = ok && !r.empty();
        std::uint64_t x = 0;
        ok = ok && r.pop(x) && x == 42;
        ok = ok && r.empty();
        result("basic", ok, ok ? "push/pop/empty behave" : "basic push/pop/empty wrong");
    }
    // pop on empty returns false
    {
        SPSCRing r(64);
        std::uint64_t x = 123;
        bool ok = !r.pop(x);              // must fail on empty
        result("empty_pop", ok, ok ? "pop on empty returns false"
                                   : "pop on empty did not return false");
    }
    // push until full returns false, then full() is true
    {
        SPSCRing r(64);
        int pushed = 0;
        while (r.push(static_cast<std::uint64_t>(pushed))) {
            if (++pushed > 100000) break;  // safety
        }
        bool ok = pushed > 0 && !r.push(999) && r.full();
        result("full", ok, ok ? ("filled " + std::to_string(pushed) + " then push=false, full()=true")
                              : "push did not stop at capacity / full() wrong");
    }
    // FIFO order over 1000 values (fits in a 1024 ring)
    {
        SPSCRing r(2048);
        bool ok = true;
        for (std::uint64_t i = 0; i < 1000 && ok; ++i) ok = r.push(i);
        for (std::uint64_t i = 0; i < 1000 && ok; ++i) {
            std::uint64_t x = 0;
            ok = r.pop(x) && x == i;
        }
        result("fifo", ok, ok ? "FIFO order preserved over 1000 values"
                              : "values came out of order / lost");
    }
    // wrap-around: 200k single push/pop cycles on a small ring
    {
        SPSCRing r(16);
        bool ok = true;
        for (std::uint64_t i = 0; i < 200000 && ok; ++i) {
            ok = r.push(i);
            std::uint64_t x = 0;
            ok = ok && r.pop(x) && x == i;
        }
        result("wrap", ok, ok ? "wrap-around correct over 200k cycles"
                              : "wrap-around lost/corrupted a value");
    }
    return 0;
}
