# ADR-0003：V1.0 认证、API 面与实时协议

- 状态：**Accepted（已确认）**
- 日期：2026-10-16
- 关联：[产品范围](../spec/0001-product-scope.md)、[ADR-0002](0002-architecture-security.md)、[API 文档](../api.md)

> 本 ADR 描述 V1.0 的认证历史契约。V1.1 已改为单管理员密码登录与首次初始化，实际行为以 [ADR-0008](0008-authentication-and-settings.md)、[SPEC-0005](../spec/0005-authentication-and-settings.md) 和 [API 文档](../api.md) 为准；本文件保留用于理解 V1.0 的迁移来源。

## 背景

Phase 1 只有无鉴权的 `/api/health` 与静态资源，绑定回环地址。V1.0 需要在同一进程内提供文件管理、Bash 终端（WebSocket）和设备指标，并满足 ADR-0002 的安全约束：统一认证、WS 握手校验、限流、审计、有界资源。认证方式此前“待决定”，本 ADR 将其定案为可配置的部署级密钥认证。

## 决策

### 1. 认证：部署级访问密钥（Token）+ 签名会话 Cookie

- 设备端持有一个**访问密钥**（access token）。来源优先级：
  1. `--auth-token-file PATH`（推荐；文件首行非空内容，去除首尾空白，权限不因此变化）
  2. 环境变量 `EDGEDESK_AUTH_TOKEN`
  3. `--auth-token TEXT`（会出现在 `ps`，仅用于本地调试）
  4. 以上都没有时：**服务启动时生成随机密钥并打印到 stdout 一次**（日志中不重复打印）。
- 客户端用 `POST /api/auth/login {"token": "..."}` 换取会话 Cookie：
  - Cookie 名 `edgedesk_session`，值 `v1.<expiry_unix>.<hmac_sha256_hex>`；
  - HMAC 密钥为**每次启动随机生成**的 32 字节进程密钥，因此重启后已签发会话全部失效；
  - 属性：`HttpOnly; SameSite=Strict; Path=/`（HTTPS 部署时由外部网关附加 `Secure`）；
  - 有效期 `--session-ttl`（默认 43200 秒 = 12 小时）。
- 也接受 `Authorization: Bearer <token>`（脚本 / 自动化用途），此时不要求 Cookie 或 Origin 校验。
- 认证**始终启用**，不存在 `--no-auth`。默认监听仍是 `127.0.0.1`；只有显式提供访问密钥（文件/环境变量）后才允许 `--host` 绑定非回环地址，否则启动报错。
- 无 Cookie/密钥访问受保护路径：`401`；密钥错误：`401`，并经过登录限流。
- 登录失败限流（按对端 IP）：默认 5 次/分钟，超限 `429`。

### 2. CSRF / CSWSH 防护

- 会话 Cookie 使用 `SameSite=Strict`。
- 所有使用 Cookie 的非安全方法（POST/PUT/DELETE/PATCH）以及所有 WebSocket 升级请求必须携带与 `Host` 一致的 `Origin`（缺失 `Origin` 时要求 Bearer 认证）。
- WebSocket 升级在 HTTP 头阶段完成认证与 Origin 校验，失败返回 HTTP `401/403`，不进入 101 升级。

### 3. 实时传输：终端与指标均使用 WebSocket

- 终端：`/ws/terminal?session_id=...`，文本帧承载 JSON 协议（详见 [API 文档](../api.md)）。
- 指标：`/ws/metrics`，连接后先发一个有界历史窗口快照，随后按采样周期推送增量样本。
- 选择 WebSocket 而非 SSE 的理由：浏览器端终端已经需要 WS；libhv 对 WS 升级/心跳支持成熟；同一鉴权中间件可以统一覆盖 HTTP 与 WS。SSE 不再单独实现。
- 指标推送使用 `服务端采样线程 → 订阅者 channel->send()` 的直接推送；每个连接的发送缓存受 WS 已排队字节数上限保护（超限丢弃旧样本并断开慢客户端），不会无界增长。

### 4. 资源限制（全部可配置，默认值如下）

| 限制 | 默认 | 参数 |
| --- | --- | --- |
| HTTP 请求体（JSON API） | 1 MiB | `--max-request-body` |
| 流式上传单文件 | 256 MiB | `--max-upload-size` |
| 文本编辑保存 | 1 MiB | `--max-text-edit-size` |
| 并发流式传输（上传+下载） | 4 | `--max-transfers` |
| 并发终端会话 | 8 | `--max-terminal-sessions` |
| 单终端无客户端保留时长 | 1800 s | `--terminal-idle-timeout` |
| 终端回放缓冲（每会话） | 256 KiB | `--terminal-backlog` |
| 通用请求限流（每 IP） | 30 次/秒，突发 60 | `--rate-limit` |
| 指标采样周期 / 历史窗口 | 1000 ms / 300 点 | `--metrics-interval` / `--metrics-history` |

### 5. 审计日志

- JSON Lines 追加写文件，默认 `--audit-log` 指向可写运行目录（见 `docs/config.md`），支持 `--no-audit` 关闭。
- 记录：登录成功/失败、上传、下载、删除、重命名、mkdir、文本保存、终端创建/关闭/重命名、限流触发、认证失败。
- 大小轮转（默认 1 MiB × 2 份），线程安全，不引入日志框架、不引入数据库。
- 审计不记录访问密钥、Cookie 值、终端输入内容；终端操作只记录会话与元数据，不声称还原全部命令。

## 影响

- 部署必须提供或记录一次启动密钥；本文档与 README 说明获取方式。
- 默认安全：不配置密钥仅回环可访问；远程部署必须显式配置密钥（仍建议置于 TLS 网关之后）。
- Cookie 会话随进程重启失效属预期行为（V1.0 无持久化会话存储）。

## 非决策

- TLS 终止方式（内置或外部网关）、多用户/角色、OIDC/OAuth、会话持久化不在 V1.0 范围内。
