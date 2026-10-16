#!/usr/bin/env bash
# EdgeDesk end-to-end integration test.
#
# Starts the built binary against a temporary file root and exercises:
#   - HTTP smoke (health + frontend shell when a web root is available)
#   - REST API suite (auth, files, transfers, metrics, sandbox, CSRF)
#   - WebSocket suite (terminal lifecycle, reconnect, metrics stream)
#   - Rate limiting (separate strict-limit instance)
#   - Audit log sanity
#
# Usage: scripts/integration_test.sh [binary] [port] [web-root] [runner]
#   runner: optional command prefix for cross-built binaries, e.g.
#           "/usr/bin/qemu-aarch64-static -L /usr/aarch64-linux-gnu"
set -euo pipefail

BIN=${1:-out/build/dev-disk/bin/edgedesk}
PORT=${2:-19091}
WEB_ROOT=${3:-web/dist}
RUNNER=${4:-}
ROOT=$(mktemp -d /tmp/edgedesk-it-XXXXXX)
STATE="$ROOT/state"
DATA="$ROOT/data"
TOKEN="integration-token-$$"
PIDS=()

log() { printf '\n[integration] %s\n' "$*"; }

start_server() {
  local port="$1"
  shift
  local -a runner=()
  if [ -n "$RUNNER" ]; then read -r -a runner <<< "$RUNNER"; fi
  "${runner[@]}" "$BIN" --host 127.0.0.1 --port "$port" "$@" > "$ROOT/server-$port.log" 2>&1 &
  PIDS+=("$!")
}

stop_servers() {
  for pid in "${PIDS[@]:-}"; do
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
  done
}
trap 'stop_servers; rm -rf "$ROOT"' EXIT

wait_health() {
  local port="$1"
  for _ in $(seq 1 200); do
    if curl -fsS "http://127.0.0.1:$port/api/health" >/dev/null 2>&1; then return 0; fi
    sleep 0.1
  done
  echo "server on port $port did not become healthy" >&2
  cat "$ROOT/server-$port.log" >&2 || true
  return 1
}

log "functional suite on port $PORT"
mkdir -p "$DATA/docs"
printf 'hello\n' > "$DATA/a.txt"
printf '# note\n' > "$DATA/docs/n.md"
start_server "$PORT" --file-root "$DATA" --auth-token "$TOKEN" --state-dir "$STATE" \
  --login-rate-limit 200 --rate-limit 500 --rate-limit-burst 500
wait_health "$PORT"

if [ -d "$WEB_ROOT" ]; then
  curl -fsS "http://127.0.0.1:$PORT/" | grep -q 'EdgeDesk' || { echo "frontend shell not served" >&2; exit 1; }
  HASHED=$(cd "$WEB_ROOT" && ls assets/*.js 2>/dev/null | head -1 || true)
  if [ -n "$HASHED" ]; then
    curl -fsS "http://127.0.0.1:$PORT/$HASHED" >/dev/null
  fi
  log "frontend shell + hashed asset OK"
fi

python3 scripts/api_test.py 127.0.0.1 "$PORT" "$TOKEN" "$DATA"
python3 scripts/ws_test.py 127.0.0.1 "$PORT" "$TOKEN"

log "rate limit suite on port $((PORT + 1))"
start_server "$((PORT + 1))" --file-root "$DATA" --auth-token "$TOKEN" --state-dir "$STATE" \
  --login-rate-limit 2 --rate-limit 2 --rate-limit-burst 2
wait_health "$((PORT + 1))"
python3 scripts/rate_limit_test.py 127.0.0.1 "$((PORT + 1))" "$TOKEN"

log "audit log sanity"
if [ -f "$STATE/audit.log" ]; then
  grep -q '"event":"login"' "$STATE/audit.log"
  grep -q '"event":"file.upload"' "$STATE/audit.log"
  grep -q '"event":"terminal.create"' "$STATE/audit.log"
else
  echo "audit log missing" >&2
  exit 1
fi

log "ALL TESTS PASSED"
