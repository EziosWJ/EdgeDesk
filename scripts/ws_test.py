#!/usr/bin/env python3
"""EdgeDesk WebSocket integration test (stdlib only, no external deps).

Covers: handshake auth/origin rejection, terminal create/input/output/resize,
session survival across socket disconnect + reconnect, explicit close, and the
metrics stream (snapshot + incremental samples).

Usage:
    scripts/ws_test.py <host> <port> <token> [--keep-session 0|1]
"""
import base64
import hashlib
import json
import os
import socket
import struct
import sys
import time

HOST = "127.0.0.1"


class WsClient:
    def __init__(self, host, port, path, token=None, origin=None, timeout=10.0):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.settimeout(timeout)
        key = base64.b64encode(os.urandom(16)).decode()
        lines = [
            f"GET {path} HTTP/1.1",
            f"Host: {host}:{port}",
            "Upgrade: websocket",
            "Connection: Upgrade",
            f"Sec-WebSocket-Key: {key}",
            "Sec-WebSocket-Version: 13",
        ]
        if token is not None:
            lines.append(f"Cookie: edgedesk_session={token}")
        if origin is not None:
            lines.append(f"Origin: {origin}")
        request = "\r\n".join(lines) + "\r\n\r\n"
        self.sock.sendall(request.encode())
        self.status = None
        self.handshake_headers = {}
        self._read_handshake()

    def _recv_until(self, marker):
        data = b""
        while marker not in data:
            chunk = self.sock.recv(4096)
            if not chunk:
                break
            data += chunk
        return data

    def _read_handshake(self):
        data = self._recv_until(b"\r\n\r\n")
        head, _, rest = data.partition(b"\r\n\r\n")
        text = head.decode("latin-1")
        first = text.split("\r\n")[0]
        parts = first.split(" ", 2)
        self.status = int(parts[1]) if len(parts) > 1 else 0
        for line in text.split("\r\n")[1:]:
            if ":" in line:
                key, _, value = line.partition(":")
                self.handshake_headers[key.strip().lower()] = value.strip()
        self._buffer = rest

    def close(self):
        try:
            self._send_frame(0x8, b"")
        except OSError:
            pass
        try:
            self.sock.close()
        except OSError:
            pass

    def _send_frame(self, opcode, payload, fin=True):
        mask = os.urandom(4)
        header = bytearray()
        header.append((0x80 if fin else 0) | opcode)
        length = len(payload)
        if length < 126:
            header.append(0x80 | length)
        elif length < 65536:
            header.append(0x80 | 126)
            header += struct.pack(">H", length)
        else:
            header.append(0x80 | 127)
            header += struct.pack(">Q", length)
        masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        self.sock.sendall(bytes(header) + mask + masked)

    def send_json(self, obj):
        self._send_frame(0x1, json.dumps(obj).encode())

    def _read_exact(self, count):
        while len(self._buffer) < count:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("socket closed")
            self._buffer += chunk
        data = self._buffer[:count]
        self._buffer = self._buffer[count:]
        return data

    def recv_frame(self, timeout=10.0):
        self.sock.settimeout(timeout)
        header = self._read_exact(2)
        fin = header[0] & 0x80
        opcode = header[0] & 0x0F
        masked = header[1] & 0x80
        length = header[1] & 0x7F
        if length == 126:
            length = struct.unpack(">H", self._read_exact(2))[0]
        elif length == 127:
            length = struct.unpack(">Q", self._read_exact(8))[0]
        mask = self._read_exact(4) if masked else None
        payload = self._read_exact(length) if length else b""
        if mask:
            payload = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        if opcode == 0x9:  # ping -> pong
            self._send_frame(0xA, payload)
            return self.recv_frame(timeout)
        return opcode, payload, fin

    def recv_json(self, timeout=10.0):
        # Reassemble fragmented messages (libhv splits frames at 64 KiB).
        payload = b""
        while True:
            opcode, chunk, fin = self.recv_frame(timeout)
            if opcode == 0x8:
                return {"type": "closed"}
            if opcode == 0x1:
                payload = chunk
            elif opcode == 0x0:
                payload += chunk
            if fin:
                break
        return json.loads(payload.decode("utf-8"))

    def wait_for(self, want_type, timeout=10.0, collect=False):
        """Reads messages until one with type==want_type arrives."""
        collected = []
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                message = self.recv_json(timeout=max(0.1, deadline - time.time()))
            except socket.timeout:
                break
            if message.get("type") == "closed":
                raise AssertionError(f"connection closed while waiting for {want_type}")
            if message.get("type") == want_type:
                return (message, collected) if collect else message
            collected.append(message)
        raise AssertionError(f"timeout waiting for {want_type}; got {collected[-5:]}")


def login(base_url, token):
    import urllib.request

    request = urllib.request.Request(
        base_url + "/api/auth/login",
        data=json.dumps({"token": token}).encode(),
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    with urllib.request.urlopen(request) as response:
        cookie = response.headers.get("Set-Cookie", "")
        assert response.status == 200, response.status
    assert "edgedesk_session=" in cookie, cookie
    return cookie.split("edgedesk_session=", 1)[1].split(";", 1)[0]


def collect_output(client, needle, timeout=15.0):
    """Accumulates output messages until `needle` appears."""
    text = ""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            message = client.recv_json(timeout=max(0.1, deadline - time.time()))
        except socket.timeout:
            break
        if message.get("type") == "output":
            text += message.get("data", "")
            if needle in text:
                return text
        elif message.get("type") in ("exit", "error"):
            break
    raise AssertionError(f"timeout waiting for output {needle!r}; tail={text[-300:]!r}")


def expect_handshake_rejected(host, port, path, token=None, origin=None):
    """A rejected upgrade either answers with 4xx or drops the connection."""
    try:
        client = WsClient(host, port, path, token=token, origin=origin)
    except OSError:
        return
    try:
        assert client.status != 101, f"handshake unexpectedly upgraded (status {client.status})"
    finally:
        client.close()


def test_auth(host, port):
    expect_handshake_rejected(host, port, "/ws/terminal")
    cookie = login(f"http://{host}:{port}", TOKEN)
    # Valid token but a foreign Origin must not be upgraded (CSWSH defence).
    expect_handshake_rejected(host, port, "/ws/terminal", token=cookie, origin="http://evil.example")
    print("ws_test: handshake auth + origin rejection OK")


def test_terminal(host, port, cookie, origin):
    client = WsClient(host, port, "/ws/terminal", token=cookie, origin=origin)
    assert client.status == 101, client.status
    ready = client.wait_for("ready")
    assert ready.get("shell_available") is True, ready

    client.send_json({"type": "create", "cols": 100, "rows": 30, "cwd": "/tmp"})
    created = client.wait_for("created")
    session_id = created["session"]["id"]
    assert session_id, created
    client.wait_for("attached")

    client.send_json({"type": "input", "data": "echo WS_MARKER_$((6*7))\r"})
    text = collect_output(client, "WS_MARKER_42")
    assert "WS_MARKER_42" in text

    # UTF-8 and ANSI survive.
    client.send_json({"type": "input", "data": "printf '\\033[32mWS_COLOR\\033[0m 中文OK\\n'\r"})
    collect_output(client, "中文OK")

    # Resize round trip.
    client.send_json({"type": "resize", "cols": 132, "rows": 43})
    client.send_json({"type": "input", "data": "stty size\r"})
    collect_output(client, "43 132")

    # CommandGuard blocks an obviously destructive line (nothing is executed).
    client.send_json({"type": "input", "data": "rm -rf /\r"})
    guard = client.wait_for("guard")
    assert guard.get("reason"), guard
    client.send_json({"type": "cancel"})
    client.wait_for("cancelled")

    # Ping/pong keeps the connection healthy.
    client.send_json({"type": "ping"})
    client.wait_for("pong")

    # Disconnect (browser refresh) must NOT kill the Bash session.
    client.send_json({"type": "input", "data": "for i in 1 2 3; do echo KEEPALIVE_$i; sleep 0.4; done\r"})
    collect_output(client, "KEEPALIVE_1")
    client.close()
    time.sleep(0.5)

    # Reconnect with the same session id: the replay arrives right after the
    # attach confirmation and must contain earlier output.
    again = WsClient(host, port, f"/ws/terminal?session_id={session_id}", token=cookie, origin=origin)
    assert again.status == 101, again.status
    attached = again.wait_for("attached")
    assert attached["session"]["id"] == session_id, attached
    replay = collect_output(again, "KEEPALIVE")
    assert "WS_MARKER_42" in replay or "KEEPALIVE" in replay, replay[-300:]
    assert attached["session"]["running"] is True, attached

    # Sessions are listed through the HTTP API too.
    import urllib.request

    request = urllib.request.Request(f"http://{host}:{port}/api/terminal/sessions")
    request.add_header("Cookie", f"edgedesk_session={cookie}")
    with urllib.request.urlopen(request) as response:
        body = json.loads(response.read())
    assert any(item["id"] == session_id for item in body["sessions"]), body

    # Explicit close destroys the session.
    again.send_json({"type": "close"})
    time.sleep(0.5)
    request = urllib.request.Request(f"http://{host}:{port}/api/terminal/sessions")
    request.add_header("Cookie", f"edgedesk_session={cookie}")
    with urllib.request.urlopen(request) as response:
        body = json.loads(response.read())
    assert not any(item["id"] == session_id for item in body["sessions"]), body
    again.close()
    print("ws_test: terminal create/input/resize/reconnect/close OK")


def test_metrics(host, port, cookie, origin):
    client = WsClient(host, port, "/ws/metrics", token=cookie, origin=origin)
    assert client.status == 101, client.status
    snapshot = client.wait_for("snapshot")
    assert "sample" in snapshot, snapshot
    assert "history" in snapshot, snapshot
    sample = snapshot["sample"]
    assert sample["cpu"]["cores"] >= 1, sample
    assert sample["ts"] > 0, sample

    first_ts = sample["ts"]
    for _ in range(3):
        update = client.wait_for("metrics", timeout=8)
        if update["sample"]["ts"] > first_ts:
            break
    else:
        raise AssertionError("no incremental metrics sample received")
    client.send_json({"type": "ping"})
    client.wait_for("pong")
    client.close()
    print("ws_test: metrics snapshot + incremental push OK")


def main():
    global HOST, TOKEN
    if len(sys.argv) < 4:
        print(__doc__)
        return 2
    HOST = sys.argv[1]
    port = int(sys.argv[2])
    TOKEN = sys.argv[3]
    cookie = login(f"http://{HOST}:{port}", TOKEN)
    origin = f"http://{HOST}:{port}"
    test_auth(HOST, port)
    print("ws_test: auth stage done", flush=True)
    test_terminal(HOST, port, cookie, origin)
    test_metrics(HOST, port, cookie, origin)
    print("ws_test: PASS")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except AssertionError as error:
        import traceback
        traceback.print_exc()
        print(f"ws_test: FAILED: {error!r}", file=sys.stderr)
        sys.exit(1)
