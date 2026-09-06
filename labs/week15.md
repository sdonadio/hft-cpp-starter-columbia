# Week 15 Lab — Latency Arbitrage, Multi-Venue & the Tournament
**Format:** in-class, guided (~45–60 min).  **Repo:** the HFT starter.

## Goal
Bring the term together. Sketch the two strategies the finale rewards — a
**cross-venue stale-quote detector** (given two venues' top of book, decide the
arb) and a **queue-aware requote** — then walk the tournament scenario
(`hft/scenarios/week10.json`: multi-venue, auctions, a tight `order_quota`, an
FPGA IPO) and the **composite grade** you'll be scored on: p50/p99/p99.9 **+** queue position
**+** fill rate **+** markout. This is final-project **demo prep** (Project
Phase 7); you'll leave with the decision logic to drop into your bot's
`on_book`.

## Setup
Make sure your Phase-0..6 bot still builds and connects, and re-read the client
hooks — everything today lives in `on_book`, `on_ack`, and `on_queue`:

```bash
cd hft/cpp_client && cmake --build build      # -> build/hft_bot
sed -n '26,37p' ../../docs/HFT_CPP_CLIENT.md  # the hooks: on_book / on_ack / on_queue / on_fill
```

Keep the offline harness handy — it's the same replay used to grade you, and it
prints the exact metrics the tournament composes. `--replay` takes **no file
argument**: the harness feeds book snapshots to your bot on **stdin**, one JSON
per line, and times the response.

```bash
python scripts/latency_replay.py --latest \
       --cmd "hft/cpp_client/build/hft_bot --replay"   # p50/p99/p99.9 + throughput
```

(`scripts/latency_report.py` is the *live-session* leaderboard — it parses the
`LAT <symbol> <micros>` lines your bot logs during a real session, e.g.
`python scripts/latency_report.py "logs/lat_*.log"`. It is not the replay
report.)

## Walk-through — we build this together

### 1. The finale scenario: read `hft/scenarios/week10.json`
The tournament runs **`hft/scenarios/week10.json`** — the HFT-course scenario
set, not the Python course's `teacher/season/` one. Point the exchange at it:

```bash
# read the rules you are about to be graded under:
python -c "import json;d=json.load(open('hft/scenarios/week10.json'));\
print(d['label'],d['order_quota'],d['position_limit'])"
# The HFT tournament — full market structure, every microsecond scored 6 1200

# (the teacher launches the venue with SCENARIO_PATH pointed at that file:)
#   SCENARIO_PATH=hft/scenarios/week10.json python -m exchange.server
```

> **Do not read `teacher/season/week10.json`** — that is the *Python*
> Systematic-Trading finale (`order_quota: 10`, `position_limit: 1000`, no IPO).
> The numbers you are graded on tonight are the HFT ones: **`order_quota: 6`**,
> **`position_limit: 1200`**, `SHORT_LOCATE_CAP: 600`, opening auction 15 ticks /
> closing auction 12, `LULD_BAND_PCT: 0.08`, and the **"Gateway Silicon" (FPGA)
> IPO** at tick 550.

Four features shape your strategy — know them before you code:

- **`multi_venue`** — the *same* symbol trades on two venues at once. Their tops
  of book can momentarily disagree → that gap is the arb (step 2).
- **auctions** — periodic auctions (open/close/halt-exit) match on a single
  clearing price, not continuous. Different logic: submit into the auction, don't
  race the queue.
- **tight `order_quota` (6)** — you get *six* messages per interval. Every quote
  and cancel is scarce; spamming gets you rate-limited (and tanks fill rate).
  Spend messages where edge is highest. `position_limit` is at its widest
  (**1200**), so the constraint is *messages*, not size.
- **FPGA IPO** — "Gateway Silicon" (symbol `FPGA`, 3,000 shares, offer range
  40–46) lists at tick 550: a listing-day primary event (the arena's `on_ipo`),
  priced fast. The point: you're racing participants with a hardware latency
  floor, so your *software* tail (Weeks 13–14) has to already be tight.

### 2. Sketch the cross-venue stale-quote detector
Two venues stream tops of book for one symbol. When venue A's **bid** rises
above venue B's **ask** (net of fees), B is showing a *stale* quote you can lift
on B and hit on A — a locked/crossed cross-venue book. The decision, as a pure
function you call from `on_book(symbol, bid, ask, mid, microprice, obi)` —
note there is **no `venue` argument**; each client sees only *its* venue, so you
cache the other venue's last touch (see the box below):

```cpp
struct Top { double bid, ask, bid_sz, ask_sz; };

// Return +1 = lift B's ask / hit A's bid (buy B, sell A); -1 = the mirror; 0 = no edge.
int cross_venue_arb(const Top& A, const Top& B, double fee_per_side, double tick) {
    // Buy on B at B.ask, sell on A at A.bid — profitable if A's bid clears B's ask + costs.
    double edge_buyB = A.bid - B.ask - 2 * fee_per_side;
    double edge_buyA = B.bid - A.ask - 2 * fee_per_side;
    if (edge_buyB > tick * 0.0) return +1;   // require > 0 edge AFTER fees
    if (edge_buyA > tick * 0.0) return -1;
    return 0;                                 // most ticks: no arb, do nothing
}
```

The **why**: the edge must survive **both sides' fees** and be captured *before
the stale venue requotes* — this is a pure latency race, which is why Weeks
13–14 mattered. Note the tradable size is `min(A.bid_sz, B.ask_sz)`; don't send
for more than rests on the thin side or you'll move against yourself. And under
the tight `order_quota` (6), only fire when `edge > 0` by a margin — a marginal
arb isn't worth a scarce message.

#### The C++ client is ONE VENUE PER PROCESS — do this
`EXCHANGE_URLS` (comma-separated, multi-venue) is **Python-only**
(`broker/config.py`, `trader/arb_trader.py`). The C++ client reads
`EXCHANGE_URL`, or `EXCHANGE_HOST` + `EXCHANGE_PORT`, **once** at startup
(`ClientConfig::from_env()` in `include/arena_client.hpp`) and speaks to exactly
one venue. Setting `EXCHANGE_URLS` on `hft_bot` does nothing. So run **two**:

```bash
# two processes, one venue each; VENUE tells each which slot it owns
VENUE=0 EXCHANGE_URL=ws://localhost:8765 ./build/hft_bot &
VENUE=1 EXCHANGE_URL=ws://localhost:8766 ./build/hft_bot &
```

Both write their own top of book into a **shared touch cache** — your Phase-4
shared-memory ring is exactly the right vehicle — and each reads the *other*
slot to spot the cross:

```cpp
struct Touch { double bid = 0, ask = 0; };
struct Cross { Touch t[2]; };            // [venue] -> that venue's touch

class Pickoff : public arena::HFTBot {
  int    venue_;                         // 0 or 1: MY venue (from $VENUE)
  Cross* x_;                             // one per symbol, in shared memory

  void on_book(const std::string& sym, double bid, double ask,
               double mid, double mp, double obi) override {
    x_->t[venue_] = {bid, ask};          // POD store, no allocation
    const Touch& o = x_->t[1 - venue_];  // the OTHER venue, from shm
    if (bid <= 0.0 || o.ask <= 0.0) return;
    if (o.ask < bid)                     // crossed -> o is stale
      sell_limit(sym, 1, bid);           // I can only trade MY venue;
  }                                      // the other process lifts o.ask
};
```

The single-process alternative: two `HFTBot`/`ArenaClient` objects in one
binary, each built from its own `ClientConfig` (copy `from_env()`, then set
`cfg.url`) and each `run()` on its own thread — same shared cache, no shm.

### 3. Sketch the queue-aware requote
In continuous trading you also earn the spread as a maker — but only if your
resting order actually *fills*, which depends on **queue position**. The client
tells you where you stand: `on_ack(sym, id, queue_ahead, level_qty)` and
`on_queue(...)`. The decision: cancel-and-requote only when it *improves* your
odds, because every requote costs a message *and* sends you to the **back** of
the new queue.

```cpp
// Requote logic driven by on_ack/on_queue state.
enum Action { HOLD, REQUOTE, CANCEL };

Action requote_decision(double my_px, double best_px, long queue_ahead,
                        long level_qty, bool price_moved_against) {
    if (price_moved_against)                 return CANCEL;   // stale/adverse: pull it
    if (my_px < best_px)                     return REQUOTE;  // no longer at best -> rejoin at best
    // At best but buried deep in the queue with little chance to fill this level:
    if (queue_ahead > level_qty / 2)         return REQUOTE;  // reprice/refresh to gain position
    return HOLD;                                              // good spot: DON'T waste a message
}
```

The **why**: `queue_ahead` is how much size fills *before you* at your price;
`level_qty` is the whole level. If you're behind more than ~half the level, your
fill odds are poor and a requote may help — but requoting resets you to the back,
so a needless requote makes things *worse* and burns quota. The winning move
most ticks is **HOLD**. This is the maker mirror of the arb: spend scarce
messages only where they change your expected fill.

### 4. Map strategy → the composite grade
The tournament score is a **composite**, not just latency. Tie each metric to a
lever you just built:

| Metric | What it measures | Your lever |
|---|---|---|
| **p50 / p99 / p99.9** | tick-to-trade latency, esp. the tail | Weeks 13–14: SIMD/LTO + no-alloc hot path + killed tail |
| **queue position** | how far back your resting orders sit | §3 requote logic; get to best early, don't self-bury |
| **fill rate** | orders that actually execute | don't over-cancel; respect `order_quota`; quote real size |
| **markout** | P&L a few ticks *after* your fill (were you right?) | §2 arb edge net of fees; skip marginal/adverse trades |

The trap: optimizing one metric wrecks another. Cancel aggressively for perfect
queue position → fill rate craters and you blow quota. Chase every arb → markout
goes *negative* on the marginal ones. Fastest bot with a bad signal → great p99.9,
losing markout. The grade rewards the **balance**.

### 5. Dry-run it offline before the live tournament
Validate on the deterministic replay so demo day has no surprises:

```bash
python scripts/latency_replay.py --latest \
       --cmd "hft/cpp_client/build/hft_bot --replay"
# -> snapshots fed / throughput / p50 p90 p99 p99.9 max, with the tail flagged
```

Sanity gates before you tag your submission commit:
- p99.9 is within a small multiple of p50 (Week-14 discipline holds under load).
- No allocations on the hot path (re-run your `perf`/allocation check from W14).
- Under a **tight quota**, message count per interval stays *under* the cap —
  watch for rate-limit rejects in the log.

## Your turn
Prep your Phase-7 tournament bot and demo:

1. Implement `cross_venue_arb` and `requote_decision` (adapt the sketches) and
   wire them into `on_book` / `on_ack` in your bot. Run **two processes** (one
   `EXCHANGE_URL` each) and cache the *other* venue's last top of book in shared
   memory so `on_book` can compare.
2. Enforce the **`order_quota`**: add a per-interval message counter and a
   guard that drops to HOLD/skip when you're near the cap. Prove it in the log.
3. Run the offline harness and record your four grade inputs (latency
   percentiles, and your own estimate of queue position / fill rate / markout
   from the report). Which metric is your weakest? Name the one change you'd
   make and predict its effect on the *other three*.
4. Prepare a 3-minute demo: baseline (Phase 0) vs now — show the p99.9 delta and
   one arb/requote decision firing on the tape.

## Checkpoint
- You can, given two `Top` snapshots and per-side fees, say whether an arb
  exists and which way — and why it must clear **both** fees before quota is spent.
- Your requote logic returns **HOLD** on a good queue spot and only REQUOTE/CANCEL
  when it improves fill odds; you can explain the requote-to-back-of-queue cost.
- You can name all four composite-grade components and the lever for each, and
  give one concrete example of a metric that trades off against another.
- You can state why `EXCHANGE_URLS` will not give a C++ bot two venues, and
  what you run instead (two processes / two clients + one shared touch cache).
- The offline harness runs your bot clean under the `hft/scenarios/week10.json`
  constraints (tail tight, no hot-path alloc, under a quota of 6) and you have a
  tagged commit ready to submit.

## Links
Week-15 lecture deck · Tournament scenario (`hft/scenarios/week10.json`:
multi-venue, auctions, `order_quota: 6`, the Gateway Silicon FPGA IPO) ·
Project Phase 7 & composite
grade (`project/README.md`) · Client hooks + latency harness
(`docs/HFT_CPP_CLIENT.md`)
