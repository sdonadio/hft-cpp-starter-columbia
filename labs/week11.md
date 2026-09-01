# Week 11 Lab — Network Protocols & Market Data

**Format:** in-class, guided (~45–60 min).  **Repo:** the HFT starter.

## Goal
Write a **single-pass, zero-allocation FIX parser** in `include/fix_parser.hpp` that
decodes a NewOrder-single into a `NewOrder` struct — tags **11** (ClOrdID), **55**
(Symbol), **54** (Side), **38** (OrderQty), **44** (Price). This is **HW11**. Along
the way we demo TCP vs UDP semantics on localhost and argue why fixed-width binary
beats text on the wire.

> Wire-protocol reality check: the **arena** speaks **JSON-over-WebSocket**
> (`shared/messages.py`), not FIX. FIX is the industry protocol you must be able to
> parse fast; next week (Week 12) we do the JSON field-extract the arena actually uses.

## Setup
```bash
cd project-starter
make fix           # builds tests/fix_test.cpp against your header — RED right now
```
Read the contract in `tests/fix_test.cpp`. The sample message (SOH shown as `\x01`):
```
8=FIX.4.2 | 9=76 | 35=D | 11=ORD123 | 55=AAPL | 54=1 | 38=100 | 44=185.50 | 10=072
```
FIX is `tag=value` pairs, each terminated by the **SOH** byte `'\x01'`. Note the
struct: `clordid` is a **pointer + length into the caller's buffer** (a view — we copy
nothing), while `symbol` is a small fixed `char[16]` we fill.

## Walk-through — we build this together

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

### 5. Demo — TCP vs UDP semantics (localhost, ~5 min)
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

### 6. Why fixed-width binary beats text
FIX text costs you a parse: scanning for `=`/SOH and doing `atoi`/`strtod` on every field.
A binary protocol (SBE/ITCH-style) is a `struct` you `reinterpret_cast` and read — often a
single `memcpy` or a pointer cast, *zero* character scanning. You lose human-readability;
you win an order of magnitude of decode latency. Exchanges publish the hot feeds in binary
for exactly this reason. Your Week-11 FIX parser is the "make text fast" exercise; binary is
the "avoid parsing entirely" endgame.

## Your turn
1. Finish `parse_new_order` so `make fix` is green and record your `fix_parse_ns_per_op`.
2. Add a **malformed-message** guard mindset: a tag with no `=`, or a non-digit in tag 38,
   must make you `return false` (the loop above already does — trace why).
3. **Optimize (stretch):** replace `strtod` for tag 44 with a hand-rolled fixed-point parse
   (integer part, `.`, fractional part). Re-measure ns/op.
4. **Optimize (stretch):** branch-light tag dispatch — most tags are 2 digits; you can read
   the tag as you scan without building a general integer.

## Checkpoint
- [ ] `make fix` → `RESULT|fix_parse|pass`
- [ ] `METRIC|fix_parse_ns_per_op` printed (know your number)
- [ ] Single pass: each input byte visited once; no `std::string`/`std::map`/allocation
- [ ] `clordid` is a view (pointer+len) into the caller's buffer, not a copy
- [ ] Malformed input returns `false` (missing `=`, non-digit qty)
- [ ] `make test` shows HW11 scored

## Links
- Edit: `include/fix_parser.hpp`
- Contract (read-only): `tests/fix_test.cpp`, `tests/bench.hpp`
- Grader: `tests/run_ci.py` (HW11), `make test`
- Arena wire protocol (JSON, not FIX): `shared/messages.py`; client: `docs/HFT_CPP_CLIENT.md`
