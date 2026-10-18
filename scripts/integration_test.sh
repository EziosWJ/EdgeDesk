#!/usr/bin/env bash
# EdgeDesk end-to-end integration test (V1.1 auth + settings + V1.0 features).
#
# Starts the built binary against temporary file/data/state directories and
# exercises:
#   - first-run initialization, login, password change, restart and recovery
#   - corrupt auth state fail-closed behavior
#   - REST API suite (files, transfers, metrics, sandbox, CSRF)
#   - WebSocket suite (terminal lifecycle, reconnect, metrics stream)
#   - Rate limiting (separate strict-limit instance, including XFF spoofing)
#   - Audit log sanity
#
# Usage: scripts/integration_test.sh [binary] [port] [web-root] [runner]
#   runner: optional command prefix for cross-built binaries, e.g.
#           "/usr/bin/qemu-aarch64-static -L /usr/aarch64-linux-gnu"
set -euo pipefail

# The suite only talks to loopback. Ignore ambient HTTP proxies: environments
# often export http_proxy with no_proxy patterns such as "127.*", which Python
# and curl do not match for IP literals, causing intermittent connect timeouts
# through the proxy instead of direct loopback connections.
export no_proxy="127.0.0.1,localhost${no_proxy:+,${no_proxy:-}}"
export NO_PROXY="$no_proxy"

BIN=${1:-out/build/dev-disk/bin/edgedesk}
PORT=${2:-19091}
WEB_ROOT=${3:-web/dist}
RUNNER=${4:-}
ROOT=$(mktemp -d /tmp/edgedesk-it-XXXXXX)
STATE="$ROOT/state"
DATA="$ROOT/data"
AUTH_DATA="$ROOT/auth-data"
CORRUPT_DATA="$ROOT/corrupt-data"
APPS="$ROOT/apps"
EXT_DATA="$ROOT/ext-data"
EXT_STATE="$ROOT/ext-state"
EXT_APPS="$ROOT/ext-apps"
EXT_FILES="$ROOT/ext-files"
TOKEN="integration-token-$$"
PASSWORD="integration password one"
NEW_PASSWORD="integration password two"
RECOVERY_PASSWORD="integration password recovery"
CORRUPT_PASSWORD="integration password corrupt"
RESTART_PORT=$((PORT + 5))
PIDS=()

log() { printf '\n[integration] %s\n' "$*"; }

start_server() {
  local port="$1"
  shift
  local -a runner=()
  if [ -n "$RUNNER" ]; then read -r -a runner <<< "$RUNNER"; fi
  "${runner[@]}" "$BIN" --host 127.0.0.1 --port "$port" "$@" \
    --apps-root "${ACTIVE_APPS:-$APPS}" > "$ROOT/server-$port.log" 2>&1 &
  SERVER_PID=$!
  PIDS+=("$SERVER_PID")
}

stop_pid() {
  local pid="$1"
  [ -n "$pid" ] || return 0
  kill "$pid" 2>/dev/null || true
  for _ in $(seq 1 80); do
    if ! kill -0 "$pid" 2>/dev/null; then
      wait "$pid" 2>/dev/null || true
      return 0
    fi
    sleep 0.1
  done
  kill -9 "$pid" 2>/dev/null || true
  wait "$pid" 2>/dev/null || true
}

stop_servers() {
  for pid in "${PIDS[@]:-}"; do stop_pid "$pid"; done
}
cleanup() {
  stop_servers
  if [ "${KEEP_TMP:-0}" != "1" ]; then rm -rf "$ROOT"; fi
}
trap cleanup EXIT

run_binary() {
  local -a runner=()
  if [ -n "$RUNNER" ]; then read -r -a runner <<< "$RUNNER"; fi
  "${runner[@]}" "$BIN" "$@"
}

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

mkdir -p "$DATA/docs" "$STATE" "$AUTH_DATA" "$CORRUPT_DATA" "$APPS" "$EXT_DATA" "$EXT_STATE" "$EXT_APPS" "$EXT_FILES"
printf 'hello\n' > "$DATA/a.txt"
printf '# note\n' > "$DATA/docs/n.md"

log "functional suite on port $PORT"
start_server "$PORT" --file-root "$DATA" --auth-token "$TOKEN" --data-dir "$AUTH_DATA" \
  --state-dir "$STATE" --login-rate-limit 200 --rate-limit 500 --rate-limit-burst 500
FUNCTIONAL_PID=$SERVER_PID
wait_health "$PORT"

if [ -d "$WEB_ROOT" ]; then
  curl -fsS "http://127.0.0.1:$PORT/" | grep -q 'EdgeDesk' || { echo "frontend shell not served" >&2; exit 1; }
  HASHED=$(cd "$WEB_ROOT" && ls assets/*.js 2>/dev/null | head -1 || true)
  if [ -n "$HASHED" ]; then
    curl -fsS "http://127.0.0.1:$PORT/$HASHED" >/dev/null
  fi
  log "frontend shell + hashed asset OK"
fi

python3 scripts/auth_test.py --host 127.0.0.1 --port "$PORT" \
  --password "$PASSWORD" --new-password "$NEW_PASSWORD" --token "$TOKEN" --cookie-out "$ROOT/old-cookie.txt"

log "restart persistence + old session invalidation"
stop_pid "$FUNCTIONAL_PID"
start_server "$RESTART_PORT" --file-root "$DATA" --auth-token "$TOKEN" --data-dir "$AUTH_DATA" \
  --state-dir "$STATE" --login-rate-limit 200 --rate-limit 500 --rate-limit-burst 500
FUNCTIONAL_PID=$SERVER_PID
wait_health "$RESTART_PORT"
python3 scripts/auth_test.py --host 127.0.0.1 --port "$RESTART_PORT" --password "$NEW_PASSWORD" \
  --token "$TOKEN" --check-old-cookie "$ROOT/old-cookie.txt"
grep -q '"theme":"light"' "$AUTH_DATA/settings.json"
grep -q '"credential_version":2' "$AUTH_DATA/auth.json"

python3 scripts/api_test.py 127.0.0.1 "$RESTART_PORT" "$TOKEN" "$DATA" "$NEW_PASSWORD"
python3 scripts/ws_test.py 127.0.0.1 "$RESTART_PORT" "$NEW_PASSWORD"
python3 scripts/extension_test.py --host 127.0.0.1 --port "$RESTART_PORT" \
  --password "$NEW_PASSWORD" --apps-root "$APPS" --file-root "$DATA" --state-dir "$STATE"

# The rate-limit instance needs exclusive access to the same auth data dir.
stop_pid "$FUNCTIONAL_PID"
log "rate limit suite on port $((PORT + 1))"
start_server "$((PORT + 1))" --file-root "$DATA" --auth-token "$TOKEN" --data-dir "$AUTH_DATA" \
  --state-dir "$STATE" --login-rate-limit 2 --rate-limit 2 --rate-limit-burst 2
RATE_PID=$SERVER_PID
wait_health "$((PORT + 1))"
python3 scripts/rate_limit_test.py 127.0.0.1 "$((PORT + 1))" "$NEW_PASSWORD"

# Recovery must refuse to touch state while a server still holds the lock.
if run_binary --reset-admin-password --data-dir "$AUTH_DATA" --state-dir "$STATE" --auth-token "$TOKEN" \
  > "$ROOT/recovery-while-running.log" 2>&1; then
  echo "reset-admin-password unexpectedly succeeded while the server was running" >&2
  exit 1
fi
test -f "$AUTH_DATA/auth.json"
stop_pid "$RATE_PID"

log "Secure cookie flag"
start_server "$((PORT + 4))" --file-root "$DATA" --auth-token "$TOKEN" --data-dir "$AUTH_DATA" \
  --state-dir "$STATE" --cookie-secure --login-rate-limit 200 --rate-limit 500 --rate-limit-burst 500
SECURE_PID=$SERVER_PID
wait_health "$((PORT + 4))"
python3 scripts/auth_test.py --host 127.0.0.1 --port "$((PORT + 4))" --password "$NEW_PASSWORD" \
  --token "$TOKEN" --expect-secure
stop_pid "$SECURE_PID"

log "local password recovery"
stop_pid "$FUNCTIONAL_PID"
stop_pid "$RATE_PID"
run_binary --reset-admin-password --data-dir "$AUTH_DATA" --state-dir "$STATE" --auth-token "$TOKEN" \
  > "$ROOT/recovery.log" 2>&1
grep -q "authentication was reset" "$ROOT/recovery.log"
test -f "$AUTH_DATA/settings.json"
grep -q '"theme":"light"' "$AUTH_DATA/settings.json"

start_server "$((PORT + 2))" --file-root "$DATA" --auth-token "$TOKEN" --data-dir "$AUTH_DATA" \
  --state-dir "$STATE" --login-rate-limit 200 --rate-limit 500 --rate-limit-burst 500
RECOVERY_PID=$SERVER_PID
wait_health "$((PORT + 2))"
python3 scripts/auth_test.py --host 127.0.0.1 --port "$((PORT + 2))" \
  --password "$RECOVERY_PASSWORD" --token "$TOKEN"
stop_pid "$RECOVERY_PID"

log "corrupt auth state fail-closed + local recovery"
printf '{not-json' > "$CORRUPT_DATA/auth.json"
chmod 600 "$CORRUPT_DATA/auth.json"
start_server "$((PORT + 3))" --file-root "$DATA" --auth-token "$TOKEN" --data-dir "$CORRUPT_DATA" \
  --state-dir "$STATE" --login-rate-limit 200 --rate-limit 500 --rate-limit-burst 500
CORRUPT_PID=$SERVER_PID
wait_health "$((PORT + 3))"
python3 scripts/auth_test.py --host 127.0.0.1 --port "$((PORT + 3))" --expect-corrupt
stop_pid "$CORRUPT_PID"
run_binary --reset-admin-password --data-dir "$CORRUPT_DATA" --state-dir "$STATE" --auth-token "$TOKEN" \
  > "$ROOT/corrupt-recovery.log" 2>&1
start_server "$((PORT + 3))" --file-root "$DATA" --auth-token "$TOKEN" --data-dir "$CORRUPT_DATA" \
  --state-dir "$STATE" --login-rate-limit 200 --rate-limit 500 --rate-limit-burst 500
CORRUPT_PID=$SERVER_PID
wait_health "$((PORT + 3))"
python3 scripts/auth_test.py --host 127.0.0.1 --port "$((PORT + 3))" \
  --password "$CORRUPT_PASSWORD" --setup-only
stop_pid "$CORRUPT_PID"

log "extension transfer crash recovery"
ACTIVE_APPS="$EXT_APPS"
start_server "$((PORT + 6))" --file-root "$EXT_FILES" --data-dir "$EXT_DATA" --state-dir "$EXT_STATE" \
  --login-rate-limit 200 --rate-limit 1000 --rate-limit-burst 2000
CRASH_PID=$SERVER_PID
wait_health "$((PORT + 6))"
python3 scripts/auth_test.py --host 127.0.0.1 --port "$((PORT + 6))" \
  --password "$NEW_PASSWORD" --setup-only
python3 scripts/extension_test.py --host 127.0.0.1 --port "$((PORT + 6))" \
  --password "$NEW_PASSWORD" --apps-root "$EXT_APPS" --file-root "$EXT_FILES" --begin-dangling-upload
kill -9 "$CRASH_PID" 2>/dev/null || true
wait "$CRASH_PID" 2>/dev/null || true
start_server "$((PORT + 6))" --file-root "$EXT_FILES" --data-dir "$EXT_DATA" --state-dir "$EXT_STATE" \
  --login-rate-limit 200 --rate-limit 1000 --rate-limit-burst 2000
CRASH_PID=$SERVER_PID
wait_health "$((PORT + 6))"
python3 scripts/extension_test.py --host 127.0.0.1 --port "$((PORT + 6))" \
  --password "$NEW_PASSWORD" --apps-root "$EXT_APPS" --file-root "$EXT_FILES" --check-clean
stop_pid "$CRASH_PID"
unset ACTIVE_APPS

log "audit log sanity"
if [ -f "$STATE/audit.log" ]; then
  grep -q '"event":"auth.setup"' "$STATE/audit.log"
  grep -q '"event":"login"' "$STATE/audit.log"
  grep -q '"event":"auth.password_change"' "$STATE/audit.log"
  grep -q '"event":"auth.reset"' "$STATE/audit.log"
  grep -q '"event":"file.upload"' "$STATE/audit.log"
  grep -q '"event":"terminal.create"' "$STATE/audit.log"
else
  echo "audit log missing" >&2
  exit 1
fi

log "ALL TESTS PASSED"
