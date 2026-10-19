# EdgeDesk V1.3 API 与 WebSocket 协议

> 状态：**已实现（V1.3.0）**。V1.2 桌面布局仍保存在浏览器 localStorage；V1.3 新增扩展
> 管理 API、运行时 API 和 `/apps` 静态资源路由。扩展内部方法/错误码契约见
> [extension-platform-contracts.md](extension-platform-contracts.md)。所有业务端点默认要求认证（见下文），响应均为
> `application/json; charset=utf-8`，错误体统一为
> `{"ok":false,"error":"<slug>","message":"...","errno":<可选>}`。

## 认证

V1.1 采用单管理员密码登录，保留独立、可选的 Bearer API Token。

| 方式 | 说明 |
| --- | --- |
| 管理员密码 + 会话 Cookie | `POST /api/auth/login {"password":"..."}` 获取 `edgedesk_session`；`HttpOnly; SameSite=Strict; Path=/; Max-Age=43200`，`--cookie-secure` 时附加 `Secure`。 |
| `Authorization: Bearer <token>` | 可选；由 `--auth-token-file` / `EDGEDESK_AUTH_TOKEN` / `--auth-token` 配置。未配置时 Bearer 不可用。 |

携带 Cookie 的 `POST/PUT/DELETE/PATCH` 必须同时发送与 `Host` 一致的
`Origin`（缺失时使用 `Referer`）；否则返回 `403 csrf_rejected`。
WebSocket 升级在 HTTP 头阶段校验认证与 Origin，失败不会进入 101。

认证状态：

| 状态 | 含义 | 业务 API |
| --- | --- | --- |
| `uninitialized` | 新安装，`auth.json` 不存在 | `503 setup_required`，只允许初始化相关端点 |
| `initialized` | 已有管理员密码 | 按认证结果允许 |
| `corrupt` | `auth.json` 无效/版本不支持/不可读 | `503 auth_state_corrupt`，只能本机恢复 |

公共端点：`GET /api/health`、`GET /api/auth/setup-status`、`POST /api/auth/setup`、
`POST /api/auth/login`（已初始化时）、`POST /api/auth/logout`、
`GET /api/auth/session`、静态资源。

### 认证端点

| 方法 | 路径 | 认证 | 说明 |
| --- | --- | --- | --- |
| `GET` | `/api/auth/setup-status` | 公共 | `{"ok":true,"initialized":bool,"state":"uninitialized\|initialized\|corrupt"}`；不返回秘密。 |
| `POST` | `/api/auth/setup` | 公共、限流 | body `{"password":"...","confirm_password":"..."}`；成功 `201`；已初始化 `409 already_initialized`；密码策略错误 `400`；损坏状态 `503`。 |
| `POST` | `/api/auth/login` | 公共、限流 | body `{"password":"..."}`；成功设置 Cookie 并返回 `expires_in`/`expires_at`；失败 `401 invalid_credentials`；超限 `429`（`Retry-After: 60`）。 |
| `POST` | `/api/auth/logout` | Cookie | 清除 Cookie；带 Cookie 时校验同源 Origin。 |
| `GET` | `/api/auth/session` | 公共 | `{"ok":true,"authenticated":bool,"method":"cookie"\|"bearer"\|"none","state":"...","expires_at":<可选>}`。 |
| `POST` | `/api/auth/password` | Cookie | body `{"current_password":"...","new_password":"...","confirm_password":"..."}`；成功后轮换会话密钥，所有旧 Cookie 失效，响应设置过期 Cookie。 |

`POST /api/auth/login` 不再接受 `{"token":"..."}`；脚本客户端应直接使用
`Authorization: Bearer`。密码至少 6 个 Unicode 字符、最多 1024 个 UTF-8 字节；
服务端使用 Argon2id 哈希。

## 健康检查

`GET /api/health` → `200`：

```json
{"status":"ok","service":"edgedesk","phase":1,"version":"1.3.0","uptime_seconds":123.4}
```

`phase` 字段为 Phase 1 契约，保持不变。

## 文件 API

根路径由 `--file-root` 决定；路径参数为沙箱内绝对路径（如 `/etc/hostname`）。
`..`、符号链接（默认）、越权重命名/删除都会被拒绝。

| 方法 | 路径 | 参数 | 说明 |
| --- | --- | --- | --- |
| `GET` | `/api/files` | `path`，`page`(默认1)，`page_size`(≤1000，默认200)，`sort`=`name\|size\|mtime\|type`，`order`=`asc\|desc`，`hidden`=0/1，`dirs_only`=0/1 | 目录列表；目录优先，返回 `permissions`/`uid`/`gid`/`mime`/`text_editable`。`dirs_only=1` 只返回 `path` 的真实子目录（符号链接不跟随、不包含），供目录树按需加载，避免递归扫描。 |
| `GET` | `/api/files/stat` | `path` | 单条目元数据（lstat 语义，不跟随符号链接）。 |
| `GET` | `/api/files/text` | `path` | UTF-8 文本读取；超过 `--max-text-edit-size` 或二进制 → `413/415`。 |
| `PUT` | `/api/files/text` | `path`，body 为文本 | 原子保存（临时文件 + fsync + renameat），保留原权限；非 UTF-8 → `415`。 |
| `PUT` | `/api/files/content` | `path`，body 为原始字节 | 流式上传；临时文件 + 原子替换；任意大小受 `--max-upload-size` 限制；成功 `201`。 |
| `GET` | `/api/files/download` | `path`，可选 `Range`/`If-Range` | 流式下载；支持单段 Range（`206`/`416`），`Accept-Ranges: bytes`，带 `ETag`/`Last-Modified`。 |
| `HEAD` | `/api/files/download` | 同上 | 仅返回头。 |
| `POST` | `/api/files/mkdir` | `{"path":"/a/b"}` | 新建目录；已存在 `409`。 |
| `POST` | `/api/files/rename` | `{"from":"/a","to":"/b","overwrite":false}` | 重命名/移动；目标存在且未允许覆盖 → `409`；跨设备 `409`。 |
| `POST` | `/api/files/delete` | `{"paths":["/a","/b"]}` | 单项或批量删除，目录递归（深度 ≤64，条目 ≤100000）；禁止删除根。 |

错误码：`invalid_argument 400`、`invalid_credentials 401`、`unauthorized 401`、
`permission_denied 403`、`not_found 404`、`already_exists 409`、`conflict 409`、
`is_a_directory 409`、`already_initialized 409`、`invalid_settings 400`、
`too_large 413`、`unsupported 415/501`、`busy 423`、`limit_exceeded 429`、
`rate_limited 429`、`io_error 500`、`setup_required 503`、`auth_state_corrupt 503`、
`auth_unavailable 503`、`unavailable 503`。

## 终端 API

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| `GET` | `/api/terminal/sessions` | 会话列表 + `shell`、`shell_available`、`max_sessions`、`idle_timeout_seconds`、`running`。 |
| `POST` | `/api/terminal/sessions` | body `{"cwd":"...","cols":100,"rows":30,"title":"..."}`；超过上限 `429`；Bash 缺失 `501`。 |
| `POST` | `/api/terminal/sessions/close` | body `{"session_id":"..."}`；显式销毁会话（SIGHUP → 必要时 SIGKILL）。 |

会话 ID 为 128 位随机十六进制；单用户产品模型下所有已认证客户端共享同一集合。

## WebSocket：`/ws/terminal`

连接示例：`ws://host/ws/terminal?session_id=<id>`（也可不带参数，先发 `attach`）。
鉴权使用 Cookie + 同源 `Origin`（浏览器自动携带）。

**客户端 → 服务端**

```json
{"type":"attach","session_id":"..."}          // 重连既有会话
{"type":"create","cwd":"/tmp","cols":100,"rows":30,"title":"Bash"}
{"type":"input","data":"ls\r"}                 // 原样输入（含控制字符）
{"type":"resize","cols":132,"rows":43}
{"type":"confirm"} | {"type":"cancel"}         // CommandGuard 确认/取消
{"type":"close"}                               // 显式销毁会话
{"type":"ping"}
```

**服务端 → 客户端**

```json
{"type":"ready","shell":"/bin/bash","shell_available":true,"max_sessions":8}
{"type":"created"|"attached","session":{...}}   // 见会话字段
{"type":"output","data":"..."}                  // 终端输出（保留 ANSI；data 始终为合法 UTF-8）
{"type":"exit","code":0}
{"type":"guard","reason":"递归删除根目录或系统路径"}
{"type":"confirmed"|"cancelled","confirmed":true}
{"type":"error","code":"session_not_found","message":"..."}
{"type":"pong"}
```

会话字段：`id,title,cwd,created_ms,last_activity_ms,cols,rows,running,exited,exit_code,subscribers,buffered_bytes,pending_confirmation`。

行为保证：

- WS 断开（刷新、导航、切 Tab、网络抖动）**不会**终止 PTY；`attach` 后会先收到
  回放缓冲（丢弃最旧数据时插入提示文本），随后是实时输出。
- `output.data` 始终是合法 UTF-8：跨 PTY 读取边界或回放/实时边界的多字节字符会先
  重组再发送，非法字节替换为 U+FFFD；JSON 序列化不会因终端字节流而失败。
- 输出超过 2 MiB 未发送完的慢客户端会被断开，避免设备内存被占满。
- 会话保持 Bash 登录语义：`/bin/bash -l`，加载 `/etc/profile` 与首个存在的
  `~/.bash_profile|~/.bash_login|~/.profile`；`~/.bashrc` 不由登录 Shell 自动加载。

## WebSocket：`/ws/metrics`

连接后首先收到快照，之后每个采样周期推送一次增量：

```json
{"type":"snapshot","interval_ms":1000,"sample":{...},"history":[{...}]}
{"type":"metrics","sample":{...}}
```

样本结构：

```json
{
  "ts": 1760000000000, "uptime_seconds": 12345.6,
  "hostname": "device", "kernel": "6.1.0", "arch": "aarch64",
  "cpu": {"cores":4,"usage_percent":12.5,"load1":0.4,"load5":0.3,"load15":0.2},
  "memory": {"total_kb":...,"available_kb":...,"used_kb":...,"used_percent":...,
             "swap_total_kb":...,"swap_free_kb":...},
  "disks": [{"mount":"/","filesystem":"ext4","total_bytes":...,"used_bytes":...,"available_bytes":...,"used_percent":...}],
  "network": [{"name":"eth0","addresses":["192.168.1.10"],"rx_bytes_per_second":...,"tx_bytes_per_second":...,"rx_bytes_total":...,"tx_bytes_total":...}],
  "unavailable": ["network_rate_initial"]
}
```

`unavailable` 列出当前无法采集的维度；对应字段为 `null` 或空数组，前端显示
“不可用/采样中”，绝不虚构为 0。慢客户端在队列超过 1 MiB 时丢弃该次样本。
`network[].addresses` 为除 `lo` 外各网卡已配置的 IPv4/IPv6 地址（来自 `getifaddrs()`）；
网卡未分配地址时为空数组。

## 系统 API

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| `GET` | `/api/system/summary` | `{"ok":true,"interval_ms":1000,"sample":{...},"history":[...]}`；`history` 查询参数（0-3600）控制携带的样本数。 |
| `GET` | `/api/system/history` | `window` 参数（1-3600）返回最近 N 个样本。 |
| `GET` | `/api/system/info` | 白名单系统信息：版本/构建类型、主机名、架构、内核、运行时间、监听地址、文件根目录、Bash、审计状态、数据/运行期目录、资源上限、`api_token_configured` 布尔值。不返回密码哈希、Token、Cookie、环境变量全集或完整命令行。 |

## 设置 API

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| `GET` | `/api/settings` | `{"ok":true,"settings":{...},"api_token_configured":bool}`。 |
| `PUT` | `/api/settings` | 部分更新白名单字段；未知字段/非法枚举/越界值 `400`。Cookie 请求必须同源，Bearer 可直接调用。 |

`settings` 字段：`theme`（`light\|dark\|system`）、`files_view`（`list\|grid`）、
`files_show_hidden`（bool）、`terminal_font_size`（10–24）、
`dashboard_metrics_window`（60–3600 秒）。写入 `data-dir/settings.json`（0600），
不会覆盖部署级配置。

## 扩展管理 API

除 `/apps/...` 静态资源外，所有扩展端点都需要管理员认证；Cookie 写请求仍需同源
`Origin`/`Referer`。扩展平台不可用（`--apps-root` 不可写）时返回
`503 extensions_unavailable`，不影响其他核心 API。

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| `GET` | `/api/extensions` | 应用列表：状态、活动修订、申请/已授权限、来源、窗口策略、未验证发布者标记。 |
| `GET` | `/api/extensions/{app_id}` | 应用详情 + 修订列表。 |
| `PUT` | `/api/extensions/install` | 原始 ZIP body（`Content-Type: application/zip`）。查询参数 `restore_data=1`、`overwrite=1`、`allow_downgrade=1`。流式写入临时文件，不受 JSON/上传大小上限约束。成功 `201`。 |
| `PUT` | `/api/extensions/{app_id}/upgrade` | 同上传方式；manifest id 必须匹配路径应用。 |
| `POST` | `/api/extensions/{app_id}/approve` | body `{"permissions":[...],"enable":bool}`；权限必须是申请集合的子集。授予 `network` 时同时记录当前活动修订声明的来源。 |
| `POST` | `/api/extensions/{app_id}/enable` / `disable` | 启用前检查修订与宿主 API 兼容；禁用立即终止运行实例。 |
| `POST` | `/api/extensions/{app_id}/permissions` | 更新有效授权；对现有实例延迟生效。授予 `network` 时记录当前活动修订来源，撤销时清除记录。 |
| `POST` | `/api/extensions/{app_id}/uninstall` | body `{"data_action":"keep"\|"delete"}`；立即撤销权限，审计保留。 |
| `POST` | `/api/extensions/{app_id}/activate` | body `{"revision_id":"...","confirm":true}`；主动升级/降级已有修订。 |
| `POST` | `/api/extensions/{app_id}/pin` | body `{"revision_id":"...","pinned":bool}`；固定修订避免清理。 |
| `POST` | `/api/extensions/{app_id}/integrity-check` | 可选 `revision_id`；核对逐文件 SHA-256。不一致返回 `409`，隔离修订/应用并清除授权。 |
| `GET` | `/api/extensions/{app_id}/revisions` | 修订列表及 active/pinned/integrity 状态。 |

安装结果包含 `app`、`revision`、`warnings`（含“发布者身份未验证”）与
`approval_required`。相同 SemVer 未确认覆盖返回 `409 conflict`；降级需
`allow_downgrade=1` 或 `activate`。

网络来源授权：有效来源为活动修订声明来源与管理员已批准来源的交集，且必须存在
`network` 授权。升级或 `activate` 切换修订时，若新修订来源集合超出已批准集合，
安装/切换仍成功且应用保持启用，但 `network` 从 `granted_permissions` 移除，
需再次调用 `approve`/`permissions` 审核；撤销、卸载（含保留数据）、隔离与重装
清除批准记录。旧 schema 中已有 `network` 授权而无批准来源时按未批准处理。

## 扩展静态资源

`GET|HEAD /apps/{appId}/{version}/{revisionId}/{relativePath}`

- 公开路由（sandbox iframe 为 opaque origin，不能携带宿主 Cookie），但仅服务**已启用、活动、
  完整性正常**的修订；其他状态返回 `404`。
- 无扩展名路径回退 `index.html`；带扩展名的缺失资源返回 `404`，不做 SPA 回退；裸 `/apps`、
  `/apps/` 前缀同样返回 `404`，不落入前端 SPA。
- 响应 `Cache-Control: public, max-age=31536000, immutable`，带完整性 SHA-256 `ETag`；
  `If-None-Match` 命中返回 `304` 且无 body。
- `GET` 以 `Transfer-Encoding: chunked` 流式发送，按 128 KiB 从修订目录 FD 读取，并有
  1 MiB 写队列高水位与 60 s 停滞保护；不把完整资源读入内存。`HEAD` 返回相同的
  `Content-Type`/`ETag`/`Content-Length` 头但不发送 body。
- 应用专属 CSP 只列出**有效来源**（声明 ∩ 已批准，且存在 `network` 授权）；无授权时
  不产生任何外部 source。CSP 不约束 `window.open`、`target="_blank"` 或 iframe 自身
  顶层导航；`object-src 'none'`、`base-uri 'none'`、`frame-ancestors 'self'`；
  `X-Frame-Options: SAMEORIGIN`。

## 扩展运行时 API

宿主机页面在创建窗口时取得运行时令牌，然后以
`X-EdgeDesk-Runtime: <token>` 调用；管理员会话 Cookie 仍是必需条件。令牌不进入 iframe。
请求体使用 JSON；文件块使用原始二进制。

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| `POST` | `/api/extensions/runtime/sessions` | body `{"app_id":"...","instance_id":"..."}`；返回权限快照、**有效网络来源**（声明 ∩ 批准，无授权时为空）、静态基址和 `token`。 |
| `DELETE` | `/api/extensions/runtime/sessions` | 结束当前令牌对应实例并关闭其终端/传输。 |
| `POST` | `/api/extensions/runtime/storage/get\|set\|delete\|list\|transaction` | 隔离 KV；键 1..1024 字节，值 JSON ≤256 KiB/键，5 MiB/应用；事务全成全败。 |
| `POST` | `/api/extensions/runtime/files/list\|stat\|read\|mkdir\|rename\|delete` | 虚拟路径；目录游标分页；`CURSOR_STALE` 返回 `409 cursor_stale`。 |
| `POST` | `/api/extensions/runtime/files/upload/begin` | body `{"path":"/x","size":N}`；返回 `transfer_id` 与 1 MiB `chunk_size`。 |
| `PUT` | `/api/extensions/runtime/files/upload/{id}/chunk?offset=N` | 原始块（1..1048576 字节），严格顺序；写入后返回 `received`。 |
| `POST` | `/api/extensions/runtime/files/upload/complete` | body `{"transfer_id":"...","expected_size":N}`；长度匹配后原子替换。 |
| `POST` | `/api/extensions/runtime/files/upload/cancel` | 释放锁并删除临时文件。 |
| `POST` | `/api/extensions/runtime/files/download/begin` | 返回 `transfer_id`、总长度、块大小；持有共享读锁。 |
| `GET` | `/api/extensions/runtime/files/download/{id}/chunk?offset=N` | 返回 ≤1 MiB 原始字节；`X-EdgeDesk-EOF: 1` 表示结束。 |
| `POST` | `/api/extensions/runtime/files/download/complete\|cancel` | 结束/取消下载并释放锁。 |
| `GET` | `/api/extensions/runtime/terminal/sessions` | 列出终端会话。 |
| `POST` | `/api/extensions/runtime/terminal/sessions` | 创建 PTY；body `title/cwd/cols/rows`。 |
| `POST` | `/api/extensions/runtime/terminal/close` | body `{"session_id":"..."}`。 |
| `POST` | `/api/extensions/runtime/errors` | 应用错误上报；只写应用/实例/修订、类型、截断消息与栈到审计。type/message/stack 先脱敏、按 UTF-8 边界截断并作为带引号的 `key="value"` 字段编码；审计 sink 再次脱敏 detail/path，并将单字段限制在 8192 字节（超出追加 `[TRUNCATED]`）。不记录原始请求正文。 |

终端输入/输出仍通过宿主页面已有的 `/ws/terminal` 中继到应用；`terminal.access` 权限快照由
宿主桥接与运行时端点共同检查。会话创建/关闭/列表写审计，不记录原始终端输入输出。

错误 slug 包括：`extensions_unavailable`、`runtime_session_invalid`、`app_not_found`、
`revision_not_found`、`permission_denied`、`file_locked`（423）、`cursor_stale`（409）、
`quota_exceeded`（413）、`too_large`、`conflict`、`unsupported`。

## 请求限制

- `Content-Length` 超过路径上限：JSON 1 MiB、文本 1 MiB、上传 256 MiB（可配置）。
- 硬上限 4 MiB：任何超过该值的请求在读取 body 前直接断开连接（拒绝恶意大包）。
- 每 IP 限流 30/s（突发 60），登录/初始化/改密 5/分钟；超限 `429`，上传路径在头阶段拒绝。
- 默认使用 TCP 对端 IP；只有 `--trusted-proxy` 列出的代理才解析 `X-Forwarded-For`。

## Process Supervisor API（已实现）

所有 `/api/supervisor/*` 端点沿用现有管理员认证、CSRF、限流、安全响应头和统一错误格式。
它们只通过本机 Unix Domain Socket 访问 `edgedesk-supervisor`；浏览器和扩展 iframe
不能直接连接该 Socket。

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/api/supervisor/health` | Supervisor 连接与健康状态 |
| GET | `/api/supervisor/services` | 服务列表；支持 `offset`、`limit`（1–1000）、`status` |
| POST | `/api/supervisor/services` | 创建服务 |
| GET | `/api/supervisor/services/{id}` | 服务详情 |
| PUT | `/api/supervisor/services/{id}` | 更新提供的字段，不自动重启 |
| DELETE | `/api/supervisor/services/{id}` | 删除已停止服务；运行中返回 409 |
| POST | `/api/supervisor/services/{id}/start` | 启动服务 |
| POST | `/api/supervisor/services/{id}/stop` | 停止服务并等待完成 |
| POST | `/api/supervisor/services/{id}/restart` | 停止后重新启动并等待完成 |
| GET | `/api/supervisor/services/{id}/logs` | 读取日志；支持 `tail_bytes`、`cursor`、`max_bytes` |

成功响应带 `"ok": true`。服务对象包含配置和运行时字段：

```json
{
  "id": "…",
  "name": "mihomo",
  "executable": "/usr/local/bin/mihomo",
  "args": ["-c", "/etc/mihomo/config.yaml"],
  "cwd": "/var/lib/mihomo",
  "env": {"LOG_LEVEL": "info"},
  "run_user": "edgedesk-service",
  "run_group": "edgedesk-service",
  "allow_root": false,
  "autostart": true,
  "restart_policy": "on-failure",
  "max_restarts": 5,
  "restart_window_sec": 60,
  "restart_delay_ms": 1000,
  "max_restart_delay_ms": 60000,
  "stable_after_sec": 300,
  "stop_timeout_ms": 10000,
  "log_max_bytes": 10485760,
  "created_at": 1792421340,
  "updated_at": 1792421340,
  "state": "RUNNING",
  "desired_state": "running",
  "pid": 1234,
  "pgid": 1234,
  "boot_id": "…",
  "started_at": 1792421400000,
  "last_exit_code": null,
  "last_signal": null,
  "last_exit_at": null,
  "last_error": "",
  "restart_count": 0,
  "failed_locked": false,
  "uptime_seconds": 42,
  "cpu_percent": 1.5,
  "rss_bytes": 10485760,
  "metrics_available": true,
  "ok": true
}
```

`state` 为 `STOPPED`、`STARTING`、`RUNNING`、`STOPPING`、`BACKOFF`、`FAILED` 或
`UNKNOWN`。`pid`/`pgid`/`started_at`/`last_exit_*`/`uptime_seconds` 在不可用时为
`null`；`cpu_percent`/`rss_bytes` 不可用时同样为 `null`，不得当作 0。

日志游标响应：

```json
{
  "ok": true,
  "service_id": "…",
  "content": "…",
  "cursor": 334,
  "next_cursor": 334,
  "size": 334,
  "reset": false,
  "truncated": false
}
```

`tail_bytes` 与 `max_bytes` 上限均为 256 KiB；不传 `cursor`/`tail_bytes` 时默认读取
16 KiB 尾部。`reset=true` 表示游标因日志轮转/截断失效，应重新读取尾部。

错误响应：

```json
{"ok": false, "error": "service_running", "message": "…"}
```

| 状态 | 错误 slug | 场景 |
|---|---|---|
| 400 | `invalid_service_config` / `invalid_request` / `invalid_params` | 配置、路径、身份或参数无效 |
| 403 | `root_confirmation_required` | 运行身份为 root 但没有 `allow_root=true` |
| 403 | `permission_denied` | 非 root Supervisor 无法切换身份等 |
| 404 | `service_not_found` | 服务 ID 不存在 |
| 409 | `service_running` / `service_stopping` / `service_state` / `service_name_conflict` | 生命周期或唯一名称冲突 |
| 502 | `supervisor_error` | IPC 返回无法映射的内部错误 |
| 503 | `supervisor_unavailable` / `supervisor_busy` / `operation_timeout` | Socket 不可达、worker 队列满或操作超时 |

CSRF 校验失败仍由通用中间件返回 403 `csrf_rejected`；未认证返回 401。
`supervisor_unavailable` 只影响进程管理端点，文件、终端、仪表盘、认证、设置和扩展
平台继续工作。

完整行为契约见 [SPEC-0009](spec/0009-process-supervisor.md)，部署见
[`deploy/README.md`](../deploy/README.md)。
