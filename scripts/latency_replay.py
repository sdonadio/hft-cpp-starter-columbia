#!/usr/bin/env python
"""
scripts/latency_replay.py — OFFLINE latency benchmark for HFT bots.

The HFT analog of scripts/backtest.py. Instead of scoring a strategy's P&L,
this replays a recorded session tape (sessions/session_*.jsonl) and measures
how FAST a compiled bot reacts to each book update. It hands every
BookSnapshot to a subprocess "bot under test" and clocks the wall-clock time
until the bot writes an order back — then reports the latency distribution.

HFT is won on the TAIL, so the report surfaces p99.9 and max prominently: a
bot that is fast on average but occasionally stalls for a millisecond loses
the race on exactly the ticks that matter.

──────────────────────────────────────────────────────────────────────────────
THE BOT PROTOCOL (newline-delimited JSON over stdin/stdout)
──────────────────────────────────────────────────────────────────────────────
The bot under test is any executable (a student's compiled C++ `hft_bot`, or a
script) that speaks this line protocol:

  * It reads ONE book snapshot per line on stdin. Each line is the exact JSON
    of a BookSnapshot message, e.g.:
        {"type":"book_snapshot","symbol":"NVDA","bids":[[900.1,50]],
         "asks":[[900.3,40]],"mid_price":900.2,"spread":0.2,"ref_price":900.2,
         "asset_type":"equity"}
  * For EACH input line it writes EXACTLY ONE line on stdout — either a JSON
    order (any shape; the harness does not validate it) or a no-op marker
    (`{}` or an empty JSON object). One line in → one line out is what makes
    the round-trip a clean latency measurement.
  * It must flush stdout after every response (else the pipe buffers and the
    measurement is meaningless — C++ students: `std::endl` or `fflush`).

──────────────────────────────────────────────────────────────────────────────
USAGE
──────────────────────────────────────────────────────────────────────────────
    # Benchmark a compiled C++ bot against the newest recording
    python scripts/latency_replay.py --latest --cmd ./hft_bot

    # Benchmark against a specific tape, warming up 200 snapshots first
    python scripts/latency_replay.py sessions/session_X.jsonl \\
        --cmd "./hft_bot --fast" --warmup 200

    # No --cmd: benchmark the built-in Python echo bot (reference baseline).
    # Runnable TODAY with no C++ present.
    python scripts/latency_replay.py --latest

    # Self-test: synthetic snapshots, no tape, no C++ — verifies the harness.
    python scripts/latency_replay.py --self-test

    # Only a symbol subset, and cap the number of snapshots replayed
    python scripts/latency_replay.py --latest --symbols NVDA,AMD --max 5000
"""

from __future__ import annotations

import argparse
import glob
import json
import pathlib
import shlex
import subprocess
import sys
import time

_ROOT = pathlib.Path(__file__).resolve().parent.parent

# Hidden flag: re-exec of THIS file acting as the reference echo bot. Kept out
# of --help; students never call it directly.
_ECHO_FLAG = "--_echo-bot"


# ─────────────────────────────────────────────────────────────────────────────
# Percentile math (pure stdlib, linear interpolation — matches numpy default)
# ─────────────────────────────────────────────────────────────────────────────

def percentile(sorted_values: list[float], p: float) -> float:
    """The p-th percentile (0..100) of an already-sorted list.

    Uses linear interpolation between closest ranks — the same method as
    numpy.percentile's default, so results line up with any numpy-based tool.
    Returns 0.0 for an empty input.
    """
    n = len(sorted_values)
    if n == 0:
        return 0.0
    if n == 1:
        return float(sorted_values[0])
    if p <= 0:
        return float(sorted_values[0])
    if p >= 100:
        return float(sorted_values[-1])
    rank = (p / 100.0) * (n - 1)
    lo = int(rank)
    hi = min(lo + 1, n - 1)
    frac = rank - lo
    return float(sorted_values[lo] + (sorted_values[hi] - sorted_values[lo]) * frac)


def summarize(latencies_us: list[float]) -> dict:
    """Percentile summary of a latency sample (microseconds)."""
    s = sorted(latencies_us)
    n = len(s)
    return {
        "count": n,
        "min": s[0] if n else 0.0,
        "p50": percentile(s, 50),
        "p90": percentile(s, 90),
        "p99": percentile(s, 99),
        "p999": percentile(s, 99.9),
        "max": s[-1] if n else 0.0,
        "mean": (sum(s) / n) if n else 0.0,
    }


# ─────────────────────────────────────────────────────────────────────────────
# Snapshot sourcing
# ─────────────────────────────────────────────────────────────────────────────

def iter_snapshots(path: pathlib.Path, symbols: set[str] | None = None):
    """Yield BookSnapshot dicts from a recording tape, in recorded order.

    Streams the file line by line so a multi-million-line tape stays within
    constant memory. Malformed lines and non-snapshot messages are skipped.
    """
    with open(path) as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            try:
                rec = json.loads(line)
                msg = rec["msg"]
            except (ValueError, TypeError, KeyError):
                continue
            if msg.get("type") != "book_snapshot":
                continue
            if symbols and msg.get("symbol") not in symbols:
                continue
            yield msg


def synthetic_snapshots(n: int, symbols: list[str] | None = None):
    """Generate `n` plausible BookSnapshot dicts for the self-test path.

    Deterministic pseudo-random walk per symbol — no numpy, no network, so the
    self-test is reproducible and runnable with nothing else present.
    """
    symbols = symbols or ["NVDA", "AMD", "AAPL"]
    px = {s: 100.0 + 50.0 * i for i, s in enumerate(symbols)}
    seed = 12345
    for k in range(n):
        sym = symbols[k % len(symbols)]
        # cheap LCG step → deterministic jitter in [-0.5, 0.5]
        seed = (1103515245 * seed + 12345) & 0x7FFFFFFF
        jitter = (seed / 0x7FFFFFFF) - 0.5
        px[sym] = max(1.0, px[sym] + jitter)
        mid = px[sym]
        half = 0.05
        yield {
            "type": "book_snapshot",
            "symbol": sym,
            "bids": [[round(mid - half, 4), 50], [round(mid - 2 * half, 4), 100]],
            "asks": [[round(mid + half, 4), 40], [round(mid + 2 * half, 4), 90]],
            "mid_price": round(mid, 4),
            "spread": round(2 * half, 4),
            "ref_price": round(mid, 4),
            "asset_type": "equity",
        }


# ─────────────────────────────────────────────────────────────────────────────
# The benchmark
# ─────────────────────────────────────────────────────────────────────────────

class BotProcessError(RuntimeError):
    """The bot under test died or produced no response."""


def benchmark(
    snapshots,
    cmd: list[str],
    warmup: int = 50,
    max_snaps: int | None = None,
) -> dict:
    """Feed each snapshot to the bot subprocess and time the round trip.

    Returns a dict with the latency sample, the percentile summary, throughput,
    and bookkeeping counters. Raises BotProcessError if the bot cannot be
    launched or stops responding.
    """
    try:
        proc = subprocess.Popen(
            cmd,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            bufsize=1,          # line buffered
        )
    except (OSError, ValueError) as exc:
        raise BotProcessError(f"could not launch bot {cmd!r}: {exc}") from exc

    latencies_us: list[float] = []
    fed = 0
    warmed = 0
    no_response = 0
    wall_start = time.perf_counter()

    try:
        assert proc.stdin is not None and proc.stdout is not None
        for snap in snapshots:
            if max_snaps is not None and fed >= max_snaps:
                break
            line = json.dumps(snap, separators=(",", ":"))
            t0 = time.perf_counter_ns()
            try:
                proc.stdin.write(line + "\n")
                proc.stdin.flush()
                reply = proc.stdout.readline()
            except (BrokenPipeError, OSError) as exc:
                raise BotProcessError(
                    f"bot pipe broke after {fed} snapshots: {exc}") from exc
            t1 = time.perf_counter_ns()

            if reply == "":     # EOF → bot exited early
                raise BotProcessError(
                    f"bot produced no output / exited after {fed} snapshots "
                    f"(exit code {proc.poll()})")
            if not reply.strip():
                no_response += 1

            fed += 1
            if warmed < warmup:
                warmed += 1
                continue
            latencies_us.append((t1 - t0) / 1000.0)   # ns → µs
    finally:
        try:
            if proc.stdin:
                proc.stdin.close()
        except OSError:
            pass
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()

    wall = time.perf_counter() - wall_start
    if not latencies_us:
        raise BotProcessError(
            f"no timed responses collected (fed {fed}, warmup {warmup}) — "
            "increase --max or lower --warmup")

    summary = summarize(latencies_us)
    return {
        "summary": summary,
        "latencies_us": latencies_us,
        "fed": fed,
        "warmup": warmed,
        "measured": len(latencies_us),
        "no_response": no_response,
        "wall_sec": wall,
        "throughput": (fed / wall) if wall > 0 else 0.0,
    }


# ─────────────────────────────────────────────────────────────────────────────
# Reporting (plain print, matching team_report.py's presentation)
# ─────────────────────────────────────────────────────────────────────────────

def _bar(value: float, lo: float, hi: float, width: int = 30) -> str:
    if hi <= lo:
        return "─" * width
    fill = int(round((value - lo) / (hi - lo) * width))
    fill = max(0, min(width, fill))
    return "█" * fill + "·" * (width - fill)


def print_report(result: dict, label: str) -> None:
    s = result["summary"]
    w = print
    w(f"═══ AlgoArena latency benchmark — {label} ═══")
    w(f"  snapshots fed   : {result['fed']:,}  "
      f"(warmup {result['warmup']:,} discarded, {result['measured']:,} timed)")
    w(f"  throughput      : {result['throughput']:,.0f} snapshots/sec  "
      f"over {result['wall_sec']:.3f}s wall")
    if result["no_response"]:
        w(f"  no-op replies   : {result['no_response']:,} "
          f"(bot answered but placed no order)")
    w("")
    w("  latency distribution (microseconds) — HFT is won on the TAIL:")
    lo, hi = s["min"], s["max"]
    rows = [
        ("p50 ", s["p50"]),
        ("p90 ", s["p90"]),
        ("p99 ", s["p99"]),
        ("p99.9", s["p999"]),
        ("max ", s["max"]),
    ]
    for name, val in rows:
        tail = "  ← TAIL" if name.strip() in ("p99.9", "max") else ""
        w(f"    {name:<6} {val:>12,.2f} µs  {_bar(val, lo, hi)}{tail}")
    w(f"    {'min':<6} {s['min']:>12,.2f} µs")
    w(f"    {'mean':<6} {s['mean']:>12,.2f} µs")
    w("")
    w(f"  VERDICT: p99.9 = {s['p999']:,.1f} µs  (this is the number that ranks "
      f"an HFT bot; a low p50 with a high p99.9 still loses the race)")


# ─────────────────────────────────────────────────────────────────────────────
# Reference echo bot (built-in baseline)
# ─────────────────────────────────────────────────────────────────────────────

def run_echo_bot() -> int:
    """Reference bot: read one snapshot per line, emit one order per line.

    Trivial passive quoter — joins the best bid. Exists so the harness is
    runnable and testable with no C++ present, and gives students a latency
    floor to beat. Speed here is essentially the Python interpreter's
    read/parse/write cost.
    """
    out = sys.stdout
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            snap = json.loads(line)
            bids = snap.get("bids") or []
            px = bids[0][0] if bids else snap.get("mid_price", 0.0)
            order = {"type": "place_order", "symbol": snap.get("symbol", "?"),
                     "side": "buy", "order_type": "limit",
                     "price": px, "quantity": 1}
        except Exception:
            order = {}
        out.write(json.dumps(order, separators=(",", ":")) + "\n")
        out.flush()
    return 0


# ─────────────────────────────────────────────────────────────────────────────
# Entry point
# ─────────────────────────────────────────────────────────────────────────────

def _default_cmd() -> list[str]:
    """The built-in echo bot, launched by re-exec'ing this file."""
    return [sys.executable, str(pathlib.Path(__file__).resolve()), _ECHO_FLAG]


def main(argv: list[str] | None = None) -> int:
    # Hidden echo-bot mode: intercept before argparse so its flags never leak.
    argv = list(sys.argv[1:] if argv is None else argv)
    if _ECHO_FLAG in argv:
        return run_echo_bot()

    ap = argparse.ArgumentParser(
        description="Offline latency benchmark for an HFT bot under test",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("file", nargs="?", help="sessions/session_*.jsonl tape")
    ap.add_argument("--latest", action="store_true",
                    help="use the newest recording in sessions/")
    ap.add_argument("--cmd", default=None,
                    help="the bot under test, e.g. './hft_bot'. Omit to "
                         "benchmark the built-in Python echo bot (baseline).")
    ap.add_argument("--self-test", action="store_true",
                    help="synthetic snapshots, no tape, no C++ — verify the harness")
    ap.add_argument("--warmup", type=int, default=50,
                    help="snapshots discarded before timing starts (default: 50)")
    ap.add_argument("--max", type=int, default=None, dest="max_snaps",
                    help="cap the number of snapshots replayed")
    ap.add_argument("--symbols", default=None,
                    help="comma-separated symbol whitelist, e.g. NVDA,AMD")
    ap.add_argument("-n", type=int, default=2000, dest="n_synth",
                    help="synthetic snapshot count for --self-test (default: 2000)")
    args = ap.parse_args(argv)

    symbols = set(args.symbols.split(",")) if args.symbols else None
    cmd = shlex.split(args.cmd) if args.cmd else _default_cmd()
    cmd_label = args.cmd if args.cmd else "built-in echo bot (baseline)"

    # ---- pick the snapshot source ----
    if args.self_test:
        snaps = synthetic_snapshots(args.n_synth, list(symbols) if symbols else None)
        label = f"SELF-TEST ({args.n_synth} synthetic snapshots) — {cmd_label}"
        # a short warmup keeps the self-test quick and still stable
        warmup = min(args.warmup, max(0, args.n_synth // 10))
    else:
        path = args.file
        if args.latest or not path:
            candidates = sorted(glob.glob(str(_ROOT / "sessions" / "session_*.jsonl")))
            if not candidates:
                print("No recordings in sessions/ — run a session, or use "
                      "--self-test", file=sys.stderr)
                return 2
            path = candidates[-1]
            print(f"  Using latest recording: {path}")
        recording = pathlib.Path(path)
        if not recording.exists():
            print(f"No such recording: {recording}", file=sys.stderr)
            return 2
        snaps = iter_snapshots(recording, symbols)
        label = f"{recording.name} — {cmd_label}"
        warmup = args.warmup

    try:
        result = benchmark(snaps, cmd, warmup=warmup, max_snaps=args.max_snaps)
    except BotProcessError as exc:
        print(f"latency_replay: {exc}", file=sys.stderr)
        return 1

    print_report(result, label)
    return 0


if __name__ == "__main__":
    sys.exit(main())
