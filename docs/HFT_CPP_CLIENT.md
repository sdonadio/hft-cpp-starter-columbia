# The Arena C++ Client (`hft/cpp_client`)

Your bot talks to the (unchanged Python) AlgoArena exchange over
JSON-over-WebSocket — on the wire you're just another client. You write **C++**;
you're graded on **tick-to-trade latency (p50 / p99 / p99.9)**.

## Get it & build it

The client ships with the arena template (`hft/cpp_client/`). From that directory:

```bash
cd hft/cpp_client
cmake -B build          # first run fetches IXWebSocket + nlohmann/json (needs internet)
cmake --build build     # -> build/hft_bot
```

C++17 + CMake ≥ 3.16. TLS (`wss://`): `cmake -B build -DHFT_USE_TLS=ON`.

## Run

```bash
TEAM_ID=<you> EXCHANGE_HOST=<arena_host> EXCHANGE_PORT=<port> ./build/hft_bot
# waits for SESSION_OPEN, then trades; prints [latency] tick->order = N us
```

## What you own

Subclass `arena::HFTBot` and override the hooks in `include/hft_bot.hpp`; write
your strategy in `src/main.cpp` (`SpreadCaptureBot`). Everything under it
(`arena_client.*`) is transport plumbing you don't touch.

- `on_book(sym, bid, ask, mid, microprice, obi)` — **the hot path**. Orders sent
  here (`buy_limit`/`sell_limit`/`buy_market`/`sell_market`/`cancel_order`) are
  latency-stamped for you (steady_clock, µs, from `book_snapshot` decode to order send).
- `on_fill(side, sym, qty, px, maker)`, `on_session(event, msg)`,
  `on_ack(sym, id, queue_ahead, level_qty)`, `on_queue(...)`.

## Measure yourself offline (this is where you optimize)

```bash
python scripts/latency_replay.py --cmd "hft/cpp_client/build/hft_bot --replay"
python scripts/latency_report.py    # -> p50 / p99 / p99.9, throughput, tail histogram
```

Deterministic replay of a recorded tape — the same numbers used to grade you.
"Won on the tail, not the mean": optimize p99.9, not the average.
