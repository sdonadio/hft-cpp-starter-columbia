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
It checks: distinct non-null slots up to capacity, `alloc()` past capacity → `nullptr`,
a freed slot is reused, 100k fill/drain rounds don't leak, then it prints
`METRIC|pool_ns_per_op`.

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
Expect `pass` on `pool_basic`, `pool_full`, `pool_reuse`, `pool_pattern`, then a
line like `METRIC|pool_ns_per_op|3.7`. Compare that mentally to a `new`/`delete`
pair (often 20–50 ns+ with far worse tails).

## Your turn
1. Finish `include/pool.hpp` so all four tests pass and the metric prints.
2. **Sketch a per-tick arena / monotonic buffer** (discuss, then try in a scratch
   file): a bump allocator with a single `offset` that only moves forward —
   `alloc(n)` returns `base_ + offset_` and adds `n`; there is *no* per-object
   `free`, only a `reset()` that sets `offset_ = 0` at the end of each tick.
   When is this better than the free-list pool? (Answer: many short-lived objects
   of *mixed* size per tick — parse scratch, temporary levels — freed all at once.
   Trade-off: you can't free individually.)
3. HW4 asks you to report your ns/alloc and argue why it's stable. Keep the pool;
   your Phase-1 bot will allocate orders/messages out of it so the hot path never
   calls `new`.

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
