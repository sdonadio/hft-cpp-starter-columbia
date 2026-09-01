# Week 9 Lab — Concurrency I: Atomics & Memory Models

**Format:** in-class, guided (~45–60 min).  **Repo:** the HFT starter.

## Goal
Feel a **data race** in the flesh: two `std::thread`s hammering a shared counter,
producing wrong (and non-deterministic) answers. Then fix it correctly with
`std::atomic` and understand **acquire/release** ordering. Prove it clean under
**ThreadSanitizer** (`-fsanitize=thread`). Finally, see *why a mutex on the hot
path spikes the tail latency* — the motivation for next week's **lock-free SPSC
ring** (`include/spsc_ring.hpp`, HW10/Phase 3). This lab leads into HW9.

## Setup
We write throwaway `.cpp` files this week (no stub to fill yet). Use the same
toolchain flags the grader uses:
```bash
cd project-starter
mkdir -p /tmp/w9
# compiler flags mirror the Makefile / CI:
#   normal:  -std=c++17 -O2 -pthread
#   tsan:    -std=c++17 -O1 -g  -pthread -fsanitize=thread
```

## Walk-through — we build this together

### 1. Reproduce the race
Two threads each increment a plain `long` a million times. Correct total is
2,000,000 — but `++` is *load, add, store*, and the threads interleave, so
updates get lost. Save as `/tmp/w9/race.cpp`:
```cpp
#include <thread>
#include <cstdio>

long counter = 0;                      // shared, unsynchronized -> UB

void work() { for (int i = 0; i < 1'000'000; ++i) ++counter; }

int main() {
    std::thread a(work), b(work);
    a.join(); b.join();
    std::printf("counter = %ld (expected 2000000)\n", counter);
}
```
```bash
g++ -std=c++17 -O2 -pthread /tmp/w9/race.cpp -o /tmp/w9/race
/tmp/w9/race ; /tmp/w9/race ; /tmp/w9/race     # run 3x
```
*Observe:* you get numbers **below** 2,000,000, and **different each run**. That
non-determinism is the signature of a data race — and per the C++ standard it's
*undefined behavior*, not just "a wrong number."

### 2. Let ThreadSanitizer name it
Don't rely on your eyes — the tool proves it. Same flags the CI uses:
```bash
g++ -std=c++17 -O1 -g -pthread -fsanitize=thread /tmp/w9/race.cpp -o /tmp/w9/race_tsan
/tmp/w9/race_tsan
```
TSan prints `WARNING: ThreadSanitizer: data race`, with both stacks (the read
and the write to `counter`) and which threads. *Why it matters:* CI greps stderr
for the string `ThreadSanitizer` — a race there is an automatic fail. This is
exactly the check Phase 3 runs on your ring.

### 3. The wrong "fix" people try first: `volatile`
`volatile long counter;` does **not** help. `volatile` means "don't cache in a
register / don't optimize away the access" — it says *nothing* about atomicity
or inter-thread ordering. Rebuild under TSan: **the race is still reported.**
`volatile` is for memory-mapped I/O, not threads. (Demonstrate live, ~30s.)

### 4. Fix it with `std::atomic`
```cpp
#include <atomic>
#include <thread>
#include <cstdio>

std::atomic<long> counter{0};          // now every ++ is indivisible

void work() {
    for (int i = 0; i < 1'000'000; ++i)
        counter.fetch_add(1, std::memory_order_relaxed);
}
int main() {
    std::thread a(work), b(work);
    a.join(); b.join();
    std::printf("counter = %ld\n", counter.load());
}
```
Rebuild both normal and TSan:
```bash
g++ -std=c++17 -O2 -pthread            /tmp/w9/fixed.cpp -o /tmp/w9/fixed
g++ -std=c++17 -O1 -g -pthread -fsanitize=thread /tmp/w9/fixed.cpp -o /tmp/w9/fixed_tsan
/tmp/w9/fixed          # -> 2000000, every time
/tmp/w9/fixed_tsan     # -> no ThreadSanitizer warning
```
*Why `relaxed` is OK here:* we only need the counter's increments to be
**atomic**; we don't publish any *other* data alongside it, so we don't need
ordering. That distinction is the heart of the memory model.

### 5. Acquire/release — the ordering you actually need for a queue
Counters are the easy case. The real HFT pattern is **one thread writes data,
another reads it** — a producer fills a slot, then publishes an index; the
consumer reads the index, then the slot. You need the consumer to see the *data*
once it sees the *index*. That's **release** on the store, **acquire** on the
load:
```cpp
std::atomic<bool> ready{false};
int payload = 0;                       // plain data, guarded by `ready`

// producer:
payload = 42;
ready.store(true, std::memory_order_release);   // everything above is visible...

// consumer:
while (!ready.load(std::memory_order_acquire)) {} // ...once we observe true here
// now reading payload is safe and sees 42
```
- **release** = "no earlier write in this thread may move *after* this store."
- **acquire** = "no later read in this thread may move *before* this load."
Together they form a *happens-before* edge: the consumer that acquires `true` is
guaranteed to see `payload == 42`. **This is precisely the ordering an SPSC ring
uses** — producer writes the cell then `release`-stores `tail`; consumer
`acquire`-loads `tail` then reads the cell. Next week you'll build it in
`include/spsc_ring.hpp`.

### 6. Why the mutex answer is *correct but slow* on the hot path
A `std::mutex` around the counter also passes TSan and gives the right total.
Correct — so why not use it in `on_book`? Sketch it and reason about the **tail**:
```cpp
std::mutex m;
long counter = 0;
void work() { for (int i=0;i<1'000'000;++i){ std::lock_guard<std::mutex> g(m); ++counter; } }
```
- Under contention a mutex can **sleep** the thread (a syscall / futex); when it
  wakes, the scheduler decides *when* — that's tens of microseconds of jitter.
- HFT is graded on **p99 / p99.9**, not the mean (`docs/HFT_CPP_CLIENT.md`:
  "won on the tail, not the mean"). A lock that's cheap 99% of the time but
  occasionally sleeps *is exactly what blows up p99.9*.
- A lock-free `atomic` / SPSC ring has **no syscall, no scheduler** in the fast
  path → a bounded, predictable tail.
Time it if you have a minute: wrap each version in `steady_clock` and compare
max/percentile latencies — the mutex's *max* is dramatically worse even when its
*mean* looks fine.

## Your turn
1. Reproduce the race and capture a TSan report (screenshot the
   `data race` warning — that's your HW9 evidence).
2. Fix with `std::atomic`; show the **same** program is TSan-clean.
3. Write the acquire/release producer/consumer from step 5 and confirm TSan is
   clean. Then **deliberately break it**: change `release`/`acquire` to
   `relaxed` on *both* sides and argue (in a comment) why it's now unsound even
   if it happens to pass on your x86 laptop. (x86 is strongly ordered and often
   "gets lucky" — TSan and other architectures won't.)
4. Add the mutex version, measure max latency vs the atomic version, and write
   one sentence connecting it to p99.9 tail grading.

## Checkpoint
- You can *show* a data race and then *show* it gone, under TSan, with the exact
  CI flags (`-O1 -g -fsanitize=thread`).
- You can define acquire and release in one line each and say why an SPSC ring
  needs them.
- You can explain why a correct mutex is still the wrong tool on the hot path.
- You're set up for **HW9** and for building `include/spsc_ring.hpp` next week
  (`make spsc`, graded on correctness + concurrency + TSan).

## Links
- Next week's target (SPSC ring): `include/spsc_ring.hpp`, driver
  `tests/spsc_correctness.cpp` / `tests/spsc_concurrency.cpp`
- How CI runs TSan (flags + grep for `ThreadSanitizer`): `tests/run_ci.py`
- Tail-latency grading rationale: `docs/HFT_CPP_CLIENT.md`
- Build: `make spsc` (once the stub is implemented), full grade `make test`
