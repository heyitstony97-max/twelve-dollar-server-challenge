#!/usr/bin/env bash
# Checks for the write pulse (M8). Builds tools/loadgen.c, then for each setting starts one server on a
# fresh copy of the seed and:
#   1. runs tools/pulse_edge.py (pipelining order, read-your-writes, half-close, RST, bursts, 100-continue);
#   2. drives heavy writes (500 users, 50 % likes, 20 % posts) and compares what was ACKNOWLEDGED with what
#      is IN THE DATABASE afterwards: every 201 like and post must be there, nothing else, integrity_check ok.
# Settings cover the default, constant checkpointing, a slow checkpoint thread (holds time out and the loop's
# last-resort checkpoint runs), no pulse, and a long pulse.
#   bash tools/pulse.sh        needs gcc, python3, curl, sqlite3 (CLI), and bin/server built
#   WALBUF=1 INLINE=1 PULSE_MAX_MS=100 QUEUE=newest bash tools/pulse.sh    the same with the M9 switches on
set -uo pipefail
SUB="$(cd "$(dirname "$0")/.." && pwd)"; ROOT="$(cd "$SUB/../.." && pwd)"
SEED="$ROOT/seed/feed.db"; TOKENS="$ROOT/seed/tokens.json"
PORT="${PORT:-3860}"; W="$(mktemp -d)"
trap 'kill ${PID:-} 2>/dev/null; rm -rf "$W"' EXIT
gcc -O2 -o "$W/loadgen" "$SUB/tools/loadgen.c" || exit 1
SL=$(sqlite3 "$SEED" "SELECT count(*) FROM likes"); SP=$(sqlite3 "$SEED" "SELECT count(*) FROM posts")
bad=0
run() { # name ENV=VAL...
  local name="$1"; shift
  rm -f "$W"/feed.db*; cp "$SEED" "$W/feed.db"
  env JWT_SECRET=twelve-dollar-challenge HOST=127.0.0.1 PORT="$PORT" SQLITE_PATH="$W/feed.db" STATS=1 TUNE=off "$@" \
    "$SUB/bin/server" > "$W/log" 2>&1 &
  PID=$!
  for _ in $(seq 100); do curl -sf "http://127.0.0.1:$PORT/health" >/dev/null && break; sleep 0.1; done
  python3 "$SUB/tools/pulse_edge.py" "$PORT" > "$W/edge.txt" 2>&1 || { bad=$((bad+1)); grep FAIL "$W/edge.txt"; }
  "$W/loadgen" 127.0.0.1 "$PORT" 500 12 10 "$TOKENS" 50 20 2 > "$W/load.txt"
  kill $PID; wait $PID 2>/dev/null; PID=
  # the edge script's own writes are in the database too: count them from its acknowledgements
  # pulse_edge.py's own acknowledged writes are in the database too: 301 likes (1 + 300) and 321 posts
  local el=301 ep=321 L P DBL DBP IC
  L=$(grep -o 'new likes=[0-9]*' "$W/load.txt" | cut -d= -f2); P=$(grep -o 'posts=[0-9]*' "$W/load.txt" | cut -d= -f2)
  DBL=$(( $(sqlite3 "$W/feed.db" "SELECT count(*) FROM likes") - SL )); DBP=$(( $(sqlite3 "$W/feed.db" "SELECT count(*) FROM posts") - SP ))
  IC=$(sqlite3 "$W/feed.db" "PRAGMA integrity_check" | head -1)
  local rst=$(( DBP - P - ep ))   # the 50 POST+RST requests of the edge script may or may not have been executed (never acknowledged)
  local ok=1
  [ "$IC" = ok ] || ok=0
  [ "$DBL" = "$(( L + el ))" ] || ok=0
  [ "$rst" -ge 0 ] && [ "$rst" -le 50 ] || ok=0
  grep -q "edge: failed=0" "$W/edge.txt" || ok=0
  echo "[$name] $(grep -o 'edge: failed=[0-9]*' "$W/edge.txt") | likes acked $(( L + el )) in db $DBL | posts acked $(( P + ep )) in db $DBP (+$rst unacknowledged RST posts) | integrity $IC | $(grep 'stats:' "$W/log" | tail -1 | sed 's/.* ckpts=/ckpts=/')"
  [ $ok = 1 ] || { bad=$((bad+1)); echo "[$name] FAILED"; }
}
run default
run ckpt64 CKPT_FRAMES=64
run slow-checkpoints CKPT_FRAMES=64 CKPT_DELAY_MS=300 CKPT_FORCE=3000
run no-pulse PULSE_MS=0 CKPT_FRAMES=64
run pulse-100ms PULSE_MS=100 CKPT_FRAMES=512
[ $bad = 0 ] && echo "PULSE OK" || { echo "PULSE FAILED ($bad)"; exit 1; }
