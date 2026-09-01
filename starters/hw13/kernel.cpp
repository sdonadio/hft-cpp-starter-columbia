// kernel.cpp — HW13 "Build Optimization (flags only)".
//
//  >>> DO NOT MODIFY THIS FILE. <<<
//  Your job is to find the COMPILER FLAGS that make it run fastest.
//  Baseline:   g++ -O0 kernel.cpp -o slow
//  Your build: g++ <YOUR FLAGS> kernel.cpp -o fast
//  Compare the printed time_ms of ./fast against ./slow and report the speedup,
//  and use `perf stat` to explain WHY each winning flag helped.
//
// The kernel is a floating-point reduction with a data-dependent branch — it
// responds well to -O2/-O3, -march=native (SIMD), -funroll-loops, -flto, and
// profile-guided optimization (PGO) on the branch. Try them and measure.

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

static double kernel(const std::vector<float>& a, const std::vector<float>& b) {
    double acc = 0.0;
    for (int iter = 0; iter < 3000; ++iter) {
        for (std::size_t i = 0; i < a.size(); ++i) {
            float x = a[i] * b[i] + 0.5f * a[i];
            if (x > 0.0f) acc += std::sqrt(x);   // branch a profile can predict
            else          acc -= x;
        }
    }
    return acc;
}

int main() {
    const std::size_t N = 1u << 16;
    std::vector<float> a(N), b(N);
    for (std::size_t i = 0; i < N; ++i) {
        a[i] = float((i * 2654435761u) % 1000) / 500.0f - 1.0f;
        b[i] = float((i * 40503u) % 1000) / 500.0f;
    }
    auto t0 = std::chrono::steady_clock::now();
    double r = kernel(a, b);
    auto t1 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::printf("checksum=%.6f time_ms=%.3f\n", r, ms);
    return 0;
}
