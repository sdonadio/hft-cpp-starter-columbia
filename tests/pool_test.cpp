// pool_test.cpp — HW4 "high-performance allocator". Contract: pool.hpp defines
//   struct Pool { Pool(std::size_t obj_size, std::size_t capacity);
//                 void* alloc(); void free(void*); };
//
// Every check below requires alloc() to hand back REAL, DISTINCT, WRITABLE
// storage. A stub that just returns nullptr fails all four — including
// pool_full, which used to pass by accident.
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
#include "pool.hpp"
#include "bench.hpp"

static void R(const char* k, bool ok, const std::string& m) {
    std::cout << "RESULT|" << k << "|" << (ok ? "pass" : "fail") << "|" << m << "\n";
}

int main() {
    // basic: distinct, non-null, non-overlapping, writable slots up to capacity
    {
        Pool p(8, 4);
        std::vector<void*> v;
        bool nonnull = true, distinct = true;
        for (int i = 0; i < 4; ++i) { void* q = p.alloc(); nonnull = nonnull && q != nullptr; v.push_back(q); }
        for (std::size_t i = 0; i < v.size(); ++i)
            for (std::size_t j = i + 1; j < v.size(); ++j) distinct = distinct && v[i] != v[j];
        // slots must not overlap: stamp each one, then re-read them all
        bool writable = nonnull;
        if (nonnull) {
            for (std::size_t i = 0; i < v.size(); ++i) {
                std::uint64_t tag = 0xA5A5'0000ull + i;
                std::memcpy(v[i], &tag, sizeof tag);
            }
            for (std::size_t i = 0; i < v.size(); ++i) {
                std::uint64_t got = 0;
                std::memcpy(&got, v[i], sizeof got);
                writable = writable && got == 0xA5A5'0000ull + i;
            }
        }
        bool ok = nonnull && distinct && writable;
        R("pool_basic", ok,
          ok         ? "4 distinct, writable, non-overlapping slots"
          : !nonnull ? "alloc() returned nullptr before capacity was reached"
          : !distinct? "alloc() handed out the same slot twice"
                     : "slots overlap (a write to one clobbered another)");
        for (void* q : v) p.free(q);
    }
    // full: fill to capacity with real slots, THEN alloc must return null
    {
        Pool p(8, 3);
        void* v[3] = {nullptr, nullptr, nullptr};
        bool filled = true;
        for (int i = 0; i < 3; ++i) { v[i] = p.alloc(); filled = filled && v[i] != nullptr; }
        for (int i = 0; i < 3; ++i)
            for (int j = i + 1; j < 3; ++j) filled = filled && v[i] != v[j];
        bool exhausted = (p.alloc() == nullptr);
        bool ok = filled && exhausted;
        R("pool_full", ok,
          ok       ? "3 distinct slots, then alloc past capacity -> null"
          : !filled? "alloc() returned nullptr/duplicate before capacity was reached"
                   : "over-allocated past capacity");
        for (int i = 0; i < 3; ++i) p.free(v[i]);
    }
    // reuse: with the pool full, freeing one slot means the next alloc IS that slot
    {
        Pool p(8, 2);
        void* a = p.alloc();
        void* b = p.alloc();
        bool filled = a && b && a != b;
        p.free(a);
        void* c = p.alloc();
        bool ok = filled && c == a;                  // only 'a' is free, so c must be 'a'
        R("pool_reuse", ok,
          ok       ? "freed slot reused"
          : !filled? "alloc() returned nullptr/duplicate before capacity was reached"
          : c == nullptr ? "no slot returned after free() (freed memory not reclaimed)"
                   : "did not reuse the freed slot");
        if (c) p.free(c);
        if (b) p.free(b);
    }
    // pattern: churn without leaking capacity, and slots stay distinct each round
    {
        Pool p(16, 64);
        std::vector<void*> live;
        bool ok = true;
        const char* why = "100k fill/drain rounds OK";
        for (int round = 0; round < 100000 && ok; ++round) {
            for (int i = 0; i < 64; ++i) {
                void* q = p.alloc();
                if (!q) { ok = false; why = "exhausted early (slots leaked, or alloc() returns nullptr)"; break; }
                live.push_back(q);
            }
            if (ok && round < 4) {                   // spot-check distinctness cheaply
                for (std::size_t i = 0; i < live.size() && ok; ++i)
                    for (std::size_t j = i + 1; j < live.size(); ++j)
                        if (live[i] == live[j]) { ok = false; why = "same slot handed out twice in one round"; break; }
            }
            for (void* q : live) p.free(q);
            live.clear();
        }
        R("pool_pattern", ok, why);
    }
    // metric: ns per alloc+free. Probe OUTSIDE the timing loop so the number
    // carries no extra branch — but say so when the pool isn't real, because an
    // alloc() that returns nullptr always "wins" this benchmark.
    {
        Pool p(16, 8);
        void* probe = p.alloc();
        bool live = probe != nullptr;
        p.free(probe);
        double ns = ns_per_op([&] {
            void* q = p.alloc(); doNotOptimize(q); p.free(q);
        }, 2000000);
        std::cout << "METRIC|pool_ns_per_op|" << ns << "\n";
        if (!live)
            std::cout << "NOTE|pool_ns_per_op is meaningless here: alloc() returned nullptr, "
                         "so this timed nothing. An unimplemented pool always looks fastest.\n";
    }
    return 0;
}
