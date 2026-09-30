# Session 5 Lab — Templates, Compile-Time & CRTP

**Format:** in-class, guided (~50 min: Part A ~20, Part B ~30).  **Repo:** the HFT starter.
**Covers:** Week 5 deck (templates) + Week 6 deck (compile-time, policies, CRTP).  **Feeds:** HW5 + HW6 (both due Sat Oct 17, 11:59 pm ET) and Project Phase 1.

## Goal
Templates as a **zero-runtime-cost** tool, then use them to remove the vtable
from the hot path. **Part A** builds a small generic container, a variadic
`sum(...)` with a C++17 **fold expression**, and compile-time branching with
`if constexpr` / SFINAE. **Part B** puts the virtual `Strategy` you priced in
Session 4 next to a **CRTP** version (static polymorphism), then adds a
`constexpr` lookup table, a **policy-selected** component, and the bot's
**compile-time message dispatch** (`std::variant` + visitor). The HFT payoff is
one **inlined codec path**: the compiler generates the exact code for each type,
and nothing is decided at runtime.

> **Where this sits.** *Runtime* polymorphism (inheritance, `virtual`, vtable/vptr,
> virtual destructors, the hot-path cost of a virtual call, `final`) was
> **Session 4** (Week 4 lab, steps 7–10). Here we assume you measured that cost
> and we remove it.

## Setup
One scratch file for the whole lab; we grow it step by step. Compile after every
step, because template errors are much cheaper to read one change at a time.
```bash
cd project-starter
cat > /tmp/w5.cpp <<'EOF2'
#include <cstdio>
int main() { std::puts("session5 scratch"); return 0; }
EOF2
g++ -std=c++17 -O2 /tmp/w5.cpp -o /tmp/w5 && /tmp/w5
```
Keep `/tmp/w5.cpp` open. Put each new snippet above `main` and call it from `main`.

## Walk-through — we build this together

## Part A: templates and generic programming
*~20 min. Steps 1–5. Keep it brisk: steps 2, 4 and 5 are your HW5.*

### 1. A function template = a code *stamp*
```cpp
template <class T>
T max_of(T a, T b) { return a < b ? b : a; }
```
`max_of(3, 4)` stamps out an `int` version; `max_of(1.5, 2.5)` stamps a `double`
version. Both are as fast as if you'd hand-written them: **the template is
resolved and inlined at compile time**. No runtime cost, no vtable, no type tag.
Verify with `-O2 -S` that the call vanishes into a `cmov`.

### 2. A tiny generic container
```cpp
#include <vector>
#include <algorithm>
#include <numeric>

template <class T>
struct RingStat {
    std::vector<T> v;
    void push(T x) { v.push_back(x); }
    T    sum()  const { return std::accumulate(v.begin(), v.end(), T{}); }
    void sort()       { std::sort(v.begin(), v.end()); }
    T    median()     { sort(); return v[v.size() / 2]; }
};
```
One definition works for `RingStat<int>`, `RingStat<double>`, `RingStat<Price>`.
We lean on the STL (`std::vector`, `std::sort`, `std::accumulate`) instead of
reinventing it. `T{}` value-initializes the accumulator to the right zero for
whatever `T` is.

### 3. Variadic template + fold expression
Sum any number of arguments of any numeric type, in one line:
```cpp
template <class... Ts>
auto sum(Ts... xs) { return (xs + ...); }        // C++17 unary right fold
```
`(xs + ...)` expands to `x0 + (x1 + (x2 + ...))` **at compile time**: no loop, no
`va_args`, no array. Show the expansion:
```cpp
auto a = sum(1, 2, 3, 4);       // int   -> 10
auto b = sum(1.5, 2.5, 3.0);    // double-> 7.0
```

### 4. Compile-time branching with `if constexpr`
One codec, two encodings, chosen by the compiler. The dead branch is *not even
compiled* into the binary:
```cpp
#include <type_traits>
#include <cstring>

template <class T>
std::size_t encode(char* out, T v) {
    if constexpr (std::is_integral_v<T>) {
        // integer path: raw little-endian copy
        std::memcpy(out, &v, sizeof(T));
        return sizeof(T);
    } else {
        // floating path: quantize to ticks first, then copy
        auto ticks = static_cast<long long>(v * 100.0 + 0.5);
        std::memcpy(out, &ticks, sizeof(ticks));
        return sizeof(ticks);
    }
}
```
With a plain `if`, both branches must compile for every `T`. With `if constexpr`
the false branch is **discarded**, which is what lets one template body serve
genuinely different types.

### 5. SFINAE — constrain what may instantiate
Before C++20 concepts, we restrict a template to, say, arithmetic types so a bad
call is a clean compile error, not a deep template spew:
```cpp
template <class T,
          class = std::enable_if_t<std::is_arithmetic_v<T>>>
T twice(T x) { return x + x; }
```
`twice(3)` and `twice(2.5)` compile; `twice("no")` is removed from the overload
set ("substitution failure is not an error"), giving a short "no matching
function" message.

## Part B: CRTP and compile-time design
*~30 min. Steps 6–10 plus the checkpoint. Step 10 is what ships in Phase 1.*

### 6. Recap: the virtual version (you wrote this in the Week 4 lab)
Same shape as your HW 4 `Strategy`, so you can port it in one sitting. (Named
`VMomentum` so it can live in the same file as the CRTP one below.)
```cpp
struct IStrategy {
    virtual double signal(double mid, double obi) = 0;
    virtual ~IStrategy() = default;
};
struct VMomentum : IStrategy {
    double signal(double mid, double obi) override { (void)mid; return obi * 0.5; }
};
```
Recall the bill: an indirect call through the vtable, no inlining across it, and a
BTB mispredict when the target varies (about 5 ns vs 0.7 ns on the M4 in the
Week 4 lab). **Keep your HW 4 numbers open; we are about to beat them.**

### 7. CRTP — static polymorphism, no vtable
The derived type is a **template parameter of the base**, so the base can call
down statically:
```cpp
template <class Derived>
struct Strategy {
    double signal(double mid, double obi) {
        return static_cast<Derived*>(this)->signal_impl(mid, obi);
    }
};
struct Momentum : Strategy<Momentum> {
    double signal_impl(double mid, double obi) { (void)mid; return obi * 0.5; }
};
```
`static_cast<Derived*>(this)->signal_impl(...)` is resolved **at compile time**:
no vtable pointer, no indirect call, and `signal_impl` **inlines** straight into
the caller. Same "override a hook" ergonomics, zero dispatch cost. Confirm with
`-O2 -S`: the CRTP call becomes a plain multiply; the virtual one keeps the
indirect `call`. Now port **your HW 4 `Strategy`** the same way and re-run your
HW 4 benchmark against it. That comparison is the HW 6 CRTP task.
```cpp
Momentum m;
double s = m.signal(100.0, 0.8);   // inlined to obi*0.5
```

### 8. A `constexpr` lookup table (compute at compile time)
Some functions are cheaper as a table baked into the binary. Build it *at compile
time* so there's no init cost and it lives in read-only memory:
```cpp
#include <array>
struct TickTable {
    std::array<double, 256> px{};
    constexpr TickTable() {
        for (int i = 0; i < 256; ++i) px[i] = 100.0 + i * 0.01;
    }
};
constexpr TickTable kTicks{};                 // built entirely at compile time
static_assert(kTicks.px[50] == 100.5);        // proven at compile time
```
The `constexpr` constructor runs during compilation; `kTicks` is just bytes in the
binary at runtime. `static_assert` proves correctness before the program ever
runs. Break it on purpose (change `100.0` to `100.001`) and watch the *build* fail.

### 9. Policy-based design — compose behavior at compile time
A component parameterized by *policies* (small classes that each supply one
decision). The compiler stitches them together and inlines through:
```cpp
struct AggressiveFees { static constexpr double taker() { return 0.0015; } };
struct RebateFees     { static constexpr double taker() { return 0.0005; } };

template <class FeePolicy>
struct Quoter {
    double edge_needed(double spread) {
        return spread - FeePolicy::taker();   // policy resolved at compile time
    }
};
Quoter<RebateFees> q;                          // pick the policy at the type level
```
Swapping `Quoter<AggressiveFees>` vs `Quoter<RebateFees>` changes behavior with
**no runtime branch**: the fee is a compile-time constant folded into the math.

### 10. The bot's compile-time message dispatch — `std::variant` + visitor
The payoff. Your inbound messages are a **closed set** of types. Model them as a
`std::variant` and dispatch with a visitor. The compiler generates a jump over a
small known set and each handler is inlined: no base class, no vtable, no heap.
```cpp
#include <variant>
struct BookUpdate { double mid, obi; };
struct Fill       { double px; int qty; };
struct SessionEvt { int code; };

using Msg = std::variant<BookUpdate, Fill, SessionEvt>;

// overload set from lambdas (the classic C++17 visitor helper)
template <class... Fs> struct overload : Fs... { using Fs::operator()...; };
template <class... Fs> overload(Fs...) -> overload<Fs...>;   // C++17 CTAD guide

void handle(const Msg& m) {
    std::visit(overload{
        [](const BookUpdate& b) { std::printf("book mid=%.2f obi=%.2f\n", b.mid, b.obi); },
        [](const Fill& f)       { std::printf("fill %d @ %.2f\n", f.qty, f.px); },
        [](const SessionEvt& e) { std::printf("session %d\n", e.code); },
    }, m);
}

// in main():
//   for (const Msg& m : {Msg{BookUpdate{100.01, 0.60}}, Msg{Fill{100.02, 5}}, Msg{SessionEvt{1}}})
//       handle(m);
```
This reuses Part A's **variadic** idea: the `overload` struct inherits `operator()`
from each lambda via `using Fs::operator()...`, plus the CTAD deduction guide.
`std::visit` knows the alternative set at compile time, so the handler is chosen
without a type tag you maintain by hand, and each branch inlines.

**Mental model for the bot:** CRTP for strategy hooks; `constexpr` tables for
anything precomputable (tick ladders, luts); policies for A/B configuration (fees,
skew) chosen at the type level; `variant` + visitor for the message codec, which
replaces a hand-rolled `switch(msg.type)` on the hot path. Because all of it is
resolved at compile time, the encoder for a `NewOrder` and the one for a `Cancel`
are each fully inlined paths. Speed comes from *not deciding at runtime*.

## Your turn
Items 1–3 are Part A (**HW5**); items 4–7 are Part B (**HW6**). HW5 and HW6 are
both due **Sat Oct 17, 11:59 pm ET**.
1. Add a variadic `avg(...)` built on your `sum(...)`:
   `return sum(xs...) / static_cast<double>(sizeof...(xs));` (note `sizeof...`).
2. Extend `RingStat<T>` with `T max() const` using `std::max_element`; keep it
   generic.
3. Add an `if constexpr` branch to `encode` for a `bool` flag type and print the
   byte counts for an `int`, a `double`, and a `bool`. **HW5:** a small generic +
   variadic utility with the dead paths compiled out; prove zero cost by diffing
   `-O2 -S` output for two instantiations.
4. Add a second CRTP strategy `MeanRevert : Strategy<MeanRevert>` and call
   `.signal()` on both through a `template <class S> void tick(Strategy<S>& s)`
   helper. Note there is **no** common base pointer.
5. Add a `Cancel { uint64_t id; }` alternative to `Msg` and **don't** add a lambda
   yet: the visitor fails to compile, and that error is the production bug caught
   at build time. Then add the handler (keep the visitor free of an `auto&`
   catch-all so a missing handler always fails to compile).
6. Give `Quoter` a second policy axis (a `SkewPolicy`) and instantiate two
   configurations.
7. **HW6:** port your HW 4 virtual `Strategy` to CRTP and compare ns/call against
   your HW 4 numbers; demonstrate static vs dynamic dispatch (show the `-O2 -S`
   difference); use a `constexpr` table at run time and keep one `static_assert`
   enforcing a real invariant; wire the `variant`+visitor codec into your bot's
   message handling for **Phase 1**.

## Checkpoint
```bash
g++ -std=c++17 -O2 -Wall -Wextra /tmp/w5.cpp -o /tmp/w5 && /tmp/w5
```
Expected: clean compile (no warnings) including the `static_assert`, and printed
results: `sum(1,2,3,4)=10`, `avg` correct, per-type encode byte counts, CRTP and
virtual `signal` giving the same answer, the `Quoter` edge for both policies, and
the visitor dispatching each message variant. Then confirm the suite still passes:
```bash
make test
```

## Links
Week-5 deck · Week-6 deck · HW5 · HW6 · Week-4 lab steps 7–10 (runtime polymorphism, the thing CRTP replaces) · Project **Phase 1** (compile-time codec: variant/CRTP).
