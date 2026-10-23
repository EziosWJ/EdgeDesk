# EdgeDesk deployment adapters and installer

`deploy/install.sh` is the supported way to install EdgeDesk on a Linux host.
It is a POSIX `sh` script (runs under dash, bash and BusyBox ash) and performs
only four kinds of work:

1. detect the available service manager;
2. collect and validate listen/path settings;
3. install the binaries, frontend (disk builds only) and configuration;
4. register `edgedesk` and `edgedesk-supervisor` with that init system.

It never edits business data. The administrator password is still set in the
Web UI on first login; the installer does not generate or log credentials.

## Quick start

```sh
# Interactive: asks for host, port, file root and prefix (TTY only)
sudo deploy/install.sh

# Non-interactive
sudo deploy/install.sh --host 127.0.0.1 --port 8080 \
  --source /opt/edgedesk-dist --file-root /srv/edgedesk-files

# Full server operations (root backend); see "Root operations profile" below
sudo deploy/install.sh --ops --host 127.0.0.1 --port 8080

# Inspect the environment without changing anything
deploy/install.sh --detect

# Print the full plan and generated files, write nothing
deploy/install.sh --dry-run --source out/build/release-embed/bin

# Stage a package tree without touching the running system
deploy/install.sh --destdir /tmp/edgedesk-stage \
  --source out/build/release-embed/bin --file-root /srv
```

`--source` points at the directory containing `edgedesk` and
`edgedesk-supervisor`. When omitted, the script looks next to itself, then in
its `bin/` subdirectory, then in the repository build directories. A release
package can therefore be uploaded as `install.sh` + `bin/` and installed
without `--source`; binaries already sitting at the install target are adopted
in place instead of requiring `--force`. `--web-root` is only needed for disk
builds; `release-embed` already contains the frontend.

## Detection

`--detect` prints machine-readable lines and exits:

```text
init=systemd
pid1=systemd
uid=0
systemd=yes
openrc=no
busybox=no
inittab=no
```

Detection checks the service manager, not only PID 1, because Alpine can run
OpenRC with a BusyBox `init`. Order: systemd (`systemctl` + `/run/systemd/system`),
OpenRC (`rc-update` + `rc-service`), then BusyBox (`busybox` + `/etc/inittab`).
Override with `--init systemd|openrc|busybox|none` when running in a container
or a non-standard image.

## What gets installed

| Path | Content |
| --- | --- |
| `/opt/edgedesk/bin/edgedesk` | Web backend |
| `/opt/edgedesk/bin/edgedesk-supervisor` | Process supervisor (unless `--no-supervisor`) |
| `/opt/edgedesk/web/` | Frontend, only when `--web-root` is passed |
| `/etc/edgedesk/edgedesk.conf` | Generated `key = value` configuration, `0640 root:<service-group>` |
| `/var/lib/edgedesk` | Persistent data, `0700 <service-user>` |
| `/var/lib/edgedesk/apps` | Extension platform root, `0700 <service-user>` |
| `/run/edgedesk` | Runtime state; recreated by systemd `RuntimeDirectory` or the boot wrapper |
| `/var/lib/edgedesk-supervisor` | Supervisor SQLite and logs, `0700 root` |

Init adapters:

- **systemd**: `edgedesk.service` and `edgedesk-supervisor.service` under
  `/etc/systemd/system`, both enabled and started. The default web unit uses
  `ProtectSystem=strict`, `NoNewPrivileges=yes`, `StateDirectory` and
  `RuntimeDirectory`. With `--ops` it instead runs as `root` with
  `UMask=0022`, `Environment=HOME=/root`, no `ProtectSystem`/`ProtectHome`/
  `PrivateTmp`/`ReadWritePaths`, and the supervisor gains `--allow-uid 0` and
  drops its mount hardening. The supervisor always keeps `KillMode=process`
  and `NoNewPrivileges=no` because it must switch service identities.
- **OpenRC**: `/etc/init.d/edgedesk` and `/etc/init.d/edgedesk-supervisor`,
  both using `supervise-daemon` (OpenRC >= 0.21), added to `default`.
- **BusyBox init**: `edgedesk-boot` and `edgedesk-supervisor-boot` wrappers plus
  one marked block in `/etc/inittab`; `SIGHUP` reloads init. The web wrapper
  drops to the service account with `setpriv`, then `su`, and only falls back
  to running as root when neither exists (with a warning on stderr).

The generated configuration uses the existing `--config` file support:

```ini
host = 127.0.0.1
port = 8080
file-root = /srv/edgedesk-files
data-dir = /var/lib/edgedesk
state-dir = /run/edgedesk
apps-root = /var/lib/edgedesk/apps
audit-log = /var/lib/edgedesk/audit.log
supervisor-socket = /run/edgedesk-supervisor.sock
```

## Root operations profile (`--ops`)

The default install is a deliberately constrained panel: the backend, terminal
and file API run as the `edgedesk` service account and systemd makes the
filesystem read-only outside the data directories. That profile cannot edit
`/etc`, run `apt`, manage `systemctl`, or administer `/opt`.

`--ops` is the explicit opt-in for a full operations console:

```sh
sudo deploy/install.sh --ops --host 127.0.0.1 --port 8080
```

It changes the privilege model, not just one command line:

- `User=root`, `Group=root`, `UMask=0022`, `Environment=HOME=/root`;
- no `ProtectSystem`, `ProtectHome`, `PrivateTmp` or `ReadWritePaths` in the
  web unit, so the terminal and file API can write the host filesystem;
- `--file-root` defaults to `/` (override with `--file-root DIR`);
- the supervisor gets `--allow-uid 0` (the backend's `SO_PEERCRED` is uid 0)
  and loses its `ProtectSystem`/`ProtectHome`/`PrivateTmp`; managed services
  inherit that namespace, so they can use `/etc`, `/home` and the host `/tmp`;
- authentication, CSRF, rate limiting and audit are unchanged: the mode changes
  the process identity and systemd namespace, not the API.

Because the panel is now equivalent to host root, treat it as an admin plane:
keep it on loopback, or put TLS/VPN and a trusted network boundary in front.
A non-loopback `--host` still requires `--allow-non-loopback`. Do not share one
administrator account between people.

`--service-user root` without `--ops` is rejected, and `--ops` with any other
`--service-user` is rejected, so the mode can never be selected by accident.

### Migrating an existing install

The application refuses a data directory owned by another UID. When the
service identity changes, the installer recursively applies the new owner to
`--data-dir` and `--apps-root` before installing files. Only dedicated
directories are accepted; broad paths such as `/`, `/var/lib` or `/etc` are
refused. A manual migration is therefore usually just:

```sh
sudo deploy/install.sh --ops --force --source /opt/edgedesk/bin
sudo systemctl restart edgedesk-supervisor edgedesk
```

The `.bak` copies of unit and config files are kept by `--force`.

## Safety rules enforced by the installer

- **Loopback by default.** A non-loopback `--host` is refused unless
  `--allow-non-loopback` is given (or confirmed interactively). The warning
  requires TLS or a trusted VPN/gateway and `--cookie-secure` for HTTPS.
- No credential, password or token is written by the installer.
- Values that end up in unit files are validated first: numeric port,
  restricted host character set, absolute paths without `..`, no whitespace,
  quotes or `#`.
- Existing files are not overwritten without `--force`; a `.bak` copy is kept.
- `--dry-run` and `--destdir` do not need root and never modify the running
  system.

## Uninstall

```sh
sudo deploy/install.sh --uninstall              # keeps /var/lib/edgedesk*
sudo deploy/install.sh --uninstall --purge      # also removes data and supervisor state
```

Uninstall stops and unregisters the detected init system, removes the binaries,
init files and `/etc/edgedesk`, and preserves persistent data unless `--purge`
is given. The service account is not removed automatically. `--purge` refuses
a broad prefix such as `/`, `/usr`, `/opt` or `/var/lib`.

## Manual installation

The generated files are intentionally close to the checked-in reference units.
Manual installation is still possible when the host has an unusual layout.

Install the binaries first, for example to `/opt/edgedesk/bin`. The persistent
state directory `/var/lib/edgedesk-supervisor` must be on a writable persistent
filesystem; on a read-only rootfs move `--database` and `--log-dir` to the
writable data partition and keep the same permissions (0700 directories, 0600
files).

### systemd

```sh
install -m 0644 deploy/systemd/edgedesk-supervisor.service \
  /etc/systemd/system/edgedesk-supervisor.service
systemctl daemon-reload
systemctl enable --now edgedesk-supervisor
systemctl status edgedesk-supervisor
```

`KillMode=process` is deliberate: `systemctl stop` sends SIGTERM to the
Supervisor, which forwards it to each managed process group, waits for the
per-service stop timeout, escalates to SIGKILL, and only then exits.
`Restart=always` restarts the Supervisor after a crash. Managed processes keep
running during that gap and are adopted on restart if their identity can be
verified.

Point the EdgeDesk web service at the same socket with
`--supervisor-socket /run/edgedesk-supervisor.sock` (the default). If the web
backend starts before the Supervisor, process-management APIs return
`503 supervisor_unavailable`; the rest of EdgeDesk remains available.

### OpenRC

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

### BusyBox init

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

Only files and commands actually executed on a platform count as a
verification record; installing a script is not acceptance. `docs/testing.md`
records the commands that were run and the remaining gaps. The host-side tests
in `tests/install/install_test.sh` stage all three adapters, syntax-check the
generated scripts, and assert the safety rules; they do not replace running
the init systems for real.
