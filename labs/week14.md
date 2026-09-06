# Week 14 Lab — Profiling & the Tail
**Format:** in-class, guided (~45–60 min).  **Repo:** the HFT starter.

## Goal
Build and run `starters/hw14/tail.cpp` and watch **p99.9 ≫ p50** — a healthy
median hiding an ugly tail. Profile it with `perf record` / `perf report` (a
flame graph), find the pathology (a **heap allocation on the hot path** plus an
**O(window) recompute** every tick), fix it in a copy (`tail_fixed.cpp`) with an
**incremental update over a reserved ring**, and watch the tail *collapse*. We
also wire up the sanitizers (ASan/UBSan/TSan). This lab **is HW14** — you fix
the systems cause, not the compiler flags.

## Setup
At the repo root, read the header so you know the target metric, then build with
the *same* flags the assignment specifies (`-O2`, so we're profiling the
*algorithm*, not a missing optimizer):

```bash
sed -n '1,14p' starters/hw14/tail.cpp        # p50/p99/p99.9/max in ns; find the tail
g++ -O2 -std=c++17 starters/hw14/tail.cpp -o tail
./tail
# p50=...ns  p99=...ns  p99.9=...ns  max=...ns  (sink=...)
```

Note the shape: `p50` is small and steady, but `p99.9` and `max` are a large
multiple of it. In HFT that tail *is* the product — you win races on p99.9, not
the mean. Our job is to pull the tail down without changing what the program
computes (the `sink` must stay the same).

## Walk-through — we build this together

### 1. Read the tail, not the mean
Run it a few times. `p50` barely moves; `p99.9`/`max` jump around. That
variance is the tell: something on the hot path occasionally does a *lot* more
work than usual. A rolling average should be constant-time per tick — so why
would one tick cost 50× another?

### 2. Form a hypothesis from the code
Open `signal()` in `starters/hw14/tail.cpp`. Two smells, both on the per-tick
hot path:

```cpp
std::vector<double> scratch;                 // (a) allocates every single tick
for (std::size_t k = lo; k <= i; ++k) scratch.push_back(prices[k]);
double s = 0.0;
for (double v : scratch) s += v;             // (b) O(window) work every tick
return s / scratch.size();
```

- **(a) heap traffic:** a fresh `std::vector` every tick means `malloc`/`free`
  on the hot path. Most calls hit the allocator's fast path (cheap), but *some*
  trigger a real `mmap`/arena refill or `push_back` growth reallocation — those
  are your p99.9 spikes.
- **(b) recompute:** summing `window` elements every tick is O(256) work you
  redo from scratch, when a rolling sum only needs +1 add and −1 subtract.

Hypothesis: (a) is the tail; (b) is a constant-factor drag. Let's prove (a) with
the profiler before we "fix" anything.

### 3. Profile with `perf record` → `perf report`
Sample where the CPU actually spends time:

```bash
perf record -g ./tail          # -g captures call stacks
perf report --stdio | head -40 # or: perf report  (interactive TUI)
```

You'll see time under `signal`, and crucially under the **allocator**
(`malloc`, `operator new`, `free`, `_int_malloc`, sometimes `mmap`/`munmap`).
That allocator time under a function that "just averages numbers" is the
smoking gun. For a flame graph, fold the stacks:

```bash
perf script | stackcollapse-perf.pl | flamegraph.pl > /tmp/tail.svg   # Brendan Gregg's tools
```

`flamegraph.pl` is the *script* that turns folded stacks into the SVG —
`flamegraph.svg` is what comes out the other end. Clone
`github.com/brendangregg/FlameGraph` and put both `.pl` files on your `PATH`.

> **Profiling your bot instead of `tail`?** `hft_bot --replay` takes **no file
> argument** — it reads one `book_snapshot` JSON per line on **stdin** and writes
> one response line back. That is exactly how the grading harness drives it
> (`python scripts/latency_replay.py --latest --cmd './build/hft_bot --replay'`).
> To hold it under `perf`, make a tape first and redirect it in:
>
> ```bash
> jq -c 'select(.msg.type=="book_snapshot").msg' \
>    sessions/session_*.jsonl > /tmp/tape.jsonl      # unwrap the recording
> perf stat -d   ./build/hft_bot --replay < /tmp/tape.jsonl
> perf record -g ./build/hft_bot --replay < /tmp/tape.jsonl
> ```

> No `perf`/FlameGraph (macOS, restricted VM)? Use `valgrind --tool=callgrind
> ./tail` + `callgrind_annotate`, or Instruments' *Allocations* +
> *Time Profiler* on macOS. Even a quick `ltrace -c ./tail` will show the
> `malloc`/`free` call counts. The point is the same: attribute the tail to the
> allocator.

### 4. Fix it in a copy — reserved buffer + incremental update
Copy the file and change *only* the hot path. Two moves:

1. **Kill the allocation:** keep one buffer, `reserve()`d once outside the loop,
   reused every tick — a ring of the last `window` prices. No per-tick `new`.
2. **Kill the recompute:** maintain a running `sum`; on each tick add the new
   price and subtract the one that fell out of the window. O(1), not O(window).

```bash
cp starters/hw14/tail.cpp starters/hw14/tail_fixed.cpp
```

Replace the per-tick `signal()` with an incremental roller that owns its state:

```cpp
// tail_fixed.cpp — O(1) per tick, zero allocation on the hot path.
struct RollingMean {
    std::vector<double> buf;   // reserved ONCE; reused as a ring
    std::size_t window, count = 0, head = 0;
    double sum = 0.0;

    explicit RollingMean(std::size_t w) : buf(w, 0.0), window(w) {}  // one alloc, up front

    double push(double px) {
        if (count == window) sum -= buf[head];   // evict the oldest
        else                 ++count;
        sum += px;                                // admit the newest
        buf[head] = px;
        head = (head + 1 == window) ? 0 : head + 1;
        return sum / count;
    }
};
```

Then the main loop becomes: construct one `RollingMean roll(WINDOW);` *before*
timing, and inside the timed loop call `sink += roll.push(prices[i]);`. No
`std::vector` is created per tick; no inner sum loop remains.

> Note the tiny semantic difference: the original window is *inclusive* of `i`
> and grows from 1..window at the start. Match the warm-up (`count` ramps to
> `window`) so your `sink` lands on the same value — the header says don't change
> what it computes. If `sink` differs, you changed the math; fix the warm-up.

Build and compare:

```bash
g++ -O2 -std=c++17 starters/hw14/tail_fixed.cpp -o tail_fixed
./tail_fixed
# p50 similar-or-better, and p99.9/max COLLAPSE toward p50
```

### 5. See the tail collapse
Put the two side by side and quote the ratio:

```bash
echo "before:"; ./tail
echo "after: "; ./tail_fixed
```

The win isn't a smaller mean — it's a **flatter distribution**. p99.9 drops
because there is no longer an allocator call that occasionally goes slow, and
every tick now does the *same* O(1) work. That is the entire game: make the hot
path do identical, bounded work every time so the tail can't blow out.

### 6. Sanitizers — catch the bugs that *become* tails
A rewrite of a ring buffer is exactly where off-by-one and UB hide. Before you
trust the numbers, run the sanitizers (they instrument the binary; slow, but
they find real bugs):

```bash
g++ -O1 -g -fsanitize=address,undefined starters/hw14/tail_fixed.cpp -o tail_asan && ./tail_asan
# ASan: out-of-bounds on buf[head], use-after-free.  UBSan: signed overflow, bad shifts.
```

For your Phase-3 threaded pipeline, add **TSan** (data races between producer
and consumer): `g++ -O1 -g -fsanitize=thread ... && ./...`. Rule of thumb: a
race or a UB is a *latency bug waiting to happen* — it corrupts state, which
later triggers a slow path or a crash right in the tail.

## Your turn
1. Produce a **before/after table**: p50, p99, p99.9, max for `tail` vs
   `tail_fixed`, and the p99.9 improvement factor. Confirm `sink` matches (same
   computation).
2. From your profile, name the top allocator symbol you eliminated (e.g.
   `operator new` / `_int_malloc`) and explain in one line why it only showed up
   in the *tail*, not the median.
3. Run `tail_fixed` under `-fsanitize=address,undefined` and paste the clean
   exit (or fix what it reports). Bonus: what happens to p99.9 if you *don't*
   `reserve`/pre-size `buf` and let it grow? Predict, then measure.
4. Write the one-page changelog the header asks for: cause → fix → measured
   effect.

## Checkpoint
- `./tail` shows p99.9 ≫ p50; your profile attributes the tail to the allocator
  on the hot path.
- `./tail_fixed` produces the **same `sink`** with p99.9/max collapsed toward
  p50 (large improvement factor), via a reserved ring + incremental sum.
- You ran ASan/UBSan on the fixed copy and it's clean.
- You can state the general rule: bounded, identical, allocation-free work per
  tick is what flattens the tail.

## Links
Week-14 lecture deck · HW14 (profile & kill the tail:
`starters/hw14/tail.cpp`) · Project Phase 6 (`project/README.md`) ·
Latency grading (`docs/HFT_CPP_CLIENT.md`)
