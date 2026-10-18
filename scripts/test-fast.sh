#!/usr/bin/env bash
# EdgeDesk L0 fast feedback loop.
#
# Builds the core-only host test target (no libhv server) and runs the native
# unit tests plus the frontend unit tests when node_modules is present.
# This is the "every edit" lane; it does not replace L1/L2 verification.
#
# Usage: scripts/test-fast.sh [--no-frontend]
#
# Environment:
#   JOBS        build parallelism (default: nproc)
#   CTEST_JOBS  test parallelism (default: 4)
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

JOBS=${JOBS:-$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}
CTEST_JOBS=${CTEST_JOBS:-4}
RUN_FRONTEND=1
START=$SECONDS

log() { printf '\n[test-fast] %s\n' "$*"; }
die() { printf '[test-fast] ERROR: %s\n' "$*" >&2; exit 1; }

usage() {
  cat <<'EOF'
Usage: scripts/test-fast.sh [--no-frontend]

L0: host-offline-tests build + native unit tests + frontend unit tests.
Skips frontend tests with a notice when web/node_modules is missing.

Environment:
  JOBS        build parallelism (default: nproc)
  CTEST_JOBS  test parallelism (default: 4)
EOF
}

for arg in "$@"; do
  case "$arg" in
    --no-frontend) RUN_FRONTEND=0 ;;
    -h|--help) usage; exit 0 ;;
    *) die "unknown option: $arg" ;;
  esac
done

log "L0 configure host-offline-tests"
cmake --preset host-offline-tests

log "L0 build host-offline-tests (-j $JOBS)"
cmake --build --preset host-offline-tests -j "$JOBS"

log "L0 native tests (ctest -j $CTEST_JOBS)"
ctest --preset host-offline-tests -j "$CTEST_JOBS" --output-on-failure

if [ "$RUN_FRONTEND" -eq 1 ]; then
  if [ ! -d web/node_modules ]; then
    log "web/node_modules missing; skipping frontend unit tests (run: cd web && npm ci)"
  else
    log "L0 frontend unit tests"
    (cd web && npm test)
  fi
fi

if command -v node >/dev/null 2>&1 && [ -f tools/ext-sdk/package.json ]; then
  log "L0 extension SDK/CLI tests"
  (cd tools/ext-sdk && node --test test/*.test.js)
else
  log "node missing; skipping extension SDK/CLI tests"
fi

log "L0 passed in $((SECONDS - START))s"
