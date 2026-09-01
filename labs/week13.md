# Week 13 Lab — Low-Latency Design (SIMD, kernel bypass, colocation)
**Format:** in-class, guided (~45–60 min).  **Repo:** the HFT starter.

## Goal
Feel the difference a compiler makes. Build the **frozen** kernel in
`starters/hw13/kernel.cpp` at `-O0` vs `-O3 -march=native -funroll-loops`,
measure the speedup (expect roughly **6.5×**), and read `perf stat` to see
*why* — auto-vectorization (SIMD) and better branch behavior. Then demo an AVX
intrinsic reduction, `__builtin_prefetch`, and `alignas` by hand, and connect
it all to the **colocation** trade-off you make in the arena. This lab **is
HW13** — you optimize **flags only**, you never touch the kernel source.

## Setup
Open a terminal at the repo root. Confirm your compiler and peek at the file
you are (not) allowed to edit:

```bash
g++ --version                 # need C++17
sed -n '1,12p' starters/hw13/kernel.cpp   # note: ">>> DO NOT MODIFY THIS FILE <<<"
```

The kernel is a floating-point reduction with a data-dependent branch, run over
a 64K array 3000 times. It is deliberately shaped to reward `-O2/-O3`,
`-march=native`, and `-funroll-loops`. Your only lever is the command line.

## Walk-through — we build this together

### 1. Establish the baseline (`-O0`)
Turn *off* the optimizer so we can see what "just the code" costs:

```bash
g++ -O0 starters/hw13/kernel.cpp -o slow
./slow
# checksum=... time_ms=NNN.NNN     <-- write this number down
```

`-O0` emits a literal translation: every `a[i]`, every `+ 0.5f`, every
`std::sqrt` is a separate instruction touching memory, one float at a time. The
checksum is what we must keep identical — if a faster build prints a different
checksum, the optimization changed the math and doesn't count.

### 2. The winning build (`-O3 -march=native -funroll-loops`)
Now let the compiler do its job:

```bash
g++ -O3 -march=native -funroll-loops starters/hw13/kernel.cpp -o fast
./fast
# checksum=... time_ms=nn.nnn      <-- ~6.5x smaller, same checksum
```

Compute the speedup: `time_ms(slow) / time_ms(fast)`. You should land near
**6.5×**. Three flags did that, and it's worth knowing which did what:

- `-O3` — inlines `kernel`, keeps `acc` in a register, strength-reduces the
  index math, and *enables the auto-vectorizer*.
- `-march=native` — lets the vectorizer target *this CPU's* widest SIMD
  (AVX2/AVX-512), so it processes 8–16 floats per instruction instead of 1.
- `-funroll-loops` — unrolls the inner loop so there are fewer branch/counter
  ops per float and more independent work for the pipeline.

### 3. Prove *why* with `perf stat`
Don't guess — measure the microarchitecture. Compare the two binaries:

```bash
perf stat -d ./slow
perf stat -d ./fast
```

Read three lines side by side:

- **instructions** — `fast` retires *far fewer* (SIMD does 8–16 floats per
  instruction; unrolling removes loop overhead).
- **insn per cycle (IPC)** — `fast` is higher: the pipeline stays full instead
  of stalling.
- **branches / branch-misses** — the `if (x > 0.0f)` is the data-dependent
  branch the header warns about. Watch the miss *rate*; this is the hook for
  PGO below.

> No `perf` on your machine (macOS / restricted VM)? Use
> `g++ -O3 -march=native -funroll-loops -fopt-info-vec starters/hw13/kernel.cpp -o fast`
> and look for `loop vectorized` lines — that's the compiler telling you SIMD
> fired. On macOS, `sudo dtrace`/Instruments or just the `time_ms` delta stands
> in for `perf`.

### 4. (Optional) squeeze the branch with PGO
The branch is *predictable* on this data, so tell the compiler the profile:

```bash
g++ -O3 -march=native -funroll-loops -fprofile-generate starters/hw13/kernel.cpp -o gen
./gen                                   # writes kernel.gcda (the profile)
g++ -O3 -march=native -funroll-loops -fprofile-use starters/hw13/kernel.cpp -o pgo
./pgo                                   # branch laid out for the common path
```

Re-run `perf stat -d ./pgo` and watch **branch-misses** drop. Same idea in the
arena: your hot path branches (quote? cancel? skip?) are also biased, and the
compiler can lay them out if you feed it a profile.

### 5. Demo — the intrinsics the compiler used, by hand
You will normally *let the compiler* vectorize. But seeing one AVX reduction
demystifies what `-march=native` bought you. In a scratch file
`scratch/simd_demo.cpp`:

```cpp
#include <immintrin.h>   // AVX
#include <cstdio>
#include <vector>

// Sum an array 8 floats at a time with one AVX accumulator, then reduce.
float simd_sum(const std::vector<float>& v) {
    __m256 acc = _mm256_setzero_ps();          // 8 lanes of 0.0f
    std::size_t i = 0;
    for (; i + 8 <= v.size(); i += 8)
        acc = _mm256_add_ps(acc, _mm256_loadu_ps(&v[i]));  // 8 adds, 1 instr
    float lanes[8];
    _mm256_storeu_ps(lanes, acc);
    float s = 0.f;
    for (float x : lanes) s += x;              // horizontal reduce of 8 lanes
    for (; i < v.size(); ++i) s += v[i];       // scalar tail
    return s;
}

int main() {
    std::vector<float> v(1000, 1.5f);
    std::printf("simd_sum=%.1f (expect 1500.0)\n", simd_sum(v));
}
```

```bash
g++ -std=c++17 -O2 -mavx scratch/simd_demo.cpp -o /tmp/simd && /tmp/simd
```

The **why**: `_mm256_add_ps` adds 8 floats in one instruction. That 8× width is
exactly what the auto-vectorizer produced for the kernel when you passed
`-march=native` — you just didn't have to write it.

### 6. Demo — `alignas` and `__builtin_prefetch`
Two more low-latency staples. Aligned data lets the CPU use aligned SIMD loads
and keeps a struct from straddling a cache line; prefetch hides memory latency
by asking for the *next* line before you touch it:

```cpp
#include <vector>
alignas(64) float lut[16];         // 64-byte aligned = one cache line, SIMD-friendly

double sum_prefetched(const std::vector<float>& a) {
    double s = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        __builtin_prefetch(&a[i + 16], 0, 3);   // pull the line we'll need soon
        s += a[i];
    }
    return s;
}
```

The **why**: a last-level-cache miss costs ~100+ ns — an eternity when your
tick-to-trade budget is single-digit microseconds. `alignas(64)` avoids
split-line penalties; prefetch turns a stall into overlapped work. Use them
surgically on the hot path, then *measure* — a wrong prefetch is just wasted
bandwidth.

### 7. Connect it to the arena: colocation economics
SIMD and LTO shrink *compute*; **colocation** shrinks *wire time* — the physics
you cannot compile away. In `docs/HFT_CPP_CLIENT.md` the harness applies a
per-order latency add-on: `LATENCY_MS_COLOCATED` (small, you paid to sit next to
the matching engine) vs `LATENCY_MS_DEFAULT` (large, you're across the network).
The trade is real money for real nanoseconds:

- Colo only helps if your *software* tail is already tight — a 200 µs GC-style
  hiccup dwarfs any wire saving. That's why HW14 (kill the tail) comes next.
- It shifts *p50 and the tail together*, so it's most valuable for latency-arb
  and queue-position races (Week 15's tournament), less so for slow signals.

## Your turn
Still **flags only** — do not edit `kernel.cpp`.

1. Build a matrix of times: `-O0`, `-O2`, `-O3`, `-O3 -march=native`,
   `-O3 -march=native -funroll-loops`, and add `-flto`. Record `time_ms` and
   confirm the **checksum never changes**. Which single flag gave the biggest
   jump on *your* CPU?
2. For your best build, run `perf stat -d` and quote the drop in
   **instructions** and **branch-misses** vs `-O0` — one sentence each on why.
3. Try `-ffast-math`. It's faster — but does the checksum still match? Explain
   in one line why `-ffast-math` is *not* free (reassociation changes FP
   results) and whether you'd ship it in a pricing kernel.

## Checkpoint
- `./fast` runs ≥ ~6× faster than `./slow` with an **identical checksum**.
- You can name what `-O3`, `-march=native`, and `-funroll-loops` each did, and
  point to the `perf stat` line that proves it.
- `scratch/simd_demo.cpp` prints `1500.0`, and you can explain how its one
  `_mm256_add_ps` mirrors what the auto-vectorizer did to the kernel.
- You can state, in one sentence, when paying for colocation is worth it and
  when it isn't.

## Links
Week-13 lecture deck · HW13 (build optimization, flags only:
`starters/hw13/kernel.cpp`) · Project Phase 5 (`project/README.md`) ·
Colocation model (`docs/HFT_CPP_CLIENT.md`)
