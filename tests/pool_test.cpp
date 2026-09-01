// pool_test.cpp — HW4 "high-performance allocator". Contract: pool.hpp defines
//   struct Pool { Pool(std::size_t obj_size, std::size_t capacity);
//                 void* alloc(); void free(void*); };
#include <cstdint>
#include <cstddef>
#include <iostream>
#include <vector>
#include "pool.hpp"
#include "bench.hpp"

static void R(const char* k, bool ok, const std::string& m) {
    std::cout << "RESULT|" << k << "|" << (ok ? "pass" : "fail") << "|" << m << "\n";
}

int main() {
    // basic: distinct non-null up to capacity
    {
        Pool p(8, 4);
        std::vector<void*> v;
        bool ok = true;
        for (int i = 0; i < 4; ++i) { void* q = p.alloc(); ok = ok && q; v.push_back(q); }
        for (size_t i = 0; i < v.size(); ++i)
            for (size_t j = i + 1; j < v.size(); ++j) ok = ok && v[i] != v[j];
        R("pool_basic", ok, ok ? "4 distinct non-null slots" : "null or duplicate slot");
        for (void* q : v) p.free(q);
    }
    // full: alloc past capacity returns null
    {
        Pool p(8, 3);
        for (int i = 0; i < 3; ++i) p.alloc();
        bool ok = (p.alloc() == nullptr);
        R("pool_full", ok, ok ? "alloc past capacity -> null" : "over-allocated past capacity");
    }
    // reuse: free then alloc reuses a slot
    {
        Pool p(8, 2);
        void* a = p.alloc(); void* b = p.alloc();
        p.free(a);
        void* c = p.alloc();
        bool ok = c != nullptr && (c == a || c == b);
        R("pool_reuse", ok, ok ? "freed slot reused" : "did not reuse freed slot");
        (void)b;
    }
    // pattern: churn without leaking capacity
    {
        Pool p(16, 64);
        std::vector<void*> live;
        bool ok = true;
        for (int round = 0; round < 100000 && ok; ++round) {
            for (int i = 0; i < 64; ++i) { void* q = p.alloc(); if (!q) { ok = false; break; } live.push_back(q); }
            for (void* q : live) p.free(q);
            live.clear();
        }
        R("pool_pattern", ok, ok ? "100k fill/drain rounds OK" : "exhausted early (slots leaked)");
    }
    // metric: ns per alloc+free
    {
        Pool p(16, 8);
        double ns = ns_per_op([&] {
            void* q = p.alloc(); doNotOptimize(q); p.free(q);
        }, 2000000);
        std::cout << "METRIC|pool_ns_per_op|" << ns << "\n";
    }
    return 0;
}
