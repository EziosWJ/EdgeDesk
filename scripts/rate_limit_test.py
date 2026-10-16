#!/usr/bin/env python3
"""Rate limiting integration test against a server started with strict limits.

Usage: scripts/rate_limit_test.py <host> <port> <token>
Expects the caller to launch the server with something like:
  --rate-limit 2 --rate-limit-burst 2 --login-rate-limit 2
"""
import json
import sys
import time
import urllib.error
import urllib.request

HOST, PORT, TOKEN = "127.0.0.1", 0, ""


def post(path, body, headers=None):
    request = urllib.request.Request(f"http://{HOST}:{PORT}{path}", data=json.dumps(body).encode(), method="POST")
    request.add_header("Content-Type", "application/json")
    for key, value in (headers or {}).items():
        request.add_header(key, value)
    try:
        with urllib.request.urlopen(request, timeout=5) as response:
            return response.status
    except urllib.error.HTTPError as error:
        return error.code


def main():
    global HOST, PORT, TOKEN
    if len(sys.argv) < 4:
        print(__doc__)
        return 2
    HOST, PORT, TOKEN = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    failures = []

    # Login limiter (burst 2): the third attempt within a minute is throttled.
    statuses = [post("/api/auth/login", {"token": "wrong"}) for _ in range(3)]
    if 429 not in statuses:
        failures.append(f"login limiter did not trigger: {statuses}")

    # General limiter (burst 2): immediate follow-up requests are throttled.
    time.sleep(3)  # let buckets refill
    statuses = []
    for _ in range(6):
        try:
            with urllib.request.urlopen(f"http://{HOST}:{PORT}/api/health", timeout=5) as response:
                statuses.append(response.status)
        except urllib.error.HTTPError as error:
            statuses.append(error.code)
    if 429 not in statuses:
        failures.append(f"request limiter did not trigger: {statuses}")

    if failures:
        for failure in failures:
            print(f"FAILED: {failure}")
        print("rate_limit_test: FAILURES")
        return 1
    print("rate_limit_test: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
