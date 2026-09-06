#pragma once
#include <cstdint>
// HW7 — a fast order book (flat, price-indexed) + a fast symbol->id map.
// side 'B'=bid, 'S'=ask.  SymMap::get returns (uint64_t)-1 if absent.
//
// Range warning (see labs/week07.md step 2): a flat array of N one-cent slots
// is a BAND, not "all prices". 1<<16 slots indexed absolutely from $0.00 covers
// only $0.00-$655.35, and the arena lists NFLX near $720 and META near $580 —
// an absolute index walks off the end and, because the two side arrays are
// adjacent members, silently corrupts the OTHER side of your own book. ASan
// cannot see that (it is an intra-object overflow) and these tests only use
// prices near $100, so CI will not catch it for you. Index against a base tick
// (slot = tick - base_tick_) and bounds-check both ends.
struct Book {
    void add(uint64_t id, char side, double px, uint32_t qty) { (void)id;(void)side;(void)px;(void)qty;
        // TODO(student)
    }
    void cancel(uint64_t id) { (void)id; /* TODO(student) */ }
    double best_bid() const { return 0.0;   /* TODO(student): O(1) */ }
    double best_ask() const { return 0.0;   /* TODO(student): O(1) */ }
};
struct SymMap {
    void put(const char* sym, uint64_t id) { (void)sym;(void)id; /* TODO(student) */ }
    uint64_t get(const char* sym) const { (void)sym; return (uint64_t)-1; /* TODO(student) */ }
};
