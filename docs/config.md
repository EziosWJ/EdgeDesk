# EdgeDesk V1.3 配置与部署

> 状态：**已实现（V1.3.0）**。优先级：**默认值 < 配置文件 < 环境变量 < 命令行**。
> `--help` 打印完整选项列表（与本文档一致）。

## 命令行 / 配置文件

配置文件为 `key = value` 文本（`#` 注释），键名与长选项相同（去掉 `--`）。
例如：

```ini
host = 127.0.0.1
port = 8080
file-root = /srv/edgedesk-files
max-upload-size = 128M
metrics-interval = 1000
audit-log = /var/lib/edgedesk/audit.log
```

环境变量：`EDGEDESK_CONFIG`（配置文件路径）、`EDGEDESK_AUTH_TOKEN`（可选 Bearer Token）、
`EDGEDESK_DATA_DIR`（持久化数据目录）、`EDGEDESK_STATE_DIR`（运行期可写目录）。

| 选项 | 默认 | 说明 |
| --- | --- | --- |
| `--host` | `127.0.0.1` | 监听地址。非回环地址需显式配置，并应置于 TLS/VPN 或可信网关之后；V1.1 网页登录使用管理员密码，Bearer Token 可选。 |
| `--port` | `8080` | 监听端口。 |
| `--http-threads` | `1` | libhv 事件循环线程数。 |
| `--web-root` | 构建期 `web/dist` | disk 模式的静态资源目录（embed 模式忽略）。显式传入但不存在 → 启动失败；编译期默认目录缺失 → 启动告警，API/终端仍可用，Web UI 返回 404。 |
| `--file-root` | `/` | 文件 API 沙箱根目录（必须是存在的绝对路径）。 |
| `--follow-symlinks` | 关闭 | 打开后允许沙箱内符号链接解析；逃逸仍被拒绝。 |
| `--max-upload-size` | `256M` | 单文件流式上传上限。 |
| `--max-text-edit-size` | `1M` | 文本读取/保存上限。 |
| `--max-request-body` | `1M` | JSON API 体积上限（硬上限 4 MiB）。 |
| `--max-transfers` | `4` | 并发上传/下载总数。 |
| `--auth-token-file` | — | 从文件读取**可选** Bearer API Token（推荐）。未配置时 Bearer 不可用，网页密码登录不受影响。 |
| `--auth-token` | — | 直接给出 Bearer Token（会出现在 `ps`，仅调试用）。 |
| `--session-ttl` | `43200` | 会话 Cookie 有效期（秒）。 |
| `--login-rate-limit` | `5` | 每 IP 每分钟登录/初始化/改密尝试次数。 |
| `--cookie-secure` | 关闭 | 为会话 Cookie 添加 `Secure`；HTTPS 部署必须开启。 |
| `--trusted-proxy` | — | 可信反向代理 IP/CIDR（可重复）。只有来自这些地址的请求才解析 `X-Forwarded-For`。 |
| `--data-dir` | `$XDG_DATA_HOME/edgedesk` 或 `$HOME/.local/share/edgedesk` | 持久化目录（0700）：`auth.json`、`settings.json`。`HOME`/`XDG_DATA_HOME` 都不可用时必须显式配置。 |
| `--apps-root` | `/var/lib/edgedesk/apps` | 扩展应用包、不可变修订、SQLite 元数据/KV、临时操作与恢复日志。需要服务用户可写；不可用时扩展 API 返回 503，核心功能继续运行。 |
| `--audit-retention-days` | `90` | 审计保留天数（1–3650）；维护线程每日清理并记录 `audit.prune` 成功/失败。 |
| `--reset-admin-password` | — | 本机恢复：重置管理员认证；要求服务已停止，不删除业务数据/设置/审计。 |
| `--rate-limit` / `--rate-limit-burst` | `30` / `60` | 通用请求速率与突发额度。 |
| `--max-terminal-sessions` | `8` | 并发 PTY 会话上限。 |
| `--terminal-idle-timeout` | `1800` | 无客户端连接且无 IO 时的保留秒数（0=永不回收）。 |
| `--terminal-backlog` | `256K` | 每会话回放缓冲（有界，丢弃最旧数据）。 |
| `--bash` | `/bin/bash` | 登录 Shell 路径；不存在时报错，**不回退 `/bin/sh`**。 |
| `--metrics-interval` | `1000` | 指标采样间隔（ms，≥200）。 |
| `--metrics-history` | `300` | 内存中保留的样本数（环形）。 |
| `--disk-refresh-cycles` | `10` | 磁盘容量每 N 个采样周期刷新一次。 |
| `--disk-mount` | 自动 | 指定统计的挂载点（可重复；默认读取 `/proc/mounts` 中真实块设备）。 |
| `--state-dir` | `$XDG_RUNTIME_DIR/edgedesk` 或 `/tmp/edgedesk-<uid>` | 运行期可写目录（0700）；审计日志默认位于此处。认证/设置不放这里。 |
| `--audit-log` | `<state-dir>/audit.log` | 审计日志路径。 |
| `--audit-max-size` / `--audit-max-files` | `1M` / `2` | 轮转大小与保留份数。 |
| `--no-audit` | — | 关闭审计。 |
| `--config` | — | 加载配置文件。 |

## 首次初始化与恢复

新安装的 `data-dir` 没有 `auth.json`，服务进入未初始化状态：

- 打开 Web UI，直接设置至少 6 个字符的管理员密码（无需初始化码）；
- 初始化成功后 `auth.json`（密码哈希及参数）以 `0600` 保存；
- 未初始化状态只开放静态资源、健康检查、初始化状态和初始化接口。远程部署必须
  在首次初始化完成前限制访问。

忘记密码或 `auth.json` 损坏时，停止服务后执行：

```bash
edgedesk --reset-admin-password --data-dir /var/lib/edgedesk
```

该命令只重置认证状态，不会删除文件数据、`settings.json` 或审计日志。
服务运行中执行会被 `.lock` 拒绝；重启后在 Web UI 直接设置新密码。

## 可选 Bearer API Token

1. `--auth-token-file /etc/edgedesk/token`（推荐，权限 `0400`，服务用户可读）。
2. `EDGEDESK_AUTH_TOKEN=... ./edgedesk`。
3. `--auth-token ...`（仅本地调试）。
4. 都未提供时 **Bearer 认证不可用**；管理员仍可通过密码登录网页。V1.1 不再自动生成
   Token，也不接受 `POST /api/auth/login {"token":...}` 换取 Cookie。

## 运行

```bash
# 单二进制（embed）：不需要 web/ 目录
./edgedesk --port 8080 --file-root /srv --data-dir /var/lib/edgedesk

# 磁盘模式：显式指向前端产物
./edgedesk --port 8080 --web-root /opt/edgedesk/web --file-root /srv \
  --data-dir /var/lib/edgedesk

# 只读根文件系统：把运行期目录放到可写分区
./edgedesk --data-dir /var/lib/edgedesk --state-dir /run/edgedesk \
  --audit-log /var/log/edgedesk-audit.log
```

强制要求：

- 默认只监听回环地址。远程访问必须显式配置 `--host`，并放在 TLS/VPN 或可信网关之后；
  HTTPS 部署应加 `--cookie-secure`。反向代理必须保留原始 `Host`，否则同源
  Origin/CSRF 校验会拒绝写请求。
- 只有在代理明确覆盖 `X-Forwarded-For` 时，才把代理地址加入 `--trusted-proxy`；
  不要信任客户端直接提供的该头。
- `--file-root` 是**文件 API 的边界，不是终端的沙箱**：Bash 以服务用户的
  真实权限运行。
- 服务不需要数据库、不需要 Node/Python 运行时；embed 模式连前端目录也不需要。

## 浏览器本地桌面布局（V1.2）

桌面布局不写入服务端配置文件，也不新增后端接口：

| 键 | 内容 | 说明 |
| --- | --- | --- |
| `edgedesk.desktop.layout.v1` | `schemaVersion`、桌面图标槽位、Dock 固定项、窗口 `appId`/几何/状态/非敏感 payload | 单浏览器 localStorage；JSON，带版本校验 |
| `edgedesk.terminals.v1` | 终端 session id 与标题 | 既有键；仅用于会话列表提示 |

- 保存内容不包含密码、认证 Cookie、API Token、文件正文或其他凭据。
- 版本不支持、JSON 损坏、单条窗口记录非法或 localStorage 不可用时，桌面以默认布局启动并显示非阻塞提示。
- 主题等跨浏览器偏好仍由 `/api/settings` 管理，不复制到桌面布局。
- 服务端从不信任布局中的路径或 session id；恢复时仍按现有认证、路径沙箱和会话归属校验执行。

## 扩展应用平台（V1.3）

- 数据根目录 `--apps-root` 默认 `/var/lib/edgedesk/apps`，权限 `0700`：
  - `extensions.db`（SQLite WAL，`0600`）保存应用状态、修订、权限、完整性索引、操作日志与 KV；
  - `revisions/<appId>/<revisionId>/` 保存不可变静态资源；
  - `data/<appId>/` 保存平台托管数据目录；
  - `tmp/` 保存未完成安装与扩展上传恢复日志，启动时幂等清理。
- 扩展平台是 SQLite 的**局部范围例外**；认证/设置仍用 JSON 文件，终端与指标不迁移。
- `--apps-root` 不可创建/不可写或数据库 schema 高于当前程序时，扩展管理/运行 API 返回
  `503 extensions_unavailable`；启动日志明确告警，核心 API 不受影响。生产部署应由
  systemd `StateDirectory`/`ReadWritePaths` 提供目录。
- 备份：停止服务后复制整个 `--apps-root`（包含数据库与修订），或用 SQLite 在线备份工具复制；
  不要只复制 `extensions.db` 而遗漏 `-wal`/`-shm`。
- 损坏数据库采取失败关闭，不自动重建；需保留现场后人工恢复或移除扩展数据根。
- 扩展安装包没有固定大小/文件数上限，按可用磁盘空间决定；上传/解压使用临时目录与原子激活。
- 审计默认保留 90 天，`--audit-retention-days` 可调整；卸载应用不会删除历史审计。
- 浏览器桌面布局仍在 localStorage；扩展窗口记录只保存 `appId` 与几何，恢复时重新校验安装、
  启用和宿主 API 兼容状态。

## 本地开发运行（VS Code / 桌面用户）

生产默认扩展数据根是 `/var/lib/edgedesk/apps`，普通桌面用户通常没有写权限；直接运行会看到
`extension platform is unavailable; configure a writable --apps-root`，核心 API 仍可用。
这不是 CMake 预设问题，`--apps-root` 是**运行时参数**。

本地开发使用仓库内脚本，它把运行时状态放到已被 `.gitignore` 忽略的 `out/dev-data/`：

```bash
scripts/run-dev.sh                 # http://127.0.0.1:8080/
EDGEDESK_PORT=8090 scripts/run-dev.sh
```

在 VS Code 中调试时，把同样的参数加入 launch/run 配置（示例）：

```json
{
  "name": "EdgeDesk dev-disk",
  "type": "cppdbg",
  "request": "launch",
  "program": "${workspaceFolder}/out/build/dev-disk/bin/edgedesk",
  "cwd": "${workspaceFolder}",
  "preLaunchTask": "edgedesk: prepare dev dirs",
  "args": [
    "--host", "127.0.0.1", "--port", "8080",
    "--web-root", "${workspaceFolder}/web/dist",
    "--file-root", "${workspaceFolder}/out/dev-data/files",
    "--data-dir", "${workspaceFolder}/out/dev-data/data",
    "--state-dir", "${workspaceFolder}/out/dev-data/state",
    "--apps-root", "${workspaceFolder}/out/dev-data/apps"
  ]
}
```

其中 `edgedesk: prepare dev dirs` 只需执行
`mkdir -p out/dev-data/files out/dev-data/data out/dev-data/state out/dev-data/apps`。
生产部署仍应使用默认 `/var/lib/edgedesk/apps`（或 systemd `StateDirectory` 提供的可写路径）。

## 信号与退出

`SIGINT` / `SIGTERM` 触发优雅退出：停止 HTTP 事件循环 → 关闭全部 PTY 会话
（SIGHUP，700 ms 宽限后 SIGKILL）→ 停止采样线程 → 写入审计 `service stop`。

## 一键安装（`deploy/install.sh`）

仓库提供 POSIX `sh` 安装器（可在 dash、bash 与 BusyBox ash 下运行），推荐用于
systemd、OpenRC 和 BusyBox init 目标。它会：

1. 探测服务管理器（不只检查 PID 1；Alpine 可能以 BusyBox init 配合 OpenRC）；
2. 交互或通过参数收集 `host`、`port`、文件根目录和安装前缀；
3. 校验参数后生成 `/etc/edgedesk/edgedesk.conf`（`0640 root:<服务组>`），
   由 systemd/OpenRC/BusyBox 统一通过 `--config` 加载；
4. 安装 `edgedesk` 与 `edgedesk-supervisor`，并按探测结果注册服务。

```bash
sudo deploy/install.sh                       # 交互输入 host/port/file-root
sudo deploy/install.sh --host 127.0.0.1 --port 8080 \
  --source out/build/release-embed/bin --file-root /srv
deploy/install.sh --detect                   # 只打印 init/PID1/可用管理命令
deploy/install.sh --dry-run --source out/build/release-embed/bin
sudo deploy/install.sh --uninstall           # 保留 /var/lib/edgedesk*
```

约束与行为：

- 默认 `127.0.0.1`；非回环 `--host` 必须显式加 `--allow-non-loopback`
  （交互终端会二次确认），并自行提供 TLS/VPN 网关；HTTPS 需 `--cookie-secure`。
- 安装器不生成密码或 Token，管理员仍在首次登录时通过 Web UI 设置。
- 端口必须是 1–65535 的十进制整数；路径必须为绝对路径且不含空白、引号、
  `#` 或 `..`；`/etc/edgedesk` 只允许受控字符，避免 unit/脚本注入。
- 已存在的文件默认拒绝覆盖；`--force` 覆盖并保留 `.bak`。
- `--dry-run` 与 `--destdir` 不要求 root，也不修改当前系统；`--destdir` 用于
  打包和测试，会跳过建用户与 init 注册。
- `--no-supervisor` 只安装 Web 后端；`--no-enable` / `--no-start` 用于打包或
  手工控制启动。
- `--uninstall` 停止并注销服务、删除二进制与 `/etc/edgedesk`，保留数据；
  `--purge` 额外删除数据、扩展目录和 Supervisor 状态，且拒绝过宽的 prefix。

具体文件、权限和三种 init 的生成内容见 [`deploy/README.md`](../deploy/README.md)。
真机验证状态见 [`docs/testing.md`](testing.md)；未实际运行的 init 不得宣称通过。

## 服务化示例（systemd）

```ini
[Unit]
Description=EdgeDesk web operations panel
After=network.target

[Service]
ExecStart=/opt/edgedesk/edgedesk --host 127.0.0.1 --port 8080 \
  --file-root /srv --apps-root /var/lib/edgedesk/apps \
  --data-dir /var/lib/edgedesk --state-dir /run/edgedesk
StateDirectory=edgedesk
User=edgedesk
Group=edgedesk
Restart=on-failure
NoNewPrivileges=yes
ProtectSystem=strict
ReadWritePaths=/srv /var/lib/edgedesk /var/lib/edgedesk/apps /run/edgedesk

[Install]
WantedBy=multi-user.target
```

## Process Supervisor 部署配置（已实现）

`edgedesk-supervisor` 是独立前台守护进程；EdgeDesk Web 后端通过 Unix Socket 调用它。
Web 后端仍只监听管理员配置的 HTTP 地址（Phase 1 仅回环），Supervisor 不监听 TCP。

### EdgeDesk Web 后端

| 选项 | 默认值 | 说明 |
|---|---|---|
| `--supervisor-socket PATH` | `/run/edgedesk-supervisor.sock` | 必须是绝对路径且短于 108 字节 |
| `--no-supervisor` | 关闭 | 不配置 Supervisor 客户端；进程管理 API 返回 503 |

未安装 Supervisor 或 Socket 不可达时，EdgeDesk 仍正常启动；只有
`/api/supervisor/*` 返回 `503 supervisor_unavailable`，其他功能不受影响。

### Supervisor 守护进程

| 选项 | 默认值 | 说明 |
|---|---|---|
| `--socket PATH` | `/run/edgedesk-supervisor.sock` | Unix Domain Socket；目录必须由 Supervisor 用户拥有且不可被组/其他用户写 |
| `--database PATH` | `/var/lib/edgedesk-supervisor/supervisor.db` | 独立 SQLite schema v1 |
| `--log-dir PATH` | `/var/lib/edgedesk-supervisor/logs` | 每服务一个有界轮转日志 |
| `--allow-uid USER\|UID` | root 时 `0`；非 root 时自身 UID | 允许连接 IPC 的对端 UID，可重复 |
| `--allow-gid GROUP\|GID` | 无 | 允许连接 IPC 的对端主 GID，可重复 |
| `--socket-group GROUP` | 不改变 | Socket 的组所有者，配合 0660 权限 |
| `--version` / `--help` | — | 版本/用法 |

数据目录以 `0700` 创建，数据库和日志以 `0600` 创建；Socket 默认 `0660`。
数据库损坏、`PRAGMA quick_check` 失败或 `user_version` 高于当前 schema 时，
Supervisor 启动失败并保留原始文件，不会静默重建或清空。

非 root 启动只用于开发和测试：Supervisor 只能运行与自身相同 UID/GID 的服务，
配置其他身份会在启动服务时返回 `permission_denied`。生产部署应使用 root
Supervisor + 显式 `run_user`/`run_group`；root 服务需要配置 `allow_root=true`，
Web UI 会要求单独确认。

### init 适配与目录

仓库提供：

- `deploy/systemd/edgedesk-supervisor.service`：前台 `Type=simple`，
  `KillMode=process`、`Restart=always`、`StateDirectory`、`UMask=0077`。
- `deploy/openrc/edgedesk-supervisor`：使用 `supervise-daemon` 的 OpenRC 脚本。
- `deploy/busybox/inittab` + `deploy/busybox/edgedesk-supervisor-boot`：
  `::respawn:` 启动，wrapper 创建 0700 状态/日志目录。

安装、权限和验证命令见 [`deploy/README.md`](../deploy/README.md)。
只读根文件系统必须把 `--database` 与 `--log-dir` 指向可写持久化分区。
三种 init 适配均已提供文件，但尚未在本仓库当前环境中的真实
systemd/OpenRC/BusyBox 目标上执行；平台状态见 [`docs/testing.md`](testing.md)。

完整安全与服务配置约定见 [SPEC-0009](spec/0009-process-supervisor.md)。
