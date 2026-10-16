# EdgeDesk V1.0 架构

> 状态：**已实现**（2026-10-16）。描述当前代码的真实结构；行为契约见 `docs/spec`，
> 技术决策见 `docs/adr`，接口见 `docs/api.md`，配置见 `docs/config.md`。

## 目录与职责

```text
src/
├── app/
│   ├── main.cpp               入口、参数解析、帮助/版本
│   └── application.{hpp,cpp}  进程级装配：创建服务、信号处理、维护线程、优雅退出
├── core/
│   ├── config.*               CLI / 配置文件 / 环境变量解析（无 libhv 类型）
│   ├── error.*                统一错误码与 HTTP 状态映射
│   ├── types.*               基础类型与工具（字节大小、路径名判断等）
│   ├── util.*                随机数、SHA-256/HMAC、时间、URL 编解码
│   └── fd.*                  RAII 文件描述符
├── file/
│   ├── path_sandbox.*        目录 FD + openat 的路径沙箱（ADR-0004）
│   ├── file_types.*          文件条目、列表、Range 解析、MIME
│   └── file_service.*        浏览/下载/上传/编辑/删除的业务实现
├── http/
│   ├── server.*              libhv 路由与 WebSocket 装配、静态资源与 SPA 回退
│   ├── middleware.*          安全响应头、限流、认证、CSRF、体积限制
│   ├── http_util.*           JSON 响应、Cookie、Host/Origin 校验等协议适配
│   ├── upload_state.hpp      流式上传的请求级状态（HttpContext userdata）
│   ├── ws_state.hpp          每个 WebSocket 连接的状态
│   └── routes/               health / auth / file / terminal / system
├── terminal/
│   ├── pty.*                 forkpty 封装（fork 与 exec 之间仅异步信号安全调用）
│   ├── terminal_session.*    单会话：读取线程、有界回放、CommandGuard 交互
│   ├── terminal_manager.*    注册表、并发上限、空闲回收、关闭
│   └── command_guard.*       危险命令提示规则（非安全边界）
├── system/
│   ├── cpu_metrics.*         /proc/stat、/proc/loadavg、/proc/uptime
│   ├── memory_metrics.*      /proc/meminfo
│   ├── disk_metrics.*        /proc/mounts + statvfs
│   ├── network_metrics.*     /proc/net/dev 与速率差分
│   └── system_metrics.*      采样线程 + 定长环形缓冲 + 订阅推送
├── security/
│   ├── auth.*                访问密钥 + 签名会话 Cookie（HMAC-SHA256）
│   ├── rate_limiter.*        per-IP 令牌桶 + 并发闸门
│   └── audit.*               JSON Lines 审计日志与轮转
└── web/
    ├── web_root.*            磁盘 / #embed 统一静态资源抽象
    └── embedded_assets.hpp   由 tools/gen_embed.py 生成的索引接口
```

分层规则：`Route → Service → Platform(POSIX)`。路由只做协议适配与错误映射，
不实现路径校验、PTY 管理或指标采集。`src/core` 不包含 libhv 类型。

## 线程模型

| 线程 | 数量 | 职责 | 禁止事项 |
| --- | --- | --- | --- |
| libhv 事件循环 | `--http-threads`（默认 1） | HTTP 解析、路由、WS 收发、定时器 | 不做长阻塞 IO |
| `hv::async` 线程池 | 2..4（按需扩展） | 文件下载流式写出等异步任务 | 不提交无界任务（有并发闸门） |
| 采样线程 | 1 | 周期性读取 `/proc`、`/sys`、`statvfs` | 不阻塞在锁上；订阅者回调只做 WS 入队 |
| 终端读取线程 | 每会话 1 | `poll` + `read` PTY、回放缓冲、广播输出 | 只读 master FD；关闭由标志位驱动 |
| 维护线程 | 1 | 每 30 s 回收空闲终端、清理限流桶 | 不做业务处理 |
| 信号线程 | 1 | `sigwait` 捕获 SIGINT/SIGTERM → 优雅停止 | 不调用非异步信号安全函数 |

共享状态一律有明确互斥：会话注册表（`TerminalManager::mutex_`）、会话回放与订阅
（`TerminalSession::mutex_`）、指标环形缓冲（`SystemMetrics::mutex_`）、限流桶、
审计写入。WS 通道发送由 libhv 自身加锁，跨线程投递是安全的。

## 资源边界

| 资源 | 默认上限 | 配置项 |
| --- | --- | --- |
| 流式上传单文件 | 256 MiB | `--max-upload-size` |
| 文本编辑/保存 | 1 MiB | `--max-text-edit-size` |
| JSON 请求体 | 1 MiB（硬上限 4 MiB） | `--max-request-body` |
| 并发传输（上传/下载） | 4 | `--max-transfers` |
| 终端会话 | 8 | `--max-terminal-sessions` |
| 每会话回放缓冲 | 256 KiB（环形丢弃最旧数据） | `--terminal-backlog` |
| 指标历史窗口 | 300 样本（约 5 分钟） | `--metrics-history` |
| 每 IP 请求速率 | 30/s，突发 60 | `--rate-limit` / `--rate-limit-burst` |
| 登录尝试 | 5 次/分钟 | `--login-rate-limit` |
| WS 订阅者 | 会话 8、指标流 16 | 代码常量（有测试） |
| 审计日志 | 1 MiB × 2 份轮转 | `--audit-max-size` / `--audit-max-files` |

## 请求生命周期（文件上传示例）

```text
浏览器 PUT /api/files/content?path=...
        │
headerHandler（libhv 头阶段，尚未读取 body）
        ├── 限流检查
        ├── 认证 / CSRF（Cookie 请求必须带同源 Origin/Referer）
        └── Content-Length ≤ --max-upload-size，否则直接拒绝
        │
state_handler(HP_HEADERS_COMPLETE)
        └── FileService::begin_upload → 目标目录内 .edgedesk-upload-*.tmp
        │
state_handler(HP_BODY) × N
        └── 有界写入 + 累计字节校验（chunked 上传同样受限）
        │
state_handler(HP_MESSAGE_COMPLETE)
        └── fsync → fchmod（保留原权限）→ renameat 原子替换 → 审计
```

连接中断时 libhv 调用 `state_handler(HP_ERROR)`，中间件同时提供
`release_upload_state()` 兜底，临时文件不会泄漏。

## 终端会话生命周期

见 [ADR-0005](adr/0005-terminal-architecture.md) 与 [API 文档](api.md)。
要点：WS 断开只解绑订阅者；刷新/导航/切换 Tab 不销毁 PTY；显式 close 或空闲
超时才回收；回放缓冲让重连客户端看到断线期间输出。

## 静态资源双模式

- **disk**：从 `--web-root` 读取，用于开发与需要替换前端的部署。
- **embed**：`tools/gen_embed.py` 生成带 `#embed` 的翻译单元（GCC 15+），
  资源编译进二进制；`WebRoot` 对上层完全一致，运行期不读前端目录。

两种模式共用 SPA 回退（未知非 `/api`、`/ws` 路径返回 `index.html`），
哈希资源 `Cache-Control: immutable`，`index.html` 使用 `no-cache`。
