#!/usr/bin/env bash
# EdgeDesk L1 pre-commit / pull-request verification.
#
# Builds the host dev-disk server, runs all native unit tests and the full
# integration suite, then builds and tests the React frontend.
#
# This lane is intentionally host-only. The embed, musl and arm64/qemu matrix
# is L2 and belongs to CI, nightly or release gates.
#
# Usage: scripts/test-pr.sh [--no-frontend] [--no-integration] [--port N]
#
# Environment:
#   JOBS        build parallelism (default: nproc)
#   CTEST_JOBS  test parallelism (default: 4)
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

JOBS=${JOBS:-$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}
CTEST_JOBS=${CTEST_JOBS:-4}
PORT=${PORT:-19091}
RUN_FRONTEND=1
RUN_INTEGRATION=1
START=$SECONDS

# Preserve the caller's EDGEDESK_BUILD_WEB setting; this script temporarily
# turns it off so CMake does not run npm a second time.
CACHE_FILE="out/build/dev-disk/CMakeCache.txt"
PREV_BUILD_WEB="ON"
if [ -f "$CACHE_FILE" ]; then
  PREV_BUILD_WEB=$(sed -n 's/^EDGEDESK_BUILD_WEB:BOOL=//p' "$CACHE_FILE")
  [ -n "$PREV_BUILD_WEB" ] || PREV_BUILD_WEB="ON"
fi
CONFIGURED=0
restore_build_web() {
  if [ "$CONFIGURED" -eq 1 ] && [ "$PREV_BUILD_WEB" != "OFF" ]; then
    cmake --preset dev-disk -DEDGEDESK_BUILD_WEB="$PREV_BUILD_WEB" >/dev/null 2>&1 || true
  fi
}
trap restore_build_web EXIT

log() { printf '\n[test-pr] %s\n' "$*"; }
die() { printf '[test-pr] ERROR: %s\n' "$*" >&2; exit 1; }

usage() {
  cat <<'EOF'
Usage: scripts/test-pr.sh [--no-frontend] [--no-integration] [--port N]

L1: frontend test/build, dev-disk build, native unit tests, integration suite.
The integration suite uses N..N+5, so pick a free base port.

Options:
  --no-frontend      reuse an existing web/dist and skip npm test/build
  --no-integration   stop after native unit tests
  --port N           integration base port (default: 19091)

Environment:
  JOBS        build parallelism (default: nproc)
  CTEST_JOBS  test parallelism (default: 4)
EOF
}

while [ $# -gt 0 ]; do
  case "$1" in
    --no-frontend) RUN_FRONTEND=0 ;;
    --no-integration) RUN_INTEGRATION=0 ;;
    --port)
      [ $# -ge 2 ] || die "--port requires a value"
      PORT=$2
      shift
      ;;
    --port=*) PORT=${1#*=} ;;
    -h|--help) usage; exit 0 ;;
    *) die "unknown option: $1" ;;
  esac
  shift
done

[[ "$PORT" =~ ^[0-9]+$ ]] || die "invalid port: $PORT"
PORT=$((10#$PORT))
if [ "$PORT" -lt 1 ] || [ "$PORT" -gt 65530 ]; then
  die "port must be between 1 and 65530"
fi

if [ "$RUN_FRONTEND" -eq 1 ]; then
  command -v npm >/dev/null 2>&1 || die "npm is required for the frontend build"
  if [ ! -d web/node_modules ]; then
    log "L1 install frontend dependencies (npm ci)"
    (cd web && npm ci --no-audit --no-fund)
  fi
  log "L1 frontend unit tests"
  (cd web && npm test)
  log "L1 frontend build"
  (cd web && npm run build)
else
  [ -d web/dist ] || die "--no-frontend requires an existing web/dist"
fi

# Keep CMake from invoking npm on every build; the frontend was built above.
# The EXIT trap restores the previous EDGEDESK_BUILD_WEB value.
log "L1 configure dev-disk (EDGEDESK_BUILD_WEB=OFF)"
CONFIGURED=1
cmake --preset dev-disk -DEDGEDESK_BUILD_WEB=OFF

log "L1 build dev-disk (-j $JOBS)"
cmake --build --preset dev-disk -j "$JOBS"

log "L1 native tests (ctest -j $CTEST_JOBS)"
ctest --preset dev-disk -j "$CTEST_JOBS" --output-on-failure

if [ "$RUN_INTEGRATION" -eq 1 ]; then
  log "L1 integration (base port $PORT, web root web/dist)"
  bash scripts/integration_test.sh out/build/dev-disk/bin/edgedesk "$PORT" web/dist
fi

log "L1 passed in $((SECONDS - START))s"
