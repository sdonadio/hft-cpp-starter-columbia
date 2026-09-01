# Week 12 Lab — Async I/O & Serialization

**Format:** in-class, guided (~45–60 min).  **Repo:** the HFT starter.

## Goal
Write a fast **`u64toa`** in `include/u64toa.hpp` that turns a `uint64_t` into decimal
text and returns the length — beating `snprintf`/`std::to_string`, and correct on the
edges (`0` and `UINT64_MAX`). This is **HW12**. We then demo a **non-blocking read loop**
(`epoll`/`kqueue`/`select`) and a **targeted JSON field extract** (pull best bid/ask
without building a DOM) — the pattern the arena client uses on its hot path.

## Setup
```bash
cd project-starter
make u64toa        # builds tests/u64toa_test.cpp against your header — RED right now
```
Read the contract in `tests/u64toa_test.cpp`: `int u64toa(uint64_t v, char* out)` writes
digits into `out` and returns the count. The grader checks `0, 7, 12345, 1000000,
9999999999, UINT64_MAX`, fuzzes 200k values against `std::to_string`, and reports your
ns/op next to `std_to_string_ns_per_op`.

## Walk-through — we build this together

### 1. Why `snprintf`/`std::to_string` are slow
`snprintf` parses a format string, handles locale, width, and flags at runtime.
`std::to_string` allocates a `std::string` (heap traffic + a destructor). On the serialize
path — stamping an order id or quantity into an outbound buffer thousands of times a
second — both are pure overhead. We want: no format parsing, no allocation, write straight
into a caller-owned buffer.

### 2. The classic reverse-then-flip
Generate digits least-significant-first (that's what `% 10` gives you), then reverse into
`out`. A **do/while** is the key detail: it runs the body once even for `v == 0`, so `0`
prints `"0"` instead of an empty string.
```cpp
#pragma once
#include <cstdint>

inline int u64toa(std::uint64_t v, char* out) {
    char tmp[20];                      // UINT64_MAX = 18446744073709551615 → 20 digits
    int n = 0;
    do {
        tmp[n++] = static_cast<char>('0' + v % 10);
        v /= 10;
    } while (v);                       // do/while → handles v == 0 correctly
    for (int i = 0; i < n; ++i)        // flip into caller's buffer, MSD first
        out[i] = tmp[n - 1 - i];
    return n;                          // length; caller null-terminates if it wants
}
```
The **why** for `tmp[20]`: the largest `uint64_t` is 20 decimal digits, so the scratch
buffer can never overflow. We return the length; the test null-terminates via `buf[n] = 0`.

### 3. Go green + read the metric
```bash
make u64toa
```
Expect `u64toa_edges` and `u64toa_fuzz` both `pass`, plus
`METRIC|u64toa_ns_per_op` vs `METRIC|std_to_string_ns_per_op`. Even this simple version
should comfortably beat `std::to_string` because you dodged the allocation.

### 4. Optimize: process two digits at a time (stretch, in-class if time)
Division is the bottleneck. Halve the number of `/`s with a static two-digit lookup table
and `% 100`:
```cpp
static constexpr char D2[201] =
    "00010203040506070809101112131415161718192021222324"
    "25262728293031323334353637383940414243444546474849"
    "50515253545556575859606162636465666768697071727374"
    "75767778798081828384858687888990919293949596979899";
// then: while (v >= 100) { unsigned r = v % 100; v /= 100; write D2[2*r], D2[2*r+1]; }
// finish the last 1–2 digits; still reverse/emit MSD-first.
```
Re-measure ns/op. This is the trick behind libraries like Milo Yip's `itoa` — same idea
as an exchange stamping millions of ids per second.

### 5. Demo — a non-blocking read loop (`epoll`/`kqueue`/`select`)
Blocking `recv()` parks your thread until bytes arrive — unacceptable when one thread must
service many sockets and never stall the strategy. Readiness APIs invert control: the OS
tells you *which* fds have data, you drain them, you never block on a quiet one. macOS uses
`kqueue`, Linux uses `epoll`; `select` is the portable (slower) fallback. Skeleton:
```cpp
// portable-ish shape (select): "which fds are readable right now?"
for (;;) {
    fd_set rd; FD_ZERO(&rd);
    FD_SET(sock, &rd);
    timeval tv{0, 0};                         // non-blocking poll: return immediately
    int n = select(sock + 1, &rd, nullptr, nullptr, &tv);
    if (n > 0 && FD_ISSET(sock, &rd)) {
        ssize_t k = recv(sock, buf, sizeof buf, 0);   // socket set O_NONBLOCK
        if (k > 0) handle(buf, k);            // may be a partial message → frame it
        // k == 0: peer closed;  k < 0 && errno==EAGAIN: nothing left, move on
    }
    // ... do other work; never blocked on a silent socket ...
}
```
Two takeaways: set the fd `O_NONBLOCK` so `recv` returns `EAGAIN` instead of parking, and
remember TCP hands you a **stream** — a `recv` may deliver a partial or multiple messages,
so you frame them yourself (same lesson as Week 11). In production you'd use `epoll`
(edge-triggered) or `kqueue`; `select` here just makes the readiness idea concrete.

### 6. Demo — targeted JSON extract (no DOM)
The arena speaks **JSON-over-WebSocket** (`shared/messages.py`). Parsing each
`book_snapshot` into a full DOM (`nlohmann::json`) allocates nodes and hashes keys — heavy
for the hot path. When you only need best bid/ask, scan for the keys and read the numbers
in place:
```cpp
// pull the number that follows "bid": out of a book_snapshot frame — no full parse
inline double extract_num(const char* json, const char* key) {
    const char* p = std::strstr(json, key);   // e.g. key = "\"bid\":"
    if (!p) return -1.0;
    p += std::strlen(key);
    while (*p == ' ' || *p == ':' || *p == '"') ++p;
    return std::strtod(p, nullptr);            // reads the number, stops at ',' or '}'
}
// double bid = extract_num(frame, "\"bid\":");
// double ask = extract_num(frame, "\"ask\":");
```
This is deliberately minimal (assumes flat, well-formed frames from the arena) — the point
is the *pattern*: touch only the two fields you trade on, allocate nothing, skip the DOM.
That is why the C++ client decodes ticks the way it does; see `on_book(...)` in
`docs/HFT_CPP_CLIENT.md`, and note the `u64toa` you just wrote is what serializes ids/qtys
back onto the wire.

## Your turn
1. Finish `u64toa` so `make u64toa` is green on edges + fuzz; record `u64toa_ns_per_op`
   and confirm it beats `std_to_string_ns_per_op`.
2. **Optimize (stretch):** implement the two-digit-table version from step 4 and re-measure.
3. **Apply:** in your project bot's `on_book`, replace any full JSON parse of best bid/ask
   with a targeted extract like step 6, and use `u64toa` when stamping outbound orders.
   Re-run the offline latency replay:
   ```bash
   python scripts/latency_replay.py --cmd "hft/cpp_client/build/hft_bot --replay"
   python scripts/latency_report.py     # watch p50 / p99 / p99.9
   ```

## Checkpoint
- [ ] `make u64toa` → `u64toa_edges` and `u64toa_fuzz` both `pass`
- [ ] `0` → `"0"` (do/while) and `UINT64_MAX` → 20 digits, both correct
- [ ] `u64toa_ns_per_op` < `std_to_string_ns_per_op`
- [ ] You can explain non-blocking readiness (`EAGAIN`, framing a stream) and why a targeted
      JSON extract beats a full DOM on the hot path
- [ ] `make test` shows HW12 scored

## Links
- Edit: `include/u64toa.hpp`
- Contract (read-only): `tests/u64toa_test.cpp`, `tests/bench.hpp`
- Grader: `tests/run_ci.py` (HW12), `make test`
- Arena JSON schema: `shared/messages.py`; client + latency harness: `docs/HFT_CPP_CLIENT.md`
