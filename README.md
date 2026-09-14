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

## Connecting to the class arena

```bash
make client                                   # builds hft/cpp_client/build/hft_bot (TLS on)
make register CODE=<class code> NAME="Team"   # one-time: writes .env (token, TEAM_ID, EXCHANGE_URL)
make run                                      # starts your bot with .env
```

`.env` is your team's secret token — it is git-ignored; never paste it in Ed or commit it.
Dashboard: https://algoarenafin.duckdns.org (MARKET · FLOW · LATENCY).

## Latency benchmark (Project Phase 0 and every phase after)

Two stdlib-only Python tools, used by the Phase 0 deliverable:

```bash
# Offline, reproducible: a fixed synthetic tape (seed 12345) is piped through
# your bot over stdin/stdout and every tick-to-order is clocked.
python3 scripts/latency_replay.py --self-test --cmd "hft/cpp_client/build/hft_bot --replay"

# Live: your bot prints one "LAT <symbol> <micros>" line per book update to
# stderr. Capture it, then rank/summarise the tail.
make run 2> logs/lat_live.log      # Ctrl-C after a few minutes
python3 scripts/latency_report.py logs/lat_live.log
```

The **offline p50 / p99 / p99.9 from `latency_replay.py` are the official
numbers** you report in each phase: everyone runs the same tape on their own
machine, so a phase's "measured improvement" is your new offline numbers against
your Phase 0 offline baseline. The live LATENCY board is for visibility and
sanity (it moves with the market and is not reproducible); it is not graded by
rank. `--self-test` alone (no `--cmd`) benchmarks a built-in Python echo bot so
you can check the harness before your client builds. Pass a recorded
`sessions/session_*.jsonl` file instead of `--self-test` if one is provided.
