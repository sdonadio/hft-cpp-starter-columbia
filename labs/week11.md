# Session 10 Lab — Network Protocols, Market Data & Fast Serialization

**Format:** in-class, guided, **one lab for the whole session** (~22 min in the room: Part A then Part B; the demos and stretches go home).  **Repo:** the HFT starter.

## Goal
Finish **two** small, graded headers, both autograded through `make`:

- **Part A — `include/fix_parser.hpp`:** a **single-pass, zero-allocation FIX parser** that decodes a NewOrder-single into a `NewOrder` struct — tags **11** (ClOrdID), **55** (Symbol), **54** (Side), **38** (OrderQty), **44** (Price). This is the stub for **HW 11**.
- **Part B — `include/u64toa.hpp`:** a fast **`u64toa`** that turns a `uint64_t` into decimal text and returns the length, going after `snprintf`/`std::to_string` on speed and correct on the edges (`0` and `UINT64_MAX`). This is the stub for **HW 12**.

Then a few short **demos** (TCP vs UDP, a non-blocking read loop, a targeted JSON extract, fixed-width binary) show where these two pieces sit on a real wire. They are instructor-led or for you to run at home; nothing in the room depends on them.

> Wire-protocol reality check: the **arena** speaks **JSON-over-WebSocket** (`shared/messages.py`), not FIX. FIX is the industry protocol you must be able to parse fast (Part A); the targeted JSON extract in the demos is the pattern your graded bot actually runs on its hot path.

## Setup
```bash
cd hft-cpp-starter-columbia   # the root of YOUR clone of the starter (has Makefile, include/, tests/)
make fix           # builds tests/fix_test.cpp against your header — RED right now
make u64toa        # builds tests/u64toa_test.cpp against your header — RED right now
```
Both are red until you fill in the headers; that is expected. The tests are the contract and are read-only.

**FIX contract** (`tests/fix_test.cpp`). The sample message (SOH shown as `\x01`):
```
8=FIX.4.2 | 9=76 | 35=D | 11=ORD123 | 55=AAPL | 54=1 | 38=100 | 44=185.50 | 10=072
```
FIX is `tag=value` pairs, each terminated by the **SOH** byte `'\x01'`. Note the struct: `clordid` is a **pointer + length into the caller's buffer** (a view — we copy nothing), while `symbol` is a small fixed `char[16]` we fill.

**u64toa contract** (`tests/u64toa_test.cpp`): `int u64toa(uint64_t v, char* out)` writes digits into `out` and returns the count. The grader checks `0, 7, 12345, 1000000, 9999999999, UINT64_MAX`, fuzzes 200k values against `std::to_string`, and reports your ns/op next to `std_to_string_ns_per_op`.

## Walk-through — we build this together

## Part A: the FIX parser

*In the room: about 16 minutes (steps 1–4). Steps 1–3 are typed together; step 4 is yours.*

### 1. The mental model: one forward scan, no copies
A slow parser splits the buffer into strings and looks up fields in a map. We won't.
We do **one left-to-right pass**: read an integer tag, expect `=`, take the value up
to the next SOH, `switch` on the tag, repeat. No `std::string`, no `std::map`, no
allocation — the message is fixed input we read in place.

### 2. Skeleton and the scan loop
```cpp
#pragma once
#include <cstdint>
#include <cstring>
#include <cstdlib>

inline bool parse_new_order(const char* buf, int len, NewOrder& out) {
    const char SOH = '\x01';
    const char* p   = buf;
    const char* end = buf + len;
    bool id=false, sym=false, side=false, qty=false, px=false;

    while (p < end) {
        // --- tag: digits up to '=' ---
        int tag = 0;
        while (p < end && *p != '=') {
            if (*p < '0' || *p > '9') return false;   // malformed tag
            tag = tag * 10 + (*p - '0');
            ++p;
        }
        if (p >= end) return false;                    // no '=' → malformed
        ++p;                                           // consume '='

        // --- value: bytes up to SOH ---
        const char* v = p;
        while (p < end && *p != SOH) ++p;
        const int vlen = static_cast<int>(p - v);
        if (p < end) ++p;                              // consume SOH
        // ... dispatch on tag (next step) ...
    }
    return id && sym && side && qty && px;             // all required fields seen
}
```
The **why**: we never look back. Each byte is visited once. `v`/`vlen` describe the
value slice without copying it.

### 3. Dispatch on the tag
Drop this `switch` inside the loop where the comment is:
```cpp
        switch (tag) {
            case 11:                                   // ClOrdID — keep a view, no copy
                out.clordid = v; out.clordid_len = vlen; id = true;
                break;
            case 55: {                                 // Symbol — copy into fixed buffer
                int n = vlen < 15 ? vlen : 15;
                std::memcpy(out.symbol, v, n);
                out.symbol[n] = '\0';
                sym = true;
                break;
            }
            case 54:                                   // Side — single char ('1'=buy,'2'=sell)
                if (vlen >= 1) { out.side = v[0]; side = true; }
                break;
            case 38: {                                 // OrderQty — hand-rolled atoi
                uint32_t q = 0;
                for (int i = 0; i < vlen; ++i) {
                    if (v[i] < '0' || v[i] > '9') return false;
                    q = q * 10 + static_cast<uint32_t>(v[i] - '0');
                }
                out.qty = q; qty = true;
                break;
            }
            case 44:                                   // Price
                out.price = std::strtod(v, nullptr);   // stops at SOH; no allocation
                px = true;
                break;
            default:
                break;                                  // ignore 8/9/35/10 etc.
        }
```
Two teaching points: (a) `clordid` stays a pointer into `buf` — that is why the struct
gives you a `_len`; copying it would be a needless allocation-shaped cost. (b)
`std::strtod` stops at the first non-numeric byte (the SOH), so it is safe here and
does **not** allocate. If you want zero library calls, hand-roll the decimal parse too.

### 4. Go green + read the metric
```bash
make fix
```
You get `RESULT|fix_parse|pass` and `METRIC|fix_parse_ns_per_op|<n>` — the grader parses
the same message 2,000,000 times and reports ns/op. A tight single-pass parser lands in
the tens-of-ns range; if you see hundreds, something is allocating or copying.

**Checkpoint stop (≈ 16 min into the lab):** hands up if you have `fix_parse|pass`. If not, grab the TA or a neighbour — you still do Part B, the easier twenty points.

## Part B: u64toa, fast integer-to-decimal text

*In the room: about 6 minutes (steps 5–7). Ten lines; the cheapest twenty points in the course.*

### 5. Why `snprintf`/`std::to_string` are slow
`snprintf` parses a format string, handles locale, width, and flags at runtime.
`std::to_string` allocates a `std::string` (heap traffic + a destructor). On the serialize
path — stamping an order id or quantity into an outbound buffer thousands of times a
second — both are pure overhead. We want: no format parsing, no allocation, write straight
into a caller-owned buffer.

### 6. The classic reverse-then-flip
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

### 7. Go green + read the metric
```bash
make u64toa
```
Expect `u64toa_edges` and `u64toa_fuzz` both `pass`, plus
`METRIC|u64toa_ns_per_op` vs `METRIC|std_to_string_ns_per_op`. Even this simple version
should land close to or below `std::to_string`. It is the *same order*, not a guaranteed win: short strings fit `std::string`'s small-buffer, so `to_string` often skips the heap too. Record both numbers and be ready to say why they are close; the stretch in *Your turn* and the magnitude sweep in HW 12 are where the gap opens.

## Demos (instructor-led or at home)

Short, throwaway, none graded. The code blocks are shapes to type in and adapt, not files in the starter. The `select`/`recv`/`socket` calls are POSIX, so they build on macOS and Linux; `epoll` is Linux-only and `kqueue` is macOS/BSD-only (the `select` shape below runs on both).

### 8. Demo — TCP vs UDP semantics (localhost)
Market data feeds are usually **UDP multicast** (fast, lossy, unordered — you design for
gaps); order entry is usually **TCP** (reliable, ordered, but with head-of-line blocking).
Feel the difference with two throwaway programs:
```cpp
// udp_rx.cpp — datagrams arrive whole-or-not-at-all, may be dropped/reordered
int s = socket(AF_INET, SOCK_DGRAM, 0);
// bind to 127.0.0.1:9000, then recvfrom() in a loop — each recvfrom = one datagram

// tcp_rx.cpp — a byte STREAM: recv() gives you *some* bytes, maybe half a message
int s = socket(AF_INET, SOCK_STREAM, 0);
// bind/listen/accept, then recv() — you must frame messages yourself (FIX tag 9 = body length)
```
The lesson for your parser: over TCP you can receive a **partial** FIX message, so real
code buffers until it has a full one (tag 9 tells you the body length). UDP hands you a
complete datagram but may silently drop it. Same bytes, opposite failure modes.

### 9. Demo — a non-blocking read loop (`epoll`/`kqueue`/`select`)
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
so you frame them yourself (same framing lesson as step 8). In production you'd use `epoll`
(edge-triggered) or `kqueue`; `select` here just makes the readiness idea concrete.

### 10. Demo — targeted JSON extract (no DOM)
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

### 11. Why fixed-width binary beats text
FIX text costs you a parse: scanning for `=`/SOH and doing `atoi`/`strtod` on every field.
A binary protocol (SBE/ITCH-style) is a `struct` you `reinterpret_cast` and read — often a
single `memcpy` or a pointer cast, *zero* character scanning. You lose human-readability;
you win an order of magnitude of decode latency. Exchanges publish the hot feeds in binary
for exactly this reason. Your Part A FIX parser is the "make text fast" exercise; binary is
the "avoid parsing entirely" endgame.

## Your turn
**In the room (before the checkpoint):**
1. Finish `parse_new_order` (Part A) so `make fix` is green and record your `fix_parse_ns_per_op`.
2. Finish `u64toa` (Part B) so `make u64toa` is green on edges + fuzz; record `u64toa_ns_per_op` and compare it to `std_to_string_ns_per_op`.

**At home (these feed HW 11 and HW 12):**
3. **Malformed-message guard:** a tag with no `=`, or a non-digit in tag 38, must make you `return false` (the loop in step 2 already does — trace why).
4. **Optimize FIX (stretch):** replace `strtod` for tag 44 with a hand-rolled fixed-point parse (integer part, `.`, fractional part). Re-measure ns/op.
5. **Optimize FIX (stretch):** branch-light tag dispatch — most tags are 2 digits; you can read the tag as you scan without building a general integer.
6. **Optimize u64toa (stretch):** the two-digit-table version below. Re-measure ns/op, and try **short numbers** as well as long ones: the 201-byte table is a cache footprint, so it can lose on small values.
```cpp
static constexpr char D2[201] =
    "00010203040506070809101112131415161718192021222324"
    "25262728293031323334353637383940414243444546474849"
    "50515253545556575859606162636465666768697071727374"
    "75767778798081828384858687888990919293949596979899";
// then: while (v >= 100) {{ unsigned r = v % 100; v /= 100; write D2[2*r], D2[2*r+1]; }}
// finish the last 1–2 digits; still reverse/emit MSD-first.
```
Halving the number of `/`s is the trick behind libraries like Milo Yip's `itoa`.
7. **Apply:** in your project bot's `on_book`, replace any full JSON parse of best bid/ask with a targeted extract like step 10, and use `u64toa` when stamping outbound orders. Re-run the offline latency replay:
   ```bash
   python scripts/latency_replay.py --cmd "hft/cpp_client/build/hft_bot --replay"
   python scripts/latency_report.py     # watch p50 / p99 / p99.9
   ```
8. Run the step 8–10 demos once on your own machine.

**HW 11 and HW 12 are both due Sat Nov 21, 11:59 pm ET** (Canvas is authoritative). The graded interfaces are exactly the ones you just wrote: `parse_new_order` in `fix_parser.hpp` (HW 11; the full assignment goes beyond tonight's stub to a batch of concatenated messages, framed on BodyLength/SOH) and `int u64toa(uint64_t v, char* out)` in `u64toa.hpp` (HW 12; cycles/op across magnitudes, plus the bonus field encoder). Tonight's two green targets are the warm-up for both.

## Checkpoint
- [ ] `make fix` → `RESULT|fix_parse|pass`, and `METRIC|fix_parse_ns_per_op` printed (know your number)
- [ ] `make u64toa` → `u64toa_edges` and `u64toa_fuzz` both `pass`, and you have recorded `u64toa_ns_per_op` next to `std_to_string_ns_per_op` (ns/op is a metric, not a pass condition)
- [ ] Single pass: each input byte visited once; no `std::string`/`std::map`/allocation
- [ ] `clordid` is a view (pointer+len) into the caller's buffer, not a copy
- [ ] Malformed input returns `false` (missing `=`, non-digit qty)
- [ ] `0` → `"0"` (do/while) and `UINT64_MAX` → 20 digits, both correct
- [ ] You can explain non-blocking readiness (`EAGAIN`, framing a stream) and why a targeted JSON extract beats a full DOM on the hot path (demos; at home is fine)
- [ ] `make test` shows HW 11 and HW 12 scored

## Links
- Edit: `include/fix_parser.hpp`, `include/u64toa.hpp`
- Contract (read-only): `tests/fix_test.cpp`, `tests/u64toa_test.cpp`, `tests/bench.hpp`
- Grader: `tests/run_ci.py` (HW 11 and HW 12), `make test`
- Arena wire protocol (JSON, not FIX): `shared/messages.py`; client + latency harness: `docs/HFT_CPP_CLIENT.md`
