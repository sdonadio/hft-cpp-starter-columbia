#!/usr/bin/env python3
"""Register your team on the class arena and write .env for the C++ bot.

    python3 scripts/register.py --arena https://algoarenafin.duckdns.org \
        --code <class code> --name "Your Team Name"

What it does
  1. POSTs to the arena's /api/register with the class code and your plan
     (default: one trader seat funded with the whole $1,000,000 budget).
  2. Receives your team token — shown ONCE — and your bot ids.
  3. Writes .env in the repo root:
        ARENA_TOKEN=...            your secret; never commit it
        TEAM_ID=<first trader id>  the bot id the C++ client connects as
        EXCHANGE_URL=wss://feed.<arena host>
     Then `./run_bot.sh` (or `make run`) loads .env and starts build/hft_bot.

Options
  --traders N          trader seats (1-5), budget split evenly    [default 1]
  --brokers N          broker desks (0-2), $100,000 each          [default 0]
  --dry-run            validate the code only; register nothing
"""
from __future__ import annotations

import argparse
import json
import pathlib
import sys
import urllib.error
import urllib.parse
import urllib.request

BUDGET = 1_000_000
MIN_TRADER = 50_000
BROKER_CAP = 100_000


def post(url: str, payload: dict) -> dict:
    body = json.dumps(payload).encode()
    req = urllib.request.Request(url, data=body, method="POST",
                                 headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            return json.loads(resp.read())
    except urllib.error.HTTPError as e:
        try:
            detail = json.loads(e.read()).get("error", "")
        except Exception:
            detail = ""
        sys.exit(f"Arena refused the request ({e.code}): {detail or e.reason}")
    except urllib.error.URLError as e:
        sys.exit(f"Could not reach the arena at {url}: {e.reason}")


def main() -> None:
    ap = argparse.ArgumentParser(description="Register a team on the class arena")
    ap.add_argument("--arena", required=True, help="dashboard URL, e.g. https://algoarenafin.duckdns.org")
    ap.add_argument("--code", required=True, help="class registration code (given in class)")
    ap.add_argument("--name", help="team name, 2-40 characters (letters, digits, spaces)")
    ap.add_argument("--traders", type=int, default=1)
    ap.add_argument("--brokers", type=int, default=0)
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()

    arena = a.arena.rstrip("/")
    endpoint = arena + "/api/register"

    ok = post(endpoint, {"code": a.code, "validate_only": True})
    if not ok.get("code_valid"):
        sys.exit("Registration code rejected")
    if a.dry_run:
        print("Code accepted; nothing registered (--dry-run).")
        return
    if not a.name:
        sys.exit("--name is required to register")
    if not (1 <= a.traders <= 5) or not (0 <= a.brokers <= 2):
        sys.exit("traders must be 1-5 and brokers 0-2")

    remaining = BUDGET - a.brokers * BROKER_CAP
    per_trader = remaining // a.traders
    if per_trader < MIN_TRADER:
        sys.exit("Not enough budget for that many seats")

    result = post(endpoint, {
        "code": a.code,
        "name": a.name.strip(),
        "exchange": False,
        "broker_capitals": [BROKER_CAP] * a.brokers,
        "trader_capitals": [per_trader] * a.traders,
    })

    host = urllib.parse.urlparse(arena).hostname or "localhost"
    if arena.startswith("https://"):
        ws_url = f"wss://feed.{host}"
    else:
        ws_url = f"ws://{host}:{result.get('exchange_port', 8765)}"
    team_id = (result.get("trader_ids") or result.get("broker_ids") or ["<bot>"])[0]

    env = pathlib.Path(".env")
    env.write_text(
        "# AlgoArena credentials — written by scripts/register.py. NEVER commit this file.\n"
        f"ARENA_TOKEN={result['token']}\n"
        f"TEAM_ID={team_id}\n"
        f"EXCHANGE_URL={ws_url}\n"
    )
    try:
        env.chmod(0o600)
    except OSError:
        pass

    print(f"""
Registered team {result['team']!r}.

  Your bot ids:   traders {result.get('trader_ids')}   brokers {result.get('broker_ids')}
  Exchange:       {ws_url}
  Credentials:    .env  (token shown once — it is saved there; never commit it)

Run your bot:     ./run_bot.sh          (loads .env, starts build/hft_bot as {team_id})
Another seat:     TEAM_ID=<other bot id> ./run_bot.sh
Dashboard:        {arena}   (LATENCY tab once you send orders from on_book)
""")


if __name__ == "__main__":
    main()
