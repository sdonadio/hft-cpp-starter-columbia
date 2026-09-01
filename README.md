# HFT in C++ — Student Starter Repo

Everything you need for the weekly coding challenges and the semester project.
Use this template, make your repo **private**, and add the instructor as a
collaborator.

## Layout

```
include/     ← the header STUBS you implement (one per challenge — start here)
tests/       ← the autograder (do not edit): drivers + bench.hpp + run_ci.py
starters/    ← provided code you build on (hw13 kernel, hw14 tail)
project/     ← the 8-phase AlgoArena project (README + phase checklist)
labs/        ← the in-class lab guides (week01 … week15)
docs/        ← how to get & build the arena C++ client
.github/     ← CI: runs the autograder on every push
```

## How grading works

Push your code and **GitHub Actions runs the autograder automatically**. On the
run's **Summary** tab you'll see per-test pass/fail and your **ns/op** metric for
each challenge. The exact same `report.json` is what the instructor collects — so
**the grade you see in CI is the grade**. Nothing runs on the instructor's machine.

Run it locally too:

```bash
make test          # = python3 tests/run_ci.py  (grades whatever you've implemented)
```

## Weekly challenges → which file to edit

| HW | File to implement | What it checks |
|----|-------------------|----------------|
| 4  | `include/pool.hpp` | O(1) pool alloc/free, placement new, ns/alloc |
| 7  | `include/order_book.hpp` | flat book best bid/ask + cancel; symbol map; ns/op |
| 8  | `include/rolling_counter.hpp` | sliding-window count + expiry; ns/op |
| 10 | `include/spsc_ring.hpp` | SPSC lock-free ring + ThreadSanitizer |
| 11 | `include/fix_parser.hpp` | single-pass FIX NewOrder parse; ns/op |
| 12 | `include/u64toa.hpp` | fast uint64→decimal; ns/op vs std |
| 13 | `starters/hw13/kernel.cpp` | **flags only** — do not edit the file; tune the build |
| 14 | `starters/hw14/tail.cpp` | profile & fix the latency tail (see the HW) |

Each stub compiles but fails its tests until you implement it — that's your
red/green signal. The exact interface is fixed by the autograder (and repeated in
each stub's comments); keep the signatures as given.

## Project

`project/` holds the 8-phase AlgoArena HFT system project. `include/spsc_ring.hpp`
(Phase 3) and `include/shm_ring.hpp` (Phase 4) are graded here too. See
`project/README.md` and `docs/HFT_CPP_CLIENT.md`.

## Requirements

A C++17 compiler (`g++`/`clang++`), `cmake` (for the arena client), `python3`,
and — for local runs — `perf`/ThreadSanitizer where a challenge asks for them.
CI installs all of this for you.
