# Week 6 Lab — Compile-Time & Policy-Based Design (CRTP)

**Format:** in-class, guided (~45–60 min).  **Repo:** the HFT starter.

## Goal
Kill the vtable on the hot path. We build, live: a **CRTP** base (static
polymorphism, no virtual dispatch) side-by-side with the virtual version so the
cost is visible; a `constexpr` lookup table computed entirely at compile time; and
a **policy-selected** component. Then we tie it all together into the bot's
**compile-time message dispatch** — a `std::variant` + visitor. Leads into **HW6**
and locks in the Phase-1 codec.

## Setup
Scratch file again; compile after each step.
```bash
cd project-starter
cat > /tmp/w6.cpp <<'EOF'
#include <cstdio>
int main() { std::puts("week6 scratch"); return 0; }
EOF
g++ -std=c++17 -O2 /tmp/w6.cpp -o /tmp/w6 && /tmp/w6
```

## Walk-through — we build this together

### 1. The virtual version (the thing we're replacing)
```cpp
struct IStrategy {
    virtual double signal(double mid, double obi) = 0;
    virtual ~IStrategy() = default;
};
struct Momentum : IStrategy {
    double signal(double mid, double obi) override { return obi * 0.5; }
};
```
Every `strat->signal(...)` is an **indirect call through a vtable**: load the
vtable pointer, load the slot, call. The compiler can't inline across it, and the
indirect branch can mispredict. Fine for cold configuration code — a tax you don't
want per tick.

### 2. CRTP — static polymorphism, no vtable
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
    double signal_impl(double mid, double obi) { return obi * 0.5; }
};
```
`static_cast<Derived*>(this)->signal_impl(...)` is resolved **at compile time** —
no vtable pointer, no indirect call, and `signal_impl` **inlines** straight into
the caller. Same "override a hook" ergonomics, zero dispatch cost. Confirm with
`-O2 -S`: the CRTP call becomes a plain multiply; the virtual one keeps the
indirect `call`.
```cpp
Momentum m;
double s = m.signal(100.0, 0.8);   // inlined to obi*0.5
```

### 3. A `constexpr` lookup table (compute at compile time)
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
runs — a class of bug caught by the compiler.

### 4. Policy-based design — compose behavior at compile time
A component parameterized by *policies* (small classes that supply one decision
each). The compiler stitches them together and inlines through:
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
**no runtime branch** — the fee is a compile-time constant folded into the math.

### 5. The bot's compile-time message dispatch — `std::variant` + visitor
Now the payoff. Your inbound messages are a **closed set** of types. Model them as
a `std::variant` and dispatch with a visitor — the compiler generates a jump over
a small known set, and each handler is inlined. No base class, no vtable, no heap:
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
        [](const BookUpdate& b) { /* hot path: react to b.mid, b.obi */ (void)b; },
        [](const Fill& f)       { /* update inventory */ (void)f; },
        [](const SessionEvt& e) { /* open/close */ (void)e; },
    }, m);
}
```
This reuses two ideas from Week 5: the **variadic** `overload` struct (inheriting
`operator()` from each lambda via `using Fs::operator()...`) and the CTAD
deduction guide. `std::visit` knows the alternative set at compile time, so the
handler is chosen without a runtime type tag you maintain by hand — and each
branch inlines.

### 6. Put the pieces together (mental model)
- **CRTP** for your strategy hooks — override behavior, keep it inlined.
- **`constexpr` tables** for anything you can precompute (tick ladders, luts).
- **Policies** for A/B configuration (fees, skew) chosen at the type level.
- **`variant` + visitor** for the message codec — the compile-time dispatch that
  replaces a hand-rolled `switch(msg.type)` on the hot path.

## Your turn
1. Add a second CRTP strategy `MeanRevert : Strategy<MeanRevert>` and call
   `.signal()` on both through a `template <class S> void tick(Strategy<S>& s)`
   helper — note there is **no** common base pointer.
2. Add a `Cancel { uint64_t id; }` alternative to `Msg` and a matching lambda in
   the visitor; let the compiler tell you if you forgot one (make the visitor
   `[](auto&){}`-free so a missing handler fails to compile).
3. Give `Quoter` a second policy axis (a `SkewPolicy`) and instantiate two
   configurations.
4. This is **HW6**: demonstrate static vs dynamic dispatch (show the `-O2 -S`
   difference) and wire the `variant`+visitor codec into your bot's message
   handling for **Phase 1**.

## Checkpoint
```bash
g++ -std=c++17 -O2 -Wall -Wextra /tmp/w6.cpp -o /tmp/w6 && /tmp/w6
```
Expected: clean compile including the `static_assert` (compile-time table proven),
and the visitor dispatching each message variant. Confirm the suite still passes:
```bash
make test
```

## Links
Week-6 deck · HW6 · Project **Phase 1** (compile-time codec: variant/CRTP).
