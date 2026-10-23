#!/usr/bin/env python3
"""SPEC-0011 native application backend integration test.

Runs against a live EdgeDesk server with an attached Process Supervisor. It
installs a ZIP containing a real host ELF backend, configures the identity,
exercises start/stop through the Supervisor, calls the backend through the
host HTTP-over-UDS proxy, upgrades while running, and verifies data retention
and independent purge.

The backend binary is supplied by the caller (see scripts/integration_test.sh,
which compiles the SDK example). Nothing is fabricated: every assertion is
against responses of the running server.
"""

import argparse
import grp
import hashlib
import json
import os
import pwd
import shutil
import stat
import sys
import tempfile
import time
import urllib.error
import urllib.request
import zipfile

FAILURES = []
BASE = ""
ORIGIN = ""
APP_ID = "dev.edgedesk.notes-example"
VERSION = "1.0.0"


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


def manifest(version):
    return {
        "id": APP_ID,
        "name": "Notes Backend Example",
        "description": "Native backend integration package",
        "version": version,
        "entry": "index.html",
        "minHostApiVersion": "1.0.0",
        "maxHostApiVersion": "1.9.9",
        "permissions": [],
        "window": {
            "defaultWidth": 820,
            "defaultHeight": 620,
            "minWidth": 420,
            "minHeight": 320,
            "instances": "single",
        },
        "backend": {"executable": "backend/notes-backend"},
    }


def make_package(directory, name, version, binary, index, backend_name="backend/notes-backend"):
    path = os.path.join(directory, name)
    info = zipfile.ZipInfo(backend_name)
    info.external_attr = (stat.S_IFREG | 0o755) << 16
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as archive:
        archive.writestr("manifest.json", json.dumps(manifest(version)))
        archive.writestr("index.html", index)
        archive.writestr(info, binary)
    return path


def make_script_package(directory, name, version):
    """Rejects at install time: the declared backend is a shell script."""
    path = os.path.join(directory, name)
    info = zipfile.ZipInfo("backend/notes-backend")
    info.external_attr = (stat.S_IFREG | 0o755) << 16
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as archive:
        archive.writestr("manifest.json", json.dumps(manifest(version)))
        archive.writestr("index.html", "<h1>bad</h1>")
        archive.writestr(info, b"#!/bin/sh\nexit 0\n")
    return path


def upload(cookie, path, upgrade=False):
    with open(path, "rb") as archive:
        payload = archive.read()
    url = f"/api/extensions/{APP_ID}/upgrade" if upgrade else "/api/extensions/install"
    return request("PUT", url, cookie=cookie, raw=payload, content_type="application/zip")


def backend_info(cookie):
    status, payload, _ = request("GET", f"/api/extensions/{APP_ID}/backend", cookie=cookie)
    return status, json_body(payload).get("backend", {})


def supervisor_service(cookie, service_id):
    status, payload, _ = request("GET", f"/api/supervisor/services/{service_id}", cookie=cookie)
    return status, json_body(payload)


def wait_backend_state(cookie, wanted, timeout=20.0):
    deadline = time.time() + timeout
    last = {}
    while time.time() < deadline:
        status, backend = backend_info(cookie)
        last = backend
        if status == 200 and (backend.get("supervisor") or {}).get("state") == wanted:
            return True, backend
        time.sleep(0.2)
    return False, last


def proxy(cookie, token, method, path, body=None):
    payload = {"method": method, "path": path}
    if body is not None:
        payload["body"] = body
    return request(
        "POST",
        "/api/extensions/runtime/backend/request",
        payload,
        cookie=cookie,
        headers={"X-EdgeDesk-Runtime": token},
    )


def service_id_for(app_id):
    return "app-" + hashlib.sha256(app_id.encode()).hexdigest()[:32]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=19098)
    parser.add_argument("--password", required=True)
    parser.add_argument("--apps-root", required=True)
    parser.add_argument("--binary", required=True, help="host ELF backend built from the SDK example")
    args = parser.parse_args()

    global BASE, ORIGIN
    BASE = f"http://{args.host}:{args.port}"
    ORIGIN = BASE

    with open(args.binary, "rb") as handle:
        binary = handle.read()
    user = pwd.getpwuid(os.geteuid()).pw_name
    group = grp.getgrgid(os.getegid()).gr_name
    # The integration harness may run as root (CI containers). Root is only
    # accepted with the explicit allow_root confirmation, never implicitly.
    allow_root = os.geteuid() == 0
    service_id = ""
    work = tempfile.mkdtemp(prefix="edgedesk-native-test-")
    cookie = login(args.password)
    if not cookie:
        return finish()

    data_dir = os.path.join(args.apps_root, "data", APP_ID)
    program = os.path.join(args.apps_root, "native", APP_ID, "backend")

    # ---- install: validates, never executes, never starts ----------------
    package = make_package(work, "notes-1.zip", VERSION, binary, "<h1>v1</h1>")
    status, payload, _ = upload(cookie, package)
    check(status == 201, f"native install failed: {status} {payload[:300]!r}")
    status, backend = backend_info(cookie)
    check(status == 200 and backend.get("present") is True, "installed app reports no backend")
    check(backend.get("state") == "stopped", f"install state is {backend.get('state')!r}, want stopped")
    check(backend.get("autostart") is False, "install must not enable autostart")
    placeholder_service_id = backend.get("service_id", "")
    check(bool(placeholder_service_id), "backend record has no service id placeholder")
    check(os.path.isfile(program), "stable backend executable was not installed")
    if os.path.isfile(program):
        check(os.stat(program).st_mode & 0o111 != 0, "installed backend is not executable")
        with open(program, "rb") as handle:
            check(handle.read() == binary, "installed backend bytes differ from the package")
    status, services, _ = request("GET", "/api/supervisor/services?limit=1000", cookie=cookie)
    listed = json_body(services).get("services", [])
    check(all(item.get("id") != placeholder_service_id for item in listed),
          "install created a Supervisor service without an identity")

    # ---- approve + enable ------------------------------------------------
    status, payload, _ = request("POST", f"/api/extensions/{APP_ID}/approve",
                                 {"permissions": [], "enable": True}, cookie=cookie)
    check(status == 200, f"approve/enable failed: {status} {payload[:200]!r}")

    # ---- identity creates the service but does not start it --------------
    status, payload, _ = request("PUT", f"/api/extensions/{APP_ID}/backend/identity",
                                 {"run_user": user, "run_group": group, "allow_root": allow_root},
                                 cookie=cookie)
    check(status == 200, f"identity save failed: {status} {payload[:300]!r}")
    status, backend = backend_info(cookie)
    service_id = backend.get("service_id", "")
    check(bool(service_id), "backend has no Supervisor service id after identity save")
    check(service_id != placeholder_service_id,
          "Supervisor-assigned service id was not persisted in the backend record")
    status, service = supervisor_service(cookie, service_id)
    check(status == 200, f"supervisor service missing after identity save: {status}")
    check(service.get("state") == "STOPPED", f"service state {service.get('state')!r} != STOPPED")
    check(service.get("run_user") == user and service.get("run_group") == group,
          "supervisor service identity does not match the configured user/group")
    check(service.get("allow_root") is allow_root, "service allow_root does not match the confirmation")
    check(service.get("autostart") is False, "identity save enabled autostart")

    # ---- autostart is independent from start/stop ------------------------
    status, payload, _ = request("PUT", f"/api/extensions/{APP_ID}/backend/autostart",
                                 {"enabled": True}, cookie=cookie)
    check(status == 200, f"autostart enable failed: {status} {payload[:200]!r}")
    status, service = supervisor_service(cookie, service_id)
    check(service.get("autostart") is True, "autostart was not applied to the Supervisor")
    status, backend = backend_info(cookie)
    check(backend.get("state") == "stopped", "autostart must not start the backend")
    status, payload, _ = request("PUT", f"/api/extensions/{APP_ID}/backend/autostart",
                                 {"enabled": False}, cookie=cookie)
    check(status == 200, f"autostart disable failed: {status} {payload[:200]!r}")

    # ---- start + proxy over UDS -----------------------------------------
    status, payload, _ = request("POST", f"/api/extensions/{APP_ID}/backend/start", {},
                                 cookie=cookie)
    check(status == 200, f"backend start failed: {status} {payload[:300]!r}")
    running, backend = wait_backend_state(cookie, "RUNNING")
    check(running, f"backend did not reach RUNNING: {backend}")
    check(backend.get("state") == "stopped", "manager state must stay stopped when running")
    check(os.path.exists(os.path.join(args.apps_root, "run", APP_ID, "backend.sock")),
          "backend socket was not created in the host run directory")

    status, payload, _ = request("POST", "/api/extensions/runtime/sessions",
                                 {"app_id": APP_ID, "instance_id": "it-native-1"}, cookie=cookie)
    check(status == 201, f"runtime session failed: {status} {payload[:200]!r}")
    token = json_body(payload).get("token", "")
    check(bool(token), "runtime session returned no token")

    status, payload, _ = proxy(cookie, token, "GET", "/health")
    body = json_body(payload)
    check(status == 200 and body.get("status") == 200, f"proxy /health failed: {status} {payload[:200]!r}")
    check((body.get("body") or {}).get("ok") is True, "proxy /health body is not the backend JSON")

    status, payload, _ = proxy(cookie, token, "POST", "/notes", {"text": "hello"})
    body = json_body(payload)
    check(status == 200 and 200 <= (body.get("status") or 0) < 300,
          f"proxy POST /notes failed: {status} {payload[:200]!r}")
    status, payload, _ = proxy(cookie, token, "GET", "/notes")
    notes = (json_body(payload).get("body") or {}).get("notes")
    check(notes == ["hello"], f"backend data not persisted through the UDS proxy: {notes!r}")

    status, payload, _ = proxy(cookie, token, "GET", "/health?x=1")
    check(status == 400, f"query strings must be rejected, got {status}")
    status, payload, _ = request(
        "POST", "/api/extensions/runtime/backend/request",
        {"method": "GET", "path": "/health", "app_id": "other.app"},
        cookie=cookie, headers={"X-EdgeDesk-Runtime": token})
    check(status == 400, f"caller-supplied app_id must be rejected, got {status}")
    status, payload, _ = request(
        "POST", "/api/extensions/runtime/backend/request",
        {"method": "GET", "path": "/health"},
        cookie=cookie, headers={"X-EdgeDesk-Runtime": "not-a-session"})
    check(status == 401, f"invalid runtime session must be 401, got {status}")

    # ---- upgrade while running: stop hook + data retention ---------------
    upgrade = make_package(work, "notes-2.zip", "1.1.0", binary, "<h1>v2</h1>")
    status, payload, _ = upload(cookie, upgrade, upgrade=True)
    check(status == 201, f"upgrade while running failed: {status} {payload[:300]!r}")
    status, service = supervisor_service(cookie, service_id)
    check(service.get("state") == "STOPPED", f"upgrade left backend in {service.get('state')!r}")
    status, backend = backend_info(cookie)
    check(backend.get("state") == "stopped", f"upgrade manager state is {backend.get('state')!r}")
    status, payload, _ = request("GET", f"/api/extensions/{APP_ID}", cookie=cookie)
    detail = json_body(payload)
    check(detail.get("app", {}).get("active_version") == "1.1.0", "active version was not upgraded")
    check(detail.get("backend", {}).get("present") is True, "app detail does not include the backend")
    check(os.path.isfile(os.path.join(data_dir, "notes.txt")), "upgrade deleted backend data")

    # ---- invalid upgrade leaves the installed backend untouched ----------
    before = open(program, "rb").read() if os.path.isfile(program) else b""
    bad = make_script_package(work, "notes-bad.zip", "1.2.0")
    status, payload, _ = upload(cookie, bad, upgrade=True)
    check(status >= 400, f"script backend was accepted on upgrade: {status}")
    after = open(program, "rb").read() if os.path.isfile(program) else b""
    check(before == after, "rejected upgrade modified the installed executable")
    status, payload, _ = request("GET", f"/api/extensions/{APP_ID}", cookie=cookie)
    check(json_body(payload).get("app", {}).get("active_version") == "1.1.0",
          "rejected upgrade changed the active version")

    # ---- start upgraded version and verify the same data -----------------
    status, payload, _ = request("POST", f"/api/extensions/{APP_ID}/backend/start", {},
                                 cookie=cookie)
    check(status == 200, f"start after upgrade failed: {status} {payload[:200]!r}")
    running, _ = wait_backend_state(cookie, "RUNNING")
    check(running, "upgraded backend did not start")
    status, payload, _ = request("POST", "/api/extensions/runtime/sessions",
                                 {"app_id": APP_ID, "instance_id": "it-native-2"}, cookie=cookie)
    token2 = json_body(payload).get("token", "")
    status, payload, _ = proxy(cookie, token2, "GET", "/notes")
    notes = (json_body(payload).get("body") or {}).get("notes")
    check(notes == ["hello"], f"backend data did not survive the upgrade: {notes!r}")

    status, payload, _ = request("POST", f"/api/extensions/{APP_ID}/backend/stop", {},
                                 cookie=cookie)
    check(status == 200, f"stop failed: {status} {payload[:200]!r}")
    stopped, _ = wait_backend_state(cookie, "STOPPED")
    check(stopped, "backend did not stop")

    # ---- uninstall keeps data and removes the service --------------------
    status, payload, _ = request("POST", f"/api/extensions/{APP_ID}/uninstall",
                                 {"data_action": "keep"}, cookie=cookie)
    check(status == 200, f"uninstall failed: {status} {payload[:300]!r}")
    status, service = supervisor_service(cookie, service_id)
    check(status == 404, f"uninstall left the Supervisor service behind: {status}")
    check(not os.path.exists(os.path.join(args.apps_root, "native", APP_ID)),
          "uninstall left native program files")
    check(os.path.exists(os.path.join(data_dir, "notes.txt")),
          "uninstall with keep deleted persistent data")

    # ---- independent purge removes only this app's data -----------------
    status, payload, _ = request("POST", f"/api/extensions/{APP_ID}/data/purge",
                                 {"confirm": True, "app_id": APP_ID}, cookie=cookie)
    check(status == 200, f"data purge failed: {status} {payload[:300]!r}")
    check(not os.path.exists(data_dir), "data purge did not remove the app data directory")
    status, payload, _ = request("POST", f"/api/extensions/{APP_ID}/data/purge",
                                 {"confirm": True, "app_id": "other.app"}, cookie=cookie)
    check(status == 400, f"purge with a mismatched app_id must be rejected, got {status}")

    shutil.rmtree(work, ignore_errors=True)
    return finish()


def finish():
    if FAILURES:
        print(f"native_backend_test: {len(FAILURES)} FAILURE(S)")
        return 1
    print("native_backend_test: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
