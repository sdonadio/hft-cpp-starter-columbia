# Week 4 Lab — Custom Allocators & Memory Pools

**Format:** in-class, guided (~45–60 min).  **Repo:** the HFT starter.

## Goal
Build a fixed-size object **Pool** together — one pre-allocated buffer, an
intrusive free-list, O(1) `alloc()`/`free()` — and see it beat `new`/`delete` on
the ns/alloc metric. This is exactly the **HW4** challenge (`include/pool.hpp`,
autograded, reports ns/alloc). The mantra for the rest of the semester:
**no `new` on the hot path.**

## Setup
```bash
cd project-starter
make pool          # builds tests/pool_test.cpp against include/pool.hpp, runs it
```
Right now the stub compiles but every test fails — `alloc()` returns `nullptr`.
That red output is your signal. Open `include/pool.hpp` in one pane and keep the
`make pool` output in another; we turn it green step by step.

Look at `tests/pool_test.cpp` (read-only, do not edit) — it *is* the contract:
```cpp
struct Pool { Pool(std::size_t obj_size, std::size_t capacity);
              void* alloc(); void free(void*); };
```
It checks, in this order:

| test | what it demands |
|---|---|
| `pool_basic` | `capacity` allocs, all non-null, all **distinct**, and **non-overlapping** — it stamps each slot and re-reads them all |
| `pool_full` | the pool **fills with real slots first**, and only *then* does `alloc()` past capacity return `nullptr` |
| `pool_reuse` | with the pool full, `free(a)` then `alloc()` must return **exactly `a`** — the only free slot |
| `pool_pattern` | 100k fill/drain rounds with no capacity leaked and no slot handed out twice |

then it prints `METRIC|pool_ns_per_op`.

> **Don't be fooled by the metric.** The stub's `alloc()` returns `nullptr`, and
> "allocating nothing" benchmarks at about **0.46 ns/op** — *faster* than a
> correct pool. The metric only means something once all four tests pass, which
> is why the test prints a `NOTE|` line calling itself out when `alloc()` hands
> back null. A fast number from broken code is the week-2 lesson again.

## Walk-through — we build this together

### 1. Why not just call `new`?
`new`/`malloc` walk a general-purpose heap: size classes, locking, arena
bookkeeping, and occasionally a syscall. That's tens to hundreds of ns of jitter
you cannot predict — poison in a tick-to-trade path. A pool trades generality for
speed: **all objects are the same size**, so allocation is "pop a pointer."

### 2. One buffer, owned once
Allocate the whole slab up front in the constructor and never grow it:
```cpp
#pragma once
#include <cstddef>
#include <cstdint>
#include <new>       // placement new

struct Pool {
    Pool(std::size_t obj_size, std::size_t capacity)
        : slot_(obj_size < sizeof(void*) ? sizeof(void*) : obj_size),
          cap_(capacity) {
        buf_  = static_cast<uint8_t*>(::operator new(slot_ * cap_));
        head_ = nullptr;
        // push every slot onto the free-list, back to front
        for (std::size_t i = cap_; i-- > 0; )
            free(buf_ + i * slot_);
    }
    ~Pool() { ::operator delete(buf_); }
    // ...
private:
    std::size_t slot_, cap_;
    uint8_t*    buf_;
    void*       head_;   // top of the free-list
};
```
**Why `slot_` is at least `sizeof(void*)`:** when a slot is *free*, we store the
"next free slot" pointer *inside the slot itself* — an intrusive free-list, zero
extra memory. A slot must therefore be big enough to hold one pointer.

### 3. `alloc()` — pop the free-list, O(1)
```cpp
void* alloc() {
    if (!head_) return nullptr;          // exhausted → null (the test relies on this)
    void* p = head_;
    head_ = *reinterpret_cast<void**>(head_);   // head = head->next
    return p;
}
```
No loop, no branch on size, no lock. Read the head, advance it, return the old
head. That's the whole allocator.

### 4. `free()` — push it back, O(1)
```cpp
void free(void* p) {
    if (!p) return;
    *reinterpret_cast<void**>(p) = head_;   // p->next = head
    head_ = p;                              // head = p
}
```
We reinterpret the freed slot as a `void*` and link it in. Note the constructor
calls `free()` on every slot to build the initial list — nice reuse.

### 5. Placement new + explicit dtor (the usage pattern)
The pool hands you raw memory. To get a *live object* you construct in place, and
because you allocated the storage yourself, you must call the destructor
yourself — `delete` would be wrong (it'd free the heap, not the pool):
```cpp
Order* o = new (pool.alloc()) Order{id, px, qty};  // placement new: construct in slot
// ... use o ...
o->~Order();                                        // explicit dtor
pool.free(o);                                        // return the slot
```
Say it out loud: **placement `new` to construct, explicit `~T()` to destroy,
`pool.free` to reclaim.** No global allocator touched.

### 6. Run it
```bash
make pool
```
Expect `pass` on all four — `pool_basic`, `pool_full`, `pool_reuse`,
`pool_pattern` — then the metric line. On an Apple M4 laptop
(`clang++ -std=c++17 -O2`) a correct free-list pool measures:

```text
RESULT|pool_basic|pass|4 distinct, writable, non-overlapping slots
RESULT|pool_full|pass|3 distinct slots, then alloc past capacity -> null
RESULT|pool_reuse|pass|freed slot reused
RESULT|pool_pattern|pass|100k fill/drain rounds OK
METRIC|pool_ns_per_op|0.56
```

**That number is machine-dependent — expect well under 1 ns/op**, and don't chase
the third digit: an alloc+free pair is two pointer stores and a load, so the
whole thing lives in timer noise (0.35–0.85 ns run to run on the same M4). What
matters is the order of magnitude. Several ns means you're doing more work than a
free-list pop; exactly 0 means your sink is missing. Compare it to a
`new`/`delete` pair on the same laptop — **12–18 ns/op**, allocator-dependent,
with far worse tails: roughly **20–50× on the mean**, and the real win isn't the
mean at all, it's that half a nanosecond is half a nanosecond *every* time.

If you want cycles instead of nanoseconds, `tests/bench.hpp` now ships a
portable counter — `cycle_count()` / `cycles_per_op()` uses `__rdtsc()` on x86
and the `cntvct_el0` timer on arm64, so the same code builds on CI and on a Mac.
Read the header comment first: on arm64 those ticks are a constant-rate timer,
not core cycles.

## Your turn

> **Heads up on scope.** This lab builds **one** of the three things HW 4 asks
> for. Canvas calls HW 4 the *Memory Triathlon*: (a) the free-list `Pool` we just
> built, (b) **a bump/arena allocator you reset per batch**, and (c) **a measured
> speedup** against `malloc`/`free` over the same allocation pattern. The arena
> and the measurement are graded, and they are not in tonight's walk-through —
> item 2 below is where you start them. Budget time for it this week.

1. Finish `include/pool.hpp` so all four tests pass and the metric prints.
2. **Build the per-tick arena / monotonic buffer** (sketch it here, finish it for
   HW 4 — this is triathlon leg two, not an optional extra): a bump allocator
   with a single `offset` that only moves forward — `alloc(n)` returns
   `base_ + offset_` and adds `n` (rounded up for alignment); there is *no*
   per-object `free`, only a `reset()` that sets `offset_ = 0` at the end of each
   tick. When is this better than the free-list pool? (Answer: many short-lived
   objects of *mixed* size per tick — parse scratch, temporary levels — freed
   all at once. Trade-off: you can't free individually, and you must not hold a
   pointer across a `reset()`.) Compare it to the pool on the same workload:
   the arena's `alloc` is one add, so it should beat even the free-list.
3. **Measure the speedup** (triathlon leg three): the same alloc/free pattern
   through `new`/`delete`, through your `Pool`, and through your arena, all in
   an `-O2` build with warm-up. Report ns/op (and cycles/op if you want —
   `cycles_per_op()` in `tests/bench.hpp` is portable now) plus your machine, and
   argue in one paragraph why the pool number is *stable*, not just small. That
   stability argument is what HW 4 actually grades.
4. Keep the pool; your Phase-1 bot will allocate orders/messages out of it so the
   hot path never calls `new` — and it will `free()` every slot it takes, or it
   stops trading after `capacity` ticks.

## Checkpoint
```bash
make pool
```
Expected: all `RESULT|pool_*|pass` lines and a `METRIC|pool_ns_per_op|<n>` line.
Then confirm the grader sees it:
```bash
make test          # run_ci.py — grades every implemented challenge
```

## Links
Week-4 deck · HW4 (`include/pool.hpp`) · Project **Phase 1** (no-alloc hot path).
