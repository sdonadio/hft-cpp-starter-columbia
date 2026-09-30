# Week 4 Lab — Custom Allocators, Memory Pools & Runtime Polymorphism

**Format:** in-class, guided (~45 min: pool steps 1–6 ≈ 25 min, polymorphism steps 7–10 ≈ 20 min; everything longer is in "Your turn").  **Repo:** the HFT starter.

## Goal
Build a fixed-size object **Pool** together — one pre-allocated buffer, an
intrusive free-list, O(1) `alloc()`/`free()` — and see it beat `new`/`delete` on
the ns/alloc metric. That is the autograded core of **HW4** (`include/pool.hpp`,
reports ns/alloc). Then put **runtime polymorphism** into that pool: an abstract
`Strategy` interface, two derived strategies placement-new'd into your slots and
destroyed through a base pointer (virtual destructor!), and a micro-benchmark that
prices a `virtual` call against a direct call and a `final` one. The mantras for
the rest of the semester: **no `new` on the hot path**, and **know what a virtual
call costs before you put one there.**

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

### 7. Runtime polymorphism: an abstract `Strategy`
Your bot client already does this. In `hft/cpp_client/include/hft_bot.hpp` the base
class declares `virtual void on_book(symbol, bid, ask, mid, microprice, obi)` and
*you* override it; the framework calls it through a base pointer and never knows
your concrete type. Here is the same shape, small enough to hold in your head:
```cpp
struct Strategy {                                   // abstract interface
    virtual ~Strategy() = default;                   // ALWAYS virtual on a base
    virtual double on_book(double bid, double ask, double obi) = 0;   // pure virtual
};
struct Momentum : Strategy {
    double on_book(double bid, double ask, double obi) override {
        return obi * 0.5 * (ask - bid);
    }
};
```
`= 0` makes `Strategy` abstract (you cannot instantiate it), and `override` makes
the compiler check that your signature really matches a base virtual — a typo
becomes a compile error instead of a silent new function. **How it works:** each
object of a class with virtuals starts with a hidden **vptr**; each such class has
one static **vtable** (an array of function pointers). `p->on_book(...)` is:
load `p->vptr`, load slot *k*, indirect call. One extra pointer per object, two
dependent loads and an indirect branch per call.

### 8. Objects in *your* pool, destroyed through the base pointer
Step 5's pattern, now with polymorphic types. Save this as `/tmp/w4_strat.cpp`:
```cpp
#include <cstdio>
#include <new>
#include <string>
#include "pool.hpp"

static int live = 0;                       // counts constructed-but-not-destroyed

struct Strategy {                          // abstract interface
    Strategy()  { ++live; }
    virtual ~Strategy() { --live; }        // <- the line that matters
    virtual double on_book(double bid, double ask, double obi) = 0;
};

struct Momentum : Strategy {
    double on_book(double bid, double ask, double obi) override {
        return obi * 0.5 * (ask - bid);
    }
};

struct MeanRev : Strategy {
    std::string tag = "a-name-long-enough-to-force-a-heap-allocation-in-SSO-land";  // owns heap memory
    double ema = 100.0;
    double on_book(double bid, double ask, double) override {
        double mid = 0.5 * (bid + ask);
        ema += 0.1 * (mid - ema);
        return ema - mid;
    }
};

int main() {
    constexpr std::size_t SLOT = sizeof(MeanRev) > sizeof(Momentum) ? sizeof(MeanRev) : sizeof(Momentum);
    Pool pool(SLOT, 8);                    // one slot size that fits every derived type

    Strategy* s[2] = {
        new (pool.alloc()) Momentum{},
        new (pool.alloc()) MeanRev{},
    };
    for (Strategy* p : s)
        std::printf("signal = %+.4f\n", p->on_book(100.00, 100.02, 0.4));

    for (Strategy* p : s) {
        p->~Strategy();                    // virtual: runs ~Momentum / ~MeanRev, then ~Strategy
        pool.free(p);                      // slot goes back on the free-list
    }
    std::printf("live objects after teardown: %d\n", live);
}
```
```bash
g++ -std=c++20 -O2 -Iinclude /tmp/w4_strat.cpp -o /tmp/w4_strat && /tmp/w4_strat
```
Expected (apple clang, M4):
```text
signal = +0.0040
signal = -0.0090
live objects after teardown: 0
```
Three things to notice. (1) The pool slot size is the **largest** derived type —
derived objects are bigger than the base, so `Pool(sizeof(Strategy), ...)` would be
a buffer overflow. (2) `p->~Strategy()` is a *virtual* call: it runs `~MeanRev`
(freeing its `std::string`), then `~Strategy`. (3) `live` is your leak counter:
back to 0 means every constructor was matched by its destructor.

**Now break it.** Delete the word `virtual` from `~Strategy()` and rebuild. That is
**undefined behaviour**: `p->~Strategy()` on a `Strategy*` runs only the *base*
destructor, so the derived part is never destroyed — `MeanRev`'s `tag` keeps its
heap buffer forever (a leak per object), and on many toolchains nothing complains.
On this M4 the program actually traps inside `~Strategy` (`EXC_BREAKPOINT`, which is the *lucky* outcome — undefined behaviour
is allowed to crash, leak silently, or appear to work). clang even warns:
`-Wdelete-abstract-non-virtual-dtor`. On Linux, add `-fsanitize=address` and run
with `ASAN_OPTIONS=detect_leaks=1` to see the leaked string reported; Apple's clang
has no LeakSanitizer, which is why the counter approach above is the portable
check. Rule: **a class with any virtual function gets a virtual destructor**.
Put the `virtual` back before moving on.

### 9. What does `virtual` cost on the hot path?
Three costs, in the order they bite: (a) **no inlining** — the compiler cannot see
through an indirect call, so the body is not merged into the caller and cannot be
constant-folded or vectorised with the surrounding code; (b) the **two dependent
loads** (vptr, then slot), which are cheap when hot in L1; (c) the **indirect
branch** — the CPU predicts its *target* in the branch-target buffer (BTB). One hot
target predicts perfectly; several targets in an unpredictable order mispredict at
roughly a pipeline flush each. Measure it — save as `/tmp/w4_vbench.cpp`, reusing
`tests/bench.hpp`:
```cpp
// Virtual vs direct vs final. Build:  g++ -std=c++20 -O2 -Itests /tmp/w4_vbench.cpp
#include <algorithm>
#include <cstdio>
#include <vector>
#include "bench.hpp"

struct Strategy {
    virtual ~Strategy() = default;
    virtual double on_book(double bid, double ask, double obi) = 0;
};
// four distinct implementations so 4 targets are genuinely different code
struct S0 final : Strategy { double on_book(double b, double a, double o) override { return o * (a - b); } };
struct S1 final : Strategy { double on_book(double b, double a, double o) override { return o + 0.5 * (a + b); } };
struct S2 final : Strategy { double on_book(double b, double a, double o) override { return (a - b) - o; } };
struct S3 final : Strategy { double on_book(double b, double a, double o) override { return o * o + b; } };
// the "direct" baseline: same body as S0, no base class at all
struct Direct { double on_book(double b, double a, double o) { return o * (a - b); } };

static double g_sink;                              // benchmark sink: a global the optimizer must assume is read

constexpr long N = 20'000'000;

template <class F> double median_ns(F&& f, int reps = 7) {
    std::vector<double> v;
    for (int i = 0; i < reps; ++i) v.push_back(ns_per_op(f, N));
    std::sort(v.begin(), v.end());
    return v[reps / 2];
}

// Each call gets a slowly varying obi and its result is added into acc, which is
// stored to a global sink every iteration: the optimizer cannot drop the call.
// The loop + sink store cost is in every row, so read the ROWS AS DIFFERENCES.
int main() {
    S0 s0; S1 s1; S2 s2; S3 s3;
    Strategy* one = &s0;                            // one hot target
    Strategy* four[4] = {&s0, &s1, &s2, &s3};

    // pattern table: 4096 pseudo-random target indices (predictable would be i&3)
    std::vector<unsigned char> pat(4096);
    unsigned x = 12345;
    for (auto& p : pat) { x = x * 1664525u + 1013904223u; p = (x >> 24) & 3; }

    double bid = 100.00, ask = 100.02;
    Direct d;

    auto run = [&](auto&& body) { double acc = 0.0; long i = 0;
        return median_ns([&] { acc += body((i & 7) * 0.1, i); ++i; doNotOptimize(acc); g_sink = acc; }); };

    double t_direct = run([&](double a, long)  { return d.on_book(bid, ask, a); });
    Strategy* volatile hide = one;                  // hides the dynamic type from the optimizer
    double t_virt1  = run([&](double a, long)  { return hide->on_book(bid, ask, a); });
    double t_virt4r = run([&](double a, long i){ return four[pat[i & 4095]]->on_book(bid, ask, a); });
    double t_virt4p = run([&](double a, long i){ return four[i & 3]->on_book(bid, ask, a); });
    S0* volatile hide0 = &s0;                       // S0 is `final`: static type known -> direct call
    double t_final  = run([&](double a, long)  { return hide0->on_book(bid, ask, a); });

    std::printf("direct call (no base class)    %6.2f ns/call\n", t_direct);
    std::printf("virtual, 1 target              %6.2f ns/call\n", t_virt1);
    std::printf("virtual, 4 targets, i&3        %6.2f ns/call\n", t_virt4p);
    std::printf("virtual, 4 targets, random     %6.2f ns/call\n", t_virt4r);
    std::printf("final class, via S0*           %6.2f ns/call\n", t_final);
    std::printf("sink=%g\n", g_sink);
}
```
```bash
g++ -std=c++20 -O2 -Itests /tmp/w4_vbench.cpp -o /tmp/w4_vbench && /tmp/w4_vbench
```
Every call result is added to `acc` and stored to a global `g_sink` behind
`doNotOptimize`, so the optimizer cannot delete the call; the printed `sink` proves
the work happened. Loop and sink cost sit in *every* row, so read the rows as
**differences**. Apple M4, apple clang 21, `-O2`, median of 7 runs of 20M calls
(three separate runs agreed to within a few percent):

```text
direct call (no base class)      0.66 ns/call
virtual, 1 target                0.74 ns/call
virtual, 4 targets, i&3          0.75 ns/call
virtual, 4 targets, random       5.30 ns/call
final class, via S0*             0.70 ns/call
```
**Read it honestly.** This is *one laptop*; on x86 or another core expect the
same *shape* but different absolute numbers (a bare direct call may be 1–2 ns, a
mispredict 3–10 ns). What generalises: (i) a **predictable** virtual call costs
about the same as a direct call — tens of percent at most, not multiples (the 4
targets in a repeating `i&3` order are learned by the predictor); (ii) an
**unpredictable** target costs several ns — that is the number that hurts, ~8×
the direct call here, and it is the mispredict, not the two loads; (iii) `final`
puts you back at direct-call cost. And the number that this micro-benchmark
*understates*: in a real bot the virtual call also blocks inlining, and inlining
is what lets the compiler delete work around the call. Do not trust the ns/call
alone — look at `-O2 -S` and count the `blr`/`call`.

### 10. `final` and devirtualization
If the compiler can prove which override runs, it drops the indirect call and
inlines. `final` is how you give it that proof:
```cpp
struct S0 final : Strategy { double on_book(double, double, double) override; };
//        ^^^^^ nothing derives from S0, so an S0* / S0& call is direct
```
Declaring a **class** `final` (or one **method** `final`) tells the compiler no
further override exists; a call through the *derived* static type (`S0*`, not
`Strategy*`) is resolved at compile time. Same-scope `Momentum m; m.on_book(...)`
is devirtualized automatically because the dynamic type is known. It does **not**
help a call through a `Strategy*` pointing at one of several types — for that the
virtual is genuinely needed, and the answer is either accept the cost (cold path),
or remove the dispatch: the compile-time route is Week 5–6 (templates, CRTP).

## Your turn

> **Heads up on scope.** This lab covers the **pool** and the **first pass** at
> the polymorphism and virtual-cost legs. HW 4 (Canvas: *A high-performance
> allocator + runtime polymorphism*) is four graded pieces: (a) the free-list
> `Pool` we built (autograded, 4 pts), (b) **a bump/arena allocator you reset per
> batch** with a **measured speedup** against `malloc`/`free` (2 pts), (c) the
> **polymorphism leg** — your own abstract interface, at least two derived
> classes, placement-new'd into your pool and destroyed through the base pointer
> with no leaks (2 pts), and (d) **your own virtual vs direct vs `final`
> measurement** with a paragraph explaining the gap (2 pts). The arena and the
> measurements are not finished in class — budget time for them this week.

1. Finish `include/pool.hpp` so all four tests pass and the metric prints.
2. **Build the per-tick arena / monotonic buffer** (HW 4 leg b): a bump allocator
   with a single `offset` that only moves forward — `alloc(n)` returns
   `base_ + offset_` and adds `n` (rounded up for alignment); there is *no*
   per-object `free`, only a `reset()` that sets `offset_ = 0` at the end of each
   tick. When is this better than the free-list pool? (Answer: many short-lived
   objects of *mixed* size per tick — parse scratch, temporary levels — freed
   all at once. Trade-off: you can't free individually, and you must not hold a
   pointer across a `reset()`.)
3. **Measure the allocator speedup**: the same alloc/free pattern through
   `new`/`delete`, your `Pool`, and your arena, `-O2`, with warm-up, median of
   several runs. Report ns/op (or `cycles_per_op()` from `tests/bench.hpp`) plus
   your machine, and argue in one paragraph why the pool number is *stable*, not
   just small.
4. **Polymorphism, your own version** (HW 4 leg c): don't reuse the lab's
   `Strategy` — invent an interface for your bot (`Signal`, `RiskCheck`,
   `Quoter`, ...), write two or more derived classes, allocate them from your
   `Pool`, call them through the base pointer, destroy with `p->~Base();
   pool.free(p);`, and show zero leaks (a live-object counter like the lab's, or
   ASan on Linux). Try both a pure-virtual base and a base with one default
   implementation.
5. **Measure the virtual cost yourself** (HW 4 leg d): adapt `/tmp/w4_vbench.cpp` to
   *your* interface. Report direct vs virtual-1-target vs virtual-N-targets
   (predictable and random order) vs `final`, with your machine, and one paragraph
   on why the gap exists (indirect branch + BTB, no inlining). Then count the
   `call`/`blr` instructions in `-O2 -S` output for the direct and virtual loops.
6. **Stretch:** `Pool` is single-type-size. What breaks if `Momentum` grows a
   `std::array<double,64>` member and you forgot to re-size the pool? Add a
   `static_assert(sizeof(Derived) <= SLOT)` guard in a helper
   `template<class T, class... A> T* make(Pool&, A&&...)`. (This is a preview of
   Week 5.)
7. Keep the pool; your Phase-1 bot will allocate orders/messages out of it so the
   hot path never calls `new` — and it will `free()` every slot it takes, or it
   stops trading after `capacity` ticks. Week 6 revisits your HW 4 `Strategy`
   with CRTP so you can put the two sets of numbers side by side.

## Checkpoint
```bash
make pool
```
Expected: all `RESULT|pool_*|pass` lines and a `METRIC|pool_ns_per_op|<n>` line.
Then confirm the grader sees it:
```bash
make test          # run_ci.py — grades every implemented challenge
```
And the polymorphism leg, on your finished pool:
```bash
g++ -std=c++20 -O2 -Iinclude /tmp/w4_strat.cpp -o /tmp/w4_strat && /tmp/w4_strat
g++ -std=c++20 -O2 -Itests   /tmp/w4_vbench.cpp -o /tmp/w4_vbench && /tmp/w4_vbench
```
Expected: `live objects after teardown: 0`, and a benchmark table where the
unpredictable 4-target row is several times the other four.

## Links
Week-4 deck (allocators + runtime polymorphism) · HW4 (`include/pool.hpp` + arena + `Strategy` in the pool + virtual-cost measurement) · `hft/cpp_client/include/hft_bot.hpp` (`virtual on_book`) · Week 5 (templates) and Week 6 (CRTP: the compile-time alternative to what you just measured) · Project **Phase 1** (no-alloc hot path).
