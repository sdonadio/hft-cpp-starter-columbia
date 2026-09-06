#pragma once
#include <cstdint>
// HW8 — sliding-window event counter. count() is called with a non-decreasing clock.
//
// Edge case that costs most people a test (see labs/week08.md step 5): ts_ns and
// now_ns are UNSIGNED. Early on, now_ns <= window_ns and the mathematical cutoff
// (now - window) is negative — there is no uint64_t that means that, and there is
// no safe value to clamp it to. Only compute the subtraction when it is
// meaningful; do not expire anything before then.
struct RollingCounter {
    explicit RollingCounter(uint64_t window_ns) { (void)window_ns;
        // TODO(student): ring/deque of timestamps; amortized O(1).
    }
    void add(uint64_t ts_ns) { (void)ts_ns; /* TODO(student) */ }
    uint64_t count(uint64_t now_ns) { (void)now_ns; return 0; /* TODO: events with ts > now-window */ }
};
