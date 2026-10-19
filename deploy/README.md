# Process Supervisor deployment adapters

These files install the standalone `edgedesk-supervisor` daemon. The daemon
runs in the foreground and never daemonizes itself; the init system owns its
restart policy. Managed services are **not** registered with init — the
Supervisor starts and stops them through its own process groups.

The web backend connects to the Unix socket and must run as a user that is
allowed by both the socket mode and `SO_PEERCRED`. The examples use the
`edgedesk` group; replace it with the account that runs the web backend:

```sh
groupadd --system edgedesk
usermod -aG edgedesk edgedesk     # only when the backend runs as this user
```

Install the binary first, for example to `/usr/sbin/edgedesk-supervisor`.
The persistent state directory `/var/lib/edgedesk-supervisor` must be on a
writable persistent filesystem; on a read-only rootfs move `--database` and
`--log-dir` to the writable data partition and keep the same permissions
(0700 directories, 0600 files).

## systemd

```sh
install -m 0644 deploy/systemd/edgedesk-supervisor.service \
  /etc/systemd/system/edgedesk-supervisor.service
systemctl daemon-reload
systemctl enable --now edgedesk-supervisor
systemctl status edgedesk-supervisor
```

`KillMode=process` is deliberate: `systemctl stop` sends SIGTERM to the
Supervisor, which forwards it to each managed process group, waits for the
per-service stop timeout, escalates to SIGKILL, and only then exits. `Restart=always`
restarts the Supervisor after a crash. Managed processes keep running during
that gap and are adopted on restart if their identity can be verified.

Point the EdgeDesk web service at the same socket with
`--supervisor-socket /run/edgedesk-supervisor.sock` (the default). If the web
backend starts before the Supervisor, process-management APIs return
`503 supervisor_unavailable`; the rest of EdgeDesk remains available.

## OpenRC

```sh
install -m 0755 deploy/openrc/edgedesk-supervisor \
  /etc/init.d/edgedesk-supervisor
rc-update add edgedesk-supervisor default
rc-service edgedesk-supervisor start
rc-service edgedesk-supervisor status
```

The script requires OpenRC with `supervise-daemon` (OpenRC >= 0.21), which
provides the foreground process supervision and respawn. Override
`EDGEDESK_SUPERVISOR_*` variables in `/etc/conf.d/edgedesk-supervisor` if the
binary, socket, database or log paths differ.

## BusyBox init

```sh
install -m 0755 deploy/busybox/edgedesk-supervisor-boot \
  /usr/sbin/edgedesk-supervisor-boot
cat deploy/busybox/inittab >> /etc/inittab
kill -HUP 1
```

`::respawn:` makes BusyBox restart the Supervisor after a crash. The wrapper
creates `/var/lib/edgedesk-supervisor` and its log directory with mode 0700
before exec'ing the daemon. On devices without a console, redirect the wrapper's
stdout/stderr to the available logger.

## Verification

Only files and commands that were actually executed on a platform count as a
verification record; installing a script is not acceptance. See
`docs/testing.md` for the commands and the current record. The host-side unit
tests exercise the daemon directly but do not substitute for systemd, OpenRC or
BusyBox runtime validation.
