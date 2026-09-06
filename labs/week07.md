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

### 2. Ticks and a price-indexed **band**
Convert price to an integer tick, then index a flat array of levels:
```cpp
#include <algorithm>
#include <array>
#include <cstdint>

static constexpr double TICK = 0.01;
static constexpr int    NLEV = 1 << 16;   // 65536 contiguous slots = a band

static inline int px_to_tick(double px) {
    return static_cast<int>(px / TICK + 0.5);   // round to nearest tick
}
```
`NLEV` slots of `uint32_t` qty is 256 KB a side, 512 KB for the book — and every
access is a single indexed load. *Why:* one array, no allocation per order, and
the hardware prefetcher loves the linear layout.

**Read this paragraph before you write any code — it is the bug that CI will not
catch for you.** `NLEV` slots of one cent is a **$655.35-wide window**, so if you
index *absolutely* (slot = tick, slot 0 = $0.00) your book covers $0.00–$655.35
and nothing above it. The arena's securities are not all inside that window:
`plugins/securities/defaults.py` lists **NFLX at a base price of $720** and
**META at $580**, and once NFLX moves at all the second one is marginal too. On
the first NFLX snapshot `px_to_tick(720.00)` is `72000`, so you write about
6,500 slots past the end of a 65,536-entry array.

Now look at where that write actually *lands*. `bid_qty` and `ask_qty` are
adjacent members of the same `Book`, so running off the end of `bid_qty` walks
straight into `ask_qty`: a bid at $719.98 is tick 71998, and
`71998 - 65536 = 6462`, so it silently **adds its quantity to the ask side at
$64.62**. Your book now reports resting size that no venue ever sent, on the
wrong side, at a price nobody quoted — and your bot crosses a spread that does
not exist. It does not crash. Worse, **`-fsanitize=address` does not catch it
either**: ASan instruments the boundaries *between* allocations, not the
boundaries between two members inside one object, so the whole thing is a
silent wrong answer with a clean sanitizer run. And the autograder only
exercises prices near $100, so **CI stays green** too. That is the worst
failure shape there is: no crash, no sanitizer report, no failing test, just a
book that is quietly lying to you.

The fix is *not* "make the array bigger until it covers the S&P": an absolute
array wide enough for a $5,000 stock at one-cent ticks is 2 MB a side
of almost entirely empty slots, which throws away the only thing you bought with
the flat layout (a tiny working set). Instead keep `NLEV` small and make it a
**movable band**: store the absolute tick of slot 0 in a `base_tick_`, index
`slot = tick - base_tick_`, and centre the band on the first price you ever see
for that symbol. You get $327 of room on either side of where the market
actually is, a bounds check that tells you when the market has walked out of the
band, and 512 KB regardless of whether the stock trades at $22 or $720. This is
the same `base_` offset the deck's flat-book slide uses, and re-basing when the
market leaves the band belongs in your Phase 2 write-up.

### 3. The level array + aggregated quantity
Each side is an array indexed by tick; the value is the **total resting qty** at
that price. Adding is just `+= qty`, cancelling is `-= qty`.
```cpp
struct Book {
    std::array<uint32_t, NLEV> bid_qty{};   // qty resting at each bid SLOT
    std::array<uint32_t, NLEV> ask_qty{};   // qty resting at each ask SLOT
    int  base_tick_ = 0;                    // absolute tick of slot 0
    bool based_     = false;                // has the band been placed yet?
    int best_bid_slot = -1;                 // highest bid slot with qty>0
    int best_ask_slot = -1;                 // lowest  ask slot with qty>0

    // absolute tick -> slot. The first price we ever see centres the band, so
    // we get ~$327 of headroom on either side of the market. std::max keeps
    // base_tick_ >= 0 for cheap stocks, where the band just starts at $0.00.
    int slot_of(int tick) {
        if (!based_) { base_tick_ = std::max(0, tick - NLEV / 2); based_ = true; }
        return tick - base_tick_;
    }
    // slot -> price, for the BBO reads
    double px_of(int slot) const { return (slot + base_tick_) * TICK; }
};
```
We track the *best slot* explicitly so BBO is O(1) — we never scan on the read
path (the hot path). We pay a tiny cost on add/cancel to keep it fresh instead.
Note `slot_of` is non-`const` (it may set the base) while `px_of` is `const` —
only the write path can move the band.

### 4. `add` — update qty, then nudge the best pointer
```cpp
void add(uint64_t id, char side, double px, uint32_t qty) {
    int i = slot_of(px_to_tick(px));
    if (i < 0 || i >= NLEV) return;         // outside the band: refuse, don't
                                            // scribble (see step 2 / Phase 2)
    orders_[id] = {side, i, qty};           // remember for cancel (see step 6)
    if (side == 'B') {
        bid_qty[i] += qty;
        if (i > best_bid_slot) best_bid_slot = i;   // new inside price
    } else {
        ask_qty[i] += qty;
        if (best_ask_slot < 0 || i < best_ask_slot) best_ask_slot = i;
    }
}
```
*Why O(1):* adding at or inside the top only ever *raises* the best pointer —
one comparison, no search. The two-sided bounds check is the whole NFLX fix: one
branch on the write path, and an out-of-band price becomes a visible dropped
order instead of silent memory corruption. In production you would re-base here
rather than return.

### 5. `best_bid` / `best_ask` — pure O(1) reads
```cpp
double best_bid() const {
    return best_bid_slot < 0 ? 0.0 : px_of(best_bid_slot);
}
double best_ask() const {
    return best_ask_slot < 0 ? 0.0 : px_of(best_ask_slot);
}
```
This is the payoff: the read your strategy calls thousands of times per tick is
a compare + multiply. No tree walk, no branch mispredict storm.

### 6. `cancel` — the only place we may scan
Cancel removes qty; if it empties the *inside* level, we must find the new best.
Keep an `id → (side, tick, qty)` record so cancel knows what to subtract:
```cpp
struct Rec { char side; int slot; uint32_t qty; };
std::unordered_map<uint64_t, Rec> orders_;   // fine: cancel is off the read path

void cancel(uint64_t id) {
    auto it = orders_.find(id);
    if (it == orders_.end()) return;
    Rec r = it->second; orders_.erase(it);
    if (r.side == 'B') {
        bid_qty[r.slot] -= r.qty;
        if (r.slot == best_bid_slot && bid_qty[r.slot] == 0) {
            while (best_bid_slot >= 0 && bid_qty[best_bid_slot] == 0) --best_bid_slot;
        }
    } else {
        ask_qty[r.slot] -= r.qty;
        if (r.slot == best_ask_slot && ask_qty[r.slot] == 0) {
            while (best_ask_slot < NLEV && ask_qty[best_ask_slot] == 0) ++best_ask_slot;
            if (best_ask_slot >= NLEV) best_ask_slot = -1;   // side is empty
        }
    }
}
```
Both scans are bounded on *both* ends — `--best_bid_slot` stops at `-1` and
`++best_ask_slot` stops at `NLEV`, and the ask side resets to the `-1` sentinel
when it runs off. Drop either bound and "cancel everything, then read the touch"
reads a garbage slot or crashes. The grader does exactly that.
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
   (sentinel) rather than reading a stale slot.
4. **Prove the band holds — and see the bug it prevents.** First reproduce the
   corruption: on an absolute-from-zero book, put a real ask at $64.62, then add
   a bid at $719.98, and print the ask level again. It grew. Note that
   `-fsanitize=address,undefined` reports **nothing** — this is an intra-object
   overflow, so the sanitizer cannot see it, which is exactly why you have to
   reason about the range instead of relying on tools. Then switch to your
   banded book: on a *fresh* `Book`, `add(1,'B',719.98,10)` and
   `add(2,'S',720.02,10)` must give a BBO of `719.98 / 720.02` (that book's band
   is now $392.32–$1047.67), and `add(3,'B',100.00,10)` on the same book must be
   *refused*, not written. Finally write down in one line what your Phase 2 book
   will do instead of refusing (re-base the band and re-key the resting
   orders).
5. Stretch: store `level_qty` per inside slot so you can report your FIFO
   `queue_ahead` — this is exactly what `on_ack(...)` gives you against the real
   CLOB.

## Checkpoint
- `make book` → `book_bbo` and `symmap` both **pass**.
- You can state, in one sentence, why the flat array is O(1) on the read path
  and where the *only* scan can happen (an emptied inside level on cancel).
- `NLEV = 1 << 16` at one-cent ticks is a **$655.35-wide band**, so the array is
  indexed against `base_tick_`, not from $0.00 — and you can say why NFLX at
  $720 would otherwise write past the end of it with CI still green.
- You have a number: "flat book BBO is ~N× faster than `std::map`."
- `make test` shows HW7 credit (`book_bbo` 12 pts, `symmap` 8 pts).

## Links
- Stub / contract: `include/order_book.hpp`
- Tests (read, don't edit): `tests/book_test.cpp`, `tests/bench.hpp`
- Reference CLOB (price-time priority, `queue_position`): `shared/orderbook.py`
- Client hot path + `on_ack` queue position: `docs/HFT_CPP_CLIENT.md`
- Build: `make book`, full grade `make test`
