# Week 8 Lab — Algorithmic Complexity & Time-Series

**Format:** in-class, guided (~45–60 min).  **Repo:** the HFT starter.

> **Midterm reminder:** the midterm is **the following session, Wed Oct 28**, not
> tonight. This lab is kept deliberately short so the last part of the session
> can be the exam map. The midterm covers Big-O reasoning, amortized analysis,
> and the order-book/time-series structures from Weeks 7–8, so tonight's two
> structures are exam material. **HW8 is this lab, due Sat Oct 31, 11:59 PM ET**
> (ten days after the session). Nothing here requires the network.

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
We let `head_`/`tail_` grow monotonically and index with
`& (buf_.size() - 1)` — a mask, valid because we size the ring to a power of two
(step 3); the count of live events is simply `tail_ - head_`.

### 3. Constructor — size the ring
```cpp
static constexpr size_t CAP = 1 << 20;
static_assert((CAP & (CAP - 1)) == 0, "CAP must be a power of two: the ring "
                                      "indexes with & (CAP-1)");

explicit RollingCounter(uint64_t window_ns) : window_(window_ns) {
    buf_.resize(CAP);              // CAP is a power of two, so % becomes a mask
}
```
*Why a big fixed buffer:* the tests push up to a few thousand live events; a
ring sized once at construction means `add` never allocates on the hot path.

*Why the `static_assert` matters, not just the comment:* `x & (CAP - 1)` is only
equal to `x % CAP` when `CAP` is a power of two — with, say, `CAP = 1000` the
mask silently indexes the wrong slot instead of failing loudly. The same rule is
what makes the deck's `Ring<T,N>` correct: writing `head = (head + 1) % N` costs
an integer division (20–40 cycles) on a non-power-of-two `N`, and
`(head - count + i) % N` does that subtraction in **unsigned** arithmetic, so it
wraps modulo 2^64 — an answer that is only still right if `N` divides 2^64, i.e.
`N` is a power of two. Assert the shape, then use the mask:
```cpp
template<class T, size_t N> struct Ring {
    static_assert((N & (N - 1)) == 0, "N must be a power of two");
    std::array<T, N> buf{};
    size_t head = 0, count = 0;
    void push(T x) { buf[head] = x; head = (head + 1) & (N - 1);
                     if (count < N) ++count; }
    T operator[](size_t i) const { return buf[(head - count + i) & (N - 1)]; }
};
```
One rule — power-of-two capacity, asserted — fixes a performance bug and a
correctness bug at the same time.

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
older events sit at `head_`, we drop them in a `while` loop — but **the guard
goes around the whole loop, not on the cutoff value**:
```cpp
uint64_t count(uint64_t now_ns) {
    if (now_ns > window_) {                 // else the window isn't full yet
        const uint64_t cutoff = now_ns - window_;   //   -> nothing can expire
        while (head_ < tail_ && buf_[head_ & (buf_.size() - 1)] <= cutoff)
            ++head_;                        // event expired: drop once, forever
    }
    return tail_ - head_;                   // live events: O(1)
}
```
Watch the boundary: the contract is "events with `ts > now - window`", i.e.
strictly greater than `cutoff` survive — so we expire `<= cutoff`.

**Why the guard is a whole-loop `if` and not `cutoff = now > window ? now - window : 0`.**
`now_ns` and `window_` are `uint64_t`. When `now_ns <= window_` the mathematical
cutoff is negative, and there is no `uint64_t` that means "negative" — clamping
it to `0` looks harmless and is not, because the loop then expires everything
`<= 0`, which kills a real event sitting at `ts == 0`. Trace the test:
`window = 1000`, events at ts `0..999`, `count(999)`. The true cutoff is
`999 - 1000 = -1`, so **every** event survives and the answer is **1000** — but
the clamped version returns **999**, and `rc_window` fails on exactly that one
event. Writing the subtraction only when it is meaningful (`now_ns > window_`)
is the fix; there is no sentinel value to clamp to. This is unsigned underflow
silently changing the meaning of a comparison, and it is the single most common
serious bug in low-latency C++. (Signed `int64_t` timestamps would make
`now - window` genuinely `-1` and the comparison would just work — a legitimate
alternative, but the contract hands you `uint64_t`, so convert deliberately and
comment it.)

The test's second phase then checks the ordinary path: `count(1999)` has
`cutoff = 999`, so ts `0..999` all expire and only `1000..1499` (500 events)
survive. That matches `rc_mixed`.

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
- You can say out loud why `cutoff = now > window ? now - window : 0` is wrong.
- **The midterm is the following session (Wed Oct 28), not tonight** — both of
  tonight's structures are on it, so be able to state their amortized cost and
  why the monotonic clock (or single pass) gives it to you.

## Links
- Stub / contract: `include/rolling_counter.hpp`
- Tests (read, don't edit): `tests/rolling_counter_test.cpp`, `tests/bench.hpp`
- Build: `make rolling`, full grade `make test`
- Where these get used live: mid-price vol + message-rate throttles feeding the
  bot's `on_book` hot path (`docs/HFT_CPP_CLIENT.md`)
