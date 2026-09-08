# AlgoArena — HFT C++ Client

A C++17 trading-bot skeleton for the **HFT variant** of the course. You write
your strategy in C++ (for speed); it connects to the **existing Python
exchange** over the **same JSON-over-WebSocket protocol** the Python bots use.
Nothing on the exchange side changes — an HFT bot and a Python bot are
indistinguishable on the wire.

The graded axis of this variant is **tick-to-order latency**: how fast your bot
turns an inbound `book_snapshot` into an outbound `place_order`. The skeleton
measures it for you (see *Latency instrumentation* below).

## Layout

```
hft/cpp_client/
├── CMakeLists.txt              # FetchContent: IXWebSocket + nlohmann/json
├── include/
│   ├── arena_client.hpp        # transport + protocol codec (plumbing)
│   └── hft_bot.hpp             # HFTBot base class — the hooks you override
├── src/
│   ├── arena_client.cpp        # WebSocket + JSON (matches shared/messages.py)
│   └── main.cpp                # ← YOUR STRATEGY LIVES HERE (SpreadCaptureBot)
└── README.md
```

## Dependencies

Both are fetched automatically at configure time via CMake `FetchContent` — you
do **not** need to install anything system-wide:

| Library | Why | Pinned tag |
|---|---|---|
| [IXWebSocket](https://github.com/machinezone/IXWebSocket) | WebSocket client, background receive/reconnect thread | `v11.4.5` |
| [nlohmann/json](https://github.com/nlohmann/json) | Header-only JSON encode/decode | `v3.11.3` |

A C++17 compiler, CMake ≥ 3.16, and network access for the first configure are
all you need.

## Build

```bash
cd hft/cpp_client
cmake -B build
cmake --build build
```

This produces one executable: `build/hft_bot`.

The first configure clones the two dependencies (needs internet); later builds
are offline.

## Run

The client reads the **same environment variables** as `trader/config.py`:

| Variable | Default | Meaning |
|---|---|---|
| `TEAM_ID` | `hft_alpha` | Your team identifier (sent in the handshake) |
| `EXCHANGE_HOST` | `localhost` | Exchange hostname |
| `EXCHANGE_PORT` | `8765` | Exchange port |
| `EXCHANGE_URL` | *(unset)* | Full URL; **overrides** host/port (e.g. `wss://feed.arena.example.edu`) |
| `ARENA_TOKEN` | *(empty)* | Team token for hosted (AUTH_REQUIRED) play |
| `ARENA_LEVEL` | `1` | Handshake unlock level |

Local play against a locally running exchange:

```bash
TEAM_ID=hft_alpha EXCHANGE_HOST=localhost EXCHANGE_PORT=8765 ./build/hft_bot
```

The bot connects, sends the handshake, and **waits for `SESSION_OPEN`** before
trading — exactly like the Python trader. Fire the session from the teacher
dashboard and you will see fills and latency samples stream to stderr. Ctrl-C
shuts down cleanly.

### Hosted / TLS (`wss://`)

`wss://` needs a TLS-enabled build. Reconfigure with:

```bash
cmake -B build -DHFT_USE_TLS=ON
cmake --build build
EXCHANGE_URL=wss://feed.arena.example.edu ARENA_TOKEN=... ./build/hft_bot
```

TLS uses the platform-native backend (Secure Transport on macOS, OpenSSL
elsewhere), so no extra package is required on most systems.

## Where you write your strategy

Everything you edit is in **`src/main.cpp`**, in the `SpreadCaptureBot` class.
The ships-with-it example is a simple spread-capture / momentum bot so the
binary does something out of the box — replace it with your edge. The hooks
(declared in `include/hft_bot.hpp`) are marked with `TODO(student)`:

- `on_book(symbol, bid, ask, mid, microprice, obi)` — a top-of-book update arrived (microprice and order-book imbalance are pre-computed for you). **Send
  orders from here** and they are automatically latency-timed. This is the
  hot path and your main surface.
- `on_fill(side, symbol, qty, price)` — a fill involving you landed.
- `on_session(event, message)` — session lifecycle + market events
  (`SESSION_OPEN`, `SESSION_CLOSED`, `SHOCK`, `CALENDAR`, …).

Order helpers to call from the hooks:
`buy_limit` / `sell_limit` / `buy_market` / `sell_market` / `cancel_order`.
You never touch raw JSON — the client serializes the exact `PlaceOrder` /
`CancelOrder` shape the Python engine expects.

You should **not** need to edit `arena_client.*` or the non-strategy parts of
`hft_bot.hpp` — that is transport plumbing.

## Latency instrumentation

The instant a `book_snapshot` frame is decoded, the client stamps a
`std::chrono::steady_clock` timepoint. When you send an order from inside
`on_book()`, the elapsed **microseconds from snapshot-arrival to order-send** is
recorded and printed to stderr:

```
[latency] tick->order = 37 us
...
[latency] n=50 avg=41 min=29 max=118 us
```

A running summary (count / avg / min / max) prints every 50 samples. Override
`record_latency(long long micros)` in your bot if you want a finer histogram or
to log samples to a file for the graded report.
