// tail.cpp — HW14 "Profile and kill a latency tail".
//
// This program processes 2,000,000 synthetic ticks and prints the per-tick
// latency distribution (p50 / p99 / p99.9 / max) in nanoseconds. Its median is
// fine, but the TAIL is ugly — there is (at least) one avoidable pathology on
// the hot path that blows up p99.9.
//
// Your job: PROFILE it (perf / a flame graph), find what spikes the tail, and
// FIX it in a copy (tail_fixed.cpp). Report p50/p99/p99.9 before vs after and a
// one-page changelog explaining the cause. Do NOT just crank compiler flags —
// find the algorithmic/systems cause.
//
// Build:  g++ -O2 -std=c++17 tail.cpp -o tail   &&   ./tail

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using clk = std::chrono::steady_clock;

// A "signal" computed per tick. The intent is a rolling average of the last
// window prices — but it recomputes from a freshly ALLOCATED buffer every tick
// (heap traffic + O(window) work on the hot path). That is the tail.
static double signal(const std::vector<double>& prices, std::size_t i, std::size_t window) {
    std::size_t lo = i >= window ? i - window : 0;
    std::vector<double> scratch;                 // <-- allocates every single tick
    for (std::size_t k = lo; k <= i; ++k) scratch.push_back(prices[k]);
    double s = 0.0;
    for (double v : scratch) s += v;
    return s / scratch.size();
}

int main() {
    const std::size_t N = 2000000, WINDOW = 256;
    std::vector<double> prices(N);
    uint64_t x = 88172645463325252ULL;
    for (std::size_t i = 0; i < N; ++i) {         // deterministic pseudo-random walk
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        prices[i] = 100.0 + (double)(x % 1000) * 0.001;
    }

    std::vector<double> lat_ns; lat_ns.reserve(N);
    double sink = 0.0;
    for (std::size_t i = 0; i < N; ++i) {
        auto t0 = clk::now();
        sink += signal(prices, i, WINDOW);        // the hot path we measure
        auto t1 = clk::now();
        lat_ns.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count());
    }

    std::sort(lat_ns.begin(), lat_ns.end());
    auto pct = [&](double p) { return lat_ns[(std::size_t)(p * (N - 1))]; };
    std::printf("p50=%.0fns  p99=%.0fns  p99.9=%.0fns  max=%.0fns  (sink=%.3f)\n",
                pct(0.50), pct(0.99), pct(0.999), lat_ns.back(), sink);
    return 0;
}
