# Week 5 Lab — Templates & Generic Programming

**Format:** in-class, guided (~45–60 min).  **Repo:** the HFT starter.

## Goal
Get fluent with templates as a **zero-runtime-cost** tool: a small generic
container, a variadic `sum(...)` with a C++17 **fold expression**, and
compile-time branching with `if constexpr` / SFINAE. The payoff for HFT is one
**inlined codec path** — the compiler generates the exact code for each type, no
dispatch at runtime. This is the groundwork for **HW5** and for the compile-time
message codec you build in Week 6.

## Setup
No stub for this week — we work in a scratch file and lean on the STL.
```bash
cd project-starter
cat > /tmp/w5.cpp <<'EOF'
#include <cstdio>
int main() { std::puts("week5 scratch"); return 0; }
EOF
g++ -std=c++17 -O2 /tmp/w5.cpp -o /tmp/w5 && /tmp/w5
```
Keep `/tmp/w5.cpp` open; we grow it step by step. Compile after every step —
template errors are much cheaper to read one change at a time.

## Walk-through — we build this together

### 1. A function template = a code *stamp*
```cpp
template <class T>
T max_of(T a, T b) { return a < b ? b : a; }
```
`max_of(3, 4)` stamps out an `int` version; `max_of(1.5, 2.5)` stamps a `double`
version. Both are as fast as if you'd hand-written them — **the template is
resolved and inlined at compile time**. There is no runtime cost, no vtable, no
type tag. Verify with `-O2 -S` that the call vanishes into a `cmov`.

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
One definition, works for `RingStat<int>`, `RingStat<double>`, `RingStat<Price>`.
Note we lean on the STL — `std::vector`, `std::sort`, `std::accumulate` — instead
of reinventing it. `T{}` value-initializes the accumulator to the right zero for
whatever `T` is.

### 3. Variadic template + fold expression
Sum any number of arguments of any numeric type, in one line:
```cpp
template <class... Ts>
auto sum(Ts... xs) { return (xs + ...); }        // C++17 unary right fold
```
`(xs + ...)` expands to `x0 + (x1 + (x2 + ...))` **at compile time** — no loop, no
`va_args`, no array. `sum(1, 2, 3)` and `sum(1.0, 2.5)` each compile to a single
folded expression. Show the expansion:
```cpp
auto a = sum(1, 2, 3, 4);       // int   -> 10
auto b = sum(1.5, 2.5, 3.0);    // double-> 7.0
```

### 4. Compile-time branching with `if constexpr`
One codec, two encodings, chosen by the compiler — the dead branch is *not even
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
With a plain `if`, both branches must compile for every `T` (the float branch
would be nonsense for a pointer type). With `if constexpr`, the false branch is
**discarded** — this is what lets one template body serve genuinely different
types.

### 5. SFINAE — constrain what may instantiate
Before C++20 concepts, we restrict a template to, say, arithmetic types so a bad
call is a clean compile error, not a deep template spew:
```cpp
template <class T,
          class = std::enable_if_t<std::is_arithmetic_v<T>>>
T twice(T x) { return x + x; }
```
`twice(3)` and `twice(2.5)` compile; `twice("no")` is removed from the overload
set (SFINAE — "substitution failure is not an error"), giving a short "no matching
function" message.

### 6. Why this matters for the bot
Your message codec will be a template over the message type. Because everything is
resolved at compile time, the encoder for a `NewOrder` and the encoder for a
`Cancel` are each **fully inlined, branch-free** paths — no runtime `switch` on a
type tag on the hot path. Speed comes from *not deciding at runtime*.

## Your turn
1. Add a variadic `avg(...)` built on your `sum(...)`:
   `return sum(xs...) / static_cast<double>(sizeof...(xs));` (note `sizeof...`).
2. Extend `RingStat<T>` with `T max() const` using `std::max_element` — keep it
   generic.
3. Add an `if constexpr` branch to `encode` for a `bool` flag type and print the
   byte counts for an `int`, a `double`, and a `bool`.
4. This feeds **HW5**: a small generic + variadic utility with the dead paths
   compiled out. Prove zero cost by diffing `-O2 -S` output for two
   instantiations.

## Checkpoint
```bash
g++ -std=c++17 -O2 -Wall -Wextra /tmp/w5.cpp -o /tmp/w5 && /tmp/w5
```
Expected: clean compile (no warnings), and printed results —
`sum(1,2,3,4)=10`, `avg` correct, and per-type encode byte counts. Then run the
autograder to confirm nothing else regressed:
```bash
make test
```

## Links
Week-5 deck · HW5 · (feeds the Week-6 compile-time codec, Project **Phase 1**).
