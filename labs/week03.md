# Week 3 Lab — Memory Management & Smart Pointers / RAII
**Format:** in-class, guided (~45–60 min).  **Repo:** the HFT starter.

## Goal
Turn a leaky raw `new/delete` into safe, zero-overhead RAII: convert to
`unique_ptr`, write a tiny RAII guard, measure the atomic-refcount tax of
`shared_ptr`, and prove *why* the `on_book` hot path must never allocate.

## Setup
```bash
mkdir -p scratch
sed -n '1,13p' include/pool.hpp     # the HW4 stub this lab motivates
```

We'll build small programs with the sanitizers on so mistakes are loud:

```bash
# leak/UAF detector:
g++ -std=c++17 -O1 -g -fsanitize=address scratch/mem.cpp -o /tmp/mem && /tmp/mem
```

## Walk-through — we build this together

### 1. The raw `new/delete` bug
Create `scratch/mem.cpp`. Here's an order object managed by hand — with the
classic bug: an early return leaks it.

```cpp
#include <cstdio>
struct Order { int id; double px; ~Order(){ printf("~Order %d\n", id); } };

bool risk_ok(double px) { return px < 1000.0; }

void handle_raw(double px) {
    Order* o = new Order{1, px};
    if (!risk_ok(o->px)) return;     // BUG: leaks o — destructor never runs
    printf("sent order %d @ %.2f\n", o->id, o->px);
    delete o;
}
int main(){ handle_raw(2000.0); handle_raw(100.0); }
```

Run under ASan — the `2000.0` path reports a leak, and you'll notice `~Order 1`
never prints for it. In a long-running trading process, that leak grows all
session. Every manual `delete` is a bug waiting for a new `return`, `throw`, or
`break` to skip it.

### 2. Fix it with `unique_ptr`
Ownership becomes automatic: the destructor runs on *every* exit path, for
free.

```cpp
#include <memory>
void handle_unique(double px) {
    auto o = std::make_unique<Order>(Order{2, px});
    if (!risk_ok(o->px)) return;     // o destroyed here, automatically
    printf("sent order %d @ %.2f\n", o->id, o->px);
}                                    // ...or here
```

Re-run under ASan: no leak, `~Order` fires on both paths. `unique_ptr` is
**zero-overhead** — same size and speed as a raw pointer, it just can't forget
to free. It's move-only, which encodes "exactly one owner" in the type system.

### 3. A small RAII guard
RAII isn't only for memory — it's for *any* paired acquire/release. HFT loves
this for scoped latency stamps and lock/unlock. Write one:

```cpp
#include <chrono>
struct ScopedTimer {
    const char* name;
    std::chrono::steady_clock::time_point t0{std::chrono::steady_clock::now()};
    ~ScopedTimer() {
        auto ns = std::chrono::duration<double,std::nano>(
                    std::chrono::steady_clock::now() - t0).count();
        printf("[%s] %.0f ns\n", name, ns);
    }
};
void decide() { ScopedTimer t{"on_book"}; /* ...work... */ }  // prints on scope exit
```

The **why**: acquire in the constructor, release in the destructor, and the
compiler guarantees the release even on exceptions or early returns. No
`finally`, no forgetting.

### 4. The `shared_ptr` refcount tax
`shared_ptr` is convenient but not free: its control block refcount is
**atomic**, so every copy is an atomic increment (and cross-core cache
traffic). Measure it against `unique_ptr` with the grader's timer:

```cpp
#include "bench.hpp"    // -Itests
auto up = std::make_unique<Order>(Order{3, 100});
auto sp = std::make_shared<Order>(Order{4, 100});

double uq = ns_per_op([&]{ Order* r = up.get();               doNotOptimize(r); }, 50'000'000);
double sh = ns_per_op([&]{ auto  c = sp; /* atomic ++/-- */   doNotOptimize(c.get()); }, 50'000'000);
printf("unique.get=%.2f ns   shared copy=%.2f ns\n", uq, sh);
```

```bash
g++ -std=c++17 -O2 -Itests scratch/mem.cpp -o /tmp/mem && /tmp/mem
```

The `shared_ptr` copy is many times slower — those are atomic RMW ops. **Rule
of thumb:** default to `unique_ptr` + raw *non-owning* pointers/references for
observers; reach for `shared_ptr` only when ownership is genuinely shared.

### 5. Why `on_book` must not allocate
Tie it together. The hot path (`on_book(sym,bid,ask,mid,microprice,obi)`) runs
per tick and is graded on p99.9. A single `new`/`make_shared` there is an
unbounded call that can lock or page-fault — invisible in p50, brutal in the
tail. The fix is the pattern the rest of the term builds on:

```cpp
// BAD: allocates on every tick — tail latency lands here
void on_book(...) { auto o = std::make_shared<Order>(...); send(o); }

// GOOD: pre-allocate once, hand out an O(1) slot from a pool (HW4)
Pool pool_{sizeof(Order), 4096};       // constructed at startup, off the hot path
void on_book(...) { void* slot = pool_.alloc(); /* placement-new, send, free */ }
```

That `Pool` is exactly the HW4 stub in `include/pool.hpp`: one pre-allocated
buffer + a free-list, O(1) `alloc`/`free`, no per-tick heap.

## Your turn
On-ramp to HW3:

1. Take the leaky `handle_raw` and rewrite it two ways — `unique_ptr`, and a
   custom RAII guard — and prove no leak under `-fsanitize=address`.
2. Benchmark: raw pointer deref vs `unique_ptr::operator*` vs copying a
   `shared_ptr`. Report ns/op + your machine. Explain the ordering.
3. Sketch (comments are fine) how you'd replace one `make_shared` in a hot
   loop with a pool slot — this is the seed of your HW4 `Pool`.

## Checkpoint
- `handle_raw` leaks under ASan; both fixed versions are clean.
- Your benchmark shows `unique_ptr` ≈ raw pointer, `shared_ptr` copy clearly slower.
- You can state, in one sentence, why an allocation in `on_book` hurts p99.9
  more than p50.

## Links
Week-3 lecture deck · HW3 (smart pointers / RAII) · leads into HW4
(`include/pool.hpp`, the object pool)
