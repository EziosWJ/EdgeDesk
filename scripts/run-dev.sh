#!/usr/bin/env bash
# Local development launcher.
#
# The production default for --apps-root is /var/lib/edgedesk/apps, which is
# normally writable only by the service account. Running the dev binary as a
# desktop user therefore reports "extension platform unavailable" unless a
# writable root is supplied. This script keeps all runtime state under
# out/dev-data/ (gitignored) and passes the required absolute paths.
#
# Usage:
#   scripts/run-dev.sh [extra edgedesk options...]
#   EDGEDESK_PORT=8090 scripts/run-dev.sh
#   EDGEDESK_BUILD_DIR=out/build/host-embed scripts/run-dev.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${EDGEDESK_BUILD_DIR:-out/build/dev-disk}"
BIN="$ROOT/$BUILD_DIR/bin/edgedesk"
DEV_ROOT="$ROOT/out/dev-data"
PORT="${EDGEDESK_PORT:-8080}"

if [ ! -x "$BIN" ]; then
  echo "error: $BIN not found; build the dev-disk preset first:" >&2
  echo "  cmake --preset dev-disk && cmake --build --preset dev-disk" >&2
  exit 1
fi

if [ ! -f "$ROOT/web/dist/index.html" ]; then
  echo "error: web/dist is missing; build the frontend first:" >&2
  echo "  (cd web && npm ci && npm run build)" >&2
  exit 1
fi

mkdir -p "$DEV_ROOT/files" "$DEV_ROOT/data" "$DEV_ROOT/state" "$DEV_ROOT/apps"

echo "[run-dev] binary: $BIN"
echo "[run-dev] file-root: $DEV_ROOT/files"
echo "[run-dev] data-dir:  $DEV_ROOT/data"
echo "[run-dev] state-dir: $DEV_ROOT/state"
echo "[run-dev] apps-root: $DEV_ROOT/apps"
echo "[run-dev] url:       http://127.0.0.1:$PORT/"

exec "$BIN" \
  --host 127.0.0.1 \
  --port "$PORT" \
  --web-root "$ROOT/web/dist" \
  --file-root "$DEV_ROOT/files" \
  --data-dir "$DEV_ROOT/data" \
  --state-dir "$DEV_ROOT/state" \
  --apps-root "$DEV_ROOT/apps" \
  "$@"
