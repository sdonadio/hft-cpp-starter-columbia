# Week 7 Lab — Data Structures for HFT (the order book)

**Format:** in-class, guided (~45–60 min).  **Repo:** the HFT starter.

## Goal
Build a **cache-friendly, price-indexed order book** together in
`include/order_book.hpp`: `add`, `cancel`, and **O(1)** `best_bid()` /
`best_ask()`. Then build a **fast open-addressing symbol→id map** (`SymMap`).
By the end you should pass `make book` and understand *why* the flat layout
beats `std::map`. This lab **is** the HW7 challenge — you leave class with it
mostly done.

> Context: the live arena exchange (`shared/orderbook.py`) is a full CLOB with
> **price-time priority** and a `queue_position` per resting order. Today we
> build the *fast local mirror* a C++ bot keeps of that book so it can read the
> BBO on the hot path without asking the exchange. Price-time priority is why
> your FIFO queue position (surfaced by `on_ack(sym,id,queue_ahead,level_qty)`
> in the client) matters — we'll respect that structure here.

## Setup
```bash
cd project-starter
cat include/order_book.hpp     # the stub + the contract in the comments
sed -n '1,45p' tests/book_test.cpp   # read the tests you must pass (do NOT edit)
make book                      # compiles + runs; fails until we implement
```
The contract (from the stub and `book_test.cpp`):
```cpp
struct Book {
    void   add(uint64_t id, char side, double px, uint32_t qty); // 'B'=bid 'S'=ask
    void   cancel(uint64_t id);
    double best_bid() const;   // O(1)
    double best_ask() const;   // O(1)
};
struct SymMap {                // returns (uint64_t)-1 if absent
    void     put(const char* sym, uint64_t id);
    uint64_t get(const char* sym) const;
};
```

## Walk-through — we build this together

### 1. Why not just `std::map<double, Level>`?
A tree map gives you `O(log n)` add/cancel and pointer-chasing to find the best
price — every node is a separate heap allocation, so walking the book thrashes
the cache. In HFT, prices are **not arbitrary doubles**: they live on a fixed
**tick grid** (e.g. \$0.01). That lets us turn "price" into an **array index**
and get O(1) everything with contiguous memory. That is the whole trick.

### 2. Ticks and a price-indexed array
Convert price to an integer tick, then index a flat array of levels:
```cpp
#include <array>
#include <cstdint>

static constexpr double TICK = 0.01;
static constexpr int    NLEV = 1 << 16;   // 65536 price levels, contiguous

static inline int px_to_tick(double px) {
    return static_cast<int>(px / TICK + 0.5);   // round to nearest tick
}
```
`NLEV` levels of `uint64_t` qty is ~512 KB — fits comfortably and every access
is a single indexed load. *Why:* one array, no allocation per order, hardware
prefetcher loves the linear layout.

### 3. The level array + aggregated quantity
Each side is an array indexed by tick; the value is the **total resting qty** at
that price. Adding is just `+= qty`, cancelling is `-= qty`.
```cpp
struct Book {
    std::array<uint32_t, NLEV> bid_qty{};   // qty resting at each bid tick
    std::array<uint32_t, NLEV> ask_qty{};   // qty resting at each ask tick
    int best_bid_tick = -1;                 // highest bid tick with qty>0
    int best_ask_tick = -1;                 // lowest  ask tick with qty>0
    // ...
};
```
We track the *best tick* explicitly so BBO is O(1) — we never scan on the read
path (the hot path). We pay a tiny cost on add/cancel to keep it fresh instead.

### 4. `add` — update qty, then nudge the best pointer
```cpp
void add(uint64_t id, char side, double px, uint32_t qty) {
    int t = px_to_tick(px);
    orders_[id] = {side, t, qty};           // remember for cancel (see step 6)
    if (side == 'B') {
        bid_qty[t] += qty;
        if (t > best_bid_tick) best_bid_tick = t;   // new inside price
    } else {
        ask_qty[t] += qty;
        if (best_ask_tick < 0 || t < best_ask_tick) best_ask_tick = t;
    }
}
```
*Why O(1):* adding at or inside the top only ever *raises* the best pointer —
one comparison, no search.

### 5. `best_bid` / `best_ask` — pure O(1) reads
```cpp
double best_bid() const {
    return best_bid_tick < 0 ? 0.0 : best_bid_tick * TICK;
}
double best_ask() const {
    return best_ask_tick < 0 ? 0.0 : best_ask_tick * TICK;
}
```
This is the payoff: the read your strategy calls thousands of times per tick is
a compare + multiply. No tree walk, no branch mispredict storm.

### 6. `cancel` — the only place we may scan
Cancel removes qty; if it empties the *inside* level, we must find the new best.
Keep an `id → (side, tick, qty)` record so cancel knows what to subtract:
```cpp
struct Rec { char side; int tick; uint32_t qty; };
std::unordered_map<uint64_t, Rec> orders_;   // fine: cancel is off the read path

void cancel(uint64_t id) {
    auto it = orders_.find(id);
    if (it == orders_.end()) return;
    Rec r = it->second; orders_.erase(it);
    if (r.side == 'B') {
        bid_qty[r.tick] -= r.qty;
        if (r.tick == best_bid_tick && bid_qty[r.tick] == 0)
            while (best_bid_tick >= 0 && bid_qty[best_bid_tick] == 0) --best_bid_tick;
    } else {
        ask_qty[r.tick] -= r.qty;
        if (r.tick == best_ask_tick && ask_qty[r.tick] == 0) {
            while (best_ask_tick < NLEV && ask_qty[best_ask_tick] == 0) ++best_ask_tick;
            if (best_ask_tick >= NLEV) best_ask_tick = -1;
        }
    }
}
```
*Why this is still fast:* the scan only runs when the inside level empties, and
it walks *one tick at a time* over contiguous memory — the common case (cancel
away from the top) is a single subtraction.

### 7. Now `SymMap` — open addressing, no per-key allocation
`std::unordered_map<std::string,...>` allocates a node and hashes a string every
lookup. We want a flat table probed linearly. Symbols are `<=16` chars (see the
test: `"BRK.B"`, `"AAPL"`).
```cpp
struct SymMap {
    static constexpr int CAP = 1 << 12;           // power of two -> mask, not %
    struct Slot { char key[16]{}; uint64_t id = (uint64_t)-1; bool used = false; };
    std::array<Slot, CAP> t_{};

    static uint64_t hash(const char* s) {         // FNV-1a
        uint64_t h = 1469598103934665603ull;
        for (; *s; ++s) { h ^= (uint8_t)*s; h *= 1099511628211ull; }
        return h;
    }
    static bool keq(const char* a, const char* b) {
        for (int i = 0; i < 16; ++i) { if (a[i] != b[i]) return false; if (!a[i]) return true; }
        return true;
    }
    void put(const char* sym, uint64_t id) {
        int i = hash(sym) & (CAP - 1);
        for (;; i = (i + 1) & (CAP - 1)) {                // linear probe
            if (!t_[i].used || keq(t_[i].key, sym)) {
                std::snprintf(t_[i].key, 16, "%s", sym);  // copy/overwrite
                t_[i].id = id; t_[i].used = true; return;
            }
        }
    }
    uint64_t get(const char* sym) const {
        int i = hash(sym) & (CAP - 1);
        for (;; i = (i + 1) & (CAP - 1)) {
            if (!t_[i].used)          return (uint64_t)-1; // empty slot => absent
            if (keq(t_[i].key, sym))  return t_[i].id;
        }
    }
};
```
*Why:* one contiguous table, mask instead of modulo, no allocation per lookup,
and probing stays in cache. Overwrite and "absent → -1" both fall out naturally
(the test checks all three).

### 8. Run it
```bash
make book
```
You want `RESULT|book_bbo|pass` and `RESULT|symmap|pass`, plus the
`METRIC|book_bbo_ns_per_op` and `METRIC|symmap_get_ns_per_op` lines — those are
your latency numbers.

## Your turn
1. Get `make book` green (both `book_bbo` and `symmap`).
2. **Micro-benchmark the alternatives.** Write a throwaway `bench.cpp` that
   times BBO lookups on your flat `Book` vs a `std::map<int,uint32_t>` book, and
   `SymMap::get` vs `std::unordered_map<std::string,uint64_t>`. Print
   `ns_per_op` for each. Expect the flat versions to win by a wide margin —
   note the factor.
3. **Handle the edge you'll hit in the arena:** what happens when a cancel makes
   a side completely empty? Confirm `best_ask()`/`best_bid()` return `0.0`
   (sentinel) rather than reading a stale tick.
4. Stretch: store `level_qty` per inside tick so you can report your FIFO
   `queue_ahead` — this is exactly what `on_ack(...)` gives you against the real
   CLOB.

## Checkpoint
- `make book` → `book_bbo` and `symmap` both **pass**.
- You can state, in one sentence, why the flat array is O(1) on the read path
  and where the *only* scan can happen (an emptied inside level on cancel).
- You have a number: "flat book BBO is ~N× faster than `std::map`."
- `make test` shows HW7 credit (`book_bbo` 12 pts, `symmap` 8 pts).

## Links
- Stub / contract: `include/order_book.hpp`
- Tests (read, don't edit): `tests/book_test.cpp`, `tests/bench.hpp`
- Reference CLOB (price-time priority, `queue_position`): `shared/orderbook.py`
- Client hot path + `on_ack` queue position: `docs/HFT_CPP_CLIENT.md`
- Build: `make book`, full grade `make test`
