#pragma once
#include <cstdint>
// HW7 — a fast order book (flat, price-indexed) + a fast symbol->id map.
// side 'B'=bid, 'S'=ask.  SymMap::get returns (uint64_t)-1 if absent.
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
