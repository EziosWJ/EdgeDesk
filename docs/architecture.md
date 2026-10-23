# EdgeDesk V1.3 架构

> 状态：**已实现**（V1.3.0 扩展平台首期 + Process Supervisor V1.0；V1.2 桌面能力保持不变）。
> 描述当前代码的真实结构；行为契约见 `docs/spec`，扩展内部契约见
> `docs/extension-platform-contracts.md`，技术决策见 `docs/adr`，接口见 `docs/api.md`，
> 配置见 `docs/config.md`。init/musl/ARM64 平台验证状态见 `docs/testing.md`。

## 目录与职责

```text
src/
├── app/
│   ├── main.cpp               入口、参数解析、帮助/版本、恢复命令分派
│   ├── recovery.*             --reset-admin-password 的本机恢复流程
│   ├── supervisor_service.*   有界 worker 队列 + Supervisor IPC 客户端适配
│   └── application.{hpp,cpp}  进程级装配：创建服务、信号处理、维护线程、优雅退出
├── core/
│   ├── config.*               CLI / 配置文件 / 环境变量解析（无 libhv 类型）
│   ├── atomic_file.*          目录 FD + 原子文件写入
│   ├── error.*                统一错误码与 HTTP 状态映射
│   ├── types.*               基础类型与工具（字节大小、路径名判断等）
│   ├── util.*                随机数、SHA-256/HMAC、时间、URL 编解码
│   └── fd.*                  RAII 文件描述符
├── file/
│   ├── path_sandbox.*        目录 FD + openat 的路径沙箱（ADR-0004）
│   ├── file_types.*          文件条目、列表、Range 解析、MIME
│   ├── file_lock.*           统一共享读/独占写锁（ADR-0013）
│   └── file_service.*        浏览/下载/上传/编辑/删除的业务实现
├── extensions/
│   ├── semver.*              严格 SemVer 与宿主 API 兼容范围
│   ├── manifest.*            manifest 核心/模块字段校验、来源与 CSP
│   ├── zip_reader.*          流式 ZIP 校验、解压、逐文件 SHA-256、完整性验证
│   ├── extension_store.*     SQLite schema/事务/KV/权限/操作日志（局部例外）
│   ├── package_manager.*     安装/升级/回滚/卸载/隔离/清理/静态修订
│   └── extension_runtime.*   运行实例快照、KV 游标、传输、锁协调、终端回收
├── http/
│   ├── server.*              libhv 路由与 WebSocket 装配、静态资源与 SPA 回退
│   ├── middleware.*          安全响应头、限流、认证、CSRF、体积限制
│   ├── http_util.*           JSON 响应、Cookie、Host/Origin 校验等协议适配
│   ├── upload_state.hpp      流式上传/扩展安装的请求级状态（HttpContext userdata）
│   ├── ws_state.hpp          每个 WebSocket 连接的状态
│   └── routes/               health / auth / file / terminal / system / extension
├── terminal/
│   ├── pty.*                 forkpty 封装（fork 与 exec 之间仅异步信号安全调用）
│   ├── terminal_session.*    单会话：读取线程、有界回放、CommandGuard 交互
│   ├── terminal_manager.*    注册表、并发上限、空闲回收、关闭
│   └── command_guard.*       危险命令提示规则（非安全边界）
├── system/
│   ├── cpu_metrics.*         /proc/stat、/proc/loadavg、/proc/uptime
│   ├── memory_metrics.*      /proc/meminfo
│   ├── disk_metrics.*        /proc/mounts + statvfs
│   ├── network_metrics.*     /proc/net/dev、getifaddrs 与速率差分
│   └── system_metrics.*      采样线程 + 定长环形缓冲 + 订阅推送
├── security/
│   ├── auth.*                密码登录、可选 Bearer、签名会话 Cookie（HMAC-SHA256）
│   ├── auth_store.*          auth.json / .lock 状态机与原子持久化
│   ├── password_hash.*       Argon2id + 有界 worker 队列
│   ├── settings_store.*      settings.json 白名单偏好
│   ├── rate_limiter.*        per-IP 令牌桶 + 并发闸门
│   └── audit.*               JSON Lines 审计日志与轮转
├── supervisor/               独立进程，不链接 libhv
│   ├── main.cpp              前台 CLI、信号、Socket/数据库/日志目录装配
│   ├── supervisor_types.*    配置/状态模型、身份解析、路径与范围校验
│   ├── supervisor_store.*    SQLite schema v1、事务、恢复元数据（局部例外）
│   ├── supervisor_engine.*   状态机、重启策略、退避、进程组停止、崩溃接管
│   ├── process_control.*     fork/setsid/fexecve、/proc 身份、信号与指标采样
│   ├── log_store.*           有界轮转日志、游标/尾部读取
│   ├── ipc_server.*          Unix Socket、SO_PEERCRED、JSON-RPC/NDJSON、限流
│   └── supervisor_client.*   EdgeDesk 侧同步 IPC 客户端
└── web/
    ├── web_root.*            磁盘 / #embed 统一静态资源抽象
    └── embedded_assets.hpp   由 tools/gen_embed.py 生成的索引接口

deploy/
├── install.sh                POSIX sh 安装器（检测 init、生成配置、注册服务）
├── systemd/                  edgedesk-supervisor.service
├── openrc/                   OpenRC supervise-daemon 脚本
└── busybox/                  inittab 示例与 boot wrapper

tools/ext-sdk/                独立 SDK、CLI、vanilla/React/Vue/Svelte 模板
scripts/extension_test.py     扩展平台集成测试（含崩溃恢复）
scripts/supervisor_test.py    Supervisor HTTP/生命周期/恢复集成测试
```

分层规则：`Route → Service → Platform(POSIX)`。路由只做协议适配与错误映射，
不实现路径校验、PTY 管理或指标采集。`src/core` 不包含 libhv 类型。

## 前端桌面层（web/src）

```text
web/src/
├── app/App.tsx              认证状态机、主题、指标 WebSocket、终端列表刷新、桌面恢复入口
├── desktop/
│   ├── appRegistry.ts       appId、名称、图标 sprite、说明、单/多实例策略、默认/最小尺寸
│   ├── desktopStore.ts      桌面图标槽位、Dock 固定项、持久化提示
│   ├── desktopPersistence.ts 版本化 localStorage、校验/迁移/回退、自动保存
│   ├── DesktopShell.tsx     壁纸、顶栏、桌面图标、窗口层、Dock、启动器、快捷键
│   ├── DesktopIcons.tsx     单击选中、双击启动、拖拽换位
│   ├── Dock.tsx             固定应用、运行指示、最小化恢复、多窗口选择器
│   ├── Launcher.tsx         AppRegistry 搜索、应用说明、空结果、焦点返回
│   ├── AppSurface.tsx       appId → 业务组件（dashboard/files/terminal/sessions/processes/settings/editor/preview）
│   └── desktop.css          桌面/窗口/Dock/启动器/NAS 小组件样式与浅色主题覆盖
├── stores/windowStore.ts    统一窗口生命周期、实例规则、z-index、几何约束、关闭守卫
├── stores/sessionStore.ts   服务端会话列表与指标窗口镜像
├── components/
│   ├── WindowLayer.tsx      渲染所有窗口；最小化隐藏但不卸载
│   └── FloatingWindow.tsx   标题栏拖动、缩放、最小化/最大化/关闭、焦点反馈
├── extensions/
│   ├── extensionApi.ts      管理/运行时 REST 适配
│   ├── extensionRegistry.ts 启用应用 → AppRegistry；禁用时关闭窗口
│   ├── hostBridge.ts        iframe 握手、MessagePort JSON-RPC、权限检查、传输/终端
│   ├── ExtensionAppView.tsx 沙箱 iframe、实例 UUID、错误/重载
│   ├── ExtensionManagerView.tsx 安装/审批/升级/回滚/卸载/完整性 UI
│   └── extensions.css       管理界面与运行时样式
└── features/                 业务组件：dashboard/files/terminal/sessions/processes/settings
```

关键不变量：

- 主业务入口统一由 `appRegistry` 提供；桌面图标、Dock、启动器不维护第二份应用清单。
- `windowStore` 是窗口状态的唯一写入点；业务组件不直接修改 z-index 或其他窗口。
- z-index 由数组顺序重新计算（`10 + index`），连续聚焦不会无限增长。
- 最小化只设置状态并隐藏视觉层；组件保持挂载，编辑器缓冲与终端 WebSocket 订阅不被销毁。
- 关闭终端窗口只卸载 UI；后端 `TerminalSession` 独立存活。恢复时按服务端真实会话列表校验，
  会话不存在只显示“已结束/不可恢复”，绝不自动 `create` 替代会话。
- `terminal` 应用是单窗口单会话的单页面终端；`sessions` 应用是多 Tab 会话管理器。
  两者共享同一后端会话集合，只有会话管理器提供显式“结束会话”。
- `DesktopPersistence` 只保存非敏感的桌面布局与重连标识；认证 Cookie、Token、文件正文不落盘。

## 扩展应用平台（V1.3）

扩展平台是现有桌面、窗口管理、认证、文件与终端能力之上的增量模块，不替换任何既有服务。

### 数据根目录

默认 `/var/lib/edgedesk/apps`，由 `--apps-root` 覆盖：

```text
apps/
├── extensions.db               SQLite 元数据与 KV（0600）
├── revisions/<appId>/<revisionId>/
│                              不可变静态资源与 manifest
├── data/<appId>/              平台托管的应用数据目录（当前 KV 在 SQLite）
└── tmp/
    ├── op-<operationId>/      安装/升级临时目录（启动时幂等清理）
    └── transfers/*.journal    扩展上传临时文件恢复日志
```

目录为 `0700`；若数据根不可创建/不可写，扩展 API 返回 `503 extensions_unavailable`，核心认证、文件、终端与指标仍可用。

### 安装与修订

- ZIP 由 `ZipReader` 流式校验：路径穿越、绝对路径、反斜杠、符号链接、特殊文件、重复/大小写重复名称、本地/中央头不一致、加密条目、ZIP64 越界、CRC/长度错误均拒绝。
- 解压在临时目录内完成，逐文件计算 SHA-256；声明大小与可用磁盘比较，写入失败清理。
- 安装路径只保留每个条目的路径、大小与 SHA-256 完整性记录，不保留解压内容或第二份文件元数据；manifest JSON DOM 在序列化后释放，结果以移动语义传递。
- 校验通过后 `rename` 到不可变修订目录，再以单个 SQLite 事务提交应用、修订、权限申请、完整性清单和活动指针。
- 相同 SemVer 覆盖和主动降级都创建新 revisionId；清理只删除未被活动、固定或隔离引用的历史修订。
- 服务重启清理未完成 operation、孤儿修订目录与上传临时文件。

### 静态资源服务

- `/apps/{appId}/{version}/{revisionId}/{path}` 由异步路由处理：`PackageManager::open_static` 在活动修订的 `PathSandbox` 内解析并打开文件，返回只读 FD、大小、MIME、ETag 与 CSP，不在路由内重复路径校验。
- 响应头一次性写出；`GET` 以 chunked 编码按 128 KiB 从 FD `pread`，写队列超过 1 MiB 时等待，60 s 无进展则中止；`HEAD` 只写 `Content-Length`，不发 body。任何资源都不整体读入内存。
- `/apps/` 前缀与前端 SPA 回退严格分离；未登记资源、非活动修订和裸前缀均为 `404`。

### 运行时

- 浏览器主机在创建窗口时调用 `POST /api/extensions/runtime/sessions`，C++ 端复制当前授权为**权限快照**并返回随机运行时令牌。
- iframe 使用 `sandbox="allow-scripts allow-forms allow-downloads allow-popups allow-popups-to-escape-sandbox allow-modals"`，不授予 `allow-same-origin`。
- 主机通过 `postMessage` 校验 `event.source`/`event.origin === "null"`/协议后转移 `MessagePort`；运行时令牌只保存在宿主页面，不进入 iframe。
- 主机桥接执行方法/参数/权限检查，C++ 运行时端点用同一令牌再次检查应用状态与权限快照。
- 禁用、升级、卸载、完整性隔离：`ExtensionRuntime::invalidate_app` 删除会话与传输任务，并关闭该实例创建的 PTY 会话；前端同步关闭窗口。
- 文件传输由 1 MiB 顺序块、逐块确认、专用端口组成；上传使用 `FileService` 临时文件与原子替换，下载持有共享读锁。

### SQLite 范围

SQLite 只位于 `src/extensions/`，保存扩展清单、状态、活动修订、权限、操作日志、完整性索引和应用 KV。认证使用 `auth.json`、设置使用 `settings.json`、终端与指标保持原有内存/文件实现；不得据此迁移其他模块。

## 认证与持久化边界

- `--data-dir`（0700）保存 `auth.json` 和 `settings.json`；`--state-dir`
  只保存运行期数据与审计日志。两者不互相回退。
- 启动时 `AuthStore` 读取 `auth.json`：缺失 → `uninitialized`；有效 → `initialized`；
  存在但无效 → `corrupt`（失败关闭，不自动重新初始化）。
- 密码哈希通过有界 worker 执行 Argon2id，libhv 事件循环只做协议适配；改密在
  互斥保护下轮换会话签名密钥，使全部旧 Cookie 失效。
- `.lock` 由服务进程持有；`--reset-admin-password` 用非阻塞 `flock` 证明服务已停止。
- 所有认证/设置文件写入使用目录 FD、临时文件、`fsync`、`renameat` 和目录 `fsync`。

## 线程模型

| 线程 | 数量 | 职责 | 禁止事项 |
| --- | --- | --- | --- |
| libhv 事件循环 | `--http-threads`（默认 1） | HTTP 解析、路由、WS 收发、定时器 | 不做长阻塞 IO |
| `hv::async` 线程池 | 2..4（按需扩展） | 文件下载流式写出等异步任务 | 不提交无界任务（有并发闸门） |
| 采样线程 | 1 | 周期性读取 `/proc`、`/sys`、`statvfs` | 不阻塞在锁上；订阅者回调只做 WS 入队 |
| 终端读取线程 | 每会话 1 | `poll` + `read` PTY、回放缓冲、广播输出 | 只读 master FD；关闭由标志位驱动 |
| 维护线程 | 1 | 每 30 s 回收空闲终端、清理限流桶、扩展运行会话/传输、每日审计保留清理 | 不做业务处理 |
| 扩展安装 | libhv 事件循环 | 流式 ZIP 写入临时文件，安装事务串行化 | 无固定包大小/文件数上限 |
| 扩展传输 | libhv 事件循环 | 1 MiB 顺序块、互斥保护任务表 | 不在事件循环上执行整文件阻塞 I/O |
| 信号线程 | 1 | `sigwait` 捕获 SIGINT/SIGTERM → 优雅停止 | 不调用非异步信号安全函数 |

共享状态一律有明确互斥：会话注册表（`TerminalManager::mutex_`）、会话回放与订阅
（`TerminalSession::mutex_`）、指标环形缓冲（`SystemMetrics::mutex_`）、限流桶、
审计写入。WS 通道发送由 libhv 自身加锁，跨线程投递是安全的。
`TerminalSession` 的订阅回调（回放、实时输出、退出）不得在 `mutex_` 持有期间调用，
否则回调重入 `info()`/`unsubscribe()` 会自锁；每个订阅者的 `delivery_mutex` 只用于
保证回放 → 实时输出 → 退出的顺序，回调内部可以安全地再次访问会话。

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
| 审计保留 | 90 天（每日清理） | `--audit-retention-days` |
| 应用 KV | 5 MiB/应用、256 KiB/键 | 代码常量（ADR-0011） |
| 扩展传输块 | 1 MiB | 代码常量（ADR-0013） |
| 历史修订 | 活动 + 最近 2 个（可固定） | `kDefaultHistoryRevisions` |
| 运行会话 | 12 h TTL | `kRuntimeSessionTtlSeconds` |
| 传输空闲 | 5 min | `kTransferIdleTimeoutMs` |

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
要点：WS 断开只解绑订阅者；刷新/导航/切换 Tab/最小化窗口不销毁 PTY；关闭终端窗口只卸载
UI，不调用会话关闭接口；显式 close 或空闲超时才回收；回放缓冲让重连客户端看到断线期间输出。
桌面恢复先读取 `/api/terminal/sessions` 的真实列表，再按保存的 session ID 关联；已消失的
会话保留窗口并显示结束状态，不创建替代 Bash。

## 静态资源双模式

- **disk**：从 `--web-root` 读取，用于开发与需要替换前端的部署。
- **embed**：`tools/gen_embed.py` 生成带 `#embed` 的翻译单元（GCC 15+），
  资源编译进二进制；`WebRoot` 对上层完全一致，运行期不读前端目录。

两种模式共用 SPA 回退（未知非 `/api`、`/ws` 路径返回 `index.html`），
哈希资源 `Cache-Control: immutable`，`index.html` 使用 `no-cache`。

## Process Supervisor V1.0（已实现）

本节描述当前代码；行为契约见 [SPEC-0009](spec/0009-process-supervisor.md)。

```text
EdgeDesk Web API -> SupervisorService（有界 worker 队列）
                  -> SupervisorClient -> Unix Domain Socket
                                             |
                                             v
                              edgedesk-supervisor（独立守护进程）
                                  | SQLite（独立数据库，schema v1）
                                  | 单线程 poll() 生命周期/自动重启
                                  | 进程组、运行身份、日志与主进程指标

systemd / OpenRC / BusyBox init -> 负责 Supervisor 自身的启动与 respawn
```

实现约束：

- `edgedesk-supervisor` 不链接 libhv，拥有独立 `main()` 与单线程 `poll()` 事件循环；
  EdgeDesk Web 后端重启不会终止托管进程。
- 路由位于 `src/http/routes/supervisor_route.*`，只做 HTTP 协议适配、认证/CSRF
  （现有中间件）、审计与错误映射。实际 IPC 调用在 `src/app/supervisor_service.*`
  的固定大小 worker 池中执行，不阻塞 libhv 事件循环。
- Supervisor 只在 Unix Socket 上监听，创建时校验目录所有权/权限，连接时检查
  `SO_PEERCRED`；不存在 TCP 管理 listener。
- 业务服务由 Supervisor 直接启动：绝对可执行路径 + argv 数组，`fork`/`setsid`/
  `fexecve`（脚本经已检查 inode 的 `/proc/self/fd/<fd>`），不经过 Shell。运行身份
  必须在配置中显式给出，root 需要 `allow_root=true`。
- 子进程在持久化 PID/PGID/boot ID/starttime 之前阻塞在启动握手上；持久化失败会
  回收子进程，不会留下无身份的孤儿。
- 进程组信号只发送给三态身份探测（`verified`/`absent`/`uncertain`）中 `verified`
  的组；证据不足时进入 `UNKNOWN`，不猜测 PGID，也不启动第二份实例。
- stop/restart 是异步操作：请求立即返回 `operation_id`，通过
  `/api/supervisor/operations/{id}` 查询；宽限期与清理不占用 Web 工作线程。
- 被动日志轮询不逐请求写审计；管理操作、认证失败和 root 特权拒绝仍然审计。
- Supervisor 使用专属 SQLite 数据库保存定义、期望状态与恢复元数据；
  不与扩展平台数据库共用，也不迁移认证、设置、终端或指标存储。
- 日志、IPC 消息、队列和连接数均有上限；CPU/RSS 不可用时返回 `null`。
- 前端 `web/src/features/processes/` 注册为内置应用 `processes`，通过现有桌面
  Store 管理窗口，仅调用认证后的 `/api/supervisor/*`。
- init 适配文件位于 `deploy/`；systemd、OpenRC、BusyBox 真机验证尚未在当前环境
  执行，不能把“脚本存在”当作平台验收通过。
- 完整设计和行为契约见 [SPEC-0009](spec/0009-process-supervisor.md)，技术决策见
  [ADR-0015](adr/0015-process-supervisor-architecture.md)、
  [ADR-0016](adr/0016-process-supervisor-security.md) 与
  [ADR-0019](adr/0019-process-supervisor-identity-and-operations.md)；加固需求见
  [SPEC-0010](spec/0010-process-supervisor-hardening.md)。
