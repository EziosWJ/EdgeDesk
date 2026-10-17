#!/usr/bin/env bash
# EdgeDesk headless page screenshot helper.
#
# Usage:
#   scripts/screenshot.sh <url> <output.png> [options]
#
# The script is a thin, validated wrapper around Playwright's CLI. It always
# uses the pinned local @playwright/test and the Chromium already present in
# Playwright's browser cache; it never runs npm/npx or downloads a browser.
# Screenshots should be written to /tmp or out/, never committed.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PLAYWRIGHT="$REPO_ROOT/web/node_modules/.bin/playwright"
# Reuse the existing browser cache; never trigger an install/download.
export PLAYWRIGHT_BROWSERS_PATH="${PLAYWRIGHT_BROWSERS_PATH:-$HOME/.cache/ms-playwright}"

URL=""
OUTPUT=""
VIEWPORT="1440x900"
FULL_PAGE=0
WAIT_SELECTOR=""
WAIT_MS=800
TIMEOUT=30000
COLOR_SCHEME=""
STORAGE=""
BROWSER_LANG="zh-CN"

log() { printf '[screenshot] %s\n' "$*"; }
die() { printf '[screenshot] ERROR: %s\n' "$*" >&2; exit 1; }

usage() {
  cat <<'EOF'
Usage: scripts/screenshot.sh <url> <output.png> [options]

Options:
  --viewport WxH       viewport size (default: 1440x900)
  --full-page          capture the full scrollable page
  --wait-selector SEL  wait for a selector before capturing
  --wait-ms N          extra settle delay after load (default: 800)
  --timeout MS         navigation/action timeout (default: 30000)
  --color-scheme MODE  "light" or "dark"
  --storage FILE       Playwright storage-state JSON (authenticated pages)
  --lang LANG          browser locale (default: zh-CN)
  -h, --help           show this help

Examples:
  scripts/screenshot.sh http://127.0.0.1:8080/login /tmp/opencode/login.png --full-page
  scripts/screenshot.sh http://127.0.0.1:8080/ /tmp/opencode/mobile.png \
    --viewport 390x844 --wait-selector '.ed-dashboard'
EOF
}

need_value() {
  [ "$#" -ge 2 ] || die "$1 requires a value"
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --viewport)
      need_value "$@"
      VIEWPORT=$2
      shift 2
      ;;
    --viewport=*)
      VIEWPORT=${1#*=}
      shift
      ;;
    --full-page)
      FULL_PAGE=1
      shift
      ;;
    --wait-selector)
      need_value "$@"
      WAIT_SELECTOR=$2
      shift 2
      ;;
    --wait-selector=*)
      WAIT_SELECTOR=${1#*=}
      shift
      ;;
    --wait-ms)
      need_value "$@"
      WAIT_MS=$2
      shift 2
      ;;
    --wait-ms=*)
      WAIT_MS=${1#*=}
      shift
      ;;
    --timeout)
      need_value "$@"
      TIMEOUT=$2
      shift 2
      ;;
    --timeout=*)
      TIMEOUT=${1#*=}
      shift
      ;;
    --color-scheme)
      need_value "$@"
      COLOR_SCHEME=$2
      shift 2
      ;;
    --color-scheme=*)
      COLOR_SCHEME=${1#*=}
      shift
      ;;
    --storage)
      need_value "$@"
      STORAGE=$2
      shift 2
      ;;
    --storage=*)
      STORAGE=${1#*=}
      shift
      ;;
    --lang)
      need_value "$@"
      BROWSER_LANG=$2
      shift 2
      ;;
    --lang=*)
      BROWSER_LANG=${1#*=}
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    -*)
      die "unknown option: $1"
      ;;
    *)
      if [ -z "$URL" ]; then
        URL=$1
      elif [ -z "$OUTPUT" ]; then
        OUTPUT=$1
      else
        die "unexpected argument: $1"
      fi
      shift
      ;;
  esac
done

[ -n "$URL" ] || { usage >&2; die "URL is required"; }
[ -n "$OUTPUT" ] || { usage >&2; die "output path is required"; }
[ -x "$PLAYWRIGHT" ] || die "local Playwright missing at $PLAYWRIGHT (capture never installs; set up web/node_modules once)"

case "$VIEWPORT" in
  *x*) ;;
  *) die "invalid viewport '$VIEWPORT' (expected WxH, e.g. 1440x900)" ;;
esac
VIEW_WIDTH=${VIEWPORT%x*}
VIEW_HEIGHT=${VIEWPORT#*x}
[[ "$VIEW_WIDTH" =~ ^[0-9]+$ && "$VIEW_HEIGHT" =~ ^[0-9]+$ ]] \
  || die "invalid viewport '$VIEWPORT' (expected WxH, e.g. 1440x900)"
[ "$VIEW_WIDTH" -gt 0 ] && [ "$VIEW_HEIGHT" -gt 0 ] \
  || die "viewport dimensions must be positive"

[[ "$WAIT_MS" =~ ^[0-9]+$ ]] || die "invalid --wait-ms: $WAIT_MS"
[[ "$TIMEOUT" =~ ^[0-9]+$ ]] || die "invalid --timeout: $TIMEOUT"

if [ -n "$COLOR_SCHEME" ]; then
  case "$COLOR_SCHEME" in
    light|dark) ;;
    *) die "--color-scheme must be 'light' or 'dark'" ;;
  esac
fi

if [ -n "$STORAGE" ]; then
  [ -f "$STORAGE" ] || die "storage state not found: $STORAGE"
  STORAGE="$(cd "$(dirname -- "$STORAGE")" && pwd)/$(basename -- "$STORAGE")"
fi

OUTPUT_DIR=$(dirname -- "$OUTPUT")
mkdir -p -- "$OUTPUT_DIR"
OUTPUT="$(cd -- "$OUTPUT_DIR" && pwd)/$(basename -- "$OUTPUT")"
[ ! -d "$OUTPUT" ] || die "output path is a directory: $OUTPUT"

ARGS=(screenshot --browser chromium --lang "$BROWSER_LANG" --timeout "$TIMEOUT" --wait-for-timeout "$WAIT_MS" --viewport-size "$VIEW_WIDTH,$VIEW_HEIGHT")
if [ "$FULL_PAGE" -eq 1 ]; then
  ARGS+=(--full-page)
fi
if [ -n "$WAIT_SELECTOR" ]; then
  ARGS+=(--wait-for-selector "$WAIT_SELECTOR")
fi
if [ -n "$COLOR_SCHEME" ]; then
  ARGS+=(--color-scheme "$COLOR_SCHEME")
fi
if [ -n "$STORAGE" ]; then
  ARGS+=(--load-storage "$STORAGE")
fi

log "capturing $URL -> $OUTPUT"
"$PLAYWRIGHT" "${ARGS[@]}" "$URL" "$OUTPUT"
[ -s "$OUTPUT" ] || die "screenshot was not created: $OUTPUT"

log "saved $OUTPUT ($(wc -c < "$OUTPUT" | tr -d ' ') bytes)"
if command -v file >/dev/null 2>&1; then
  file -b "$OUTPUT" | sed 's/^/image: /'
fi
