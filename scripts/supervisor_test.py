#!/usr/bin/env python3
"""EdgeDesk Process Supervisor integration test (stdlib only).

The shell driver starts `edgedesk-supervisor` and an `edgedesk` server that
talks to it over a temporary Unix socket, then invokes this script in phases so
the driver can restart the backend, restart the supervisor and simulate an
unavailable daemon in between:

  crud                       authenticated CRUD, lifecycle, logs and security
  after-backend-restart      the managed process survives an EdgeDesk restart
  after-supervisor-restart   the new daemon adopts the running process; stop it
  after-manual-stop-restart  an explicit stop is not undone by daemon restart
  unavailable                supervisor RPCs fail with 503 while EdgeDesk works

Every phase logs in normally, so authentication and CSRF remain part of the
contract under test. No request ever sends a PID; process identity only comes
back from the API.
"""
import argparse
import json
import os
import pwd
import grp
import sys
import time
import urllib.error
import urllib.request

FAILURES = []


def check(condition, message):
    if condition:
        return
    FAILURES.append(message)
    print(f"FAILED: {message}")


class Client:
    def __init__(self, host, port, password):
        self.base = f"http://{host}:{port}"
        self.password = password
        self.cookie = ""
        self.origin = {"Origin": self.base}

    def request(self, method, path, body=None, headers=None, cookie=None, timeout=20):
        data = None
        if body is not None:
            data = body if isinstance(body, bytes) else json.dumps(body).encode()
        req = urllib.request.Request(self.base + path, data=data, method=method)
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

    def login(self):
        status, payload, headers = self.request(
            "POST", "/api/auth/login", {"password": self.password})
        check(status == 200, f"login status {status}: {payload[:160]!r}")
        raw = headers.get("Set-Cookie", "")
        check("edgedesk_session=" in raw, f"login cookie missing: {raw}")
        self.cookie = raw.split("edgedesk_session=", 1)[1].split(";", 1)[0]
        return self.cookie

    def json(self, method, path, body=None, origin=True):
        headers = dict(self.origin) if origin else {}
        status, payload, _ = self.request(method, path, body, headers=headers,
                                          cookie=self.cookie)
        try:
            parsed = json.loads(payload.decode())
        except Exception:
            parsed = {}
        return status, parsed


def identity():
    uid = os.geteuid()
    gid = os.getegid()
    try:
        user = pwd.getpwuid(uid).pw_name
    except KeyError:
        user = str(uid)
    try:
        group = grp.getgrgid(gid).gr_name
    except KeyError:
        group = str(gid)
    return uid, gid, user, group, uid == 0


def service_payload(name, executable, args, policy="never"):
    uid, gid, user, group, _ = identity()
    return {
        "name": name,
        "executable": executable,
        "args": args,
        "cwd": "/",
        "env": {},
        "run_user": user,
        "run_group": group,
        "allow_root": uid == 0,
        "autostart": False,
        "restart_policy": policy,
        "max_restarts": 2,
        "restart_window_sec": 60,
        "restart_delay_ms": 50,
        "max_restart_delay_ms": 200,
        "stable_after_sec": 5,
        "stop_timeout_ms": 2000,
        "log_max_bytes": 65536,
    }


def wait_service(client, service_id, wanted, timeout=12.0):
    deadline = time.monotonic() + timeout
    last = {}
    while time.monotonic() < deadline:
        status, body = client.json("GET", f"/api/supervisor/services/{service_id}")
        if status == 200:
            last = body
            if body.get("state") == wanted:
                return body
        time.sleep(0.1)
    check(False, f"service {service_id} did not reach {wanted}; last={last!r}")
    return last


def wait_operation(client, operation_id, timeout=20.0):
    """Polls the non-blocking stop/restart operation until it settles.

    A timeout here means the caller stopped waiting; it must not be interpreted
    as the operation having failed or the service having stopped.
    """
    deadline = time.monotonic() + timeout
    last = {}
    while time.monotonic() < deadline:
        status, body = client.json(
            "GET", f"/api/supervisor/operations/{operation_id}")
        if status != 200:
            last = body
            break
        last = body
        if body.get("state") != "pending":
            return body
        time.sleep(0.05)
    check(False, f"operation {operation_id} did not settle; last={last!r}")
    return last


def pid_alive(pid):
    if not isinstance(pid, int) or pid <= 1:
        return False
    return os.path.isdir(f"/proc/{pid}")


def load_state(path):
    with open(path, "r", encoding="utf-8") as handle:
        return json.load(handle)


def save_state(path, state):
    with open(path, "w", encoding="utf-8") as handle:
        json.dump(state, handle, sort_keys=True)
        handle.write("\n")


def audit_event_count(path, event):
    """Count exact audit events in the JSON-lines audit log."""
    if not path or not os.path.exists(path):
        return 0
    needle = f'"event":"{event}"'
    count = 0
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            if needle in line:
                count += 1
    return count


def audit_contains(path, needle):
    if not path or not os.path.exists(path):
        return False
    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        return any(needle in line for line in handle)


def test_log_polling_does_not_audit(client, service_id, audit_log):
    """PSH-08: passive tail/cursor polling must not grow the audit log.

    The processes UI polls this endpoint every ~2s while a service is open, so a
    per-request audit event would grow without bound. Management operations and
    root-privilege denials must still be recorded.
    """
    if not audit_log:
        print("note: --audit-log not supplied; skipping PSH-08 audit growth check")
        return
    before = audit_event_count(audit_log, "supervisor.logs")
    for index in range(40):
        status, body = client.json(
            "GET", f"/api/supervisor/services/{service_id}/logs?tail_bytes=4096")
        check(status == 200, f"log poll {index} status {status}: {body!r}")
    after = audit_event_count(audit_log, "supervisor.logs")
    check(after == before,
          f"log polling grew audit log by {after - before} event(s) "
          f"(before={before}, after={after})")
    check(audit_event_count(audit_log, "supervisor.create") > 0,
          "management create was not audited")
    check(audit_contains(audit_log, "root_confirmation_required"),
          "denied root-privilege request was not audited")


def test_unauthenticated_and_csrf(client):
    status, body, _ = client.request("GET", "/api/supervisor/health")
    check(status == 401, f"unauthenticated supervisor health status {status}")
    status, _, _ = client.request("GET", "/api/supervisor/services")
    check(status == 401, f"unauthenticated supervisor list status {status}")
    # Cookie requests without a same-origin Origin must be rejected before the
    # request reaches a worker or the daemon.
    status, _, _ = client.request("POST", "/api/supervisor/services",
                                  service_payload("csrf-denied", "/bin/sleep", ["1"]),
                                  cookie=client.cookie)
    check(status == 403, f"CSRF supervisor create status {status}")


def phase_crud(client, state_path, audit_log):
    uid, _, user, group, is_root = identity()
    client.login()
    test_unauthenticated_and_csrf(client)

    status, health = client.json("GET", "/api/supervisor/health")
    check(status == 200 and health.get("status") == "ok",
          f"supervisor health {status}: {health!r}")
    check(health.get("service") == "edgedesk-supervisor", f"health service {health!r}")
    check(health.get("degraded") is False, f"health degraded {health!r}")

    # A missing identity must never silently become root.
    missing = service_payload("missing-identity", "/bin/sleep", ["1"])
    missing.pop("run_user")
    missing.pop("run_group")
    status, body = client.json("POST", "/api/supervisor/services", missing)
    check(status == 400, f"missing identity status {status}: {body!r}")
    check(body.get("error") == "invalid_service_config", f"missing identity body {body!r}")

    # Root requires the explicit confirmation flag, whatever the daemon UID.
    root_body = service_payload("root-without-confirmation", "/bin/sleep", ["1"])
    root_body.update({"run_user": "0", "run_group": "0", "allow_root": False})
    status, body = client.json("POST", "/api/supervisor/services", root_body)
    check(status == 403, f"root without confirmation status {status}: {body!r}")
    check(body.get("error") == "root_confirmation_required",
          f"root without confirmation body {body!r}")

    name = f"it-sleeper-{os.getpid()}"
    status, created = client.json(
        "POST", "/api/supervisor/services",
        service_payload(name, "/bin/sleep", ["300"]))
    check(status == 200 and created.get("id"), f"create status {status}: {created!r}")
    service_id = created.get("id", "")
    check(created.get("name") == name and created.get("state") == "STOPPED",
          f"created service {created!r}")
    check(created.get("pid") is None, f"new service already has a pid {created!r}")

    status, body = client.json(
        "POST", "/api/supervisor/services",
        service_payload(name, "/bin/sleep", ["300"]))
    check(status == 409, f"duplicate name status {status}: {body!r}")

    status, listed = client.json("GET", "/api/supervisor/services?offset=0&limit=50")
    ids = {item.get("id") for item in listed.get("services", [])}
    check(status == 200 and service_id in ids, f"service list {status}: {listed!r}")

    # Partial update preserves unspecified fields.
    renamed = name + "-renamed"
    status, updated = client.json(
        "PUT", f"/api/supervisor/services/{service_id}",
        {"name": renamed, "args": ["301"]})
    check(status == 200 and updated.get("name") == renamed,
          f"update status {status}: {updated!r}")
    check(updated.get("args") == ["301"] and updated.get("cwd") == "/",
          f"update did not preserve fields {updated!r}")

    status, body = client.json("POST", f"/api/supervisor/services/{service_id}/start",
                               {})
    check(status == 200, f"start status {status}: {body!r}")
    running = wait_service(client, service_id, "RUNNING")
    pid = running.get("pid")
    check(isinstance(pid, int) and pid_alive(pid), f"running pid {pid!r}")
    check(running.get("pgid") == pid, f"process group {running.get('pgid')!r} != pid {pid!r}")
    check(running.get("desired_state") == "running", f"desired state {running!r}")

    status, body = client.json("DELETE", f"/api/supervisor/services/{service_id}")
    check(status == 409, f"delete running service status {status}: {body!r}")

    status, restarted = client.json(
        "POST", f"/api/supervisor/services/{service_id}/restart", {})
    check(status == 200 and restarted.get("operation_id"),
          f"restart status {status}: {restarted!r}")
    operation = wait_operation(client, restarted.get("operation_id", ""))
    check(operation.get("state") == "succeeded",
          f"restart operation {operation!r}")
    running = wait_service(client, service_id, "RUNNING")
    new_pid = running.get("pid")
    check(isinstance(new_pid, int) and pid_alive(new_pid) and new_pid != pid,
          f"restart pid {new_pid!r} (was {pid!r})")
    pid = new_pid

    status, stopped = client.json("POST", f"/api/supervisor/services/{service_id}/stop",
                                  {})
    check(status == 200 and stopped.get("operation_id"),
          f"stop status {status}: {stopped!r}")
    operation = wait_operation(client, stopped.get("operation_id", ""))
    result = operation.get("result") or {}
    check(operation.get("state") == "succeeded"
          and result.get("state") == "STOPPED",
          f"stop operation {operation!r}")
    check(not pid_alive(pid), f"stopped process {pid} still exists")

    status, started = client.json("POST", f"/api/supervisor/services/{service_id}/start",
                                  {})
    check(status == 200, f"second start status {status}: {started!r}")
    running = wait_service(client, service_id, "RUNNING")
    pid = running.get("pid")
    check(isinstance(pid, int) and pid_alive(pid), f"second start pid {pid!r}")

    # The logger service proves argv execution and output capture. The literal
    # shell metacharacters must appear unchanged: no shell is involved.
    marker = state_path + ".shell-marker"
    if os.path.exists(marker):
        os.unlink(marker)
    logger_name = f"it-logger-{os.getpid()}"
    literal = f"$(touch {marker})"
    status, logger = client.json(
        "POST", "/api/supervisor/services",
        service_payload(logger_name, "/bin/echo", [literal, "semi;colon", "space value"]))
    check(status == 200 and logger.get("id"), f"logger create {status}: {logger!r}")
    logger_id = logger.get("id", "")
    status, body = client.json("POST", f"/api/supervisor/services/{logger_id}/start", {})
    check(status == 200, f"logger start {status}: {body!r}")
    wait_service(client, logger_id, "STOPPED")

    content = ""
    deadline = time.monotonic() + 5.0
    while time.monotonic() < deadline:
        status, body = client.json(
            "GET", f"/api/supervisor/services/{logger_id}/logs?tail_bytes=65536")
        if status == 200:
            content = body.get("content", "")
            if literal in content:
                break
        time.sleep(0.1)
    check(status == 200, f"logs status {status}: {body!r}")
    check(literal in content, f"argv literal missing from logs: {content!r}")
    check("semi;colon" in content and "space value" in content,
          f"argv elements altered in logs: {content!r}")
    check(not os.path.exists(marker), "shell metacharacters were executed")
    check(body.get("truncated") is False, f"unexpected log truncation {body!r}")

    status, body = client.json("DELETE", f"/api/supervisor/services/{logger_id}")
    check(status == 200 and body.get("deleted") is True,
          f"logger delete {status}: {body!r}")

    status, body = client.json("GET", "/api/supervisor/services/does-not-exist")
    check(status == 404, f"missing service status {status}: {body!r}")
    status, body = client.json("POST", "/api/supervisor/services/does-not-exist/start",
                               {})
    check(status == 404, f"missing service start status {status}: {body!r}")

    test_log_polling_does_not_audit(client, service_id, audit_log)

    save_state(state_path, {
        "service_id": service_id,
        "name": renamed,
        "pid": pid,
    })


def phase_after_backend_restart(client, state_path, audit_log):
    state = load_state(state_path)
    client.login()
    service = wait_service(client, state["service_id"], "RUNNING", timeout=10.0)
    check(service.get("pid") == state["pid"],
          f"pid changed across backend restart: {service.get('pid')!r} != {state['pid']!r}")
    check(pid_alive(state["pid"]), "process died with the EdgeDesk backend")
    status, health = client.json("GET", "/api/supervisor/health")
    check(status == 200 and health.get("running", 0) >= 1,
          f"health after backend restart {status}: {health!r}")


def phase_after_supervisor_restart(client, state_path, audit_log):
    state = load_state(state_path)
    client.login()
    deadline = time.monotonic() + 10.0
    service = {}
    while time.monotonic() < deadline:
        status, service = client.json(
            "GET", f"/api/supervisor/services/{state['service_id']}")
        if status == 200:
            break
        time.sleep(0.1)
    check(status == 200, f"service after supervisor restart {status}: {service!r}")
    check(service.get("state") == "RUNNING",
          f"service was not adopted after supervisor restart {service!r}")
    check(service.get("pid") == state["pid"],
          f"adopted pid {service.get('pid')!r} != {state['pid']!r}")
    check(pid_alive(state["pid"]), "process died with the supervisor")

    status, body = client.json(
        "POST", f"/api/supervisor/services/{state['service_id']}/stop", {})
    check(status == 200 and body.get("operation_id"),
          f"stop after adoption {status}: {body!r}")
    operation = wait_operation(client, body.get("operation_id", ""))
    result = operation.get("result") or {}
    check(operation.get("state") == "succeeded",
          f"stop after adoption operation {operation!r}")
    check(not pid_alive(state["pid"]), "process survived an explicit stop")
    check(result.get("desired_state") == "stopped",
          f"desired state {result!r}")


def phase_after_manual_stop_restart(client, state_path, audit_log):
    state = load_state(state_path)
    client.login()
    service = wait_service(client, state["service_id"], "STOPPED", timeout=10.0)
    check(service.get("pid") is None, f"manual stop was undone {service!r}")
    check(not pid_alive(state["pid"]), "manual stop was undone by daemon restart")
    status, body = client.json("DELETE", f"/api/supervisor/services/{state['service_id']}")
    check(status == 200 and body.get("deleted") is True,
          f"delete stopped service {status}: {body!r}")
    status, body = client.json("DELETE", f"/api/supervisor/services/{state['service_id']}")
    check(status == 404, f"delete missing service status {status}: {body!r}")


def phase_unavailable(client, state_path, audit_log):
    state = load_state(state_path)
    client.login()
    status, body = client.json("GET", "/api/supervisor/health")
    check(status == 503, f"unavailable health status {status}: {body!r}")
    check(body.get("error") == "supervisor_unavailable",
          f"unavailable health body {body!r}")
    status, body = client.json("GET", "/api/supervisor/services")
    check(status == 503 and body.get("error") == "supervisor_unavailable",
          f"unavailable list {status}: {body!r}")
    check(pid_alive(state["pid"]),
          "managed process did not survive an unavailable supervisor")

    status, body = client.json("GET", "/api/health", origin=False)
    check(status == 200 and body.get("status") == "ok",
          f"core health while supervisor down {status}: {body!r}")
    status, body = client.json("GET", "/api/auth/session", origin=False)
    check(status == 200 and body.get("authenticated") is True,
          f"session while supervisor down {status}: {body!r}")
    status, body = client.json("GET", "/api/files?path=/", origin=False)
    check(status == 200 and isinstance(body.get("entries"), list),
          f"files while supervisor down {status}: {body!r}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--password", required=True)
    parser.add_argument("--state", required=True)
    parser.add_argument("--audit-log", default="")
    parser.add_argument("--phase", required=True, choices=[
        "crud", "after-backend-restart", "after-supervisor-restart",
        "after-manual-stop-restart", "unavailable",
    ])
    args = parser.parse_args()

    client = Client(args.host, args.port, args.password)
    phases = {
        "crud": phase_crud,
        "after-backend-restart": phase_after_backend_restart,
        "after-supervisor-restart": phase_after_supervisor_restart,
        "after-manual-stop-restart": phase_after_manual_stop_restart,
        "unavailable": phase_unavailable,
    }
    phases[args.phase](client, args.state, args.audit_log)

    if FAILURES:
        print(f"supervisor {args.phase}: {len(FAILURES)} failure(s)")
        return 1
    print(f"supervisor {args.phase}: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
