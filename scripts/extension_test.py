#!/usr/bin/env python3
"""EdgeDesk extension platform integration test (standard library only).

Runs against an already-running, initialized server. Covers install/approval,
static revisions, runtime sessions, KV quotas/transactions, file transfers and
locks, terminal lifecycle, upgrade/overwrite/downgrade, integrity isolation,
reinstall recovery and uninstall data disposition.

Usage:
  scripts/extension_test.py --port 19091 --password secret \
      --apps-root /tmp/apps --file-root /tmp/files --state-dir /tmp/state
  scripts/extension_test.py ... --begin-dangling-upload
  scripts/extension_test.py ... --check-clean
"""
import argparse
import hashlib
import json
import os
import sys
import tempfile
import urllib.error
import urllib.request
import zipfile

FAILURES = []
ORIGIN = ""

# Fake secrets injected through the runtime error-report bridge. The audit log
# must never contain these values and must remain one JSON object per line.
AUDIT_ERROR_SECRETS = [
    "IT-BEARER-SECRET-0f0f",
    "IT-COOKIE-SECRET-1a1a",
    "IT-APIKEY-SECRET-2b2b",
    "IT-PASSWORD-SECRET-3c3c",
    "IT-QUERY-SECRET-4d4d",
    "IT-FRAGMENT-SECRET-5e5e",
]


def check(condition, message):
    if condition:
        return True
    FAILURES.append(message)
    print(f"FAILED: {message}")
    return False


def request(method, path, body=None, cookie=None, raw=None, content_type="application/json",
            headers=None):
    data = raw if raw is not None else (json.dumps(body).encode() if body is not None else None)
    req = urllib.request.Request(BASE + path, data=data, method=method)
    if data is not None:
        req.add_header("Content-Type", content_type)
    if cookie:
        req.add_header("Cookie", "edgedesk_session=" + cookie)
        req.add_header("Origin", ORIGIN)
    for key, value in (headers or {}).items():
        req.add_header(key, value)
    try:
        with urllib.request.urlopen(req, timeout=60) as response:
            return response.status, response.read(), response.headers
    except urllib.error.HTTPError as error:
        return error.code, error.read(), error.headers


def json_body(payload):
    try:
        return json.loads(payload.decode())
    except Exception:
        return {}


def login(password):
    status, payload, headers = request("POST", "/api/auth/login", {"password": password})
    check(status == 200, f"login failed: {status} {payload[:200]!r}")
    raw = headers.get("Set-Cookie", "")
    if "edgedesk_session=" not in raw:
        check(False, "login did not set a session cookie")
        return ""
    return raw.split("edgedesk_session=", 1)[1].split(";", 1)[0]


def make_zip(path, manifest, index, script, extra=None):
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as archive:
        archive.writestr("manifest.json", json.dumps(manifest))
        archive.writestr("index.html", index)
        archive.writestr("assets/app.js", script)
        for name, content in (extra or {}).items():
            archive.writestr(name, content)


def manifest_for(app_id, version, permissions=None, origins=None):
    manifest = {
        "id": app_id,
        "name": "Integration App",
        "description": "Extension integration test",
        "version": version,
        "entry": "index.html",
        "minHostApiVersion": "1.0.0",
        "maxHostApiVersion": "1.9.9",
        "permissions": permissions or ["storage", "files.manage", "terminal.access"],
        "window": {
            "defaultWidth": 720,
            "defaultHeight": 480,
            "minWidth": 320,
            "minHeight": 240,
            "instances": "multi",
        },
    }
    if origins:
        manifest["network"] = {"origins": origins}
    return manifest


def install(cookie, path, upgrade_app=None, query=""):
    url = f"/api/extensions/{upgrade_app}/upgrade{query}" if upgrade_app else f"/api/extensions/install{query}"
    with open(path, "rb") as archive:
        data = archive.read()
    return request("PUT", url, cookie=cookie, raw=data, content_type="application/zip")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=19091)
    parser.add_argument("--password", required=True)
    parser.add_argument("--apps-root", required=True)
    parser.add_argument("--file-root", required=True)
    parser.add_argument("--state-dir", default="")
    parser.add_argument("--begin-dangling-upload", action="store_true")
    parser.add_argument("--check-clean", action="store_true")
    args = parser.parse_args()

    global BASE, ORIGIN
    BASE = f"http://{args.host}:{args.port}"
    ORIGIN = BASE

    if args.check_clean:
        leftovers = []
        for root, _dirs, files in os.walk(args.file_root):
            for name in files:
                if name.startswith(".edgedesk-upload-") and name.endswith(".tmp"):
                    leftovers.append(os.path.join(root, name))
        check(not leftovers, f"orphaned upload temp files survived restart: {leftovers}")
        journal_dir = os.path.join(args.apps_root, "tmp", "transfers")
        journals = os.listdir(journal_dir) if os.path.isdir(journal_dir) else []
        check(not journals, f"transfer journals survived restart: {journals}")
        cookie = login(args.password)
        if cookie:
            status, payload, _ = request("GET", "/api/extensions/io.example.dangling", cookie=cookie)
            check(status == 200 and json_body(payload).get("app", {}).get("status") == "enabled",
                  f"app state was not recovered after restart: {status} {payload[:200]!r}")
        return finish("extension_restart_cleanup")

    cookie = login(args.password)
    if not cookie:
        return finish("extension_test")

    app_id = "io.example.integration"
    revision_dir = os.path.join(args.apps_root, "revisions", app_id)
    # Deterministic 2 MiB asset: large enough to exercise the streaming route.
    large_asset = bytes(range(256)) * 8192
    with tempfile.TemporaryDirectory(prefix="edgedesk-ext-it-") as temp:
        v1 = os.path.join(temp, "v1.zip")
        make_zip(v1, manifest_for(app_id, "1.0.0", origins=["https://*.example.com"]),
                 "<h1>integration v1</h1>", "console.log('v1')",
                 extra={"assets/large.bin": large_asset})

        status, payload, _ = install(cookie, v1)
        body = json_body(payload)
        check(status == 201, f"install status {status}: {payload[:300]!r}")
        check(body.get("approval_required") is True, "fresh install must require approval")
        check(body.get("app", {}).get("status") == "pending_review", "fresh install must be pending_review")
        rev1 = body.get("revision", {}).get("revision_id", "")
        check(len(rev1) == 32, f"unexpected revision id: {rev1!r}")
        check(any("not verified" in warning or "identity" in warning for warning in body.get("warnings", [])),
              "unsigned publisher warning missing")

        status, _, _ = request("GET", f"/apps/{app_id}/1.0.0/{rev1}/")
        check(status == 404, f"pending app static asset should be 404, got {status}")

        status, payload, _ = request("POST", f"/api/extensions/{app_id}/approve",
                                     {"permissions": ["storage", "files.manage", "terminal.access", "network"],
                                      "enable": True}, cookie)
        check(status == 200, f"approve failed: {status} {payload[:200]!r}")
        check(json_body(payload).get("app", {}).get("status") == "enabled", "app was not enabled")

        status, payload, headers = request("GET", f"/apps/{app_id}/1.0.0/{rev1}/")
        check(status == 200, f"enabled static asset failed: {status}")
        check(b"integration v1" in payload, "static body mismatch")
        csp = headers.get("Content-Security-Policy", "")
        check("https://*.example.com" in csp and "https://example.com" in csp,
              f"wildcard CSP sources missing: {csp}")
        check("object-src 'none'" in csp, "CSP object-src missing")
        status, _, _ = request("GET", f"/apps/{app_id}/1.0.0/{rev1}/missing.js")
        check(status == 404, "missing asset with extension must not SPA-fallback")
        status, _, _ = request("GET", "/apps")
        check(status == 404, "bare /apps must not SPA-fallback")
        status, _, _ = request("GET", "/apps/")
        check(status == 404, "bare /apps/ must not SPA-fallback")

        # Large asset: streamed body, HEAD without a body, and ETag 304.
        asset_url = f"/apps/{app_id}/1.0.0/{rev1}/assets/large.bin"
        status, payload, headers = request("GET", asset_url)
        check(status == 200 and payload == large_asset,
              f"streamed static asset mismatch: {status} {len(payload)} bytes")
        large_etag = headers.get("ETag", "")
        check(large_etag.startswith('"'), f"large asset ETag missing: {large_etag!r}")
        status, payload, headers = request("HEAD", asset_url)
        check(status == 200 and payload == b"",
              f"HEAD static asset returned a body: {status} {len(payload)} bytes")
        check(headers.get("Content-Length") == str(len(large_asset)),
              f"HEAD Content-Length mismatch: {headers.get('Content-Length')!r}")
        status, payload, _ = request("GET", asset_url, headers={"If-None-Match": large_etag})
        check(status == 304 and payload == b"", f"If-None-Match did not produce 304: {status}")

        status, payload, _ = request("POST", "/api/extensions/runtime/sessions",
                                     {"app_id": app_id, "instance_id": "11111111-2222-3333-4444-555555555555"},
                                     cookie)
        session = json_body(payload)
        token = session.get("token", "")
        check(status == 201 and len(token) == 64, f"runtime session failed: {status} {payload[:200]!r}")
        check(session.get("host_api_version") == "1.0.0", "host API version mismatch")
        runtime_headers = {"X-EdgeDesk-Runtime": token}

        def runtime(path, body):
            return request("POST", "/api/extensions/runtime/" + path, body, cookie, headers=runtime_headers)

        error_message = (
            "Authorization: Bearer " + AUDIT_ERROR_SECRETS[0] + "\n"
            "Cookie: edgedesk_session=" + AUDIT_ERROR_SECRETS[1] + "; theme=dark\n"
            "{\"api_key\":\"" + AUDIT_ERROR_SECRETS[2] + "\"}\n"
            "password=" + AUDIT_ERROR_SECRETS[3] + "\n"
            "https://example.com/cb?token=" + AUDIT_ERROR_SECRETS[4] +
            "#access_token=" + AUDIT_ERROR_SECRETS[5]
        )
        status, payload, _ = runtime("errors", {
            "type": "RuntimeError Bearer " + AUDIT_ERROR_SECRETS[0],
            "message": error_message,
            "stack": "Error: token=" + AUDIT_ERROR_SECRETS[4] + " at app.js:1:1",
        })
        check(status == 200 and json_body(payload).get("ok") is True,
              f"runtime error report failed: {status} {payload[:200]!r}")

        # Non-string fields are untrusted input too: they must not crash the
        # route or be serialized verbatim.
        status, payload, _ = runtime("errors", {
            "type": 42, "message": {"nested": True}, "stack": [1, 2],
        })
        check(status == 200 and json_body(payload).get("ok") is True,
              f"non-string error fields should be ignored: {status} {payload[:200]!r}")

        status, payload, _ = runtime("storage/set", {"key": "settings", "value": {"n": 1}})
        check(status == 200, f"storage set failed: {status} {payload[:200]!r}")
        status, payload, _ = runtime("storage/get", {"key": "settings"})
        check(status == 200 and json_body(payload).get("value") == {"n": 1}, "storage round-trip failed")
        status, payload, _ = runtime("storage/list", {})
        check(status == 200 and json_body(payload).get("usage_bytes") == 7, "storage list/usage mismatch")
        status, payload, _ = runtime("storage/set", {"key": "huge", "value": "x" * 300000})
        check(status == 413 and json_body(payload).get("error") == "quota_exceeded",
              f"per-key quota was not enforced: {status} {payload[:200]!r}")
        status, payload, _ = runtime("storage/transaction", {"operations": [
            {"op": "set", "key": "a", "value": [1, 2]},
            {"op": "set", "key": "too-big", "value": "y" * 300000},
        ]})
        check(status == 413, f"oversized transaction should fail: {status}")
        status, _, _ = runtime("storage/get", {"key": "a"})
        check(status == 404, "failed transaction partially committed")

        status, payload, _ = runtime("files/list", {"path": "/", "limit": 1})
        listing = json_body(payload)
        cursor = listing.get("next_cursor", "")
        check(status == 200, f"files list failed: {status} {payload[:200]!r}")
        cursor_file = os.path.join(args.file_root, "cursor-change-" + os.urandom(4).hex() + ".txt")
        open(cursor_file, "w").write("change")
        if cursor:
            status, payload, _ = runtime("files/list", {"path": "/", "cursor": cursor})
            check(status == 409 and json_body(payload).get("error") == "cursor_stale",
                  f"cursor staleness not detected: {status} {payload[:200]!r}")
            os.unlink(cursor_file)

        status, _, _ = runtime("files/mkdir", {"path": "/ext-it-dir"})
        check(status == 201, f"mkdir failed: {status}")
        status, _, _ = runtime("files/rename", {"from": "/ext-it-dir", "to": "/ext-it-renamed"})
        check(status == 200, f"rename failed: {status}")
        status, _, _ = runtime("files/delete", {"paths": ["/ext-it-renamed"]})
        check(status == 200, f"delete failed: {status}")

        payload_bytes = os.urandom(2_500_000)
        status, payload, _ = runtime("files/upload/begin", {"path": "/ext-upload.bin", "size": len(payload_bytes)})
        transfer = json_body(payload).get("transfer_id", "")
        check(status == 200 and transfer, f"upload begin failed: {status} {payload[:200]!r}")
        status, payload, _ = runtime("files/upload/begin", {"path": "/ext-upload.bin", "size": 1})
        check(status == 423 and json_body(payload).get("error") == "file_locked",
              f"write lock conflict not enforced: {status} {payload[:200]!r}")
        offset = 0
        while offset < len(payload_bytes):
            chunk = payload_bytes[offset:offset + 1048576]
            status, payload, _ = request(
                "PUT", f"/api/extensions/runtime/files/upload/{transfer}/chunk?offset={offset}",
                cookie=cookie, raw=chunk, content_type="application/octet-stream", headers=runtime_headers)
            check(status == 200, f"chunk {offset} failed: {status} {payload[:200]!r}")
            offset += len(chunk)
        status, payload, _ = runtime("files/upload/complete",
                                     {"transfer_id": transfer, "expected_size": len(payload_bytes)})
        check(status == 201, f"upload complete failed: {status} {payload[:200]!r}")
        target = os.path.join(args.file_root, "ext-upload.bin")
        check(os.path.isfile(target) and hashlib.sha256(open(target, "rb").read()).digest()
              == hashlib.sha256(payload_bytes).digest(), "uploaded bytes differ")
        check(not [name for name in os.listdir(args.file_root)
                   if name.startswith(".edgedesk-upload-")], "upload temp file was not cleaned")

        status, payload, _ = runtime("files/download/begin", {"path": "/ext-upload.bin"})
        download = json_body(payload).get("transfer_id", "")
        check(status == 200 and download, f"download begin failed: {status}")
        collected = bytearray()
        offset = 0
        while True:
            status, chunk, headers = request(
                "GET", f"/api/extensions/runtime/files/download/{download}/chunk?offset={offset}",
                cookie=cookie, headers=runtime_headers)
            check(status == 200, f"download chunk failed: {status} {chunk[:200]!r}")
            if status != 200:
                break
            collected += chunk
            offset += len(chunk)
            if headers.get("X-EdgeDesk-EOF") == "1":
                break
        status, _, _ = runtime("files/download/complete", {"transfer_id": download})
        check(status == 200, f"download complete failed: {status}")
        check(hashlib.sha256(collected).digest() == hashlib.sha256(payload_bytes).digest(),
              "downloaded bytes differ")

        status, payload, _ = runtime("terminal/sessions", {"title": "ext-it", "cols": 90, "rows": 30})
        terminal = json_body(payload).get("session", {})
        check(status == 201 and terminal.get("id"), f"terminal create failed: {status} {payload[:200]!r}")
        status, payload, _ = request("GET", "/api/extensions/runtime/terminal/sessions",
                                     cookie=cookie, headers=runtime_headers)
        check(status == 200 and any(item["id"] == terminal.get("id")
                                    for item in json_body(payload).get("sessions", [])),
              "terminal session missing from list")
        status, _, _ = runtime("terminal/close", {"session_id": terminal.get("id", "")})
        check(status == 200, f"terminal close failed: {status}")

        # A terminal left open must be terminated by disable.
        status, payload, _ = runtime("terminal/sessions", {"title": "ext-it-disable", "cols": 80, "rows": 24})
        open_terminal = json_body(payload).get("session", {}).get("id", "")
        check(status == 201 and open_terminal, f"second terminal create failed: {status}")

        # Disable invalidates the runtime session immediately.
        status, _, _ = request("POST", f"/api/extensions/{app_id}/disable", {}, cookie)
        check(status == 200, f"disable failed: {status}")
        status, payload, _ = request("GET", "/api/terminal/sessions", cookie=cookie)
        remaining = [item.get("id") for item in json_body(payload).get("sessions", [])]
        check(open_terminal not in remaining, "disable did not terminate the app's terminal session")
        status, payload, _ = runtime("storage/get", {"key": "settings"})
        check(status in (401, 403), f"disabled app runtime session still worked: {status}")
        status, _, _ = request("POST", f"/api/extensions/{app_id}/enable", {}, cookie)
        check(status == 200, f"enable failed: {status}")

        status, payload, _ = request("POST", "/api/extensions/runtime/sessions",
                                     {"app_id": app_id, "instance_id": "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"},
                                     cookie)
        token = json_body(payload).get("token", "")
        runtime_headers = {"X-EdgeDesk-Runtime": token}

        # Upgrade and same-version overwrite.
        v2 = os.path.join(temp, "v2.zip")
        make_zip(v2, manifest_for(app_id, "1.1.0"), "<h1>integration v2</h1>", "console.log('v2')")
        status, payload, _ = install(cookie, v2, upgrade_app=app_id)
        check(status == 201, f"upgrade failed: {status} {payload[:300]!r}")
        rev2 = json_body(payload).get("revision", {}).get("revision_id", "")
        check(rev2 and rev2 != rev1, "upgrade did not create a new revision")
        status, payload, _ = request("GET", f"/apps/{app_id}/1.1.0/{rev2}/")
        check(status == 200 and b"integration v2" in payload, "upgraded static content missing")
        status, payload, _ = install(cookie, v2, upgrade_app=app_id)
        check(status == 409 and json_body(payload).get("error") == "conflict",
              f"same version without overwrite should conflict: {status}")
        status, payload, _ = install(cookie, v2, upgrade_app=app_id, query="?overwrite=1")
        check(status == 201, f"same-version overwrite failed: {status} {payload[:200]!r}")
        rev_overwrite = json_body(payload).get("revision", {}).get("revision_id", "")
        check(rev_overwrite != rev2, "overwrite reused a revision id")

        # Downgrade to revision 1.
        status, payload, _ = request("POST", f"/api/extensions/{app_id}/activate",
                                     {"revision_id": rev1, "confirm": True}, cookie)
        check(status == 200, f"activate downgrade failed: {status} {payload[:200]!r}")
        status, payload, _ = request("GET", f"/apps/{app_id}/1.0.0/{rev1}/")
        check(status == 200 and b"integration v1" in payload, "downgrade did not activate v1")
        status, payload, _ = request("POST", "/api/extensions/runtime/sessions",
                                     {"app_id": app_id, "instance_id": "abababab-cdcd-efef-0101-232323232323"},
                                     cookie)
        token = json_body(payload).get("token", "")
        runtime_headers = {"X-EdgeDesk-Runtime": token}

        # Defense in depth: a file absent from the integrity manifest is never
        # served, even while the stored revision integrity state is still ok.
        unregistered = os.path.join(revision_dir, rev1, "unregistered.txt")
        with open(unregistered, "w") as handle:
            handle.write("unregistered")
        status, _, _ = request("GET", f"/apps/{app_id}/1.0.0/{rev1}/unregistered.txt")
        check(status == 404, f"unregistered asset was served: {status}")
        status, payload, _ = request("GET", f"/apps/{app_id}/1.0.0/{rev1}/")
        check(status == 200 and b"integration v1" in payload,
              f"registered asset became unavailable: {status}")
        os.unlink(unregistered)

        # Tampering with a historical revision marks only that revision
        # non-runnable: app status, active revision, grants, static serving and
        # the live runtime session must all remain untouched.
        historical_tampered = os.path.join(revision_dir, rev2, "index.html")
        with open(historical_tampered, "w") as handle:
            handle.write("<h1>historical tampered</h1>")
        status, payload, _ = request("POST", f"/api/extensions/{app_id}/integrity-check",
                                     {"revision_id": rev2}, cookie)
        result = json_body(payload)
        check(status == 409 and result.get("ok") is False,
              f"historical tampering was not detected: {status} {payload[:200]!r}")
        check(result.get("app_isolated") is False, "historical mismatch reported app isolation")
        status, payload, _ = request("GET", f"/api/extensions/{app_id}", cookie=cookie)
        detail = json_body(payload)
        app_state = detail.get("app", {})
        check(app_state.get("status") == "enabled", "historical mismatch changed app status")
        check(app_state.get("active_revision_id") == rev1, "historical mismatch changed the active revision")
        check(bool(app_state.get("granted_permissions")), "historical mismatch cleared grants")
        status, payload, _ = request("GET", f"/apps/{app_id}/1.0.0/{rev1}/")
        check(status == 200 and b"integration v1" in payload,
              "active revision stopped serving after historical mismatch")
        status, _, _ = runtime("storage/get", {"key": "settings"})
        check(status == 200, f"historical mismatch invalidated the active runtime session: {status}")
        status, payload, _ = request("POST", f"/api/extensions/{app_id}/activate",
                                     {"revision_id": rev2, "confirm": True}, cookie)
        check(status == 409, f"integrity-failed revision was activatable: {status} {payload[:200]!r}")
        if args.state_dir:
            with open(os.path.join(args.state_dir, "audit.log")) as handle:
                audit_lines = handle.readlines()
            expected = f'"revision={rev2} path=index.html revision_isolated"'
            check(any('"event":"extension.integrity"' in line and expected in line for line in audit_lines),
                  "audit did not record the revision-scoped integrity mismatch")
        tampered = os.path.join(revision_dir, rev1, "index.html")
        with open(tampered, "w") as handle:
            handle.write("<h1>tampered</h1>")
        status, payload, _ = request("POST", f"/api/extensions/{app_id}/integrity-check",
                                     {"revision_id": rev1}, cookie)
        result = json_body(payload)
        check(status == 409 and result.get("ok") is False, f"integrity check missed tampering: {status}")
        check(result.get("app_isolated") is True, "app was not isolated")
        status, payload, _ = runtime("storage/get", {"key": "settings"})
        check(status in (401, 403), f"isolated app runtime session still worked: {status}")
        status, payload, _ = request("GET", f"/apps/{app_id}/1.0.0/{rev1}/")
        check(status == 404, "isolated revision still served")
        if args.state_dir:
            with open(os.path.join(args.state_dir, "audit.log")) as handle:
                audit_lines = handle.readlines()
            expected = f'"revision={rev1} path=index.html app_isolated"'
            check(any('"event":"extension.integrity"' in line and expected in line for line in audit_lines),
                  "audit did not record app-scoped integrity isolation")

        # Repair by reinstalling a good package, then approve again.
        repaired = os.path.join(temp, "repaired.zip")
        make_zip(repaired, manifest_for(app_id, "1.0.0"), "<h1>repaired</h1>", "console.log('fixed')")
        status, payload, _ = install(cookie, repaired, query="?overwrite=1")
        check(status == 201, f"repair install failed: {status} {payload[:200]!r}")
        check(json_body(payload).get("approval_required") is True, "repair must require approval")
        status, _, _ = request("POST", f"/api/extensions/{app_id}/approve",
                               {"permissions": ["storage", "files.manage", "terminal.access"], "enable": True},
                               cookie)
        check(status == 200, f"repair approval failed: {status}")
        status, payload, _ = request("GET", f"/api/extensions/{app_id}", cookie=cookie)
        detail = json_body(payload)
        check(status == 200 and detail.get("app", {}).get("status") == "enabled", f"repaired app not enabled: {status} {payload[:400]!r}")

        # Uninstall with data retention, then restore on reinstall.
        status, payload, _ = request("POST", "/api/extensions/runtime/sessions",
                                     {"app_id": app_id, "instance_id": "cdcdcdcd-efef-0101-2323-454545454545"},
                                     cookie)
        token = json_body(payload).get("token", "")
        runtime_headers = {"X-EdgeDesk-Runtime": token}
        check(status == 201 and token, f"post-repair runtime session failed: {status} {payload[:200]!r}")
        status, payload, _ = runtime("storage/set", {"key": "keep-me", "value": {"kept": True}})
        check(status == 200, f"could not seed retained data: {status} {payload[:300]!r}")
        status, _, _ = request("POST", f"/api/extensions/{app_id}/uninstall", {"data_action": "keep"}, cookie)
        check(status == 200, f"uninstall keep failed: {status}")
        status, payload, _ = request("GET", f"/api/extensions/{app_id}", cookie=cookie)
        check(json_body(payload).get("app", {}).get("status") == "uninstalled", f"app not marked uninstalled: {payload[:400]!r}")
        status, payload, _ = install(cookie, repaired, query="?restore_data=1&overwrite=1")
        check(status == 201, f"reinstall with restore failed: {status} {payload[:200]!r}")
        check(json_body(payload).get("retained_data_restored") is True, "retained data was not restored")
        status, _, _ = request("POST", f"/api/extensions/{app_id}/approve",
                               {"permissions": ["storage"], "enable": True}, cookie)
        check(status == 200, "approval after restore failed")
        status, payload, _ = request("POST", "/api/extensions/runtime/sessions",
                                     {"app_id": app_id, "instance_id": "99999999-8888-7777-6666-555555555555"},
                                     cookie)
        token = json_body(payload).get("token", "")
        runtime_headers = {"X-EdgeDesk-Runtime": token}
        status, payload, _ = runtime("storage/get", {"key": "keep-me"})
        check(status == 200 and json_body(payload).get("value") == {"kept": True}, f"retained KV not restored: {status} {payload[:300]!r}")

        status, _, _ = request("POST", f"/api/extensions/{app_id}/uninstall", {"data_action": "delete"}, cookie)
        check(status == 200, f"uninstall delete failed: {status}")
        status, payload, _ = request("GET", f"/api/extensions/{app_id}", cookie=cookie)
        check(status == 404, f"deleted app record still visible: {status} {payload[:300]!r}")

        # ---- network permission and origin policy ----
        net_id = "io.example.network"
        net_v1 = os.path.join(temp, "net-v1.zip")
        make_zip(net_v1, manifest_for(net_id, "1.0.0", ["storage", "network"],
                                      ["https://*.example.net"]),
                 "<h1>network v1</h1>", "console.log('net1')")
        status, payload, _ = install(cookie, net_v1)
        check(status == 201, f"network install failed: {status} {payload[:200]!r}")
        net_rev1 = json_body(payload).get("revision", {}).get("revision_id", "")

        # Approve storage first: network stays ungranted, so no external sources.
        status, _, _ = request("POST", f"/api/extensions/{net_id}/approve",
                               {"permissions": ["storage"], "enable": True}, cookie)
        check(status == 200, f"network install approval failed: {status}")
        status, payload, headers = request("GET", f"/apps/{net_id}/1.0.0/{net_rev1}/")
        check(status == 200, f"network install static failed: {status}")
        csp = headers.get("Content-Security-Policy", "")
        check("example.net" not in csp, f"ungranted network leaked into CSP: {csp}")

        status, _, _ = request("POST", f"/api/extensions/{net_id}/permissions",
                               {"permissions": ["storage", "network"]}, cookie)
        check(status == 200, "network grant failed")
        status, _, headers = request("GET", f"/apps/{net_id}/1.0.0/{net_rev1}/")
        csp = headers.get("Content-Security-Policy", "")
        check("https://*.example.net" in csp and "https://example.net" in csp,
              f"approved wildcard sources missing from CSP: {csp}")
        for directive in ("script-src", "style-src", "img-src", "font-src", "connect-src", "media-src",
                          "frame-src", "child-src", "form-action"):
            segment = csp.split(directive, 1)[1].split(";", 1)[0]
            check("example.net" in segment, f"{directive} did not receive the approved origin: {segment}")

        status, payload, _ = request("POST", "/api/extensions/runtime/sessions",
                                     {"app_id": net_id,
                                      "instance_id": "22222222-3333-4444-5555-666666666666"}, cookie)
        net_session = json_body(payload)
        check(status == 201, f"network session failed: {status} {payload[:200]!r}")
        check(net_session.get("origins") == ["https://*.example.net"],
              f"session origins not limited to the approved set: {net_session.get('origins')}")

        # Revoking network removes CSP sources and new sessions lose origins.
        status, _, _ = request("POST", f"/api/extensions/{net_id}/permissions",
                               {"permissions": ["storage"]}, cookie)
        check(status == 200, f"network revoke failed: {status}")
        status, _, headers = request("GET", f"/apps/{net_id}/1.0.0/{net_rev1}/")
        csp = headers.get("Content-Security-Policy", "")
        check("example.net" not in csp, f"revoked network still in CSP: {csp}")
        check("form-action 'self'" in csp, f"CSP form-action was not reset: {csp}")
        status, payload, _ = request("POST", "/api/extensions/runtime/sessions",
                                     {"app_id": net_id,
                                      "instance_id": "33333333-4444-5555-6666-777777777777"}, cookie)
        revoked_session = json_body(payload)
        check(status == 201 and revoked_session.get("origins") == [],
              f"revoked session kept origins: {revoked_session.get('origins')}")

        # Re-grant, then disable/re-enable: approvals persist across sessions.
        status, _, _ = request("POST", f"/api/extensions/{net_id}/permissions",
                               {"permissions": ["storage", "network"]}, cookie)
        check(status == 200, f"network re-grant failed: {status}")
        status, _, _ = request("POST", f"/api/extensions/{net_id}/disable", {}, cookie)
        check(status == 200, "network disable failed")
        status, _, _ = request("POST", f"/api/extensions/{net_id}/enable", {}, cookie)
        check(status == 200, "network enable failed")
        status, payload, _ = request("POST", "/api/extensions/runtime/sessions",
                                     {"app_id": net_id,
                                      "instance_id": "44444444-5555-6666-7777-888888888888"}, cookie)
        reenabled = json_body(payload)
        check(status == 201 and reenabled.get("origins") == ["https://*.example.net"],
              f"re-enabled session lost approved origins: {reenabled.get('origins')}")

        # Upgrading to an expanded origin set removes network and needs review.
        net_v2 = os.path.join(temp, "net-v2.zip")
        make_zip(net_v2, manifest_for(net_id, "1.1.0", ["storage", "network"],
                                      ["https://*.example.net", "https://api.example.net"]),
                 "<h1>network v2</h1>", "console.log('net2')")
        status, payload, _ = install(cookie, net_v2, upgrade_app=net_id)
        check(status == 201, f"network upgrade failed: {status} {payload[:200]!r}")
        upgraded_net = json_body(payload).get("app", {})
        net_rev2 = json_body(payload).get("revision", {}).get("revision_id", "")
        check(upgraded_net.get("status") == "enabled", "network upgrade disabled the app unexpectedly")
        check("network" in upgraded_net.get("requested_permissions", []),
              "upgraded manifest did not request network")
        check("network" not in upgraded_net.get("granted_permissions", []),
              "expanded origins silently retained the network grant")
        status, _, headers = request("GET", f"/apps/{net_id}/1.1.0/{net_rev2}/")
        csp = headers.get("Content-Security-Policy", "")
        check("example.net" not in csp, f"expanded origins leaked into CSP before review: {csp}")
        status, payload, _ = request("POST", "/api/extensions/runtime/sessions",
                                     {"app_id": net_id,
                                      "instance_id": "55555555-6666-7777-8888-999999999999"}, cookie)
        upgraded_session = json_body(payload)
        check(status == 201 and upgraded_session.get("origins") == [],
              f"unreviewed upgrade kept origins: {upgraded_session.get('origins')}")

        # Explicit re-approval makes both origins effective.
        status, _, _ = request("POST", f"/api/extensions/{net_id}/approve",
                               {"permissions": ["storage", "network"], "enable": True}, cookie)
        check(status == 200, "network re-approval after upgrade failed")
        status, _, headers = request("GET", f"/apps/{net_id}/1.1.0/{net_rev2}/")
        csp = headers.get("Content-Security-Policy", "")
        check("https://*.example.net" in csp and "https://api.example.net" in csp,
              f"re-approved origins missing from CSP: {csp}")
        status, payload, _ = request("POST", "/api/extensions/runtime/sessions",
                                     {"app_id": net_id,
                                      "instance_id": "66666666-7777-8888-9999-aaaaaaaaaaaa"}, cookie)
        approved_session = json_body(payload)
        check(status == 201 and sorted(approved_session.get("origins", [])) ==
              sorted(["https://*.example.net", "https://api.example.net"]),
              f"session origins do not match the approved set: {approved_session.get('origins')}")

        status, _, _ = request("POST", f"/api/extensions/{net_id}/uninstall", {"data_action": "keep"},
                               cookie)
        check(status == 200, f"network uninstall failed: {status}")
        status, payload, _ = request("GET", f"/api/extensions/{net_id}", cookie=cookie)
        check(status == 200 and json_body(payload).get("app", {}).get("status") == "uninstalled",
              f"network app was not marked uninstalled: {status} {payload[:300]!r}")

    if args.state_dir:
        audit_path = os.path.join(args.state_dir, "audit.log")
        if os.path.isfile(audit_path):
            text = open(audit_path, encoding="utf-8", errors="replace").read()
            for event in ("extension.install", "extension.approve", "extension.upgrade",
                          "extension.integrity", "extension.uninstall", "extension.file",
                          "extension.terminal", "extension.storage", "extension.error"):
                check(event in text, f"audit event missing: {event}")
            check(args.password not in text, "audit leaked credentials")
            for line in text.splitlines():
                try:
                    json.loads(line)
                except json.JSONDecodeError:
                    check(False, f"audit line is not valid JSON: {line[:160]!r}")
            for secret in AUDIT_ERROR_SECRETS:
                check(secret not in text, f"audit leaked error-report secret: {secret}")
            check("[REDACTED]" in text, "audit redaction marker missing")
        else:
            check(False, f"audit log missing: {audit_path}")

    if args.begin_dangling_upload:
        cookie2 = login(args.password)
        dangling_id = "io.example.dangling"
        with tempfile.TemporaryDirectory(prefix="edgedesk-ext-dangling-") as temp:
            path = os.path.join(temp, "dangling.zip")
            make_zip(path, manifest_for(dangling_id, "1.0.0", ["storage", "files.manage"]),
                     "<h1>dangling</h1>", "console.log('dangling')")
            status, payload, _ = install(cookie2, path)
            check(status == 201, f"dangling install failed: {status} {payload[:200]!r}")
            status, _, _ = request("POST", f"/api/extensions/{dangling_id}/approve",
                                   {"permissions": ["storage", "files.manage"], "enable": True}, cookie2)
            check(status == 200, f"dangling approve failed: {status}")
            status, payload, _ = request(
                "POST", "/api/extensions/runtime/sessions",
                {"app_id": dangling_id, "instance_id": "12121212-3434-5656-7878-909090909090"}, cookie2)
            token = json_body(payload).get("token", "")
            check(status == 201 and token, f"dangling session failed: {status}")
            headers = {"X-EdgeDesk-Runtime": token}
            status, payload, _ = request(
                "POST", "/api/extensions/runtime/files/upload/begin",
                {"path": "/dangling.bin", "size": 2 * 1024 * 1024}, cookie2, headers=headers)
            transfer = json_body(payload).get("transfer_id", "")
            check(status == 200 and transfer, f"dangling upload begin failed: {status}")
            status, _, _ = request(
                "PUT", f"/api/extensions/runtime/files/upload/{transfer}/chunk?offset=0",
                cookie=cookie2, raw=os.urandom(1024 * 1024), content_type="application/octet-stream",
                headers=headers)
            check(status == 200, f"dangling chunk failed: {status}")
            leftovers = [name for name in os.listdir(args.file_root)
                         if name.startswith(".edgedesk-upload-") and name.endswith(".tmp")]
            check(bool(leftovers), "dangling upload did not create a temp file to recover")

    return finish("extension_test")


def finish(name):
    if FAILURES:
        print(f"{name}: {len(FAILURES)} FAILURE(S)")
        return 1
    print(f"{name}: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
