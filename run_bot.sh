#!/usr/bin/env bash
# Start the arena C++ bot with the credentials written by scripts/register.py.
#   ./run_bot.sh                      # connects as TEAM_ID from .env
#   TEAM_ID=<other bot id> ./run_bot.sh
# Build first:  cd hft/cpp_client && cmake -B build -DHFT_USE_TLS=ON && cmake --build build
set -euo pipefail
cd "$(dirname "$0")"
if [ ! -f .env ]; then
  echo "No .env — register first:  python3 scripts/register.py --arena https://<arena> --code <code> --name \"Team\"" >&2
  exit 1
fi
set -a; . ./.env; set +a
BOT=hft/cpp_client/build/hft_bot
[ -x "$BOT" ] || { echo "No $BOT — build it: cd hft/cpp_client && cmake -B build -DHFT_USE_TLS=ON && cmake --build build" >&2; exit 1; }
echo "connecting as $TEAM_ID to $EXCHANGE_URL"
exec "$BOT"
