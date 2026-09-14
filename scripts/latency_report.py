#!/usr/bin/env python
"""
scripts/latency_report.py — per-bot latency LEADERBOARD from live logs.

The live analog of scripts/latency_replay.py. Where the replayer benchmarks
one bot offline against a tape, this parses the latency instrumentation that
LIVE bots emit during a session and ranks them. It answers the HFT variant's
grading question directly: whose bot is fastest on the tail?

──────────────────────────────────────────────────────────────────────────────
THE LATENCY-LOG LINE FORMAT (agreed contract — pass this to the C++ author)
──────────────────────────────────────────────────────────────────────────────
Each bot emits ONE line per book snapshot it reacts to, to its stderr or to a
log file it writes:

        LAT <symbol> <micros>

  * literal token `LAT`, then whitespace-separated fields
  * <symbol>: the security ticker (e.g. NVDA)
  * <micros>: tick-to-order latency in microseconds — the wall time from
    receiving the BookSnapshot to writing the order onto the socket. Integer
    or float; both parse.

The parser scans each line for the `LAT` token, so a leading log prefix is
tolerated (bots that log with timestamps are fine):

        2026-08-25T14:03:11.481Z INFO LAT NVDA 42.7
        LAT AMD 128

ONE LOG FILE = ONE BOT. The bot's identity is taken from the file name (stem,
with a leading `lat_`/`latency_` prefix and a `.log`/`.txt`/`.csv` suffix
stripped) unless --name is given for a single file.

──────────────────────────────────────────────────────────────────────────────
USAGE
──────────────────────────────────────────────────────────────────────────────
    # One bot's log
    python scripts/latency_report.py logs/lat_team_alpha.log

    # A whole session's worth — glob many bot logs into one leaderboard
    python scripts/latency_report.py "logs/lat_*.log"
    python scripts/latency_report.py logs/lat_a.log logs/lat_b.log --json

    # Self-test: synthetic logs, no bots — verify the parser + ranking
    python scripts/latency_report.py --self-test
"""

from __future__ import annotations

import argparse
import glob
import json
import pathlib
import sys
import tempfile

# Reuse the ONE percentile implementation from the replayer so the offline and
# live numbers are computed identically.
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from latency_replay import percentile  # noqa: E402


# ─────────────────────────────────────────────────────────────────────────────
# Parsing
# ─────────────────────────────────────────────────────────────────────────────

def parse_lat_line(line: str) -> tuple[str, float] | None:
    """Parse one log line → (symbol, micros), or None if it is not a LAT line.

    Scans for the `LAT` token so a leading log prefix (timestamps, levels) is
    tolerated. Malformed micros or a missing symbol → None (silently skipped).
    """
    parts = line.split()
    for i, tok in enumerate(parts):
        if tok == "LAT" and i + 2 < len(parts):
            # need a symbol at i+1 and micros at i+2
            symbol = parts[i + 1]
            try:
                micros = float(parts[i + 2])
            except ValueError:
                return None
            if micros < 0:
                return None
            return symbol, micros
    return None


def parse_log(path: pathlib.Path) -> dict:
    """Parse one bot log file → {symbol: [micros, ...]} plus a flat 'all' list.

    Streams the file so a large log stays within constant memory (only the
    latency samples are retained, which is inherent to computing percentiles).
    """
    by_symbol: dict[str, list[float]] = {}
    all_us: list[float] = []
    lines = 0
    parsed = 0
    with open(path) as fh:
        for line in fh:
            lines += 1
            got = parse_lat_line(line)
            if got is None:
                continue
            symbol, micros = got
            by_symbol.setdefault(symbol, []).append(micros)
            all_us.append(micros)
            parsed += 1
    return {"by_symbol": by_symbol, "all": all_us,
            "lines": lines, "parsed": parsed}


def bot_name(path: pathlib.Path) -> str:
    """Derive a bot id from a log file name.

    Strips a leading lat_/latency_ prefix and the extension:
        lat_team_alpha.log → team_alpha ,  latency_scalper.csv → scalper
    """
    stem = path.stem
    for prefix in ("latency_", "lat_"):
        if stem.startswith(prefix):
            stem = stem[len(prefix):]
            break
    return stem or path.name


# ─────────────────────────────────────────────────────────────────────────────
# Leaderboard
# ─────────────────────────────────────────────────────────────────────────────

def bot_summary(name: str, samples: list[float]) -> dict:
    """Percentile summary for one bot's flat latency sample."""
    s = sorted(samples)
    n = len(s)
    return {
        "bot": name,
        "samples": n,
        "p50": percentile(s, 50),
        "p99": percentile(s, 99),
        "p999": percentile(s, 99.9),
        "max": s[-1] if n else 0.0,
        "min": s[0] if n else 0.0,
    }


def build_leaderboard(files: list[pathlib.Path],
                      name_override: str | None = None) -> dict:
    """Parse every log file and rank the bots by tail latency (p99.9 asc).

    A bot with no LAT lines at all is reported separately (it either did not
    emit instrumentation or never traded) so it is not silently dropped.
    """
    bots: list[dict] = []
    silent: list[str] = []
    for p in files:
        name = name_override if (name_override and len(files) == 1) else bot_name(p)
        parsed = parse_log(p)
        if not parsed["all"]:
            silent.append(name)
            continue
        row = bot_summary(name, parsed["all"])
        row["file"] = p.name
        bots.append(row)
    # Fastest tail first — HFT ranks on p99.9, tie-broken by p99 then p50.
    bots.sort(key=lambda r: (r["p999"], r["p99"], r["p50"]))
    return {"bots": bots, "silent": silent}


# ─────────────────────────────────────────────────────────────────────────────
# Reporting (plain print, matching team_report.py's presentation)
# ─────────────────────────────────────────────────────────────────────────────

def print_leaderboard(board: dict) -> None:
    w = print
    bots = board["bots"]
    w("═══ AlgoArena HFT latency leaderboard "
      "(ranked by p99.9 — the tail wins the race) ═══")
    if not bots:
        w("  (no LAT lines parsed from any log — check the log format:"
          " 'LAT <symbol> <micros>')")
    else:
        w(f"  {'#':>2}  {'bot':<20} {'p50':>12} {'p99':>12} {'p99.9':>12} "
          f"{'max':>12}   {'samples':>9}")
        w(f"  {'':>2}  {'':<20} {'(µs)':>12} {'(µs)':>12} {'(µs)':>12} "
          f"{'(µs)':>12}")
        for i, b in enumerate(bots, 1):
            w(f"  {i:>2}. {b['bot']:<20} {b['p50']:>12,.2f} {b['p99']:>12,.2f} "
              f"{b['p999']:>12,.2f} {b['max']:>12,.2f}   {b['samples']:>9,}")
        best = bots[0]
        w("")
        w(f"  WINNER: {best['bot']}  (p99.9 = {best['p999']:,.1f} µs over "
          f"{best['samples']:,} ticks)")
    if board["silent"]:
        w("")
        w(f"  no instrumentation / no trades: {', '.join(board['silent'])}")


# ─────────────────────────────────────────────────────────────────────────────
# Self-test
# ─────────────────────────────────────────────────────────────────────────────

def _write_synthetic_logs(dirpath: pathlib.Path) -> list[pathlib.Path]:
    """Write a few synthetic bot logs with different latency profiles."""
    profiles = {
        # name : (base_us, tail_spike_us, n)
        "fast_cpp":   (8.0, 40.0, 500),
        "medium_cpp": (25.0, 300.0, 500),
        "slow_py":    (120.0, 2500.0, 500),
    }
    symbols = ["NVDA", "AMD", "AAPL"]
    seed = 999
    files = []
    for name, (base, spike, n) in profiles.items():
        p = dirpath / f"lat_{name}.log"
        with open(p, "w") as fh:
            fh.write("# synthetic latency log — format: LAT <symbol> <micros>\n")
            for k in range(n):
                seed = (1103515245 * seed + 12345) & 0x7FFFFFFF
                r = seed / 0x7FFFFFFF
                # 1% of samples are tail spikes; the rest cluster near base
                micros = (base + spike * r) if r > 0.99 else (base * (0.5 + r))
                sym = symbols[k % len(symbols)]
                fh.write(f"2026-08-25T00:00:00Z INFO LAT {sym} {micros:.2f}\n")
        files.append(p)
    return files


def run_self_test() -> int:
    with tempfile.TemporaryDirectory() as td:
        files = _write_synthetic_logs(pathlib.Path(td))
        board = build_leaderboard(files)
        print_leaderboard(board)
    return 0


# ─────────────────────────────────────────────────────────────────────────────
# Entry point
# ─────────────────────────────────────────────────────────────────────────────

def _expand(patterns: list[str]) -> list[pathlib.Path]:
    """Expand glob patterns / literal paths into a sorted unique file list."""
    seen: dict[str, pathlib.Path] = {}
    for pat in patterns:
        matches = glob.glob(pat)
        for m in (matches or ([pat] if pathlib.Path(pat).exists() else [])):
            seen[str(pathlib.Path(m).resolve())] = pathlib.Path(m)
    return [seen[k] for k in sorted(seen)]


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(
        description="Per-bot HFT latency leaderboard from live logs",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("logs", nargs="*",
                    help="log file(s) or glob(s), e.g. 'logs/lat_*.log'")
    ap.add_argument("--name", default=None,
                    help="bot name override (only when a single file is given)")
    ap.add_argument("--self-test", action="store_true",
                    help="synthetic logs, no bots — verify the parser + ranking")
    ap.add_argument("--json", action="store_true", help="emit JSON")
    args = ap.parse_args(argv)

    if args.self_test:
        return run_self_test()

    if not args.logs:
        ap.error("give one or more log files/globs, or use --self-test")

    files = _expand(args.logs)
    if not files:
        print("latency_report: no matching log files", file=sys.stderr)
        return 2

    board = build_leaderboard(files, name_override=args.name)
    if args.json:
        print(json.dumps(board, indent=1))
    else:
        print_leaderboard(board)
    return 0


if __name__ == "__main__":
    sys.exit(main())
