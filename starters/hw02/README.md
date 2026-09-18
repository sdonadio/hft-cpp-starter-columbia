# HW 2 — Pointers, references & the cost of a copy

**10 points · individual · due Saturday 26 September 2026, 11:59 pm ET · submit on CourseWorks**

Week 2 said: *a reference is an alias, a pointer is an address, a copy costs
bytes, and a cache miss costs ~100 ns.* This homework makes you prove all four
with a clock instead of taking my word for it.

---

## Build and run — do this first, before you write anything

```bash
cd starters/hw02
make                 # == g++ -std=c++17 -O2 -Wall hw2.cpp -o hw2 && ./hw2
```

It compiles as shipped and prints all four tables. The numbers are wrong
(`WRONG`, `0.000`, `1.00x`) because the four functions are empty. That is your
red signal. Your job is to turn them green.

> Note what the shipped, empty version prints for Table 2: **the same time for
> by-value and by-const-ref**. An empty function has nothing to copy, so there
> is nothing to measure. Remember that shape — an identical p50 on both rows is
> exactly what an *elided* benchmark looks like too, and it is the most common
> way this homework loses points.

You will also want, once, for your write-up:

```bash
make debug           # -O0. Compare the numbers. Then never report them.
```

---

## What to fill in

Four TODO blocks in `hw2.cpp`. **The signatures are fixed — do not change them.**

| TODO | Function | Points |
|------|----------|--------|
| 1a / 1b | `void swap_ref(int&, int&)` and `void swap_ptr(int*, int*)` | 3 |
| 2a / 2b | `double sum_by_value(Big)` and `double sum_by_cref(const Big&)` | 3 |
| 3a / 3b | `long sum_vector(const std::vector<int>&)` and `long sum_list(const Node*)` | 2 |
| 4 | nothing to code — report the build, the method and your machine | 2 |

Everything else (the percentile harness, the data setup, the shuffled list, the
four tables) is written for you. Read the harness — you are expected to be able
to explain what it does.

In `sum_by_value` and `sum_by_cref` the **bodies must be identical**. The only
difference between them is how the argument arrives; if you also change the
arithmetic you are no longer measuring the copy.

---

## What to report

A short `README.md` or PDF alongside your `hw2.cpp`. It needs:

### 1. Your machine and build (2 pts live here)

State all of it — a benchmark without a machine is not a result:

- CPU (model, core count), RAM, OS
- Compiler and exact version (`g++ --version`)
- The exact flags you used: `-std=c++17 -O2 -Wall`
- That you used `std::chrono::steady_clock`, that you warmed up, and how many
  samples you took
- Anything you did about noise (closed other apps, laptop on power, etc.)

### 2. The four tables

Paste the program's own output, or retype it. Table 2 and Table 3 must show
**p50, p99 and p99.9** — a mean alone is not a distribution and loses a point.

### 3. Explanation A — `swap_ref` vs `swap_ptr` (part of the 3 pts)

A short paragraph. Cover, in your own words:

- What the callee actually receives in each case, and what it costs
- Why `swap_ptr` needs `*` and `swap_ref` does not
- Why swapping the *pointers* inside `swap_ptr` does nothing to the caller
- `nullptr` is expressible for the pointer version and not for the reference
  version — say what that means for a caller, and whether you added a null
  check (either choice is fine; say which and why)
- Which one you would use in an HFT hot path and why

### 4. Explanation B — contiguous vs pointer chasing (part of the 2 pts)

Same amount of arithmetic, same number of `int`s summed, very different time.
Explain *why*, in terms of the week-2 memory hierarchy:

- cache lines: how many `int`s ride along in one 64-byte line vs how many
  useful bytes you get from a 16-byte `Node`
- the hardware prefetcher: why it helps the vector and cannot help the chase
- the dependent-load chain: each `p->next` must complete before the next hop
  can even start, so misses serialise instead of overlapping
- where this shows up in our course: it is the reason the order book (HW 7) is
  a flat array and not a `map` of nodes

Also say something about the **copy** result: is the by-value overhead roughly
what you would predict from `sizeof(Big)` and your machine's memory bandwidth,
or not? Order-of-magnitude reasoning is enough.

---

## Submission checklist (this is the rubric)

- [ ] **(3)** `swap_ref` and `swap_ptr` both print `OK` in Table 1, and the
      difference between references and pointers is explained in prose.
- [ ] **(3)** `sum_by_value` vs `sum_by_cref` benchmarked, both correctness-checked
      as equal and non-zero, with the p50/p99/p99.9 table and the ratio.
- [ ] **(2)** Contiguous vs pointer-chasing traversal of the **same** 1,048,576
      elements, measured, with the cache explanation.
- [ ] **(2)** Every timing from an `-O2` (or `-O3`) build, with warm-up,
      percentiles reported, and your machine + compiler + flags stated.
- [ ] `hw2.cpp` and the write-up attached to CourseWorks (`.cpp` + `.md`/`.pdf`,
      or a `.zip`, or a link to your repo — a repo link still needs the write-up).

**Not graded:** false sharing. We do it in the week-2 lab; it is not on this
task list.

---

## Traps that cost points every year

1. **No `doNotOptimize`.** `-O2` deletes a result nobody uses, and you report
   0 ns or two identical rows. The harness sinks every result for you — do not
   remove it, and if you write extra benchmarks of your own, sink those too.
2. **Swapping the pointers, not the pointees.** `int* t = a; a = b; b = t;`
   inside `swap_ptr` reorders two local copies of two addresses. The caller
   never sees it. Table 1 will say `WRONG`.
3. **Reporting `-O0` numbers.** A debug build hides the effect you were asked to
   measure behind its own overhead. `make debug` exists so you can see this
   happen once, on purpose, and write a sentence about it.
4. **A mean instead of percentiles.** One number is a lie; the tail is the
   grade in this course.
5. **Different bodies in the two `sum_*` functions.** Then the gap is your
   arithmetic, not the copy.
6. **"The list is slower because pointers are slow."** Pointers are not slow —
   a dereference is one instruction. *Missing the cache* is slow. Say that.
