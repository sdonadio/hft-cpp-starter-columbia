# Week 8 Lab — Algorithmic Complexity & Time-Series (midterm week)

**Format:** in-class, guided (~45–60 min).  **Repo:** the HFT starter.

> **Midterm reminder:** the Week 8 **midterm is this session** — this lab is the
> warm-up, kept deliberately short. The midterm covers Big-O reasoning,
> amortized analysis, and the order-book/time-series structures from Weeks 7–8.
> Finish the walk-through, get `make rolling` green, then we take the exam.
> Nothing here requires the network.

## Goal
Build a **sliding-window event counter** in `include/rolling_counter.hpp` with
**amortized O(1)** `add` and `count`, expiring events as a live clock advances.
Then build an **O(1) online mean/variance** (Welford) for streaming stats. This
lab **is** the HW8 challenge.

## Setup
```bash
cd project-starter
cat include/rolling_counter.hpp             # stub + contract
sed -n '1,40p' tests/rolling_counter_test.cpp   # the tests (do NOT edit)
make rolling                                # fails until implemented
```
Contract (from the stub / `rolling_counter_test.cpp`):
```cpp
struct RollingCounter {
    explicit RollingCounter(uint64_t window_ns);
    void     add(uint64_t ts_ns);        // record an event
    uint64_t count(uint64_t now_ns);     // events with ts > now - window_ns
};
```
Key fact the tests rely on: `count()` is always called with a **non-decreasing**
`now_ns` (a real clock only moves forward). That is what makes O(1) possible.

## Walk-through — we build this together

### 1. Complexity framing (this is the midterm's core idea)
Naive `count(now)` = "scan all events, keep those newer than `now - window`" is
**O(n)** per call — fatal at HFT message rates. But since `now` only advances,
an event that has expired *stays* expired. So we never re-examine it: we drop it
**once**, from the front. Each event is added once and removed once → **O(1)
amortized**. Recognizing "monotonic clock ⇒ front only ever shrinks" is the
whole insight.

### 2. Pick the structure: a ring buffer of timestamps
A `std::deque<uint64_t>` works and is the easy version. For HFT we prefer a
fixed **ring buffer** — no allocation, contiguous, cache-friendly. Timestamps
arrive in order, so the ring is naturally sorted oldest→newest.
```cpp
#include <cstdint>
#include <vector>

struct RollingCounter {
    uint64_t window_;
    std::vector<uint64_t> buf_;   // ring of timestamps
    size_t head_ = 0, tail_ = 0;  // [head_, tail_) live, indices grow forever
    // ...
};
```
We let `head_`/`tail_` grow monotonically and index with `% buf_.size()`; the
count of live events is simply `tail_ - head_`.

### 3. Constructor — size the ring
```cpp
explicit RollingCounter(uint64_t window_ns) : window_(window_ns) {
    buf_.resize(1 << 20);          // generous; power of two so % is a mask
}
```
*Why a big fixed buffer:* the tests push up to a few thousand live events; a
ring sized once at construction means `add` never allocates on the hot path.

### 4. `add` — append at the tail
```cpp
void add(uint64_t ts_ns) {
    buf_[tail_ & (buf_.size() - 1)] = ts_ns;
    ++tail_;
}
```
O(1), no branches. (Production code would guard against overrun; the grader's
volumes stay within the buffer.)

### 5. `count` — expire from the front, then return the size
The threshold is `now - window`; anything with `ts <= threshold` is dead. Since
older events sit at `head_`, we drop them in a `while` loop:
```cpp
uint64_t count(uint64_t now_ns) {
    uint64_t cutoff = now_ns > window_ ? now_ns - window_ : 0;
    while (head_ < tail_ && buf_[head_ & (buf_.size() - 1)] <= cutoff)
        ++head_;                       // event expired: drop once, forever
    return tail_ - head_;              // live events: O(1)
}
```
Watch the boundary: the contract is "events with `ts > now - window`", i.e.
strictly greater than `cutoff` survive — so we expire `<= cutoff`. Trace the
test: `window=1000`, events at ts `0..999`, `count(999)` → `cutoff = -1→0`... in
the test's second phase `count(1999)` has `cutoff=999`, so ts `0..999` all
expire and only `1000..1499` (500 events) survive. That matches `rc_mixed`.

### 6. Run it
```bash
make rolling
```
Want `RESULT|rc_window|pass`, `RESULT|rc_mixed|pass`, and a
`METRIC|rolling_ns_per_op` line.

### 7. Bonus structure — Welford's online mean/variance (O(1), one pass)
Rolling stats (e.g. volatility of the mid) must update per tick without storing
history and without catastrophic cancellation. Welford does it in O(1) space and
time:
```cpp
struct Welford {
    uint64_t n = 0;
    double   mean = 0.0, m2 = 0.0;
    void add(double x) {
        ++n;
        double d = x - mean;
        mean += d / n;              // running mean
        m2   += d * (x - mean);     // running sum of squared deviations
    }
    double variance() const { return n > 1 ? m2 / (n - 1) : 0.0; }
    double stddev()   const { return std::sqrt(variance()); }
};
```
*Why not sum-of-squares minus square-of-sum?* That subtracts two huge nearly
equal numbers and loses precision; Welford updates the mean incrementally and
stays stable. Same complexity, correct answer.

## Your turn
1. Get `make rolling` green (`rc_window` **and** `rc_mixed`).
2. Get the boundary exactly right: is an event *at* `now - window` in or out?
   Re-read the contract comment and confirm against `rc_mixed`.
3. Add a tiny `main` (throwaway) that feeds a Welford 1,000 samples and prints
   mean/stddev; sanity-check against a hand computation on 5 values.
4. Midterm self-check: for both structures, state the **amortized** cost of each
   operation and *why* the monotonic clock (or single pass) makes it so.

## Checkpoint
- `make rolling` → `rc_window` and `rc_mixed` **pass**; `make test` shows HW8
  credit.
- You can explain "why amortized O(1)" for the counter in one sentence
  (each event added once, removed once).
- Welford compiles and gives a stable variance.
- **Then take the midterm.**

## Links
- Stub / contract: `include/rolling_counter.hpp`
- Tests (read, don't edit): `tests/rolling_counter_test.cpp`, `tests/bench.hpp`
- Build: `make rolling`, full grade `make test`
- Where these get used live: mid-price vol + message-rate throttles feeding the
  bot's `on_book` hot path (`docs/HFT_CPP_CLIENT.md`)
