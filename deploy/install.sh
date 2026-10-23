#!/bin/sh
# EdgeDesk installer for Linux.
#
# POSIX sh on purpose: this script must run under dash, bash and BusyBox ash,
# including targets where no C++ toolchain or package manager exists. It
# detects the available service manager, writes a key=value configuration file,
# installs the web backend and (unless --no-supervisor) the standalone
# process supervisor, and registers both with the detected init system.
#
# Modes:
#   deploy/install.sh                 install (interactive when stdin is a TTY)
#   deploy/install.sh --detect        print detected environment and exit
#   deploy/install.sh --dry-run       print the full plan and generated files
#   deploy/install.sh --uninstall     remove init registration and binaries
#
# Run as root for a real install. --dry-run, --detect and --destdir do not
# require root and never touch the running system.
set -eu

SCRIPT_DIR=$(CDPATH= cd -P "$(dirname "$0")" && pwd)

PROG_NAME=edgedesk
MODE=install
INIT=auto
HOST=127.0.0.1
PORT=8080
PREFIX=/opt/edgedesk
BIN_DIR=
CONFIG_DIR=/etc/edgedesk
DATA_DIR=/var/lib/edgedesk
STATE_DIR=/run/edgedesk
APPS_ROOT=/var/lib/edgedesk/apps
SUPERVISOR_DB=/var/lib/edgedesk-supervisor/supervisor.db
SUPERVISOR_LOG_DIR=/var/lib/edgedesk-supervisor/logs
SUPERVISOR_SOCKET=/run/edgedesk-supervisor.sock
FILE_ROOT=/srv
SERVICE_USER=edgedesk
SERVICE_GROUP=edgedesk
SOURCE_DIR=
WEB_ROOT_SRC=
DESTDIR=
WITH_SUPERVISOR=1
ENABLE_SERVICE=1
START_SERVICE=1
ASSUME_YES=0
FORCE=0
PURGE=0
DRY_RUN=0
ALLOW_NON_LOOPBACK=0
BIN_DIR_EXPLICIT=0
APPS_ROOT_EXPLICIT=0
STATE_DIR_EXPLICIT=0
SUPERVISOR_DIRS_EXPLICIT=0
SERVICE_GROUP_EXPLICIT=0
CONFIG_FILE=
INIT_DETECTED=

say() {
    printf '%s\n' "$*"
}

warn() {
    printf 'warning: %s\n' "$*" >&2
}

die() {
    printf 'error: %s\n' "$*" >&2
    exit 2
}

usage() {
    cat <<'EOF'
EdgeDesk installer

Usage:
  deploy/install.sh [options]
  deploy/install.sh --detect
  deploy/install.sh --dry-run [options]
  deploy/install.sh --uninstall [options]

General:
  --detect                 Print init/user/binary detection and exit
  --dry-run                Print every action and generated file; write nothing
  --uninstall              Stop and remove the services, keep persistent data
  --purge                  With --uninstall: also remove data/apps/web dirs
  --force                  Overwrite existing files (a .bak copy is kept)
  --yes                    Never prompt; use flags/defaults
  --init NAME              systemd | openrc | busybox | none | auto (default auto)
  --destdir DIR            Stage all files under DIR for packaging/tests;
                           does not create users or register services
  --no-start               Do not start the services now
  --no-enable              Do not enable the services at boot
  --no-supervisor          Do not install/register edgedesk-supervisor

Listen and paths:
  --host ADDR              Listen address (default 127.0.0.1; non-loopback
                           requires --allow-non-loopback)
  --port PORT              Listen port (default 8080)
  --allow-non-loopback     Accept a non-loopback --host; put TLS/VPN in front
  --prefix DIR             Install prefix (default /opt/edgedesk)
  --bin-dir DIR            Binary directory (default PREFIX/bin)
  --config-dir DIR         Configuration directory (default /etc/edgedesk)
  --data-dir DIR           Persistent data directory (default /var/lib/edgedesk)
  --state-dir DIR          Runtime state directory (default /run/edgedesk)
  --apps-root DIR          Extension apps root (default DATA_DIR/apps)
  --file-root DIR          File API sandbox root (default /srv; must exist)
  --supervisor-db PATH     Supervisor SQLite path
  --supervisor-log-dir DIR Supervisor log directory
  --supervisor-socket PATH Supervisor Unix socket path
  --service-user NAME      Account for the web backend (default edgedesk)
  --service-group NAME     Group allowed to use the supervisor socket
  --source DIR             Directory containing edgedesk[-supervisor]
  --web-root DIR           Frontend directory for disk builds (copied to
                           PREFIX/web); omit for #embed builds

Examples:
  sudo deploy/install.sh --host 127.0.0.1 --port 8080
  sudo deploy/install.sh --init openrc --file-root /srv/edgedesk-files
  sudo deploy/install.sh --uninstall
  deploy/install.sh --dry-run --destdir /tmp/edgedesk-stage
EOF
}

need_value() {
    if [ "$#" -lt 2 ]; then
        die "option $1 requires a value"
    fi
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --detect) MODE=detect; shift ;;
        --dry-run) DRY_RUN=1; shift ;;
        --uninstall) MODE=uninstall; shift ;;
        --purge) PURGE=1; shift ;;
        --force) FORCE=1; shift ;;
        --yes|-y) ASSUME_YES=1; shift ;;
        --no-start) START_SERVICE=0; shift ;;
        --no-enable) ENABLE_SERVICE=0; shift ;;
        --no-supervisor) WITH_SUPERVISOR=0; shift ;;
        --allow-non-loopback) ALLOW_NON_LOOPBACK=1; shift ;;
        --host) need_value "$@"; HOST=$2; shift 2 ;;
        --host=*) HOST=${1#*=}; shift ;;
        --port) need_value "$@"; PORT=$2; shift 2 ;;
        --port=*) PORT=${1#*=}; shift ;;
        --init) need_value "$@"; INIT=$2; shift 2 ;;
        --init=*) INIT=${1#*=}; shift ;;
        --prefix) need_value "$@"; PREFIX=$2; shift 2 ;;
        --prefix=*) PREFIX=${1#*=}; shift ;;
        --bin-dir) need_value "$@"; BIN_DIR=$2; BIN_DIR_EXPLICIT=1; shift 2 ;;
        --bin-dir=*) BIN_DIR=${1#*=}; BIN_DIR_EXPLICIT=1; shift ;;
        --config-dir) need_value "$@"; CONFIG_DIR=$2; shift 2 ;;
        --config-dir=*) CONFIG_DIR=${1#*=}; shift ;;
        --data-dir) need_value "$@"; DATA_DIR=$2; shift 2 ;;
        --data-dir=*) DATA_DIR=${1#*=}; shift ;;
        --state-dir) need_value "$@"; STATE_DIR=$2; STATE_DIR_EXPLICIT=1; shift 2 ;;
        --state-dir=*) STATE_DIR=${1#*=}; STATE_DIR_EXPLICIT=1; shift ;;
        --apps-root) need_value "$@"; APPS_ROOT=$2; APPS_ROOT_EXPLICIT=1; shift 2 ;;
        --apps-root=*) APPS_ROOT=${1#*=}; APPS_ROOT_EXPLICIT=1; shift ;;
        --file-root) need_value "$@"; FILE_ROOT=$2; shift 2 ;;
        --file-root=*) FILE_ROOT=${1#*=}; shift ;;
        --supervisor-db) need_value "$@"; SUPERVISOR_DB=$2; SUPERVISOR_DIRS_EXPLICIT=1; shift 2 ;;
        --supervisor-db=*) SUPERVISOR_DB=${1#*=}; SUPERVISOR_DIRS_EXPLICIT=1; shift ;;
        --supervisor-log-dir) need_value "$@"; SUPERVISOR_LOG_DIR=$2; SUPERVISOR_DIRS_EXPLICIT=1; shift 2 ;;
        --supervisor-log-dir=*) SUPERVISOR_LOG_DIR=${1#*=}; SUPERVISOR_DIRS_EXPLICIT=1; shift ;;
        --supervisor-socket) need_value "$@"; SUPERVISOR_SOCKET=$2; shift 2 ;;
        --supervisor-socket=*) SUPERVISOR_SOCKET=${1#*=}; shift ;;
        --service-user) need_value "$@"; SERVICE_USER=$2; shift 2 ;;
        --service-user=*) SERVICE_USER=${1#*=}; shift ;;
        --service-group) need_value "$@"; SERVICE_GROUP=$2; SERVICE_GROUP_EXPLICIT=1; shift 2 ;;
        --service-group=*) SERVICE_GROUP=${1#*=}; SERVICE_GROUP_EXPLICIT=1; shift ;;
        --source) need_value "$@"; SOURCE_DIR=$2; shift 2 ;;
        --source=*) SOURCE_DIR=${1#*=}; shift ;;
        --web-root) need_value "$@"; WEB_ROOT_SRC=$2; shift 2 ;;
        --web-root=*) WEB_ROOT_SRC=${1#*=}; shift ;;
        --destdir) need_value "$@"; DESTDIR=$2; shift 2 ;;
        --destdir=*) DESTDIR=${1#*=}; shift ;;
        --help|-h) usage; exit 0 ;;
        *) die "unknown option: $1 (see --help)" ;;
    esac
done

# Derived paths. Defaults follow the selected data directory so a custom
# --data-dir does not silently keep writing to /var/lib/edgedesk.
if [ "$BIN_DIR_EXPLICIT" -eq 0 ]; then
    BIN_DIR="$PREFIX/bin"
fi
if [ "$APPS_ROOT_EXPLICIT" -eq 0 ]; then
    APPS_ROOT="$DATA_DIR/apps"
fi
if [ "$STATE_DIR_EXPLICIT" -eq 0 ]; then
    STATE_DIR="/run/$SERVICE_USER"
fi
if [ "$SUPERVISOR_DIRS_EXPLICIT" -eq 0 ]; then
    SUPERVISOR_DB="/var/lib/edgedesk-supervisor/supervisor.db"
    SUPERVISOR_LOG_DIR="/var/lib/edgedesk-supervisor/logs"
fi
CONFIG_FILE="$CONFIG_DIR/edgedesk.conf"
SUPERVISOR_DB_DIR=$(dirname "$SUPERVISOR_DB")

if [ -n "$DESTDIR" ]; then
    DESTDIR=${DESTDIR%/}
fi

rooted() {
    printf '%s%s' "$DESTDIR" "$1"
}

pid1_name() {
    if [ -r /proc/1/comm ]; then
        tr -d '\n' < /proc/1/comm
    else
        printf 'unknown'
    fi
}

detect_init() {
    if [ "$INIT" != auto ]; then
        printf '%s' "$INIT"
        return
    fi
    if command -v systemctl >/dev/null 2>&1 && [ -d /run/systemd/system ]; then
        printf 'systemd'
        return
    fi
    # OpenRC can run under a BusyBox or openrc-init PID 1 (Alpine). Check the
    # service-manager commands, not only the PID 1 binary.
    if command -v rc-update >/dev/null 2>&1 && command -v rc-service >/dev/null 2>&1; then
        printf 'openrc'
        return
    fi
    if command -v busybox >/dev/null 2>&1 && [ -f /etc/inittab ]; then
        printf 'busybox'
        return
    fi
    printf 'unknown'
}

print_detect() {
    init_name=$(detect_init)
    printf 'init=%s\n' "$init_name"
    printf 'pid1=%s\n' "$(pid1_name)"
    printf 'uid=%s\n' "$(id -u)"
    if command -v systemctl >/dev/null 2>&1 && [ -d /run/systemd/system ]; then
        printf 'systemd=yes\n'
    else
        printf 'systemd=no\n'
    fi
    if command -v rc-update >/dev/null 2>&1 && command -v rc-service >/dev/null 2>&1; then
        printf 'openrc=yes\n'
    else
        printf 'openrc=no\n'
    fi
    if command -v busybox >/dev/null 2>&1; then
        printf 'busybox=yes\n'
    else
        printf 'busybox=no\n'
    fi
    if [ -f /etc/inittab ]; then
        printf 'inittab=yes\n'
    else
        printf 'inittab=no\n'
    fi
}

is_loopback() {
    case "$1" in
        127.*|::1|localhost) return 0 ;;
    esac
    return 1
}

validate_host() {
    case "$1" in
        '') die "--host must not be empty" ;;
        *[!A-Za-z0-9.:_-]*) die "--host contains unsupported characters: $1" ;;
        .*|*.|*..*|-*) die "invalid --host: $1" ;;
    esac
}

validate_port() {
    case "$1" in
        ''|*[!0-9]*|0*) die "--port must be an integer between 1 and 65535: $1" ;;
    esac
    if [ "$1" -lt 1 ] || [ "$1" -gt 65535 ]; then
        die "--port must be an integer between 1 and 65535: $1"
    fi
}

validate_path() {
    label=$1
    value=$2
    case "$value" in
        /*) ;;
        *) die "$label must be an absolute path: $value" ;;
    esac
    case "$value" in
        *[!A-Za-z0-9/._+@%:-]*) die "$label contains unsupported characters: $value" ;;
        */../*|*/..|*/.) die "$label must not contain '..' components: $value" ;;
        *//*) die "$label must not contain empty path components: $value" ;;
    esac
}

validate_name() {
    label=$1
    value=$2
    case "$value" in
        ''|-*|*[!A-Za-z0-9._-]*) die "$label is not a valid user/group name: $value" ;;
    esac
}

require_root() {
    if [ "$DRY_RUN" -eq 1 ] || [ -n "$DESTDIR" ]; then
        return 0
    fi
    if [ "$(id -u)" -ne 0 ]; then
        die "this operation must run as root (use --dry-run or --destdir to preview)"
    fi
}

prompt_value() {
    label=$1
    default=$2
    answer=
    printf '%s [%s]: ' "$label" "$default"
    read -r answer || true
    if [ -n "$answer" ]; then
        printf '%s' "$answer"
    else
        printf '%s' "$default"
    fi
}

interactive_confirm() {
    question=$1
    if [ "$ASSUME_YES" -eq 1 ] || [ ! -t 0 ]; then
        return 1
    fi
    answer=
    printf '%s [y/N]: ' "$question" >&2
    read -r answer || true
    case "$answer" in
        y|Y|yes|YES) return 0 ;;
    esac
    return 1
}

resolve_source() {
    if [ -n "$SOURCE_DIR" ]; then
        return
    fi
    for candidate in "$SCRIPT_DIR" "$SCRIPT_DIR/bin" "$SCRIPT_DIR/../bin" \
        "$SCRIPT_DIR/../out/build/release-embed/bin" \
        "$SCRIPT_DIR/../out/build/dev-disk/bin" \
        "$SCRIPT_DIR/../out/build/host-embed/bin"; do
        if [ -x "$candidate/edgedesk" ]; then
            SOURCE_DIR=$candidate
            return
        fi
    done
    die "cannot find the edgedesk binary; build it or pass --source DIR"
}

preflight_target() {
    target=$(rooted "$1")
    if [ -e "$target" ] && [ "$FORCE" -eq 0 ]; then
        die "refusing to overwrite $target; re-run with --force"
    fi
}

# Uploaded release packages may already contain the binaries at BIN_DIR.
# That is not an overwrite: install_binary() adopts them in place.
preflight_binary() {
    if [ "$1" = "$(rooted "$2")" ]; then
        return 0
    fi
    preflight_target "$2"
}

install_binary() {
    src=$1
    dst=$2
    [ -f "$src" ] || die "missing binary: $src"
    [ -x "$src" ] || die "binary is not executable: $src"
    target=$(rooted "$dst")
    # A release package is often uploaded directly into BIN_DIR. Replacing a
    # file with itself is pointless and would otherwise require --force.
    if [ "$src" = "$target" ]; then
        if [ "$DRY_RUN" -eq 1 ]; then
            say "binary already at target: $target (0755 root:root)"
            return
        fi
        chmod 0755 "$target"
        if [ -z "$DESTDIR" ]; then
            chown root:root "$target"
        fi
        say "binary already at target: $target"
        return
    fi
    if [ "$DRY_RUN" -eq 1 ]; then
        say "install $src -> $target (0755 root:root)"
        return
    fi
    mkdir -p "$(dirname "$target")"
    tmp="$target.edgedesk-new.$$"
    cp "$src" "$tmp"
    chmod 0755 "$tmp"
    if [ -z "$DESTDIR" ]; then
        chown root:root "$tmp"
    fi
    mv "$tmp" "$target"
}

ensure_dir() {
    target=$(rooted "$1")
    mode=$2
    owner=$3
    group=$4
    if [ "$DRY_RUN" -eq 1 ]; then
        say "create directory $target (mode $mode, $owner:$group)"
        return
    fi
    mkdir -p "$target"
    chmod "$mode" "$target"
    if [ -z "$DESTDIR" ]; then
        chown "$owner:$group" "$target"
    fi
}

# write_file TARGET MODE OWNER GROUP < content
write_file() {
    target=$(rooted "$1")
    mode=$2
    owner=$3
    group=$4
    if [ "$DRY_RUN" -eq 1 ]; then
        printf '\n--- %s (mode %s, %s:%s) ---\n' "$target" "$mode" "$owner" "$group"
        cat
        printf '\n'
        return
    fi
    mkdir -p "$(dirname "$target")"
    if [ -e "$target" ] && [ "$FORCE" -eq 1 ]; then
        cp "$target" "$target.bak"
    fi
    tmp="$target.edgedesk-new.$$"
    (umask 077; cat > "$tmp")
    chmod "$mode" "$tmp"
    if [ -z "$DESTDIR" ]; then
        chown "$owner:$group" "$tmp"
    fi
    mv "$tmp" "$target"
}

nologin_path() {
    for candidate in /usr/sbin/nologin /sbin/nologin /bin/false; do
        if [ -x "$candidate" ]; then
            printf '%s' "$candidate"
            return
        fi
    done
    printf '/bin/false'
}

group_exists() {
    grep -q "^$SERVICE_GROUP:" /etc/group 2>/dev/null
}

ensure_service_account() {
    if [ "$DRY_RUN" -eq 1 ] || [ -n "$DESTDIR" ]; then
        say "service account: $SERVICE_USER:$SERVICE_GROUP (not created in preview/destdir mode)"
        return
    fi

    if id "$SERVICE_USER" >/dev/null 2>&1; then
        if [ "$SERVICE_GROUP_EXPLICIT" -eq 0 ]; then
            SERVICE_GROUP=$(id -gn "$SERVICE_USER")
        elif ! group_exists; then
            die "service user $SERVICE_USER exists but group $SERVICE_GROUP does not"
        fi
        say "service account already exists: $SERVICE_USER:$SERVICE_GROUP"
        return
    fi

    if ! group_exists; then
        if command -v groupadd >/dev/null 2>&1; then
            groupadd --system "$SERVICE_GROUP"
        elif command -v addgroup >/dev/null 2>&1; then
            addgroup -S "$SERVICE_GROUP"
        else
            die "cannot create group $SERVICE_GROUP: no groupadd/addgroup"
        fi
    fi

    nologin=$(nologin_path)
    if command -v useradd >/dev/null 2>&1; then
        useradd --system --gid "$SERVICE_GROUP" --home-dir "$DATA_DIR" \
            --no-create-home --shell "$nologin" "$SERVICE_USER"
    elif command -v adduser >/dev/null 2>&1; then
        adduser -S -D -H -h "$DATA_DIR" -s "$nologin" -G "$SERVICE_GROUP" "$SERVICE_USER"
    else
        die "cannot create user $SERVICE_USER: no useradd/adduser"
    fi
    say "created service account: $SERVICE_USER:$SERVICE_GROUP"
}

config_body() {
    cat <<EOF
# EdgeDesk configuration. Generated by deploy/install.sh.
# Precedence: defaults < this file < environment < command line.
host = $HOST
port = $PORT
file-root = $FILE_ROOT
data-dir = $DATA_DIR
state-dir = $STATE_DIR
apps-root = $APPS_ROOT
audit-log = $DATA_DIR/audit.log
EOF
    if [ -n "$WEB_ROOT_SRC" ]; then
        printf 'web-root = %s/web\n' "$PREFIX"
    fi
    if [ "$WITH_SUPERVISOR" -eq 1 ]; then
        printf 'supervisor-socket = %s\n' "$SUPERVISOR_SOCKET"
    else
        printf 'no-supervisor = true\n'
    fi
}

systemd_web_unit() {
    cat <<EOF
[Unit]
Description=EdgeDesk web operations panel
After=network.target
StartLimitIntervalSec=60
StartLimitBurst=10

[Service]
Type=simple
User=$SERVICE_USER
Group=$SERVICE_GROUP
UMask=0077
ExecStart=$BIN_DIR/edgedesk --config $CONFIG_FILE
Restart=on-failure
RestartSec=2
NoNewPrivileges=yes
PrivateTmp=yes
ProtectHome=yes
ProtectSystem=strict
ReadWritePaths=$FILE_ROOT $DATA_DIR $STATE_DIR $APPS_ROOT
EOF
    if [ "$DATA_DIR" = "/var/lib/$SERVICE_USER" ]; then
        printf 'StateDirectory=%s\n' "$SERVICE_USER"
        printf 'StateDirectoryMode=0700\n'
    fi
    if [ "$STATE_DIR" = "/run/$SERVICE_USER" ]; then
        printf 'RuntimeDirectory=%s\n' "$SERVICE_USER"
        printf 'RuntimeDirectoryMode=0700\n'
    else
        printf 'ExecStartPre=+/bin/mkdir -p -m 0700 %s\n' "$STATE_DIR"
        printf 'ExecStartPre=+/bin/chown %s:%s %s\n' "$SERVICE_USER" "$SERVICE_GROUP" "$STATE_DIR"
    fi
    cat <<EOF

[Install]
WantedBy=multi-user.target
EOF
}

systemd_supervisor_unit() {
    cat <<EOF
[Unit]
Description=EdgeDesk process supervisor
After=network.target
StartLimitIntervalSec=60
StartLimitBurst=10

[Service]
Type=simple
UMask=0077
ExecStart=$BIN_DIR/edgedesk-supervisor \\
  --socket $SUPERVISOR_SOCKET \\
  --database $SUPERVISOR_DB \\
  --log-dir $SUPERVISOR_LOG_DIR \\
  --socket-group $SERVICE_GROUP \\
  --allow-gid $SERVICE_GROUP

# Managed services form their own process groups. Signal only the supervisor
# first so it can forward SIGTERM, wait for stop_timeout_ms and escalate.
KillMode=process
TimeoutStopSec=90
Restart=always
RestartSec=2

# The supervisor runs as root to switch service identities. Do not add
# NoNewPrivileges=yes here.
NoNewPrivileges=no
PrivateTmp=yes
ProtectHome=yes
ProtectSystem=full
ReadWritePaths=$SUPERVISOR_DB_DIR $SUPERVISOR_LOG_DIR
EOF
    if [ "$SUPERVISOR_DB_DIR" = "/var/lib/edgedesk-supervisor" ]; then
        printf 'StateDirectory=edgedesk-supervisor\n'
        printf 'StateDirectoryMode=0700\n'
    else
        printf 'ExecStartPre=+/bin/mkdir -p -m 0700 %s %s\n' "$SUPERVISOR_DB_DIR" "$SUPERVISOR_LOG_DIR"
        printf 'ExecStartPre=+/bin/chown root:root %s %s\n' "$SUPERVISOR_DB_DIR" "$SUPERVISOR_LOG_DIR"
    fi
    cat <<EOF

[Install]
WantedBy=multi-user.target
EOF
}

openrc_web_script() {
    cat <<EOF
#!/sbin/openrc-run
# Generated by deploy/install.sh. Requires OpenRC with supervise-daemon.
name="edgedesk"
description="EdgeDesk web operations panel"

: "\${EDGEDESK_BIN:=$BIN_DIR/edgedesk}"
: "\${EDGEDESK_CONFIG:=$CONFIG_FILE}"
: "\${EDGEDESK_USER:=$SERVICE_USER}"
: "\${EDGEDESK_GROUP:=$SERVICE_GROUP}"

command="\${EDGEDESK_BIN}"
command_args="--config \${EDGEDESK_CONFIG}"
command_user="\${EDGEDESK_USER}:\${EDGEDESK_GROUP}"
supervisor="supervise-daemon"
respawn_delay=2
respawn_max=0
pidfile="$STATE_DIR/edgedesk.pid"

depend() {
    need localmount
    after bootmisc
    use net
}

start_pre() {
    checkpath --directory --mode 0700 --owner "\${EDGEDESK_USER}:\${EDGEDESK_GROUP}" $DATA_DIR
    checkpath --directory --mode 0700 --owner "\${EDGEDESK_USER}:\${EDGEDESK_GROUP}" $APPS_ROOT
    checkpath --directory --mode 0700 --owner "\${EDGEDESK_USER}:\${EDGEDESK_GROUP}" $STATE_DIR
}
EOF
}

openrc_supervisor_script() {
    cat <<EOF
#!/sbin/openrc-run
# Generated by deploy/install.sh. Requires OpenRC with supervise-daemon.
name="edgedesk-supervisor"
description="EdgeDesk process supervisor"

: "\${EDGEDESK_SUPERVISOR_BIN:=$BIN_DIR/edgedesk-supervisor}"
: "\${EDGEDESK_SUPERVISOR_SOCKET:=$SUPERVISOR_SOCKET}"
: "\${EDGEDESK_SUPERVISOR_DB:=$SUPERVISOR_DB}"
: "\${EDGEDESK_SUPERVISOR_LOG_DIR:=$SUPERVISOR_LOG_DIR}"
: "\${EDGEDESK_SUPERVISOR_GROUP:=$SERVICE_GROUP}"

command="\${EDGEDESK_SUPERVISOR_BIN}"
command_args="--socket \${EDGEDESK_SUPERVISOR_SOCKET} \\
  --database \${EDGEDESK_SUPERVISOR_DB} \\
  --log-dir \${EDGEDESK_SUPERVISOR_LOG_DIR} \\
  --socket-group \${EDGEDESK_SUPERVISOR_GROUP} \\
  --allow-gid \${EDGEDESK_SUPERVISOR_GROUP}"
command_user="root:root"
supervisor="supervise-daemon"
respawn_delay=2
respawn_max=0
pidfile="/run/edgedesk-supervisor.pid"

depend() {
    need localmount
    after bootmisc
    use net
}

start_pre() {
    checkpath --directory --mode 0700 $SUPERVISOR_DB_DIR
    checkpath --directory --mode 0700 $SUPERVISOR_LOG_DIR
    checkpath --directory --mode 0755 /run
}
EOF
}

busybox_web_wrapper() {
    cat <<EOF
#!/bin/sh
# Generated by deploy/install.sh: BusyBox init wrapper for the web backend.
set -eu
umask 0077

mkdir -p $DATA_DIR $APPS_ROOT $STATE_DIR
chmod 0700 $DATA_DIR $APPS_ROOT $STATE_DIR
if [ "\$(id -u)" -eq 0 ]; then
    chown $SERVICE_USER:$SERVICE_GROUP $DATA_DIR $APPS_ROOT $STATE_DIR
fi

if [ "\$(id -u)" -eq 0 ] && command -v setpriv >/dev/null 2>&1; then
    exec setpriv --reuid=$SERVICE_USER --regid=$SERVICE_GROUP --init-groups \\
        $BIN_DIR/edgedesk --config $CONFIG_FILE
fi
if [ "\$(id -u)" -eq 0 ] && command -v su >/dev/null 2>&1; then
    exec su -s /bin/sh -c 'exec $BIN_DIR/edgedesk --config $CONFIG_FILE' $SERVICE_USER
fi
echo "edgedesk-boot: no setpriv/su available; running $BIN_DIR/edgedesk as \$(id -un)" >&2
exec $BIN_DIR/edgedesk --config $CONFIG_FILE
EOF
}

busybox_supervisor_wrapper() {
    cat <<EOF
#!/bin/sh
# Generated by deploy/install.sh: BusyBox init wrapper for the supervisor.
set -eu
umask 0077

mkdir -p $SUPERVISOR_DB_DIR $SUPERVISOR_LOG_DIR
chmod 0700 $SUPERVISOR_DB_DIR $SUPERVISOR_LOG_DIR
if [ "\$(id -u)" -eq 0 ]; then
    chown root:root $SUPERVISOR_DB_DIR $SUPERVISOR_LOG_DIR
fi

exec $BIN_DIR/edgedesk-supervisor \\
    --socket $SUPERVISOR_SOCKET \\
    --database $SUPERVISOR_DB \\
    --log-dir $SUPERVISOR_LOG_DIR \\
    --socket-group $SERVICE_GROUP \\
    --allow-gid $SERVICE_GROUP
EOF
}

inittab_block() {
    printf '# BEGIN EDGEDESK (managed by deploy/install.sh)\n'
    if [ "$WITH_SUPERVISOR" -eq 1 ]; then
        printf '::respawn:%s/edgedesk-supervisor-boot\n' "$BIN_DIR"
    fi
    printf '::respawn:%s/edgedesk-boot\n' "$BIN_DIR"
    printf '# END EDGEDESK\n'
}

preflight_install() {
    preflight_target "$CONFIG_FILE"
    preflight_binary "$SOURCE_DIR/edgedesk" "$BIN_DIR/edgedesk"
    current_init=$(detect_init)
    case "$current_init" in
        systemd)
            preflight_target /etc/systemd/system/edgedesk.service
            if [ "$WITH_SUPERVISOR" -eq 1 ]; then
                preflight_target /etc/systemd/system/edgedesk-supervisor.service
            fi
            ;;
        openrc)
            preflight_target /etc/init.d/edgedesk
            if [ "$WITH_SUPERVISOR" -eq 1 ]; then
                preflight_target /etc/init.d/edgedesk-supervisor
            fi
            ;;
        busybox)
            preflight_target "$BIN_DIR/edgedesk-boot"
            if [ "$WITH_SUPERVISOR" -eq 1 ]; then
                preflight_target "$BIN_DIR/edgedesk-supervisor-boot"
            fi
            ;;
        none) ;;
        *)
            die "unsupported init system '$current_init'; pass --init systemd|openrc|busybox|none"
            ;;
    esac
    if [ "$WITH_SUPERVISOR" -eq 1 ]; then
        preflight_binary "$SOURCE_DIR/edgedesk-supervisor" "$BIN_DIR/edgedesk-supervisor"
    fi
}

install_files() {
    ensure_dir "$PREFIX" 0755 root root
    ensure_dir "$BIN_DIR" 0755 root root
    ensure_dir "$CONFIG_DIR" 0750 root "$SERVICE_GROUP"
    ensure_dir "$DATA_DIR" 0700 "$SERVICE_USER" "$SERVICE_GROUP"
    ensure_dir "$APPS_ROOT" 0700 "$SERVICE_USER" "$SERVICE_GROUP"
    ensure_dir "$STATE_DIR" 0700 "$SERVICE_USER" "$SERVICE_GROUP"
    if [ "$WITH_SUPERVISOR" -eq 1 ]; then
        ensure_dir "$SUPERVISOR_DB_DIR" 0700 root root
        ensure_dir "$SUPERVISOR_LOG_DIR" 0700 root root
    fi

    install_binary "$SOURCE_DIR/edgedesk" "$BIN_DIR/edgedesk"
    if [ "$WITH_SUPERVISOR" -eq 1 ]; then
        install_binary "$SOURCE_DIR/edgedesk-supervisor" "$BIN_DIR/edgedesk-supervisor"
    fi

    if [ -n "$WEB_ROOT_SRC" ]; then
        [ -f "$WEB_ROOT_SRC/index.html" ] || die "web root has no index.html: $WEB_ROOT_SRC"
        target=$(rooted "$PREFIX/web")
        if [ "$DRY_RUN" -eq 1 ]; then
            say "copy $WEB_ROOT_SRC -> $target (0755 root:root)"
        else
            mkdir -p "$target"
            (umask 022; cp -R "$WEB_ROOT_SRC/." "$target/")
            chmod 0755 "$target"
            if [ -z "$DESTDIR" ]; then
                chown -R root:root "$target"
            fi
        fi
    fi

    config_body | write_file "$CONFIG_FILE" 0640 root "$SERVICE_GROUP"

    case "$INIT" in
        systemd)
            systemd_web_unit | write_file /etc/systemd/system/edgedesk.service 0644 root root
            if [ "$WITH_SUPERVISOR" -eq 1 ]; then
                systemd_supervisor_unit | write_file /etc/systemd/system/edgedesk-supervisor.service 0644 root root
            fi
            ;;
        openrc)
            openrc_web_script | write_file /etc/init.d/edgedesk 0755 root root
            if [ "$WITH_SUPERVISOR" -eq 1 ]; then
                openrc_supervisor_script | write_file /etc/init.d/edgedesk-supervisor 0755 root root
            fi
            ;;
        busybox)
            busybox_web_wrapper | write_file "$BIN_DIR/edgedesk-boot" 0755 root root
            if [ "$WITH_SUPERVISOR" -eq 1 ]; then
                busybox_supervisor_wrapper | write_file "$BIN_DIR/edgedesk-supervisor-boot" 0755 root root
            fi
            install_busybox_inittab
            ;;
        none)
            say "init registration skipped (--init none)"
            ;;
    esac
}

install_busybox_inittab() {
    inittab=$(rooted /etc/inittab)
    if [ "$DRY_RUN" -eq 1 ]; then
        printf '\n--- %s (append) ---\n' "$inittab"
        inittab_block
        printf '\n'
        return
    fi
    if [ ! -f "$inittab" ]; then
        mkdir -p "$(dirname "$inittab")"
        say "no $inittab; created"
        : > "$inittab"
    elif grep -q '^# BEGIN EDGEDESK' "$inittab" && [ "$FORCE" -eq 0 ]; then
        die "$inittab already contains an EDGEDESK block; re-run with --force"
    fi
    tmp="$inittab.edgedesk-new.$$"
    awk '
        /^# BEGIN EDGEDESK/ { skip = 1; next }
        /^# END EDGEDESK/ { skip = 0; next }
        !skip { print }
    ' "$inittab" > "$tmp"
    inittab_block >> "$tmp"
    cat "$tmp" > "$inittab"
    rm -f "$tmp"
    say "updated $inittab"
}

apply_init() {
    if [ -n "$DESTDIR" ]; then
        say "files staged under $DESTDIR; user creation and init registration were skipped"
        return
    fi
    if [ "$DRY_RUN" -eq 1 ]; then
        return
    fi
    case "$INIT" in
        systemd)
            systemctl daemon-reload
            if [ "$ENABLE_SERVICE" -eq 1 ]; then
                if [ "$WITH_SUPERVISOR" -eq 1 ]; then
                    systemctl enable edgedesk-supervisor.service
                fi
                systemctl enable edgedesk.service
            fi
            if [ "$START_SERVICE" -eq 1 ]; then
                if [ "$WITH_SUPERVISOR" -eq 1 ]; then
                    systemctl restart edgedesk-supervisor.service
                fi
                systemctl restart edgedesk.service
            fi
            ;;
        openrc)
            if [ "$ENABLE_SERVICE" -eq 1 ]; then
                if [ "$WITH_SUPERVISOR" -eq 1 ]; then
                    rc-update add edgedesk-supervisor default
                fi
                rc-update add edgedesk default
            fi
            if [ "$START_SERVICE" -eq 1 ]; then
                if [ "$WITH_SUPERVISOR" -eq 1 ]; then
                    rc-service edgedesk-supervisor restart || rc-service edgedesk-supervisor start
                fi
                rc-service edgedesk restart || rc-service edgedesk start
            fi
            ;;
        busybox)
            if [ "$START_SERVICE" -eq 1 ]; then
                if [ "$(pid1_name)" = init ]; then
                    kill -HUP 1
                    say "sent SIGHUP to init to reload /etc/inittab"
                else
                    warn "PID 1 is not BusyBox init; reload /etc/inittab manually"
                fi
            else
                say "inittab installed; run 'kill -HUP 1' when you want to start"
            fi
            ;;
        none) ;;
    esac
}

print_next_steps() {
    if [ "$DRY_RUN" -eq 1 ] || [ -n "$DESTDIR" ]; then
        return
    fi
    scheme=http
    say ""
    say "EdgeDesk installed."
    say "  Web UI:      $scheme://$HOST:$PORT/ (set the administrator password on first login)"
    say "  Config:      $CONFIG_FILE"
    say "  Data:        $DATA_DIR"
    if [ "$WITH_SUPERVISOR" -eq 1 ]; then
        say "  Supervisor:  $SUPERVISOR_SOCKET"
    fi
    case "$INIT" in
        systemd)
            say "  Status:      systemctl status edgedesk.service"
            ;;
        openrc)
            say "  Status:      rc-service edgedesk status"
            ;;
        busybox)
            say "  Logs:        console / configured logger for the boot wrapper"
            ;;
    esac
}

do_install() {
    validate_host "$HOST"
    validate_port "$PORT"
    validate_name "--service-user" "$SERVICE_USER"
    validate_name "--service-group" "$SERVICE_GROUP"
    validate_path "--prefix" "$PREFIX"
    validate_path "--bin-dir" "$BIN_DIR"
    validate_path "--config-dir" "$CONFIG_DIR"
    validate_path "--data-dir" "$DATA_DIR"
    validate_path "--state-dir" "$STATE_DIR"
    validate_path "--apps-root" "$APPS_ROOT"
    validate_path "--file-root" "$FILE_ROOT"
    validate_path "--supervisor-db" "$SUPERVISOR_DB"
    validate_path "--supervisor-log-dir" "$SUPERVISOR_LOG_DIR"
    validate_path "--supervisor-socket" "$SUPERVISOR_SOCKET"
    if [ -n "$WEB_ROOT_SRC" ]; then
        validate_path "--web-root" "$WEB_ROOT_SRC"
    fi
    if [ -n "$DESTDIR" ]; then
        validate_path "--destdir" "$DESTDIR"
    fi

    if [ "$WITH_SUPERVISOR" -eq 1 ]; then
        if [ "${#SUPERVISOR_SOCKET}" -ge 108 ]; then
            die "--supervisor-socket must be shorter than 108 bytes"
        fi
    fi

    if ! is_loopback "$HOST"; then
        warn "host $HOST is not a loopback address."
        warn "Remote access requires TLS or a trusted VPN/gateway in front of EdgeDesk"
        warn "and --cookie-secure when HTTPS terminates at the backend."
        if [ "$ALLOW_NON_LOOPBACK" -eq 0 ]; then
            if ! interactive_confirm "Continue with non-loopback host $HOST?"; then
                die "refusing non-loopback --host without --allow-non-loopback"
            fi
        fi
    fi

    if [ "$FILE_ROOT" = / ]; then
        warn "--file-root / exposes the whole filesystem through the file API"
    fi
    if [ ! -d "$FILE_ROOT" ] && [ "$DRY_RUN" -eq 0 ]; then
        die "--file-root directory does not exist: $FILE_ROOT"
    fi

    resolve_source
    if [ ! -x "$SOURCE_DIR/edgedesk" ]; then
        die "missing executable: $SOURCE_DIR/edgedesk"
    fi
    if [ "$WITH_SUPERVISOR" -eq 1 ] && [ ! -x "$SOURCE_DIR/edgedesk-supervisor" ]; then
        die "missing executable: $SOURCE_DIR/edgedesk-supervisor (or use --no-supervisor)"
    fi

    INIT=$(detect_init)
    case "$INIT" in
        systemd|openrc|busybox|none) ;;
        unknown)
            die "cannot detect a supported init system; pass --init systemd|openrc|busybox|none"
            ;;
        *)
            die "unsupported --init value: $INIT"
            ;;
    esac

    require_root
    preflight_install

    say "EdgeDesk install plan"
    say "  init:        $INIT (PID 1: $(pid1_name))"
    say "  listen:      $HOST:$PORT"
    say "  prefix:      $PREFIX"
    say "  config:      $CONFIG_FILE"
    say "  data:        $DATA_DIR"
    say "  apps:        $APPS_ROOT"
    say "  file root:   $FILE_ROOT"
    say "  user/group:  $SERVICE_USER:$SERVICE_GROUP"
    say "  supervisor:  $([ "$WITH_SUPERVISOR" -eq 1 ] && printf 'yes' || printf 'no')"
    say ""

    ensure_service_account
    install_files
    apply_init
    print_next_steps
}

remove_path() {
    target=$(rooted "$1")
    if [ "$DRY_RUN" -eq 1 ]; then
        say "remove $target"
        return
    fi
    rm -f "$target"
}

remove_tree() {
    target=$(rooted "$1")
    if [ "$DRY_RUN" -eq 1 ]; then
        say "remove directory tree $target"
        return
    fi
    if [ -d "$target" ]; then
        rm -rf "$target"
    fi
}

remove_busybox_inittab() {
    inittab=$(rooted /etc/inittab)
    if [ ! -f "$inittab" ]; then
        return
    fi
    tmp="$inittab.edgedesk-new.$$"
    awk '
        /^# BEGIN EDGEDESK/ { skip = 1; next }
        /^# END EDGEDESK/ { skip = 0; next }
        !skip { print }
    ' "$inittab" > "$tmp"
    if [ "$DRY_RUN" -eq 1 ]; then
        say "remove EDGEDESK block from $inittab"
        rm -f "$tmp"
        return
    fi
    cat "$tmp" > "$inittab"
    rm -f "$tmp"
    if [ "$(pid1_name)" = init ]; then
        kill -HUP 1
    fi
}

do_uninstall() {
    validate_path "--prefix" "$PREFIX"
    validate_path "--bin-dir" "$BIN_DIR"
    validate_path "--config-dir" "$CONFIG_DIR"
    validate_path "--data-dir" "$DATA_DIR"
    validate_path "--apps-root" "$APPS_ROOT"
    validate_path "--supervisor-db" "$SUPERVISOR_DB"
    validate_path "--supervisor-log-dir" "$SUPERVISOR_LOG_DIR"
    validate_name "--service-user" "$SERVICE_USER"
    validate_name "--service-group" "$SERVICE_GROUP"

    # Validate --purge before removing anything: a broad prefix must fail
    # without leaving a half-uninstalled system.
    if [ "$PURGE" -eq 1 ]; then
        case "$PREFIX" in
            /|/usr|/usr/local|/opt|/var|/var/lib|/run|/etc)
                die "refusing --purge with a broad --prefix: $PREFIX"
                ;;
        esac
    fi

    INIT=$(detect_init)
    if [ "$INIT" = unknown ]; then
        INIT=none
    fi
    require_root

    say "EdgeDesk uninstall plan"
    say "  init:        $INIT"
    say "  binaries:    $BIN_DIR"
    say "  config:      $CONFIG_DIR"
    say "  keep data:   $([ "$PURGE" -eq 1 ] && printf 'no' || printf 'yes')"
    say ""

    case "$INIT" in
        systemd)
            if [ -z "$DESTDIR" ] && [ "$DRY_RUN" -eq 0 ]; then
                systemctl stop edgedesk.service 2>/dev/null || true
                systemctl stop edgedesk-supervisor.service 2>/dev/null || true
                systemctl disable edgedesk.service 2>/dev/null || true
                systemctl disable edgedesk-supervisor.service 2>/dev/null || true
            fi
            remove_path /etc/systemd/system/edgedesk.service
            remove_path /etc/systemd/system/edgedesk-supervisor.service
            if [ -z "$DESTDIR" ] && [ "$DRY_RUN" -eq 0 ]; then
                systemctl daemon-reload
                systemctl reset-failed edgedesk.service 2>/dev/null || true
                systemctl reset-failed edgedesk-supervisor.service 2>/dev/null || true
            fi
            ;;
        openrc)
            if [ -z "$DESTDIR" ] && [ "$DRY_RUN" -eq 0 ]; then
                rc-service edgedesk stop 2>/dev/null || true
                rc-service edgedesk-supervisor stop 2>/dev/null || true
                rc-update del edgedesk default 2>/dev/null || true
                rc-update del edgedesk-supervisor default 2>/dev/null || true
            fi
            remove_path /etc/init.d/edgedesk
            remove_path /etc/init.d/edgedesk-supervisor
            ;;
        busybox)
            if [ -z "$DESTDIR" ] && [ "$DRY_RUN" -eq 0 ]; then
                for pid in $(pidof edgedesk 2>/dev/null || true); do
                    kill "$pid" 2>/dev/null || true
                done
                for pid in $(pidof edgedesk-supervisor 2>/dev/null || true); do
                    kill "$pid" 2>/dev/null || true
                done
            fi
            remove_path "$BIN_DIR/edgedesk-boot"
            remove_path "$BIN_DIR/edgedesk-supervisor-boot"
            remove_busybox_inittab
            ;;
        none) ;;
    esac

    remove_path "$BIN_DIR/edgedesk"
    remove_path "$BIN_DIR/edgedesk-supervisor"
    remove_path "$CONFIG_FILE"
    if [ "$DRY_RUN" -eq 0 ]; then
        rmdir "$(rooted "$CONFIG_DIR")" 2>/dev/null || true
    fi

    if [ "$PURGE" -eq 1 ]; then
        remove_tree "$PREFIX/web"
        remove_tree "$DATA_DIR"
        if [ "$WITH_SUPERVISOR" -eq 1 ]; then
            remove_tree "$SUPERVISOR_DB_DIR"
            remove_tree "$SUPERVISOR_LOG_DIR"
        fi
    else
        say "persistent data preserved under $DATA_DIR; use --purge to remove it"
    fi

    if [ "$DRY_RUN" -eq 0 ]; then
        rmdir "$(rooted "$BIN_DIR")" 2>/dev/null || true
        rmdir "$(rooted "$PREFIX")" 2>/dev/null || true
    fi
    say "uninstall complete; the account $SERVICE_USER was not removed"
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

if [ "$MODE" = detect ]; then
    print_detect
    exit 0
fi

if [ "$MODE" = uninstall ]; then
    do_uninstall
    exit 0
fi

# Interactive defaults only for a real terminal; automation must not block.
if [ "$ASSUME_YES" -eq 0 ] && [ -t 0 ] && [ "$DRY_RUN" -eq 0 ] && [ -z "$DESTDIR" ]; then
    HOST=$(prompt_value "Listen host" "$HOST")
    PORT=$(prompt_value "Listen port" "$PORT")
    FILE_ROOT=$(prompt_value "File API root" "$FILE_ROOT")
    PREFIX=$(prompt_value "Install prefix" "$PREFIX")
    if [ "$BIN_DIR_EXPLICIT" -eq 0 ]; then
        BIN_DIR="$PREFIX/bin"
    fi
fi

do_install
