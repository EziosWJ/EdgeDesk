#!/usr/bin/env python3
"""EdgeDesk REST API integration test (stdlib only).

Usage: scripts/api_test.py <host> <port> <token> <file_root>

Covers: health contract, auth/session, CSRF, file listing/stat/text/mkdir/
rename/delete, streaming upload, Range download, path sandbox rejections,
system metrics and 404 handling.
"""
import json
import os
import sys
import urllib.error
import urllib.request

HOST = "127.0.0.1"
PORT = 0
TOKEN = ""
FILE_ROOT = ""
FAILURES = []


def check(condition, message):
    if condition:
        return
    FAILURES.append(message)
    print(f"FAILED: {message}")


def request(method, path, body=None, headers=None, cookie=None, raw=False, timeout=15):
    url = f"http://{HOST}:{PORT}{path}"
    data = None
    if body is not None:
        data = body if isinstance(body, bytes) else json.dumps(body).encode()
    req = urllib.request.Request(url, data=data, method=method)
    req.add_header("Content-Type", "application/json")
    if raw:
        req.headers["Content-Type"] = "application/octet-stream"
    if headers:
        for key, value in headers.items():
            req.add_header(key, value)
    if cookie:
        req.add_header("Cookie", f"edgedesk_session={cookie}")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as response:
            payload = response.read()
            return response.status, payload, dict(response.headers)
    except urllib.error.HTTPError as error:
        return error.code, error.read(), dict(error.headers)


def json_body(payload):
    return json.loads(payload.decode())


def login(token=None):
    if token is None:
        token = TOKEN
    status, payload, headers = request("POST", "/api/auth/login", {"token": token})
    check(status == 200, f"login status {status}")
    cookie = headers.get("Set-Cookie", "")
    check("HttpOnly" in cookie and "SameSite=Strict" in cookie, f"cookie flags: {cookie}")
    return cookie.split("edgedesk_session=", 1)[1].split(";", 1)[0]


def test_health():
    status, payload, headers = request("GET", "/api/health")
    check(status == 200, f"health status {status}")
    body = json_body(payload)
    check(body["status"] == "ok" and body["service"] == "edgedesk" and body["phase"] == 1, f"health body {body}")
    check(headers.get("X-Content-Type-Options") == "nosniff", "security header missing")
    check("Content-Security-Policy" in headers, "CSP header missing")


def test_auth_flow():
    status, _, _ = request("GET", "/api/files")
    check(status == 401, f"unauthenticated files status {status}")
    status, _, _ = request("POST", "/api/auth/login", {"token": "definitely-wrong"})
    check(status == 401, f"wrong token status {status}")
    cookie = login()
    status, payload, _ = request("GET", "/api/auth/session", cookie=cookie)
    check(status == 200 and json_body(payload)["authenticated"] is True, "session check failed")
    status, session_payload, _ = request("GET", "/api/auth/session")
    check(status == 200 and json_body(session_payload)["authenticated"] is False,
          "unauthenticated session check failed")
    # bearer token works as well
    status, _, _ = request("GET", "/api/files?path=/", headers={"Authorization": f"Bearer {TOKEN}"})
    check(status == 200, f"bearer auth status {status}")
    return cookie


def test_csrf(cookie):
    status, payload, _ = request("POST", "/api/files/mkdir", {"path": "/csrf"}, cookie=cookie)
    check(status == 403, f"csrf POST without origin status {status}")
    check(json_body(payload)["error"] == "csrf_rejected", "csrf error code")
    status, _, _ = request("POST", "/api/files/mkdir", {"path": "/csrf"}, cookie=cookie,
                           headers={"Origin": f"http://{HOST}:{PORT}"})
    check(status == 201, f"mkdir with origin status {status}")


def test_files(cookie):
    origin = {"Origin": f"http://{HOST}:{PORT}"}
    status, payload, _ = request("GET", "/api/files?path=/", cookie=cookie)
    check(status == 200, f"list status {status}")
    body = json_body(payload)
    names = {entry["name"] for entry in body["entries"]}
    check("docs" in names and "a.txt" in names, f"list entries {names}")
    entry = next(item for item in body["entries"] if item["name"] == "a.txt")
    check(entry["permissions"] == "rw-r--r--", f"permissions {entry['permissions']}")
    check(entry["uid"] >= 0 and entry["mtime"] > 0, "uid/mtime missing")

    status, payload, _ = request("GET", "/api/files/stat?path=/a.txt", cookie=cookie)
    check(status == 200 and json_body(payload)["entry"]["size"] > 0, "stat failed")

    status, payload, _ = request("GET", "/api/files/text?path=/a.txt", cookie=cookie)
    check(status == 200 and "content" in json_body(payload), "text read failed")

    # 编辑保存（原子保存）
    status, payload, _ = request("PUT", "/api/files/text?path=/edited.txt", b"line1\nline2\n", cookie=cookie,
                                 headers=origin, raw=True)
    check(status == 200, f"text write status {status}")
    status, payload, _ = request("GET", "/api/files/text?path=/edited.txt", cookie=cookie)
    check(json_body(payload)["content"] == "line1\nline2\n", "text round trip failed")

    # 二进制内容不能作为文本保存
    status, _, _ = request("PUT", "/api/files/text?path=/bin.dat", b"a\x00b", cookie=cookie, headers=origin, raw=True)
    check(status == 415, f"binary text write status {status}")

    # mkdir + rename + delete
    status, _, _ = request("POST", "/api/files/mkdir", {"path": "/dir1"}, cookie=cookie, headers=origin)
    check(status == 201, f"mkdir status {status}")
    status, _, _ = request("POST", "/api/files/rename", {"from": "/dir1", "to": "/dir2"}, cookie=cookie,
                           headers=origin)
    check(status == 200, f"rename status {status}")
    status, _, _ = request("GET", "/api/files/stat?path=/dir2", cookie=cookie)
    check(status == 200, "renamed directory missing")
    status, _, _ = request("POST", "/api/files/rename", {"from": "/dir2", "to": "/edited.txt"}, cookie=cookie,
                           headers=origin)
    check(status == 409, f"rename over existing without overwrite status {status}")

    # 目录递归删除
    status, _, _ = request("POST", "/api/files/mkdir", {"path": "/dir2/sub"}, cookie=cookie, headers=origin)
    check(status == 201, "nested mkdir failed")
    status, payload, _ = request("POST", "/api/files/delete", {"paths": ["/dir2"]}, cookie=cookie, headers=origin)
    check(status == 200, f"delete status {status}")
    check(json_body(payload)["removed_dirs"] == 2, f"delete counters {payload}")

    # 路径沙箱
    for path in ["/../etc/passwd", "/docs/../../etc", "/%2e%2e/etc/passwd"]:
        status, _, _ = request("GET", f"/api/files?path={path}", cookie=cookie)
        check(status in (400, 403, 404), f"traversal {path} status {status}")

    # JSON 体积限制
    big = json.dumps({"paths": ["/" + "x" * 2000000]})
    status, _, _ = request("POST", "/api/files/delete", big.encode(), cookie=cookie, headers=origin, raw=True)
    check(status in (413, 403), f"oversized json status {status}")


def test_transfer(cookie):
    origin = {"Origin": f"http://{HOST}:{PORT}"}
    payload = os.urandom(300000)
    status, response, _ = request("PUT", "/api/files/content?path=/upload.bin", payload, cookie=cookie,
                                  headers=origin, raw=True, timeout=30)
    check(status == 201, f"upload status {status} body {response[:120]}")
    with open(os.path.join(FILE_ROOT, "upload.bin"), "rb") as handle:
        check(handle.read() == payload, "uploaded bytes differ")

    status, body, headers = request("GET", "/api/files/download?path=/upload.bin", cookie=cookie)
    check(status == 200 and body == payload, "full download differs")
    check(headers.get("Accept-Ranges") == "bytes", "Accept-Ranges missing")

    status, body, headers = request("GET", "/api/files/download?path=/upload.bin", cookie=cookie,
                                    headers={"Range": "bytes=10-19"})
    check(status == 206 and body == payload[10:20], f"range download status {status}")
    check(headers.get("Content-Range") == f"bytes 10-19/{len(payload)}", f"content-range {headers.get('Content-Range')}")

    status, _, _ = request("GET", "/api/files/download?path=/upload.bin", cookie=cookie,
                           headers={"Range": "bytes=99999999-"})
    check(status == 416, f"unsatisfiable range status {status}")

    status, body, _ = request("PUT", "/api/files/content?path=/upload.bin.new", b"x" * 100, cookie=cookie,
                              headers=origin, raw=True)
    check(status == 201, "small upload failed")
    request("POST", "/api/files/delete", {"paths": ["/upload.bin.new"]}, cookie=cookie, headers=origin)


def test_system(cookie):
    status, payload, _ = request("GET", "/api/system/summary", cookie=cookie)
    check(status == 200, f"summary status {status}")
    body = json_body(payload)
    check(body["sample"]["cpu"]["cores"] >= 1, f"cpu cores {body['sample']}")
    check("memory" in body["sample"], "memory missing")
    check("disks" in body["sample"], "disks missing")

    status, payload, _ = request("GET", "/api/system/history?window=10", cookie=cookie)
    check(status == 200 and "samples" in json_body(payload), "history failed")


def test_not_found(cookie):
    status, payload, _ = request("GET", "/api/nope", cookie=cookie)
    check(status == 404, f"unknown api status {status}")
    status, _, _ = request("GET", "/api/files?path=/missing", cookie=cookie)
    check(status == 404, f"missing dir status {status}")


def main():
    global HOST, PORT, TOKEN, FILE_ROOT
    if len(sys.argv) < 5:
        print(__doc__)
        return 2
    HOST, PORT, TOKEN, FILE_ROOT = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4]
    test_health()
    cookie = test_auth_flow()
    test_csrf(cookie)
    test_files(cookie)
    test_transfer(cookie)
    test_system(cookie)
    test_not_found(cookie)
    if FAILURES:
        print(f"api_test: {len(FAILURES)} FAILURE(S)")
        return 1
    print("api_test: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
