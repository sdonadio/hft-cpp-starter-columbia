// u64toa_test.cpp — HW12. Contract: u64toa.hpp defines
//   int u64toa(uint64_t v, char* out);   // writes decimal digits, returns length
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include "u64toa.hpp"
#include "bench.hpp"

static void R(const char* k, bool ok, const std::string& m) {
    std::cout << "RESULT|" << k << "|" << (ok ? "pass" : "fail") << "|" << m << "\n";
}
static bool check(uint64_t v) {
    char buf[32]; int n = u64toa(v, buf); buf[n] = 0;
    std::string want = std::to_string(v);
    return want.size() == (size_t)n && want == buf;
}

int main() {
    {
        bool ok = check(0) && check(7) && check(12345) && check(1000000)
               && check(9999999999ULL) && check(UINT64_MAX);
        R("u64toa_edges", ok, ok ? "0, small, 10-digit, and UINT64_MAX correct"
                                 : "wrong output on an edge case");
    }
    {
        bool ok = true; uint64_t x = 88172645463325252ULL;  // xorshift, no RNG needed
        for (int i = 0; i < 200000 && ok; ++i) {
            x ^= x << 13; x ^= x >> 7; x ^= x << 17;
            ok = check(x);
        }
        R("u64toa_fuzz", ok, ok ? "200k pseudo-random values match std::to_string"
                                : "mismatch vs std::to_string");
    }
    {
        char buf[32]; uint64_t x = 123456789012345ULL;
        double ns = ns_per_op([&] { int n = u64toa(x, buf); doNotOptimize(n); doNotOptimize(buf[0]); }, 3000000);
        double ns_std = ns_per_op([&] { auto s = std::to_string(x); doNotOptimize(s[0]); }, 3000000);
        std::cout << "METRIC|u64toa_ns_per_op|" << ns << "\n";
        std::cout << "METRIC|std_to_string_ns_per_op|" << ns_std << "\n";
    }
    return 0;
}
