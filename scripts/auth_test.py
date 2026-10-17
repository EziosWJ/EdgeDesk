#!/usr/bin/env python3
"""EdgeDesk V1.1 authentication/settings integration test.

Usage examples:
  scripts/auth_test.py --port 19091 --password "old password" \
      --new-password "new password" --token token --cookie-out /tmp/old-cookie
  scripts/auth_test.py --port 19091 --password "new password" --check-old-cookie /tmp/old-cookie
  scripts/auth_test.py --port 19091 --expect-corrupt
  scripts/auth_test.py --port 19092 --password "new password" --setup-only

The script assumes the server is already running. It performs setup when the
instance is uninitialized, then logs in and exercises the V1.1 auth/settings
contract. Standard library only.
"""
import argparse
import json
import os
import sys
import urllib.error
import urllib.request

FAILURES = []


def check(condition, message):
    if condition:
        return
    FAILURES.append(message)
    print(f"FAILED: {message}")


def request(base, method, path, body=None, headers=None, cookie=None, timeout=15):
    data = None
    if body is not None:
        data = body if isinstance(body, bytes) else json.dumps(body).encode()
    req = urllib.request.Request(base + path, data=data, method=method)
    req.add_header("Content-Type", "application/json")
    for key, value in (headers or {}).items():
        req.add_header(key, value)
    if cookie:
        req.add_header("Cookie", f"edgedesk_session={cookie}")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as response:
            return response.status, response.read(), dict(response.headers)
    except urllib.error.HTTPError as error:
        return error.code, error.read(), dict(error.headers)


def body_of(payload):
    try:
        return json.loads(payload.decode())
    except Exception:
        return {}


def cookie_from(headers):
    raw = headers.get("Set-Cookie", "")
    if "edgedesk_session=" not in raw:
        return None, raw
    return raw.split("edgedesk_session=", 1)[1].split(";", 1)[0], raw


def login(base, password, expect_secure=False):
    status, payload, headers = request(base, "POST", "/api/auth/login", {"password": password})
    cookie, raw = cookie_from(headers)
    check(status == 200, f"login status {status}: {payload[:160]!r}")
    check("HttpOnly" in raw and "SameSite=Strict" in raw, f"cookie flags: {raw}")
    if expect_secure:
        check("Secure" in raw, f"cookie is missing Secure: {raw}")
    else:
        check("Secure" not in raw, f"cookie unexpectedly has Secure: {raw}")
    return cookie or ""


def protected_denied(base):
    status, payload, _ = request(base, "GET", "/api/files?path=/")
    check(status == 503, f"protected API in pre-auth state should be 503, got {status}")
    return status


def setup_if_needed(base, password):
    status, payload, _ = request(base, "GET", "/api/auth/setup-status")
    check(status == 200, f"setup-status status {status}")
    state = body_of(payload).get("state")
    if state == "uninitialized":
        protected_denied(base)
        status, payload, _ = request(base, "POST", "/api/auth/setup",
                                      {"password": password, "confirm_password": password})
        check(status == 201, f"setup status {status}: {payload[:160]!r}")

        status, _, _ = request(base, "POST", "/api/auth/setup",
                                {"password": password, "confirm_password": password})
        check(status == 409, f"setup replay status {status}")

        status, payload, _ = request(base, "GET", "/api/auth/setup-status")
        check(body_of(payload).get("state") == "initialized", f"setup state {payload!r}")
    elif state == "corrupt":
        check(False, "unexpected corrupt auth state")
    else:
        check(state == "initialized", f"unexpected setup state {state}")
        status, _, _ = request(base, "POST", "/api/auth/setup",
                                {"password": password, "confirm_password": password})
        check(status == 409, f"setup on initialized instance should be 409, got {status}")


def test_auth_flow(base, password, token, expect_secure=False):
    status, _, _ = request(base, "GET", "/api/files?path=/")
    check(status == 401, f"unauthenticated files status {status}")

    status, _, _ = request(base, "POST", "/api/auth/login", {"password": "definitely-wrong"})
    check(status == 401, f"wrong password status {status}")

    cookie = login(base, password, expect_secure)
    status, payload, _ = request(base, "GET", "/api/auth/session", cookie=cookie)
    session = body_of(payload)
    check(status == 200 and session.get("authenticated") is True, f"session {payload!r}")
    check(session.get("method") == "cookie", f"session method {session.get('method')}")
    check(isinstance(session.get("expires_at"), int), "session expiry missing")

    status, payload, _ = request(base, "GET", "/api/auth/session")
    check(status == 200 and body_of(payload).get("authenticated") is False, "anonymous session")

    status, _, _ = request(base, "POST", "/api/files/mkdir", {"path": "/csrf-denied"}, cookie=cookie)
    check(status == 403, f"cookie POST without Origin status {status}")
    status, _, _ = request(base, "POST", "/api/files/mkdir", {"path": f"/csrf-allowed-{os.getpid()}"},
                           cookie=cookie, headers={"Origin": base})
    check(status == 201, f"cookie POST with Origin status {status}")

    if token:
        status, _, _ = request(base, "GET", "/api/files?path=/",
                               headers={"Authorization": f"Bearer {token}"})
        check(status == 200, f"Bearer auth status {status}")

    return cookie


def test_settings(base, cookie):
    status, payload, _ = request(base, "GET", "/api/settings", cookie=cookie)
    settings = body_of(payload)
    check(status == 200 and settings.get("ok") is True, f"settings GET {payload[:160]!r}")
    check(isinstance(settings.get("api_token_configured"), bool), "token-configured flag missing")

    status, payload, _ = request(base, "PUT", "/api/settings",
                                 {"theme": "light", "terminal_font_size": 16},
                                 cookie=cookie, headers={"Origin": base})
    updated = body_of(payload).get("settings", {})
    check(status == 200 and updated.get("theme") == "light", f"settings PUT {payload[:160]!r}")
    check(updated.get("terminal_font_size") == 16, "terminal font size not saved")

    status, _, _ = request(base, "PUT", "/api/settings", {"unknown_field": 1},
                           cookie=cookie, headers={"Origin": base})
    check(status == 400, f"unknown settings field status {status}")
    status, _, _ = request(base, "PUT", "/api/settings", {"theme": "neon"},
                           cookie=cookie, headers={"Origin": base})
    check(status == 400, f"invalid settings enum status {status}")

    status, payload, _ = request(base, "GET", "/api/system/info", cookie=cookie)
    info = body_of(payload)
    check(status == 200 and info.get("version"), f"system info {payload[:160]!r}")
    serialized = json.dumps(info).lower()
    for secret in ("password", "hash", "cookie", "authorization"):
        check(secret not in serialized, f"system info leaked {secret}")
    check(isinstance(info.get("api_token_configured"), bool), "token configured flag must be boolean")


def test_password_change(base, cookie, old_password, new_password, cookie_out, token="", expect_secure=False):
    if cookie_out:
        with open(cookie_out, "w", encoding="utf-8") as handle:
            handle.write(cookie)

    status, _, _ = request(base, "POST", "/api/auth/password",
                           {"current_password": "wrong-current", "new_password": new_password,
                            "confirm_password": new_password},
                           cookie=cookie, headers={"Origin": base})
    check(status == 400, f"wrong current password status {status}")

    status, payload, _ = request(base, "POST", "/api/auth/password",
                                 {"current_password": old_password, "new_password": new_password,
                                  "confirm_password": new_password},
                                 cookie=cookie, headers={"Origin": base})
    check(status == 200, f"password change status {status}: {payload[:160]!r}")

    status, _, _ = request(base, "GET", "/api/files?path=/", cookie=cookie)
    check(status == 401, f"old cookie after password change status {status}")
    status, _, _ = request(base, "POST", "/api/auth/login", {"password": old_password})
    check(status == 401, f"old password after change status {status}")
    login(base, new_password, expect_secure)
    if token:
        status, _, _ = request(base, "GET", "/api/files?path=/", headers={"Authorization": f"Bearer {token}"})
        check(status == 200, f"Bearer token changed after password change: {status}")


def test_old_cookie(base, cookie_file):
    with open(cookie_file, "r", encoding="utf-8") as handle:
        cookie = handle.read().strip()
    status, _, _ = request(base, "GET", "/api/files?path=/", cookie=cookie)
    check(status == 401, f"old session cookie survived restart: {status}")


def test_corrupt(base):
    status, payload, _ = request(base, "GET", "/api/auth/setup-status")
    check(status == 200 and body_of(payload).get("state") == "corrupt", f"corrupt status {payload!r}")
    protected_denied(base)
    status, _, _ = request(base, "POST", "/api/auth/setup",
                           {"code": "x", "password": "another correct horse",
                            "confirm_password": "another correct horse"})
    check(status == 503, f"setup in corrupt state status {status}")
    status, _, _ = request(base, "POST", "/api/auth/login", {"password": "another correct horse"})
    check(status == 503, f"login in corrupt state status {status}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--password", default="")
    parser.add_argument("--new-password", default="")
    parser.add_argument("--token", default="")
    parser.add_argument("--cookie-out", default="")
    parser.add_argument("--check-old-cookie", default="")
    parser.add_argument("--expect-corrupt", action="store_true")
    parser.add_argument("--expect-secure", action="store_true")
    parser.add_argument("--setup-only", action="store_true")
    args = parser.parse_args()

    base = f"http://{args.host}:{args.port}"
    if args.expect_corrupt:
        test_corrupt(base)
    else:
        if not args.password:
            parser.error("--password is required")
        if args.check_old_cookie:
            test_old_cookie(base, args.check_old_cookie)
        setup_if_needed(base, args.password)
        if args.setup_only:
            pass
        else:
            cookie = test_auth_flow(base, args.password, args.token, args.expect_secure)
            test_settings(base, cookie)
            if args.new_password:
                test_password_change(base, cookie, args.password, args.new_password, args.cookie_out, args.token, args.expect_secure)

    if FAILURES:
        print(f"auth_test: {len(FAILURES)} FAILURE(S)")
        return 1
    print("auth_test: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
