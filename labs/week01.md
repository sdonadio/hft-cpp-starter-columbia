# Week 1 Lab — HFT Landscape & Your Arena
**Format:** in-class, guided (~45–60 min).  **Repo:** the HFT starter.

## Goal
Get oriented: clone and build the arena C++ client, connect and land on the
LATENCY board, run `make test` to see the whole term's work laid out in red,
and hand-compute the core order-book metrics (mid, spread, microprice, OBI)
from a snapshot in a scratch `.cpp`.

## Setup
Open a terminal at the repo root and confirm your toolchain (C++17 + a recent
`g++`/`clang++`, plus CMake ≥ 3.16 for the client):

```bash
g++ --version        # need C++17
cmake --version      # need >= 3.16 for the arena client
ls include tests     # the stubs you'll implement + the autograder
```

You should see the header stubs in `include/` (`pool.hpp`, `order_book.hpp`,
`spsc_ring.hpp`, …) and the graders in `tests/`. Don't edit anything in
`tests/` — that's the autograder.

## Walk-through — we build this together

### 1. Read the map: `make test`
Run the full autograder before writing a single line:

```bash
make test        # -> python3 tests/run_ci.py, writes report.json
```

Almost everything is **red**. That's intentional: each failing test is a
challenge you'll implement over the term. Every stub in `include/` *compiles*
but *fails* until you make it real. Peek at the score:

```bash
cat report.json | python3 -m json.tool | head -20
```

`"score"` vs `"max_score"` (e.g. 25 / 220) is your term-long progress bar.
Note the two kinds of signal in that file: **tests** (correctness, pass/fail)
and **metrics** (speed, ns/op) — HFT grades you on both. Keep this number in
mind; you'll watch it climb.

### 2. Build the arena C++ client
Your bot is *just another WebSocket client* to the (unchanged Python) arena
exchange — you write C++, you're graded on tick-to-trade latency. Build it
(see `docs/HFT_CPP_CLIENT.md`):

```bash
cd hft/cpp_client
cmake -B build          # first run fetches IXWebSocket + nlohmann/json (needs internet)
cmake --build build     # -> build/hft_bot
```

For TLS venues (`wss://`) add `-DHFT_USE_TLS=ON` to the configure step.

### 3. Connect and get on the LATENCY board
Point the bot at the class exchange and run it:

```bash
TEAM_ID=<your_team> EXCHANGE_HOST=<arena_host> EXCHANGE_PORT=<port> ./build/hft_bot
# waits for SESSION_OPEN, then trades; prints:  [latency] tick->order = N us
```

The one hook that matters is the hot path:

```cpp
// include/hft_bot.hpp — you override this
void on_book(const std::string& sym, double bid, double ask,
             double mid, double microprice, double obi) override {
    // decision goes here; orders sent from here are latency-stamped for you
}
```

Every order you send from `on_book` is timestamped from `book_snapshot` decode
to send. That µs number is what puts you on the **LATENCY** dashboard tab, and
you're ranked on **p50 / p99 / p99.9** — you "win on the tail, not the mean."

### 4. Where the metrics come from
The arena hands you `mid`, `microprice`, and `obi` pre-computed — but you must
understand them to trade on them. In a scratch file, compute them yourself.
Create `scratch/book_metrics.cpp`:

```cpp
#include <cstdio>
int main() {
    // one snapshot: best bid/ask and the sizes resting there
    double bid_px = 100.00, ask_px = 100.02;
    double bid_sz = 800,    ask_sz = 200;

    double mid    = (bid_px + ask_px) / 2.0;              // 100.01
    double spread = ask_px - bid_px;                      // 0.02 (2 cents)

    // microprice: mid weighted TOWARD the thinner side (size-weighted).
    // note the cross-weighting — bid size pulls price up toward the ask.
    double micro  = (ask_px * bid_sz + bid_px * ask_sz) / (bid_sz + ask_sz);

    // order-book imbalance in [-1, 1]: +1 = all bids, -1 = all asks.
    double obi    = (bid_sz - ask_sz) / (bid_sz + ask_sz);

    printf("mid=%.4f spread=%.4f micro=%.4f obi=%.4f\n",
           mid, spread, micro, obi);
    return 0;
}
```

```bash
g++ -std=c++17 -O2 scratch/book_metrics.cpp -o /tmp/bm && /tmp/bm
# mid=100.0100 spread=0.0200 micro=100.0160 obi=0.6000
```

The **why**: with 800 lots bid vs 200 offered, buyers dominate — `obi = +0.6`
and the microprice (100.016) sits *above* the mid, leaning toward the ask.
Microprice is a better short-horizon predictor of the next mid than the mid
itself; that lean is the edge HFT strategies trade on.

## Your turn
Extend `scratch/book_metrics.cpp` into a tiny function and sanity-check it
against three snapshots — this is exactly the metric layer HW1 asks you to
formalize:

```cpp
struct Metrics { double mid, spread, micro, obi; };
Metrics compute(double bp, double bs, double ap, double as);
```

1. Add the case `bid_sz == ask_sz` — confirm `obi == 0` and `micro == mid`.
2. Add a crossed/locked guard: what should `spread` do if `ask_px <= bid_px`?
3. Print all four metrics for: balanced (500/500), bid-heavy (900/100),
   ask-heavy (100/900). Verify the microprice leans the way OBI says.

## Checkpoint
- `make test` runs and writes `report.json` (mostly red — expected).
- `hft/cpp_client/build/hft_bot` exists and connects; you saw a
  `[latency] tick->order = N us` line and your team on the LATENCY tab.
- Your scratch program prints the four metrics and the balanced case gives
  `obi = 0`, `micro = mid`.

## Links
Week-1 lecture deck · HW1 (order-book metrics) · Project overview
(`docs/HFT_CPP_CLIENT.md`)
