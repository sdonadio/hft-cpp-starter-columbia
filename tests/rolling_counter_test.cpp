// rolling_counter_test.cpp — HW8. Contract: rolling_counter.hpp defines
//   struct RollingCounter {
//     explicit RollingCounter(uint64_t window_ns);
//     void add(uint64_t ts_ns);            // record an event
//     uint64_t count(uint64_t now_ns);     // events with ts > now - window_ns
//   };
// count() is called with non-decreasing now_ns (a live clock).
#include <cstdint>
#include <iostream>
#include <string>
#include "rolling_counter.hpp"
#include "bench.hpp"

static void R(const char* k, bool ok, const std::string& m) {
    std::cout << "RESULT|" << k << "|" << (ok ? "pass" : "fail") << "|" << m << "\n";
}

int main() {
    {
        RollingCounter rc(1000);
        for (uint64_t t = 0; t < 1000; ++t) rc.add(t);          // 1000 events, ts 0..999
        bool ok = rc.count(999) == 1000;                        // window not yet full-expired
        // add more, then advance the clock so the first 1000 fall out of the window
        for (uint64_t t = 1000; t < 1500; ++t) rc.add(t);       // now 1500 total
        bool ok2 = rc.count(1999) == 500;                       // only ts in (999,1999] survive
        R("rc_window", ok, ok ? "counts a full window correctly" : "wrong count for a full window");
        R("rc_mixed", ok2, ok2 ? "expires old events as the clock advances"
                                : "did not expire events correctly");
    }
    {
        RollingCounter rc(1000); uint64_t t = 0;
        double ns = ns_per_op([&] { rc.add(t); uint64_t c = rc.count(t); doNotOptimize(c); ++t; }, 2000000);
        std::cout << "METRIC|rolling_ns_per_op|" << ns << "\n";
    }
    return 0;
}
