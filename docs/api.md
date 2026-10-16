# EdgeDesk V1.0 API 与 WebSocket 协议

> 状态：**已实现**。所有端点默认要求认证（见下文），响应均为
> `application/json; charset=utf-8`，错误体统一为
> `{"ok":false,"error":"<slug>","message":"...","errno":<可选>}`。

## 认证

| 方式 | 说明 |
| --- | --- |
| `Authorization: Bearer <token>` | 适合脚本/自动化；不需要 Origin。 |
| 会话 Cookie `edgedesk_session` | `POST /api/auth/login` 获取；`HttpOnly; SameSite=Strict; Max-Age=43200`。 |

携带 Cookie 的 `POST/PUT/DELETE/PATCH` 必须同时发送与 `Host` 一致的
`Origin`（缺失时使用 `Referer`）；否则返回 `403 csrf_rejected`。
WebSocket 升级在 HTTP 头阶段校验认证与 Origin，失败不会进入 101。

公共端点：`GET /api/health`、`POST /api/auth/login`、`POST /api/auth/logout`、
`GET /api/auth/session`、静态资源。

### 认证端点

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| `POST` | `/api/auth/login` | body `{"token":"..."}`；成功设置 Cookie，失败 `401 invalid_token`；超限 `429`（`Retry-After: 60`）。 |
| `POST` | `/api/auth/logout` | 清除 Cookie。 |
| `GET` | `/api/auth/session` | `{"ok":true,"authenticated":bool,"method":"cookie"\|"bearer"\|"none"}`。 |

## 健康检查

`GET /api/health` → `200`：

```json
{"status":"ok","service":"edgedesk","phase":1,"version":"1.0.0","uptime_seconds":123.4}
```

`phase` 字段为 Phase 1 契约，保持不变。

## 文件 API

根路径由 `--file-root` 决定；路径参数为沙箱内绝对路径（如 `/etc/hostname`）。
`..`、符号链接（默认）、越权重命名/删除都会被拒绝。

| 方法 | 路径 | 参数 | 说明 |
| --- | --- | --- | --- |
| `GET` | `/api/files` | `path`，`page`(默认1)，`page_size`(≤1000，默认200)，`sort`=`name\|size\|mtime\|type`，`order`=`asc\|desc`，`hidden`=0/1 | 目录列表；目录优先，返回 `permissions`/`uid`/`gid`/`mime`/`text_editable`。 |
| `GET` | `/api/files/stat` | `path` | 单条目元数据（lstat 语义，不跟随符号链接）。 |
| `GET` | `/api/files/text` | `path` | UTF-8 文本读取；超过 `--max-text-edit-size` 或二进制 → `413/415`。 |
| `PUT` | `/api/files/text` | `path`，body 为文本 | 原子保存（临时文件 + fsync + renameat），保留原权限；非 UTF-8 → `415`。 |
| `PUT` | `/api/files/content` | `path`，body 为原始字节 | 流式上传；临时文件 + 原子替换；任意大小受 `--max-upload-size` 限制；成功 `201`。 |
| `GET` | `/api/files/download` | `path`，可选 `Range`/`If-Range` | 流式下载；支持单段 Range（`206`/`416`），`Accept-Ranges: bytes`，带 `ETag`/`Last-Modified`。 |
| `HEAD` | `/api/files/download` | 同上 | 仅返回头。 |
| `POST` | `/api/files/mkdir` | `{"path":"/a/b"}` | 新建目录；已存在 `409`。 |
| `POST` | `/api/files/rename` | `{"from":"/a","to":"/b","overwrite":false}` | 重命名/移动；目标存在且未允许覆盖 → `409`；跨设备 `409`。 |
| `POST` | `/api/files/delete` | `{"paths":["/a","/b"]}` | 单项或批量删除，目录递归（深度 ≤64，条目 ≤100000）；禁止删除根。 |

错误码：`invalid_argument 400`、`unauthorized 401`、`permission_denied 403`、
`not_found 404`、`already_exists/conflict/is_a_directory 409`、`too_large 413`、
`unsupported 415/501`、`busy 423`、`limit_exceeded 429`、`io_error 500`、
`unavailable 503`。

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
{"type":"output","data":"..."}                  // 终端原始输出（含 ANSI/UTF-8）
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
  "network": [{"name":"eth0","rx_bytes_per_second":...,"tx_bytes_per_second":...,"rx_bytes_total":...,"tx_bytes_total":...}],
  "unavailable": ["network_rate_initial"]
}
```

`unavailable` 列出当前无法采集的维度；对应字段为 `null` 或空数组，前端显示
“不可用/采样中”，绝不虚构为 0。慢客户端在队列超过 1 MiB 时丢弃该次样本。

## 系统 API

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| `GET` | `/api/system/summary` | `{"ok":true,"interval_ms":1000,"sample":{...},"history":[...]}`；`history` 查询参数（0-3600）控制携带的样本数。 |
| `GET` | `/api/system/history` | `window` 参数（1-3600）返回最近 N 个样本。 |

## 请求限制

- `Content-Length` 超过路径上限：JSON 1 MiB、文本 1 MiB、上传 256 MiB（可配置）。
- 硬上限 4 MiB：任何超过该值的请求在读取 body 前直接断开连接（拒绝恶意大包）。
- 每 IP 限流 30/s（突发 60），登录 5/分钟；超限 `429`，上传路径在头阶段拒绝。
