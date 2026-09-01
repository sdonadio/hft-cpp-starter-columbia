#pragma once
#include <cstdint>
// HW8 — sliding-window event counter. count() is called with a non-decreasing clock.
struct RollingCounter {
    explicit RollingCounter(uint64_t window_ns) { (void)window_ns;
        // TODO(student): ring/deque of timestamps; amortized O(1).
    }
    void add(uint64_t ts_ns) { (void)ts_ns; /* TODO(student) */ }
    uint64_t count(uint64_t now_ns) { (void)now_ns; return 0; /* TODO: events with ts > now-window */ }
};
