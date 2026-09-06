# Week 3 Lab — Memory Management & Smart Pointers / RAII
**Format:** in-class, guided (~45–60 min).  **Repo:** the HFT starter.

## Goal
Turn a leaky raw `new/delete` into safe, zero-overhead RAII: find the leak with
the right tool *for your OS*, convert to `unique_ptr`, write the Rule of
Three/Five by hand once so you never want to again, write a tiny RAII guard,
measure the atomic-refcount tax of `shared_ptr`, and prove *why* the `on_book`
hot path must never allocate.

## Setup
```bash
mkdir -p scratch
sed -n '1,13p' include/pool.hpp     # the HW4 stub this lab motivates
```

Two build recipes for the night — use the right one:

```bash
# CORRECTNESS runs (sanitizers, leak hunting): -O0 -g, so the tools can see everything
clang++ -std=c++17 -O0 -g -fsanitize=address scratch/mem.cpp -o /tmp/mem

# TIMING runs (step 5 only): -O2, or you measured nothing (week 2)
clang++ -std=c++17 -O2 -Itests scratch/mem.cpp -o /tmp/mem
```

Use `g++` instead of `clang++` if that's your compiler; on Linux they behave the
same here. **Do not** hunt memory bugs at `-O2`: the optimizer can delete the
very allocation you're trying to catch. (Try it — the double-free in step 3 is
invisible at `-O1` and loud at `-O0`.)

## Walk-through — we build this together

### 1. The raw `new/delete` bug — and finding it on *your* OS
Create `scratch/mem.cpp`. Here's an order object managed by hand — with the
classic bug: an early return leaks it.

```cpp
#include <cstdio>
struct Order {
    int id; double px;
    Order(int i, double p) : id(i), px(p) {}
    ~Order(){ printf("~Order %d\n", id); }
};

bool risk_ok(double px) { return px < 1000.0; }

void handle_raw(double px) {
    Order* o = new Order(1, px);
    if (!risk_ok(o->px)) return;     // BUG: leaks o — destructor never runs
    printf("sent order %d @ %.2f\n", o->id, o->px);
    delete o;
}
int main(){ handle_raw(2000.0); handle_raw(100.0); }
```

The program prints `~Order 1` **once**, for the `100.0` call. The `2000.0` call
allocated an `Order` and walked away from it. Now prove it with a tool — and
**the tool depends on your platform**, because *AddressSanitizer on macOS has no
LeakSanitizer*.

**macOS (Apple Silicon or Intel) — use `leaks`:**

```bash
clang++ -std=c++17 -O0 -g scratch/mem.cpp -o /tmp/mem   # no sanitizer needed
leaks --atExit -- /tmp/mem
```

Expected (addresses and node counts will differ):

```text
sent order 1 @ 100.00
~Order 1
Process 13279: 193 nodes malloced for 31 KB
Process 13279: 1 leak for 32 total leaked bytes.
STACK OF 1 INSTANCE OF 'ROOT LEAK: <malloc in handle_raw(double)>':
    1 (32 bytes) ROOT LEAK: <malloc in handle_raw(double) 0x...> [32]
```

`leaks` also prints `Process NNNNN is not debuggable. Due to security
restrictions...` and a short crash-report-style header. Ignore both — they are
normal on macOS and the leak report underneath them is still correct.

32 bytes, not 16: `malloc` rounds up to its size class. Two things to know about
`leaks`: it is **conservative** — it scans the stack and registers for anything
that *looks* like a pointer, so a stale copy of the leaked pointer left in a dead
stack slot or a register makes the leak "reachable" and it reports **0 leaks**.
That is exactly why we build at `-O0 -g` here; at `-O1` this same program reports
`0 leaks for 0 total leaked bytes`. Trust a positive result, never a negative one.

**Linux (or Docker on any host) — ASan's LeakSanitizer does the job properly:**

```bash
# native Linux, or:  docker run --rm -it -v "$PWD":/w -w /w gcc:14 bash
g++ -std=c++17 -O0 -g -fsanitize=address scratch/mem.cpp -o /tmp/mem
/tmp/mem            # detect_leaks is ON by default on Linux
```

Expected shape (exit code 1):

```text
sent order 1 @ 100.00
~Order 1
=================================================================
==31==ERROR: LeakSanitizer: detected memory leaks

Direct leak of 16 byte(s) in 1 object(s) allocated from:
    #0 0x... in operator new(unsigned long)
    #1 0x... in handle_raw(double) /w/scratch/mem.cpp:11
    #2 0x... in main /w/scratch/mem.cpp:15

SUMMARY: AddressSanitizer: 16 byte(s) leaked in 1 allocation(s).
```

LSan gives you the exact allocation site and the exact 16 bytes; `leaks` gives
you a size class and a function name. Both are enough tonight.

> **The trap, so you don't lose an hour to it:** on macOS the ASan build above
> exits **0 and silently reports nothing**, and if you try to force the issue
> with `ASAN_OPTIONS=detect_leaks=1 /tmp/mem` you get
> `AddressSanitizer: detect_leaks is not supported on this platform.` and
> **exit 134**. That is not your bug. `-fsanitize=address` is still worth using
> on macOS — it catches use-after-free, double-free and overflows, just not
> leaks. For leaks on a Mac: `leaks`, or a Linux container.

In a long-running trading process that leak grows all session. Every manual
`delete` is a bug waiting for a new `return`, `throw`, or `break` to skip it.

### 2. Fix it with `unique_ptr`
Ownership becomes automatic: the destructor runs on *every* exit path, for
free.

```cpp
#include <memory>
void handle_unique(double px) {
    auto o = std::make_unique<Order>(2, px);   // construct IN PLACE, no temporary
    if (!risk_ok(o->px)) return;               // o destroyed here, automatically
    printf("sent order %d @ %.2f\n", o->id, o->px);
}                                              // ...or here
```

Note the arguments: `make_unique<Order>(2, px)` forwards `2` and `px` to
`Order`'s constructor and builds the object once, directly in the heap slot.
Writing `make_unique<Order>(Order{2, px})` instead builds a **temporary `Order`
on the stack**, copies it into the heap, and destroys the temporary — so you get
an extra `~Order 2` per call, printed *before* the "sent order" line, and the
destructor lesson turns to mush. Compare:

```text
-- make_unique<Order>(Order{2, px})  (the temporary version — confusing) --
~Order 2                 <- the temporary dying, before anything is "sent"
~Order 2                 <- the owned object, rejected path
~Order 2                 <- another temporary
sent order 2 @ 100.00
~Order 2

-- make_unique<Order>(2, px)  (in place — what you want) --
~Order 2                 <- rejected path: destroyed at the early return
sent order 2 @ 100.00
~Order 2                 <- accepted path: destroyed at the closing brace
```

One `~Order` per call, in the right place. **Never pass a fully-built object to
`make_unique`/`make_shared` when you can pass its constructor arguments** — this
is the same "don't copy what you can construct" reflex that HW 2 was about.

Re-run the leak check from step 1: clean on both platforms, and `~Order` fires
on both paths. `unique_ptr` is **zero-overhead** — same size and speed as a raw
pointer, it just can't forget to free. It's move-only, which encodes "exactly
one owner" in the type system.

### 3. When you *must* own a raw buffer: the Rule of Three (and Five)
`unique_ptr` is the answer 95% of the time. The other 5% — you're writing the
container, or wrapping a C API — you own a raw `new[]` yourself, and then the
compiler-generated copy operations are a **bug**. This is a graded HW 3 task, so
do it here first.

If a class has a destructor that releases something, the default *copy*
constructor and *copy assignment* still do a member-by-member shallow copy: two
objects, one buffer, two `delete[]`s. That's the **Rule of Three** — declare all
three or none:

```cpp
#include <algorithm>
#include <cstddef>

struct PxBuf {                                                  // owns a raw new[]
    explicit PxBuf(std::size_t n) : n_(n), p_(new double[n]) {}
    ~PxBuf() { delete[] p_; }                                   // 1. destructor

    PxBuf(const PxBuf& o) : n_(o.n_), p_(new double[o.n_]) {     // 2. copy ctor
        std::copy(o.p_, o.p_ + n_, p_);                          //    DEEP copy
    }
    PxBuf& operator=(const PxBuf& o) {                           // 3. copy assign
        if (this == &o) return *this;        // self-assignment must be safe
        double* tmp = new double[o.n_];      // allocate BEFORE you free anything,
        std::copy(o.p_, o.p_ + o.n_, tmp);   // so a throw leaves *this intact
        delete[] p_;
        p_ = tmp; n_ = o.n_;
        return *this;
    }

    double& operator[](std::size_t i) { return p_[i]; }
    std::size_t size() const { return n_; }
private:
    std::size_t n_;
    double*     p_;
};
```

Delete the copy constructor and copy assignment from that class, keep the
destructor, and run this at `-O0 -g -fsanitize=address`:

```cpp
PxBuf a(3); a[0] = 101.5;
PxBuf b = a;                 // shallow copy: b.p_ == a.p_
```

```text
==14134==ERROR: AddressSanitizer: attempting double-free on 0x603000001c60
    #1 ... in PxBuf::~PxBuf() r3bad.cpp:5
    #2 ... in PxBuf::~PxBuf() r3bad.cpp:5
```

Two destructors, one buffer. (Build the same file at `-O1` and ASan says
nothing — the optimizer removed the copy. Correctness runs go at `-O0`.)

**Rule of Five** adds the two move operations — a move constructor and move
assignment that *steal* the pointer and null out the source:

```cpp
    PxBuf(PxBuf&& o) noexcept : n_(o.n_), p_(o.p_) { o.p_ = nullptr; o.n_ = 0; }
    PxBuf& operator=(PxBuf&& o) noexcept {
        if (this != &o) { delete[] p_; p_ = o.p_; n_ = o.n_; o.p_ = nullptr; o.n_ = 0; }
        return *this;
    }
```

Without them, `std::vector<PxBuf>` reallocating copies every buffer instead of
moving pointers — an O(n) deep copy where O(1) would do. `noexcept` is not
decoration: `vector` only uses your move constructor during reallocation if it's
`noexcept`.

**And why `= delete` is usually the right answer in this course.** An HFT object
is often something you never want silently duplicated — a pool, a ring buffer, a
socket, a book. Copying it is either meaningless or catastrophically slow, and a
copy on the hot path is exactly the allocation you spent week 2 learning to
hate. Say so in the type:

```cpp
struct Pool {
    Pool(const Pool&)            = delete;   // a second owner of the same slab
    Pool& operator=(const Pool&) = delete;   // is a double-free waiting to happen
    Pool(Pool&&)                 = delete;   // and moving it invalidates every
    Pool& operator=(Pool&&)      = delete;   // pointer you already handed out
};
```

Now an accidental copy is a **compile error** at the call site instead of a
double-free in production, and it costs zero runtime. The decision ladder:
**prefer a member `unique_ptr`/`vector` and write none of the five (Rule of
Zero) → if the class genuinely owns a raw resource, write all five → if copying
makes no sense, `= delete` it.**

### 4. A small RAII guard
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

### 5. The `shared_ptr` refcount tax — it's the *copy*, not the deref
`shared_ptr` gets called "slow", which is sloppy. Dereferencing a `shared_ptr`
is **free**: it holds the object pointer right next to the control-block
pointer, so `sp.get()` and `*sp` are the same load a raw pointer would do. What
costs money is **copying** it, because the refcount is **atomic**: an atomic
increment now, an atomic decrement later, plus cache-line traffic between cores
sharing the control block. Measure all three with the grader's timer:

```cpp
#include "bench.hpp"    // -Itests
auto up = std::make_unique<Order>(3, 100.0);
auto sp = std::make_shared<Order>(4, 100.0);

double uq  = ns_per_op([&]{ Order* r = up.get(); doNotOptimize(r); }, 50'000'000);
double shd = ns_per_op([&]{ Order* r = sp.get(); doNotOptimize(r); }, 50'000'000);   // deref
double shc = ns_per_op([&]{ auto c = sp; doNotOptimize(c.get()); }, 50'000'000);     // COPY
printf("unique.get=%.2f ns  shared.get=%.2f ns  shared copy=%.2f ns\n", uq, shd, shc);
```

```bash
clang++ -std=c++17 -O2 -Itests scratch/mem.cpp -o /tmp/mem && /tmp/mem
```

Measured on an Apple M-series laptop, `-O2`:

```text
unique.get=0.49 ns  shared.get=0.23 ns  shared copy=3.31 ns
```

`shared.get()` and `unique.get()` are the same operation and both round to
"free" — that spread is noise, not a finding. The copy is **~14× the deref**,
and that ratio is the whole story. **Rules of thumb:** default to `unique_ptr`
plus raw *non-owning* pointers or references for observers; reach for
`shared_ptr` only when ownership is genuinely shared; and when you do, pass it
as `const shared_ptr&` down the call chain so you pay the refcount **once**, not
once per stack frame. Never copy one inside `on_book`.

### 6. Why `on_book` must not allocate
Tie it together. The hot path (`on_book(sym,bid,ask,mid,microprice,obi)`) runs
per tick and is graded on p99.9. A single `new`/`make_shared` there is an
unbounded call that can lock or page-fault — invisible in p50, brutal in the
tail. The fix is the pattern the rest of the term builds on:

```cpp
// BAD: allocates on every tick — tail latency lands here
void on_book(...) { auto o = std::make_shared<Order>(...); send(o); }

// GOOD: pre-allocate once, hand out an O(1) slot from a pool (HW4)
Pool pool_{sizeof(Order), 4096};       // a MEMBER, constructed at startup
void on_book(...) {
    void* slot = pool_.alloc();
    if (!slot) return;                 // pool exhausted — never ignore this
    Order* o = new (slot) Order(...);  // placement new
    send(o);
    o->~Order(); pool_.free(slot);     // and give the slot BACK
}
```

That `Pool` is exactly the HW4 stub in `include/pool.hpp`: one pre-allocated
buffer + a free-list, O(1) `alloc`/`free`, no per-tick heap. Note the two things
students forget and week 4 will hammer: it's a **member**, not a local (a 4096-slot
pool of `Order` is ~128 KB and a non-main thread on macOS gets a 512 KB stack),
and every `alloc()` needs a matching `free()` or the bot stops trading after
4096 ticks.

## Your turn
On-ramp to HW3 — all four Canvas tasks are here:

1. Take the leaky `handle_raw` and rewrite it two ways — `unique_ptr`, and a
   custom RAII guard — and prove no leak (`leaks --atExit` on macOS,
   `-fsanitize=address` on Linux). Say which platform and tool you used.
2. **(Graded, step 3.)** Write a class that manages a raw `new[]`/`delete[]`
   buffer with a correct destructor, copy constructor and copy assignment
   (Rule of Three). Show it survives self-assignment and a `vector` that
   reallocates, under ASan at `-O0`. Then rewrite it with a member
   `std::unique_ptr<double[]>` and say exactly which of the three you were able
   to delete, and why.
3. Benchmark raw pointer deref vs `unique_ptr::operator*` vs `shared_ptr`
   *deref* vs `shared_ptr` *copy*. Report ns/op + your machine. Explain the
   ordering — and be explicit that the deref is free and the copy is not.
4. Sketch (comments are fine) how you'd replace one `make_shared` in a hot
   loop with a pool slot — this is the seed of your HW4 `Pool`.

## Checkpoint
- `handle_raw` leaks, and you *saw* it: `leaks --atExit` reports 1 leak (macOS),
  or LSan reports 16 bytes from `handle_raw` (Linux). Both fixed versions clean.
- Your `make_unique<Order>(2, px)` version prints exactly one `~Order` per call,
  *after* the "sent order" line on the accepted path.
- Your Rule-of-Three class is ASan-clean at `-O0`; deleting the copy ctor and
  copy assignment (while keeping the destructor) produces a double-free.
- Your benchmark shows `unique.get() ≈ shared.get()` ≈ free, and a `shared_ptr`
  copy an order of magnitude worse.
- You can state, in one sentence, why an allocation in `on_book` hurts p99.9
  more than p50.

## Links
Week-3 lecture deck · HW3 (smart pointers / RAII — note the **Rule of Three**
task) · leads into HW4 (`include/pool.hpp`, the object pool)
