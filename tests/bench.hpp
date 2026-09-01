// bench.hpp — tiny portable micro-benchmark helpers for the challenge autograders.
// Reports ns/op via steady_clock (portable on x86 CI and arm64 laptops). Students
// use __rdtsc in their own writeups; the grader reports ns/op as the machine-
// relative metric.
#pragma once
#include <chrono>
#include <utility>

template <class T>
inline void doNotOptimize(T&& v) {
    asm volatile("" : : "g"(v) : "memory");
}
inline void clobber() { asm volatile("" : : : "memory"); }

template <class F>
double ns_per_op(F&& f, long iters) {
    for (long i = 0; i < iters / 10 + 1; ++i) f();          // warm up
    auto t0 = std::chrono::steady_clock::now();
    for (long i = 0; i < iters; ++i) f();
    auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::nano>(t1 - t0).count() / iters;
}
