// bench.hpp — tiny portable micro-benchmark helpers for the challenge autograders.
// Reports ns/op via steady_clock (portable on x86 CI and arm64 laptops), plus a
// portable tick counter for "cycles/op" writeups. The grader reports ns/op as
// the machine-relative metric.
#pragma once
#include <chrono>
#include <cstdint>
#include <utility>

#if defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#endif

template <class T>
inline void doNotOptimize(T&& v) {
    asm volatile("" : : "g"(v) : "memory");
}
inline void clobber() { asm volatile("" : : : "memory"); }

// ---------------------------------------------------------------------------
// cycle_count() — a monotonically increasing hardware tick counter.
//
//   x86 / x86-64 : __rdtsc(). The invariant TSC: constant reference rate,
//                  ~1 tick per nominal core cycle. This is the counter HW 4
//                  used to ask for, and it only exists here.
//   arm64        : cntvct_el0, the architectural *virtual timer*, read with `mrs`.
//                  IMPORTANT: this is a CONSTANT-RATE system timer, NOT a core
//                  cycle counter. It does not scale with CPU frequency, so a
//                  "cycle" here is a fixed unit of time, and its rate varies by
//                  part: cntfrq_el0 measures 1 GHz on an Apple M4 (1 tick = 1 ns)
//                  and 24 MHz on M1/M2-era Apple Silicon (1 tick = ~41 ns, far too
//                  coarse for one operation). Never assume the rate — divide by
//                  cycle_count_hz(). Reading it costs ~0.6 ns. A true per-core
//                  cycle counter (the PMU) needs root on macOS.
//   anything else: steady_clock nanoseconds, so the helper still compiles and
//                  the number still means "time per op".
//
// Divide by cycle_count_hz() to convert ticks to seconds on any platform.
// ---------------------------------------------------------------------------
inline std::uint64_t cycle_count() {
#if defined(__x86_64__) || defined(__i386__)
    return static_cast<std::uint64_t>(__rdtsc());
#elif defined(__aarch64__)
    std::uint64_t t;
    asm volatile("mrs %0, cntvct_el0" : "=r"(t));
    return t;
#else
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
}

// Ticks per second for cycle_count(). On x86 the TSC rate is not architecturally
// discoverable, so this returns 0 there: calibrate it yourself against
// steady_clock (time a fixed sleep/spin and divide), or report ns/op.
inline std::uint64_t cycle_count_hz() {
#if defined(__x86_64__) || defined(__i386__)
    return 0;                        // unknown — calibrate against steady_clock
#elif defined(__aarch64__)
    std::uint64_t f;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(f));
    return f;                        // 1'000'000'000 on an M4, 24'000'000 on an M1
#else
    return 1'000'000'000ull;         // steady_clock nanoseconds
#endif
}

template <class F>
double ns_per_op(F&& f, long iters) {
    for (long i = 0; i < iters / 10 + 1; ++i) f();          // warm up
    auto t0 = std::chrono::steady_clock::now();
    for (long i = 0; i < iters; ++i) f();
    auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::nano>(t1 - t0).count() / iters;
}

// Ticks per op from cycle_count(). Meaningful on x86 (TSC ~ nominal cycles); on
// arm64 these are constant-rate timer ticks (see above), so divide by
// cycle_count_hz() if you want seconds — or just report ns_per_op(), which is
// what the grader records.
template <class F>
double cycles_per_op(F&& f, long iters) {
    for (long i = 0; i < iters / 10 + 1; ++i) f();          // warm up
    auto c0 = cycle_count();
    for (long i = 0; i < iters; ++i) f();
    auto c1 = cycle_count();
    return static_cast<double>(c1 - c0) / static_cast<double>(iters);
}
