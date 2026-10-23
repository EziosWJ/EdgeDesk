#!/bin/sh
# Deterministic tests for deploy/install.sh. They never touch the running
# system: every install is staged under a temporary --destdir, and init
# registration/user creation is skipped in that mode.
#
# Usage: sh tests/install/install_test.sh <repo-root>
set -eu

REPO_ROOT=${1:-$(CDPATH= cd -P "$(dirname "$0")/../.." && pwd)}
INSTALL="$REPO_ROOT/deploy/install.sh"
WORK=${TMPDIR:-/tmp}/edgedesk-install-test.$$
PASS=0

cleanup() {
    rm -rf "$WORK"
}
trap cleanup EXIT HUP INT TERM

fail() {
    printf 'FAIL: %s\n' "$*" >&2
    exit 1
}

ok() {
    PASS=$((PASS + 1))
    printf 'ok %d - %s\n' "$PASS" "$1"
}

assert_file() {
    [ -f "$1" ] || fail "expected file: $1"
}

assert_no_file() {
    [ ! -e "$1" ] || fail "expected no file: $1"
}

assert_contains() {
    grep -Fq -- "$2" "$1" || fail "expected '$2' in $1"
}

assert_not_contains() {
    if grep -Fq -- "$2" "$1"; then
        fail "did not expect '$2' in $1"
    fi
}

expect_fail() {
    description=$1
    shift
    if "$@" >"$WORK/last.out" 2>&1; then
        fail "$description (command unexpectedly succeeded)"
    fi
    ok "$description"
}

mkdir -p "$WORK/bin" "$WORK/srv" "$WORK/stage-systemd" "$WORK/stage-openrc" \
    "$WORK/stage-busybox" "$WORK/stage-nosup" "$WORK/stage-host" "$WORK/stage-uninstall" \
    "$WORK/stage-ops" "$WORK/stage-ops-openrc" "$WORK/stage-ops-busybox"

printf '#!/bin/sh\nexit 0\n' > "$WORK/bin/edgedesk"
printf '#!/bin/sh\nexit 0\n' > "$WORK/bin/edgedesk-supervisor"
chmod 0755 "$WORK/bin/edgedesk" "$WORK/bin/edgedesk-supervisor"

# ---------------------------------------------------------------------------
# Syntax and detection
# ---------------------------------------------------------------------------

sh -n "$INSTALL"
ok "install.sh passes sh -n"

if command -v dash >/dev/null 2>&1; then
    dash -n "$INSTALL"
    ok "install.sh passes dash -n"
fi

"$INSTALL" --detect > "$WORK/detect.out"
assert_contains "$WORK/detect.out" "init="
assert_contains "$WORK/detect.out" "pid1="
assert_contains "$WORK/detect.out" "systemd="
ok "detect prints machine-readable environment"

# ---------------------------------------------------------------------------
# systemd staging
# ---------------------------------------------------------------------------

"$INSTALL" --destdir "$WORK/stage-systemd" --init systemd \
    --source "$WORK/bin" --file-root "$WORK/srv" \
    --host 127.0.0.1 --port 18080 --no-start --no-enable > "$WORK/systemd.out"

STAGE="$WORK/stage-systemd"
assert_file "$STAGE/opt/edgedesk/bin/edgedesk"
assert_file "$STAGE/opt/edgedesk/bin/edgedesk-supervisor"
assert_file "$STAGE/etc/edgedesk/edgedesk.conf"
assert_file "$STAGE/etc/systemd/system/edgedesk.service"
assert_file "$STAGE/etc/systemd/system/edgedesk-supervisor.service"
ok "systemd staging installs binaries, config and both units"

assert_contains "$STAGE/etc/edgedesk/edgedesk.conf" "host = 127.0.0.1"
assert_contains "$STAGE/etc/edgedesk/edgedesk.conf" "port = 18080"
assert_contains "$STAGE/etc/edgedesk/edgedesk.conf" "file-root = $WORK/srv"
assert_contains "$STAGE/etc/edgedesk/edgedesk.conf" "supervisor-socket = /run/edgedesk-supervisor.sock"
ok "config file carries listen and path values"

assert_contains "$STAGE/etc/systemd/system/edgedesk.service" \
    "ExecStart=/opt/edgedesk/bin/edgedesk --config /etc/edgedesk/edgedesk.conf"
assert_contains "$STAGE/etc/systemd/system/edgedesk.service" "User=edgedesk"
assert_contains "$STAGE/etc/systemd/system/edgedesk.service" "ProtectSystem=strict"
assert_contains "$STAGE/etc/systemd/system/edgedesk.service" "StateDirectory=edgedesk"
assert_contains "$STAGE/etc/systemd/system/edgedesk.service" "RuntimeDirectory=edgedesk"
ok "web unit uses config file, service account and hardening"

assert_contains "$STAGE/etc/systemd/system/edgedesk-supervisor.service" "KillMode=process"
assert_contains "$STAGE/etc/systemd/system/edgedesk-supervisor.service" "--socket-group edgedesk"
assert_contains "$STAGE/etc/systemd/system/edgedesk-supervisor.service" "NoNewPrivileges=no"
ok "supervisor unit keeps process-group stop and socket group"

if grep -Rq "0\.0\.0\.0" "$STAGE" 2>/dev/null; then
    fail "default install must not reference 0.0.0.0"
fi
ok "default install never binds all interfaces"

# ---------------------------------------------------------------------------
# Root operations profile (--ops)
# ---------------------------------------------------------------------------

"$INSTALL" --destdir "$WORK/stage-ops" --init systemd --ops \
    --source "$WORK/bin" --file-root "$WORK/srv" --port 18081 \
    --no-start --no-enable > "$WORK/ops.out"

STAGE="$WORK/stage-ops"
assert_file "$STAGE/etc/systemd/system/edgedesk.service"
assert_contains "$STAGE/etc/edgedesk/edgedesk.conf" "file-root = $WORK/srv"
assert_contains "$STAGE/etc/systemd/system/edgedesk.service" "User=root"
assert_contains "$STAGE/etc/systemd/system/edgedesk.service" "Group=root"
assert_contains "$STAGE/etc/systemd/system/edgedesk.service" "UMask=0022"
assert_contains "$STAGE/etc/systemd/system/edgedesk.service" "Environment=HOME=/root"
assert_contains "$STAGE/etc/systemd/system/edgedesk.service" "StateDirectory=edgedesk"
assert_contains "$STAGE/etc/systemd/system/edgedesk.service" "RuntimeDirectory=edgedesk"
assert_not_contains "$STAGE/etc/systemd/system/edgedesk.service" "ProtectSystem"
assert_not_contains "$STAGE/etc/systemd/system/edgedesk.service" "ProtectHome"
assert_not_contains "$STAGE/etc/systemd/system/edgedesk.service" "PrivateTmp"
assert_not_contains "$STAGE/etc/systemd/system/edgedesk.service" "ReadWritePaths"
ok "--ops generates a root web unit without filesystem hardening"

assert_contains "$STAGE/etc/systemd/system/edgedesk-supervisor.service" "--allow-uid 0"
assert_not_contains "$STAGE/etc/systemd/system/edgedesk-supervisor.service" "ProtectSystem"
ok "--ops authorizes the root backend and relaxes the supervisor sandbox"

"$INSTALL" --dry-run --init systemd --ops --source "$WORK/bin" --yes \
    > "$WORK/ops-default.out"
assert_contains "$WORK/ops-default.out" "file-root = /"
assert_contains "$WORK/ops-default.out" "User=root"
ok "--ops defaults the file API root to /"

"$INSTALL" --destdir "$WORK/stage-ops-openrc" --init openrc --ops \
    --source "$WORK/bin" --file-root "$WORK/srv" --no-start --no-enable \
    > "$WORK/ops-openrc.out"
STAGE="$WORK/stage-ops-openrc"
assert_contains "$STAGE/etc/init.d/edgedesk" 'EDGEDESK_USER:=root'
assert_contains "$STAGE/etc/init.d/edgedesk-supervisor" "--allow-uid 0"
sh -n "$STAGE/etc/init.d/edgedesk"
sh -n "$STAGE/etc/init.d/edgedesk-supervisor"
ok "--ops propagates to the OpenRC adapters"

"$INSTALL" --destdir "$WORK/stage-ops-busybox" --init busybox --ops \
    --source "$WORK/bin" --file-root "$WORK/srv" --no-start --no-enable \
    > "$WORK/ops-busybox.out"
STAGE="$WORK/stage-ops-busybox"
assert_contains "$STAGE/opt/edgedesk/bin/edgedesk-boot" "umask 0022"
assert_contains "$STAGE/opt/edgedesk/bin/edgedesk-supervisor-boot" "--allow-uid 0"
sh -n "$STAGE/opt/edgedesk/bin/edgedesk-boot"
sh -n "$STAGE/opt/edgedesk/bin/edgedesk-supervisor-boot"
ok "--ops propagates to the BusyBox wrappers"

expect_fail "--ops rejects a non-root service user" \
    "$INSTALL" --dry-run --init systemd --ops --source "$WORK/bin" \
    --service-user alice --file-root "$WORK/srv"

expect_fail "root without --ops is rejected" \
    "$INSTALL" --dry-run --init systemd --source "$WORK/bin" \
    --service-user root --file-root "$WORK/srv"

expect_fail "ownership adoption refuses a broad data directory" \
    "$INSTALL" --dry-run --init systemd --ops --source "$WORK/bin" \
    --file-root "$WORK/srv" --data-dir /var/lib

# ---------------------------------------------------------------------------
# OpenRC staging
# ---------------------------------------------------------------------------

"$INSTALL" --destdir "$WORK/stage-openrc" --init openrc \
    --source "$WORK/bin" --file-root "$WORK/srv" --no-start --no-enable > "$WORK/openrc.out"

STAGE="$WORK/stage-openrc"
assert_file "$STAGE/etc/init.d/edgedesk"
assert_file "$STAGE/etc/init.d/edgedesk-supervisor"
sh -n "$STAGE/etc/init.d/edgedesk"
sh -n "$STAGE/etc/init.d/edgedesk-supervisor"
assert_contains "$STAGE/etc/init.d/edgedesk" 'supervisor="supervise-daemon"'
assert_contains "$STAGE/etc/init.d/edgedesk" 'command_user="${EDGEDESK_USER}:${EDGEDESK_GROUP}"'
assert_contains "$STAGE/etc/init.d/edgedesk-supervisor" 'command_user="root:root"'
ok "OpenRC scripts are valid sh and use supervise-daemon"

# ---------------------------------------------------------------------------
# BusyBox staging
# ---------------------------------------------------------------------------

"$INSTALL" --destdir "$WORK/stage-busybox" --init busybox \
    --source "$WORK/bin" --file-root "$WORK/srv" --no-start --no-enable > "$WORK/busybox.out"

STAGE="$WORK/stage-busybox"
assert_file "$STAGE/opt/edgedesk/bin/edgedesk-boot"
assert_file "$STAGE/opt/edgedesk/bin/edgedesk-supervisor-boot"
sh -n "$STAGE/opt/edgedesk/bin/edgedesk-boot"
sh -n "$STAGE/opt/edgedesk/bin/edgedesk-supervisor-boot"
assert_contains "$STAGE/etc/inittab" "::respawn:/opt/edgedesk/bin/edgedesk-boot"
assert_contains "$STAGE/etc/inittab" "::respawn:/opt/edgedesk/bin/edgedesk-supervisor-boot"

count=$(grep -c '^# BEGIN EDGEDESK' "$STAGE/etc/inittab" || true)
[ "$count" -eq 1 ] || fail "expected one EDGEDESK inittab block, found $count"
ok "BusyBox wrappers are valid sh and inittab has one managed block"

# Re-running with --force must replace, not duplicate, the inittab block.
"$INSTALL" --destdir "$STAGE" --init busybox --source "$WORK/bin" \
    --file-root "$WORK/srv" --no-start --no-enable --force > "$WORK/busybox2.out"
count=$(grep -c '^# BEGIN EDGEDESK' "$STAGE/etc/inittab" || true)
[ "$count" -eq 1 ] || fail "expected one EDGEDESK block after --force, found $count"
ok "--force replaces the managed inittab block"

# ---------------------------------------------------------------------------
# Release layout: install.sh next to bin/, binaries already in place
# ---------------------------------------------------------------------------

mkdir -p "$WORK/pkg/bin"
cp "$INSTALL" "$WORK/pkg/install.sh"
cp "$WORK/bin/edgedesk" "$WORK/pkg/bin/edgedesk"
cp "$WORK/bin/edgedesk-supervisor" "$WORK/pkg/bin/edgedesk-supervisor"

sh "$WORK/pkg/install.sh" --dry-run --init systemd --yes \
    --prefix "$WORK/pkg" --config-dir "$WORK/pkg/etc" \
    --data-dir "$WORK/pkg/data" --state-dir "$WORK/pkg/run" \
    --apps-root "$WORK/pkg/apps" --file-root "$WORK/srv" > "$WORK/pkg.out"

assert_contains "$WORK/pkg.out" "binary already at target: $WORK/pkg/bin/edgedesk"
if grep -Fq "install $WORK/pkg/bin/edgedesk ->" "$WORK/pkg.out"; then
    fail "pre-placed binary must not be copied over itself"
fi
ok "script+bin layout resolves without --source and avoids self-overwrite"

# ---------------------------------------------------------------------------
# Overwrite protection and --no-supervisor
# ---------------------------------------------------------------------------

expect_fail "reinstall without --force refuses to overwrite" \
    "$INSTALL" --destdir "$WORK/stage-systemd" --init systemd \
    --source "$WORK/bin" --file-root "$WORK/srv" --no-start --no-enable

"$INSTALL" --destdir "$WORK/stage-nosup" --init systemd --no-supervisor \
    --source "$WORK/bin" --file-root "$WORK/srv" --no-start --no-enable > "$WORK/nosup.out"
assert_no_file "$WORK/stage-nosup/etc/systemd/system/edgedesk-supervisor.service"
assert_no_file "$WORK/stage-nosup/opt/edgedesk/bin/edgedesk-supervisor"
assert_contains "$WORK/stage-nosup/etc/edgedesk/edgedesk.conf" "no-supervisor = true"
ok "--no-supervisor installs only the web backend"

# ---------------------------------------------------------------------------
# Validation
# ---------------------------------------------------------------------------

expect_fail "port above 65535 is rejected" \
    "$INSTALL" --dry-run --init systemd --source "$WORK/bin" \
    --file-root "$WORK/srv" --port 70000

expect_fail "zero-padded port is rejected" \
    "$INSTALL" --dry-run --init systemd --source "$WORK/bin" \
    --file-root "$WORK/srv" --port 08080

expect_fail "host with whitespace is rejected" \
    "$INSTALL" --dry-run --init systemd --source "$WORK/bin" \
    --file-root "$WORK/srv" --host "bad host"

expect_fail "path with spaces is rejected" \
    "$INSTALL" --dry-run --init systemd --source "$WORK/bin" \
    --file-root "$WORK/srv" --data-dir "/var/lib/edge desk"

expect_fail "non-loopback host is refused by default" \
    "$INSTALL" --dry-run --init systemd --source "$WORK/bin" \
    --file-root "$WORK/srv" --host 0.0.0.0 --yes

expect_fail "--purge refuses a broad prefix" \
    "$INSTALL" --uninstall --purge --init systemd --destdir "$WORK/stage-systemd" \
    --prefix /opt

"$INSTALL" --destdir "$WORK/stage-host" --init systemd --source "$WORK/bin" \
    --file-root "$WORK/srv" --host 0.0.0.0 --allow-non-loopback \
    --no-start --no-enable > "$WORK/host.out"
assert_contains "$WORK/stage-host/etc/edgedesk/edgedesk.conf" "host = 0.0.0.0"
ok "explicit --allow-non-loopback is required for a public bind"

# ---------------------------------------------------------------------------
# Dry run writes nothing
# ---------------------------------------------------------------------------

mkdir -p "$WORK/dry-marker"
"$INSTALL" --dry-run --init systemd --source "$WORK/bin" \
    --file-root "$WORK/srv" --prefix "$WORK/dry-marker/prefix" \
    --data-dir "$WORK/dry-marker/data" --state-dir "$WORK/dry-marker/run" \
    --apps-root "$WORK/dry-marker/apps" --yes > "$WORK/dry.out"
[ -z "$(ls -A "$WORK/dry-marker")" ] || fail "dry-run created files"
assert_contains "$WORK/dry.out" "ExecStart="
assert_contains "$WORK/dry.out" "port = 8080"
ok "dry-run prints the plan and writes nothing"

# ---------------------------------------------------------------------------
# Uninstall
# ---------------------------------------------------------------------------

"$INSTALL" --uninstall --init systemd --destdir "$WORK/stage-systemd" \
    --no-start --no-enable > "$WORK/uninstall.out"
STAGE="$WORK/stage-systemd"
assert_no_file "$STAGE/opt/edgedesk/bin/edgedesk"
assert_no_file "$STAGE/etc/systemd/system/edgedesk.service"
assert_no_file "$STAGE/etc/edgedesk/edgedesk.conf"
[ -d "$STAGE/var/lib/edgedesk" ] || fail "uninstall must preserve persistent data"
ok "uninstall removes binaries/units/config and preserves data"

"$INSTALL" --destdir "$WORK/stage-purge" --init systemd --source "$WORK/bin" \
    --file-root "$WORK/srv" --no-start --no-enable > "$WORK/purge-install.out"
"$INSTALL" --uninstall --purge --init systemd --destdir "$WORK/stage-purge" \
    > "$WORK/uninstall-purge.out"
assert_no_file "$WORK/stage-purge/var/lib/edgedesk"
assert_no_file "$WORK/stage-purge/var/lib/edgedesk-supervisor"
assert_no_file "$WORK/stage-purge/etc/edgedesk/edgedesk.conf"
ok "--purge removes data and supervisor state"

printf 'PASS: %d checks\n' "$PASS"
