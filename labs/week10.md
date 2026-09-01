# Week 10 Lab — Concurrency II: Lock-Free Pipelines

**Format:** in-class, guided (~45–60 min).  **Repo:** the HFT starter.

## Goal
Build a correct, ThreadSanitizer-clean **single-producer / single-consumer (SPSC)
lock-free ring buffer** in `include/spsc_ring.hpp`. This is the handoff between the
thread that reads the socket (**producer**) and the thread that runs your strategy
(**consumer**) — no mutex, no `malloc` on the hot path. This lab *is* **HW10** and
**Project Phase 3**.

By the end you should pass `make spsc` locally and have a ring that survives the
grader's cross-thread stress + TSan runs.

## Setup
```bash
cd project-starter
make spsc          # builds tests/spsc_correctness.cpp against your header — RED right now
```
Open two files side by side:
- `include/spsc_ring.hpp`  ← the stub you edit
- `tests/spsc_correctness.cpp` ← the contract (read it; do not edit it)

The contract is fixed: `SPSCRing(capacity_pow2)`, `push(v)→bool`, `pop(out)→bool`,
`empty()`, `full()`. `push` returns `false` when full; `pop` returns `false` when empty.

## Walk-through — we build this together

### 1. Why lock-free, and why *SPSC* specifically
A mutex on the tick path is a syscall waiting to happen: under contention the loser
parks in the kernel and you eat microseconds — death for p99.9. SPSC is the easy,
fast special case: **exactly one thread pushes, exactly one thread pops.** Because
each index has a single writer, we never need a CAS loop — plain atomic loads/stores
with the right memory ordering are enough. That is the whole trick.

### 2. Power-of-two capacity → mask instead of modulo
We index the backing array with `index & mask` instead of `index % cap`. `%` on the
hot path is a division; `&` is one cycle. This only works when `cap` is a power of
two, so `mask = cap - 1`.
```cpp
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

struct SPSCRing {
    explicit SPSCRing(std::size_t capacity_pow2)
        : cap_(capacity_pow2), mask_(capacity_pow2 - 1), buf_(capacity_pow2) {}
    // ...
private:
    const std::size_t cap_, mask_;
    std::vector<std::uint64_t> buf_;
};
```
We use **monotonic** counters `head_`/`tail_` (they only ever increase) and mask them
at access time. Size is simply `tail_ - head_`; empty is `head_ == tail_`; full is
`tail_ - head_ == cap_`. Unsigned wraparound of the counters is well-defined and the
subtraction stays correct, so we get the full `cap_` slots with no wasted element.

### 3. `alignas(64)` — kill false sharing
`head_` is written by the consumer; `tail_` is written by the producer. If they land
in the same 64-byte cache line, every producer store invalidates the consumer's line
and vice versa — two cores ping-ponging one line, silently serializing you. Put each
counter on its own line:
```cpp
    alignas(64) std::atomic<std::size_t> head_{0};   // consumer writes, producer reads
    alignas(64) std::atomic<std::size_t> tail_{0};   // producer writes, consumer reads
```

### 4. `push` — the producer side (acquire/release)
```cpp
    bool push(std::uint64_t v) {
        const std::size_t t = tail_.load(std::memory_order_relaxed);   // we own tail_
        const std::size_t h = head_.load(std::memory_order_acquire);   // see consumer's frees
        if (t - h == cap_) return false;                               // full
        buf_[t & mask_] = v;                                           // write the payload...
        tail_.store(t + 1, std::memory_order_release);                 // ...then publish it
        return true;
    }
```
The **why**: the `release` store on `tail_` guarantees the payload write above it is
visible *before* the consumer can observe the new `tail_`. The producer reads its own
`tail_` `relaxed` (nobody else writes it) but reads `head_` with `acquire` so it sees
slots the consumer has freed.

### 5. `pop` — the consumer side (mirror image)
```cpp
    bool pop(std::uint64_t& out) {
        const std::size_t h = head_.load(std::memory_order_relaxed);   // we own head_
        const std::size_t t = tail_.load(std::memory_order_acquire);   // see producer's writes
        if (h == t) return false;                                      // empty
        out = buf_[h & mask_];                                         // read the payload...
        head_.store(h + 1, std::memory_order_release);                 // ...then release the slot
        return true;
    }
```
The `acquire` load of `tail_` pairs with the producer's `release` store — that pairing
is exactly what makes `buf_[h & mask_]` a safe, race-free read. This acquire/release
pair is the entire correctness argument; ThreadSanitizer will hold you to it.

### 6. `empty()` / `full()`
```cpp
    bool empty() const {
        return head_.load(std::memory_order_acquire) ==
               tail_.load(std::memory_order_acquire);
    }
    bool full() const {
        return tail_.load(std::memory_order_acquire) -
               head_.load(std::memory_order_acquire) == cap_;
    }
```

### 7. Green on the single-threaded contract
```bash
make spsc
```
You should see `basic`, `empty_pop`, `full`, `fifo`, `wrap` all `pass`.

### 8. The real test — two threads + ThreadSanitizer
The grader compiles `tests/spsc_concurrency.cpp` twice: once for throughput, once
with `-fsanitize=thread` to prove there are no data races. Reproduce both locally:
```bash
# throughput / cross-thread FIFO (2M items by default)
g++ -std=c++17 -O2 -pthread -Iinclude tests/spsc_concurrency.cpp -o /tmp/spsc_conc && /tmp/spsc_conc

# ThreadSanitizer — must print NO "ThreadSanitizer" warnings
g++ -std=c++17 -O1 -g -pthread -fsanitize=thread -Iinclude \
    tests/spsc_concurrency.cpp -o /tmp/spsc_tsan
SPSC_N=200000 /tmp/spsc_tsan
```
If TSan flags a race, you almost certainly weakened an ordering to `relaxed` where an
`acquire`/`release` was required (step 4/5). That is the #1 mistake — fix the pairing,
not the symptom.

## Your turn
1. Finish the header so **all seven** grades pass: `basic, empty_pop, full, fifo, wrap,
   concurrent, tsan` (`make test` scores them; Phase 3 = 90 pts of pass/fail here).
2. Note your `spsc_ops_per_sec` metric from the concurrency run.
3. **Optimize (stretch):** cache the other side's counter in a member so you don't hit
   the shared atomic every call — e.g. the producer keeps a `head_cache_` and only
   re-reads `head_` (acquire) when it *looks* full. Re-measure ops/sec. Keep it TSan-clean.
4. **Project wiring:** in your Phase 3 bot, run the socket reader on one thread pushing
   decoded ticks into an `SPSCRing`, and the strategy on another popping them. The ring
   *is* your tick-to-trade queue.

## Checkpoint
- [ ] `make spsc` → all single-threaded cases `pass`
- [ ] `/tmp/spsc_conc` → `RESULT|concurrent|pass` and a real `THROUGHPUT` number
- [ ] TSan build runs with **zero** `ThreadSanitizer` warnings
- [ ] `head_`/`tail_` are each `alignas(64)`; `push`/`pop` use acquire/release (not all `relaxed`, not all `seq_cst`)
- [ ] `make test` shows Phase 3 (`spsc_ring.hpp`) fully scored

## Links
- Edit: `include/spsc_ring.hpp`
- Contracts (read-only): `tests/spsc_correctness.cpp`, `tests/spsc_concurrency.cpp`
- Grader: `tests/run_ci.py` (Phase 3 rubric), `make test`
- Project: `project/README.md` (Phase 3), `docs/HFT_CPP_CLIENT.md`
