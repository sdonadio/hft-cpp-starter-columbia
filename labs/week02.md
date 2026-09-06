# Week 2 Lab — C++ Performance Foundations
**Format:** in-class, guided (~45–60 min).  **Repo:** the HFT starter.

## Goal
Learn to measure like the autograder does — `steady_clock`, warm-up, ns/op —
then *see* the cost of the things that decide HFT latency: heap vs stack, a
cache-friendly contiguous scan vs pointer-chasing, and `alignas(64)` erasing
false sharing.

## Setup
We reuse the grader's own timing helper, `tests/bench.hpp`. Open it and read
the 20 lines — you'll use the exact same tool the autograder uses to fill in
the `ns_per_op` metrics in `report.json`:

```bash
sed -n '1,25p' tests/bench.hpp
```

Key pieces: `ns_per_op(fn, iters)` warms up (`iters/10`) before timing with
`steady_clock`, and `doNotOptimize(v)` / `clobber()` stop the optimizer from
deleting the work you're trying to measure. Make a scratch dir:

```bash
mkdir -p scratch
```

## Walk-through — we build this together

### 1. Your first honest micro-benchmark
Create `scratch/perf.cpp`. The golden rule: **always** feed results to
`doNotOptimize`, or `-O2` deletes your benchmark and you'll "measure" 0 ns.

```cpp
#include "bench.hpp"     // build with -Itests
#include <cstdio>

int main() {
    long n = 50'000'000;
    double x = 1.0;
    double ns = ns_per_op([&]{ x = x * 1.0000001 + 1.0; doNotOptimize(x); }, n);
    printf("fma-ish: %.3f ns/op\n", ns);
}
```

```bash
g++ -std=c++17 -O2 -Itests scratch/perf.cpp -o /tmp/perf && /tmp/perf
```

Try it once *without* `doNotOptimize` — watch the time collapse to ~0. That's
the optimizer, not speed. This is why `bench.hpp` ships those two helpers.

### 2. Stack vs heap — and the sink that makes it honest
Allocation is not free. The stack is a pointer bump; the heap is a call into
the allocator. Time both:

```cpp
// stack: lives in the current frame, freed by returning
auto stack_ns = ns_per_op([]{
    int buf[64];
    buf[0] = 7; doNotOptimize(buf);      // sink the ARRAY, not buf[0]
}, 20'000'000);

// heap: new/delete every iteration
auto heap_ns = ns_per_op([]{
    int* p = new int[64];
    p[0] = 7; doNotOptimize(p);          // sink the POINTER, not p[0]
    delete[] p;
}, 20'000'000);
printf("stack=%.2f ns  heap=%.2f ns  (%.1fx)\n",
       stack_ns, heap_ns, heap_ns / stack_ns);
```

Measured on an Apple M-series laptop, `clang++ -std=c++17 -O2`:

```text
stack=0.23 ns  heap=12.33 ns  (53.5x)
```

**Sink the pointer, not the value.** If you write `doNotOptimize(p[0])` you have
only told the compiler "the `int` I loaded must escape" — nothing at all keeps
the *allocation* alive, and C++14 explicitly permits an implementation to elide
a matched `new`/`delete` pair whose storage never escapes. clang takes that
permission: the same benchmark then reports **stack 0.45 ns vs heap 0.52 ns,
about 1.2×**, and you would "measure" a heap allocation as cheaper than an L1
hit. Passing `p` itself makes the *address* escape, so the allocation has to
really happen, and the honest 50× shows up. The rule generalises: your sink
must protect the thing whose cost you are trying to measure — for an allocator
that is the pointer, not the bytes behind it.

Two sanity checks worth doing now: drop `doNotOptimize` entirely and the loop
reports **0.00 ns/op**; rebuild both variants at `-O0` and you get **13.4 vs
1.7 ns, only ~8×** — a debug build does not reveal the truth, it hides a 50×
effect behind an 8× one. If a number is faster than an L1 hit (~1 ns), it is
not a number.

The **why for HFT**: an allocation on the tick-to-trade path is an unbounded
call that can also fault or lock — it poisons your p99.9. That observation is
the whole point of the `Pool` challenge (HW4): pre-allocate once, hand out
slots in O(1).

### 3. Cache-friendly vs pointer-chasing
Same amount of data, same sum — only the *memory layout* differs.

```cpp
#include <vector>
#include <numeric>
const int N = 1'000'000;

// contiguous: prefetcher-friendly, one cache line feeds the next
std::vector<int> arr(N);
std::iota(arr.begin(), arr.end(), 0);
auto seq = ns_per_op([&]{
    long s = 0; for (int v : arr) s += v; doNotOptimize(s);
}, 200);

// pointer chase: each node is a separate heap allocation -> cache miss per hop
struct Node { int v; Node* next; };
Node* head = nullptr;
for (int i = 0; i < N; ++i) head = new Node{i, head};
auto chase = ns_per_op([&]{
    long s = 0; for (Node* p = head; p; p = p->next) s += p->v; doNotOptimize(s);
}, 200);
printf("contiguous=%.0f ns  pointer-chase=%.0f ns\n", seq, chase);
```

The chase is often **5–10×** slower for identical work: every `p->next` is a
likely L1/L2 miss the CPU can't prefetch. **Why it matters:** this is the case
for storing the order book in flat arrays (HW7) instead of a `map<node*>`.

### 4. `alignas(64)` kills false sharing
Two threads writing two *different* counters should be independent — unless
both counters share one 64-byte cache line. Then every write invalidates the
other core's copy (false sharing), and throughput tanks.

```cpp
#include <thread>
#include <atomic>

struct Shared { std::atomic<long> a{0}, b{0}; };            // same line -> ping-pong
struct Padded { alignas(64) std::atomic<long> a{0};
                alignas(64) std::atomic<long> b{0}; };      // separate lines

template <class S> double race() {
    S s;
    auto t0 = std::chrono::steady_clock::now();
    std::thread ta([&]{ for (long i=0;i<50'000'000;++i) s.a.fetch_add(1, std::memory_order_relaxed); });
    std::thread tb([&]{ for (long i=0;i<50'000'000;++i) s.b.fetch_add(1, std::memory_order_relaxed); });
    ta.join(); tb.join();
    auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double,std::milli>(t1 - t0).count();
}
printf("shared=%.1f ms  padded(alignas 64)=%.1f ms\n", race<Shared>(), race<Padded>());
```

```bash
g++ -std=c++17 -O2 -pthread -Itests scratch/perf.cpp -o /tmp/perf && /tmp/perf
```

The padded version is usually **2–4×** faster despite doing the *same* atomic
work. **Why:** a cache line is the unit of coherency; keep hot per-thread data
on its own line. This is the trick that makes the SPSC ring (Phase 3) actually
scale — its head and tail indices get `alignas(64)`.

### 5. Read a metric out of `report.json`
The grader records speed, not just pass/fail. After `make test`:

```bash
python3 -c "import json;print(json.load(open('report.json'))['metrics']['pool_ns_per_op'])"
```

Note how tiny the target is (sub-nanosecond ns/op for pool/book/fix) — that's
the bar these challenges are graded against. Your job all term is to push
those numbers down.

## Your turn
This is the on-ramp to HW2 (pointers/references + benchmarking):

1. Write `sum_by_value(std::vector<int>)` vs `sum_by_ref(const std::vector<int>&)`
   and benchmark both with `ns_per_op`. Explain the gap (the copy).
2. Sweep the contiguous scan over sizes that fit in L1 (~32 KB), L2, and RAM;
   plot ns/op vs size and mark where each cache level "falls off."
3. Report every number as ns/op with warm-up, and state your machine — an HFT
   benchmark without those is not a result.

## Checkpoint
- `/tmp/perf` prints four comparisons; each slow variant is clearly slower.
- Removing `doNotOptimize` visibly collapses a timing to ~0 (you can explain why).
- You pulled one number out of `report.json['metrics']` from the command line.

## Links
Week-2 lecture deck · HW2 (pointers/references + benchmarking) ·
`tests/bench.hpp`
