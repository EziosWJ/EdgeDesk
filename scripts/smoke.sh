#!/usr/bin/env bash
set -euo pipefail
BIN=${1:-out/build/dev-disk/bin/edgedesk}
PORT=${2:-19091}
ROOT=${3:-web/dist}
"$BIN" --host 127.0.0.1 --port "$PORT" --web-root "$ROOT" > /tmp/edgedesk-smoke-"$PORT".log 2>&1 &
pid=$!
trap 'kill "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true' EXIT
for _ in {1..60}; do
  if curl -fsS "http://127.0.0.1:$PORT/api/health" >/tmp/edgedesk-health-"$PORT".json; then break; fi
  sleep 0.2
done
python3 - "$PORT" <<'PY'
import json, sys, urllib.request
port=sys.argv[1]
url=f'http://127.0.0.1:{port}'
health=json.load(urllib.request.urlopen(url+'/api/health'))
assert health['status']=='ok' and health['phase']==1, health
page=urllib.request.urlopen(url+'/').read()
assert b'EdgeDesk' in page
print('SMOKE PASS: GET /api/health, GET /')
PY
