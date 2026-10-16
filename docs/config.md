# EdgeDesk V1.0 配置与部署

> 状态：**已实现**。优先级：**默认值 < 配置文件 < 环境变量 < 命令行**。
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

环境变量：`EDGEDESK_CONFIG`（配置文件路径）、`EDGEDESK_AUTH_TOKEN`（访问密钥）、
`EDGEDESK_STATE_DIR`（运行期可写目录）。

| 选项 | 默认 | 说明 |
| --- | --- | --- |
| `--host` | `127.0.0.1` | 监听地址。非回环地址必须显式提供访问密钥（文件/环境变量/`--auth-token`），否则启动失败。 |
| `--port` | `8080` | 监听端口。 |
| `--http-threads` | `1` | libhv 事件循环线程数。 |
| `--web-root` | 构建期 `web/dist` | disk 模式的静态资源目录（embed 模式忽略）。 |
| `--file-root` | `/` | 文件 API 沙箱根目录（必须是存在的绝对路径）。 |
| `--follow-symlinks` | 关闭 | 打开后允许沙箱内符号链接解析；逃逸仍被拒绝。 |
| `--max-upload-size` | `256M` | 单文件流式上传上限。 |
| `--max-text-edit-size` | `1M` | 文本读取/保存上限。 |
| `--max-request-body` | `1M` | JSON API 体积上限（硬上限 4 MiB）。 |
| `--max-transfers` | `4` | 并发上传/下载总数。 |
| `--auth-token-file` | — | 从文件读取访问密钥（推荐）。 |
| `--auth-token` | — | 直接给出密钥（会出现在 `ps`，仅调试用）。 |
| `--session-ttl` | `43200` | 会话 Cookie 有效期（秒）。 |
| `--login-rate-limit` | `5` | 每 IP 每分钟登录尝试次数。 |
| `--rate-limit` / `--rate-limit-burst` | `30` / `60` | 通用请求速率与突发额度。 |
| `--max-terminal-sessions` | `8` | 并发 PTY 会话上限。 |
| `--terminal-idle-timeout` | `1800` | 无客户端连接且无 IO 时的保留秒数（0=永不回收）。 |
| `--terminal-backlog` | `256K` | 每会话回放缓冲（有界，丢弃最旧数据）。 |
| `--bash` | `/bin/bash` | 登录 Shell 路径；不存在时报错，**不回退 `/bin/sh`**。 |
| `--metrics-interval` | `1000` | 指标采样间隔（ms，≥200）。 |
| `--metrics-history` | `300` | 内存中保留的样本数（环形）。 |
| `--disk-refresh-cycles` | `10` | 磁盘容量每 N 个采样周期刷新一次。 |
| `--disk-mount` | 自动 | 指定统计的挂载点（可重复；默认读取 `/proc/mounts` 中真实块设备）。 |
| `--state-dir` | `$XDG_RUNTIME_DIR/edgedesk` 或 `/tmp/edgedesk-<uid>` | 运行期可写目录（0700）。 |
| `--audit-log` | `<state-dir>/audit.log` | 审计日志路径。 |
| `--audit-max-size` / `--audit-max-files` | `1M` / `2` | 轮转大小与保留份数。 |
| `--no-audit` | — | 关闭审计。 |
| `--config` | — | 加载配置文件。 |

## 访问密钥的取得

1. `--auth-token-file /etc/edgedesk/token`（推荐，权限 `0400`，服务用户可读）。
2. `EDGEDESK_AUTH_TOKEN=... ./edgedesk`。
3. `--auth-token ...`（仅本地调试）。
4. 都未提供时：服务生成**一次性随机密钥**并只打印到 stdout 一次：

   ```text
   EdgeDesk access token (generated for this run): 3f6a...
   ```

   重启后旧密钥失效（会话也随之失效）。

## 运行

```bash
# 单二进制（embed）：不需要 web/ 目录
./edgedesk --port 8080 --file-root /srv

# 磁盘模式：显式指向前端产物
./edgedesk --port 8080 --web-root /opt/edgedesk/web --file-root /srv

# 只读根文件系统：把运行期目录放到可写分区
./edgedesk --state-dir /run/edgedesk --audit-log /var/log/edgedesk-audit.log
```

强制要求：

- 默认只监听回环地址。远程访问建议放在 TLS/VPN 网关之后，并使用
  `--host 0.0.0.0 --auth-token-file ...`（认证仍然必需）。
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
  --file-root /srv --auth-token-file /etc/edgedesk/token \
  --state-dir /run/edgedesk
User=edgedesk
Group=edgedesk
Restart=on-failure
NoNewPrivileges=yes
ProtectSystem=strict
ReadWritePaths=/srv /run/edgedesk

[Install]
WantedBy=multi-user.target
```
