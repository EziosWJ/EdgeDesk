# EdgeDesk V1.1 配置与部署

> 状态：**已实现（V1.1.0）**。优先级：**默认值 < 配置文件 < 环境变量 < 命令行**。
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
| `--data-dir` | `$XDG_DATA_HOME/edgedesk` 或 `$HOME/.local/share/edgedesk` | 持久化目录（0700）：`auth.json`、`init-code`、`settings.json`。`HOME`/`XDG_DATA_HOME` 都不可用时必须显式配置。 |
| `--reset-admin-password` | — | 本机恢复：重置管理员认证并生成新的初始化码；要求服务已停止，不删除业务数据/设置/审计。 |
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

新安装的 `data-dir` 没有 `auth.json`，服务进入未初始化状态，并生成一次性
`init-code`（权限 `0600`）：

- 前台 TTY 启动时，初始化码只在终端打印一次；
- systemd/非 TTY 启动时只打印文件路径，请以服务用户或 root 读取；
- 打开 Web UI，输入初始化码并设置至少 12 个字符的管理员密码；
- 初始化成功后 `init-code` 被删除，`auth.json`（密码哈希及参数）以 `0600` 保存。

忘记密码或 `auth.json` 损坏时，停止服务后执行：

```bash
edgedesk --reset-admin-password --data-dir /var/lib/edgedesk
```

该命令只重置认证状态并生成新的初始化码，不会删除文件数据、`settings.json`
或审计日志。服务运行中执行会被 `.lock` 拒绝。

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

## 信号与退出

`SIGINT` / `SIGTERM` 触发优雅退出：停止 HTTP 事件循环 → 关闭全部 PTY 会话
（SIGHUP，700 ms 宽限后 SIGKILL）→ 停止采样线程 → 写入审计 `service stop`。

## 服务化示例（systemd）

```ini
[Unit]
Description=EdgeDesk web operations panel
After=network.target

[Service]
ExecStart=/opt/edgedesk/edgedesk --host 127.0.0.1 --port 8080 \
  --file-root /srv --data-dir /var/lib/edgedesk \
  --state-dir /run/edgedesk
StateDirectory=edgedesk
User=edgedesk
Group=edgedesk
Restart=on-failure
NoNewPrivileges=yes
ProtectSystem=strict
ReadWritePaths=/srv /var/lib/edgedesk /run/edgedesk

[Install]
WantedBy=multi-user.target
```
