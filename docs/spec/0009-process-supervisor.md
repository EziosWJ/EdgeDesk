# SPEC-0009：EdgeDesk Process Supervisor

- 状态：**Confirmed design / Not implemented**（Grill 8 轮决策已确认；实现尚未开始）
- 日期：2026-10-19
- 目标版本：EdgeDesk 后续版本（建议 V1.4）
- 前置基线：[产品范围 SPEC-0001](0001-product-scope.md)、[桌面工作区 SPEC-0006](0006-desktop-workspace.md)、[扩展平台 SPEC-0007](0007-extension-applications.md)
- 技术决策：[ADR-0015：独立 Supervisor 架构](../adr/0015-process-supervisor-architecture.md)、[ADR-0016：权限与执行安全](../adr/0016-process-supervisor-security.md)
- 实施计划：[Process Supervisor 实施计划](../tasks/0009-process-supervisor.md)

> 本文定义已确认的产品行为与实现边界，不表示相关代码已经存在。完成情况以代码、测试和实际平台验证为准。

## 1. 目标与定位

为 EdgeDesk 增加跨 Linux 发行版的本机常驻进程托管能力。用户通过现有 Web 桌面创建、启动、停止、重启和查看后台服务，并配置异常退出后的自动恢复策略。

核心目标：**EdgeDesk Web 后端重启或崩溃时，Supervisor 和被托管的业务进程保持独立；Supervisor 自身的启动与故障恢复由目标系统的 init 适配层负责。**

适用场景包括 Mihomo、Go/C++ 服务、工业采集程序及其他能够以前台进程运行的可执行程序。

## 2. 已确认的 Grill 决策

1. Supervisor 是独立守护进程。
2. EdgeDesk 后端通过 Unix Domain Socket 管理 Supervisor；不开放 Supervisor TCP API。
3. 提供 systemd、OpenRC、BusyBox init 启动适配。
4. 服务启动使用可执行文件路径和参数数组，不经过 Shell。
5. 支持 `always`、`on-failure`、`never`、指数退避、重启次数限制、稳定运行后重置计数及 `FAILED` 状态。
6. 服务以独立进程组运行；停止时先发 SIGTERM，超时后发 SIGKILL。
7. 使用独立 SQLite 数据库保存服务定义和管理状态。
8. Supervisor 以 root 运行；每个服务可指定运行用户与用户组；Socket 和 API 必须实施严格访问控制。

## 3. V1.0 范围

### 3.1 必须实现

- 服务配置 CRUD、配置校验和持久化。
- 启动、停止、重启及生命周期状态。
- 自动重启策略、指数退避、次数限制、稳定运行计数和失败锁定。
- Supervisor 重启后的实例识别，避免盲目重复拉起。
- stdout/stderr 捕获、有限大小日志、尾部/增量日志读取。
- 主进程 PID、运行时长、退出原因、重启次数、CPU 与 RSS 展示。
- Unix Domain Socket IPC、对端身份验证、请求大小/并发/超时限制。
- EdgeDesk 现有认证体系保护的管理 API、审计记录和内置进程管理 UI。
- systemd、OpenRC、BusyBox init 的 Supervisor 启动适配。
- 服务运行身份隔离，明确配置用户、用户组和 root 特例。

### 3.2 不在 V1.0 范围

- 替代 PID 1 或完整替代 systemd。
- 容器编排、CI/CD、远程或集群进程部署控制平面。
- cgroups 资源配额、强制内存上限、CPU 配额及完整资源隔离。
- Socket Activation、复杂依赖图和服务启动顺序编排。
- 远程 Supervisor API、多用户角色体系。
- Shell 命令字符串、管道、命令替换、Shell 重定向解析。
- 自动发现或接管现有系统服务。
- 保证清理主动脱离原进程组的全部后代进程。
- 将进程管理能力开放给不受信任的扩展 iframe。

## 4. 架构与边界

```text
EdgeDesk React 内置应用
       │ 经过现有认证的 HTTP API
       ▼
EdgeDesk C++ Web 后端
       │ Unix Domain Socket
       ▼
edgedesk-supervisor（root，独立进程）
       ├── SQLite：服务定义、期望状态、恢复元数据
       ├── 生命周期管理与自动重启
       ├── 子进程组与运行身份管理
       ├── stdout/stderr 日志
       └── /proc 主进程指标

系统 init ──负责启动、停止及重新拉起──> edgedesk-supervisor
```

约束：

- Supervisor 不依赖 EdgeDesk Web 后端存活。
- EdgeDesk 后端不直接 fork、kill 或 wait 被托管服务；通过 IPC 调用 Supervisor。
- 浏览器和扩展 iframe 不直接访问 Unix Socket。
- 只有 Supervisor 自身注册到 init；业务服务由 Supervisor 直接管理。
- Supervisor 以前台模式运行，由 init 监管，不自行 daemonize。
- Supervisor 进程内不得把无界的进程等待、日志读取或 IPC 请求阻塞在 libhv 事件循环中。
- 复用项目现有工具与 SQLite 供应方式；新增依赖必须先说明理由、版本、许可证和 ARM/musl 影响。

## 5. 服务定义

每个服务具有稳定的系统生成 ID 和唯一名称。配置至少包含：

| 字段 | 约束 |
|---|---|
| `id` | 稳定唯一 ID，由服务端生成 |
| `name` | 非空、唯一，长度受限 |
| `executable` | 绝对路径；启动前验证为可执行文件 |
| `args` | 字符串数组，保持参数边界；不接受 Shell 命令字符串 |
| `cwd` | 已存在的绝对工作目录 |
| `env` | 显式键值映射；不记录秘密值到审计日志 |
| `run_user` | 用户名或 UID，必须显式指定 |
| `run_group` | 用户组名或 GID，必须显式指定 |
| `autostart` | 下次系统启动时是否自动启动 |
| `restart_policy` | `always`、`on-failure`、`never` |
| `max_restarts` | 重启时间窗口内允许的最大重启次数 |
| `restart_window_sec` | 重启次数统计窗口 |
| `restart_delay_ms` | 初始重启延迟 |
| `max_restart_delay_ms` | 指数退避最大延迟 |
| `stable_after_sec` | 稳定运行后重置连续失败计数的时间 |
| `stop_timeout_ms` | SIGTERM 后等待的宽限时间 |
| `log_max_bytes` | 单个服务日志文件大小上限 |

默认建议值：初始重启延迟 1 秒、最大延迟 60 秒、60 秒窗口内最多重启 5 次、稳定运行 300 秒后重置连续失败计数、停止宽限期 10 秒、单服务日志文件 10 MiB。它们是默认配置而非不可修改的常量，所有时间和数量必须进行范围校验。

安全及行为规则：

- 不调用 `system()`、`popen()` 或 Shell 解析命令。
- 不执行变量展开、通配符展开、命令替换或管道拼接。
- 参数数组中的空格和空字符串必须按数组语义传递。
- 不继承 Supervisor 的完整 root 环境；只使用受控基础环境和服务显式设置的环境变量。
- 必须在服务启动前校验可执行文件、工作目录、运行用户/组和配置权限。
- root 运行需要 UI 明确确认；不能将空的运行身份默认为 root。
- 修改运行中服务的配置不自动重启它，需用户显式执行重启。
- 删除运行中的服务前必须先停止，并在 UI 中明确告知操作结果。
- 环境变量、参数可能包含秘密；审计事件应记录字段是否改变，而非默认记录秘密内容。

## 6. 生命周期与重启策略

### 6.1 状态

- `STOPPED`：服务已停止。
- `STARTING`：正在启动。
- `RUNNING`：主进程运行中。
- `STOPPING`：正在停止。
- `BACKOFF`：等待下一次自动启动。
- `FAILED`：达到重启限制，自动拉起被锁定。
- `UNKNOWN`：无法安全确认既有进程实例的身份或状态。

### 6.2 重启策略

| 策略 | 语义 |
|---|---|
| `always` | 非人为停止导致进程退出后重新启动，包括退出码为 0 |
| `on-failure` | 非零退出码或被信号终止时重启；正常退出不重启 |
| `never` | 退出后不自动重启 |

采用指数退避和适度随机抖动，避免多个服务同时故障时形成重启风暴。超过重启限制后进入 `FAILED`；用户显式手动启动，或修改策略后确认重新启动，才解除锁定并重新计算计数。

稳定运行达到 `stable_after_sec` 后清除连续失败计数。重启计数和重启时间窗口必须持久化，以防 Supervisor 重启导致限制失效。

### 6.3 人工操作与开机启动

- 人工停止将当前系统 boot ID 下的期望状态设为 `STOPPED`，抑制自动拉起。
- 人工启动将当前 boot ID 下的期望状态设为 `RUNNING`，并解除 `FAILED` 锁定。
- Supervisor 进程自身重启时保留当前 boot ID 下的人工期望状态，不能把用户刚刚停止的服务再次拉起。
- 系统 boot ID 改变时，清除仅针对上一启动周期的人工覆盖，并依据 `autostart` 决定是否启动。
- `autostart=false` 不代表服务在当前系统启动周期内被人工启动后，Supervisor 自身重启时应将其停止。

### 6.4 进程组与信号

- 每个服务建立独立进程组，记录 PID、PGID、系统 boot ID 和可验证的进程启动时间。
- 停止时先向整个进程组发送 SIGTERM，在宽限期内等待；超时后发送 SIGKILL，并验证清理结果。
- 所有系统调用检查返回值；进程和文件描述符使用明确的 RAII/所有权管理。
- 不接受客户端传入的任意 PID 或 PGID 进行信号操作。
- 进程组不等于完整进程树隔离；主动 setsid 或切换进程组的后代不在绝对清理保证内。

## 7. Supervisor 异常恢复与防重复启动

Supervisor 可能在服务仍运行时崩溃或升级重启。恢复时必须先识别旧实例，再决定是否启动新实例。

- 保存系统 boot ID、PID、PGID、进程启动时间及最后已知生命周期状态。
- 只在同一系统 boot ID 下使用持久化运行身份进行恢复判断。
- 对仍存在的进程组，通过系统进程信息和已存身份进行校验。
- 只有确认旧实例不存在或已安全停止，才启动新实例。
- 如果身份无法可靠验证，标记为 `UNKNOWN`，不得向可能被复用的 PID/PGID 发信号，也不得直接启动第二份服务。
- 恢复过程幂等；重复执行不会创建第二份服务或丢失配置。
- Supervisor 数据库不可读、损坏或 schema 不兼容时，不得静默重建空数据库覆盖原配置。

验收必须包含杀死 Supervisor 但保留被托管进程的场景。若目标平台无法满足安全恢复条件，应明确报告限制，不得宣称完整恢复成功。

## 8. 持久化、日志与指标

### 8.1 SQLite

默认数据库：`/var/lib/edgedesk-supervisor/supervisor.db`，支持通过配置覆盖。

- 数据库仅由 Supervisor 使用，不与扩展平台的 `extensions.db` 共用。
- 数据目录默认权限 0700，数据库文件默认权限 0600。
- 服务配置、当前系统 boot ID 下的期望状态、重启计数、最近退出原因及恢复元数据必须持久化。
- schema 必须有版本号和可重复执行的迁移。
- 只读根文件系统必须将数据库放在明确的持久化可写分区。
- 数据库权限错误、损坏和版本不兼容时，启动失败或进入明确降级状态，并保留原始数据。

### 8.2 日志

- 捕获 stdout/stderr，使用服务独立的日志文件。
- 默认单服务日志文件上限 10 MiB；达到上限后执行有界截断/轮转策略，后续输出仍可写入。
- 支持尾部读取和基于游标的增量读取。
- UI/WebSocket 断开不影响日志采集。
- 日志读取和写入均有界；不能因慢客户端或日志洪流无限增长内存。
- 日志可能包含敏感数据，沿用管理员认证与访问控制。

### 8.3 指标

- 展示主进程 PID、运行时长、最近退出码或信号、重启次数、CPU 使用率和 RSS。
- CPU/RSS 通过 Linux `/proc` 采集，需处理进程退出和 PID 复用。
- V1.0 的 CPU/RSS 仅表示主进程，不代表整个进程组总量。
- 指标不可用时返回明确的 unavailable/null 状态，不得将缺失数据伪装为 0。

## 9. IPC 协议与安全

### 9.1 IPC 基线

- Linux `AF_UNIX` stream socket。
- 默认路径：`/run/edgedesk-supervisor.sock`，支持显式配置。
- 采用 JSON-RPC 2.0 消息语义，NDJSON 分帧（一行一个 JSON 对象）。
- 每个请求必须有 ID；响应包含结果或标准化错误。
- 单消息大小、连接数、请求超时、日志返回长度均有上限。
- 不创建 TCP listener，不开放 localhost HTTP 管理接口。
- Socket 父目录、所有权、权限与路径必须安全校验，不跟随恶意符号链接。

建议 RPC 方法：`health`、`service.list`、`service.get`、`service.create`、`service.update`、`service.delete`、`service.start`、`service.stop`、`service.restart`、`service.logs`。

### 9.2 权限

- Supervisor 以 root 运行。
- Socket 默认 root 持有，权限 0660；使用专用组授权 EdgeDesk 后端连接。
- 同时校验文件系统权限与 Linux `SO_PEERCRED` 中的 UID/GID，允许的对端身份必须显式配置。
- EdgeDesk Web API 继续执行现有管理员认证、CSRF、限流和审计机制。
- Supervisor API 只操作已登记的服务 ID；禁止任意 PID 信号、任意文件读写及 Shell 执行。
- root 服务必须在 UI 明确确认，普通服务必须以显式配置的用户/组启动。
- 扩展 iframe 不得直连 IPC，也不得获得等价的任意特权执行能力。

### 9.3 进程创建实现约束

服务管理器可能是多线程进程。实现者必须评估目标 libc 和 musl 工具链下的进程创建、补充组设置、UID/GID 切换与 exec 顺序；不得在多线程进程的 fork 子路径中执行未经验证的复杂 C++/锁操作。优先采用目标工具链已验证的 spawn/launcher 方案，并为失败的身份切换、exec 失败和子进程退出增加测试。任何需要改变本 SPEC 安全语义的实现方案必须先更新 ADR。

## 10. init 适配与部署

| init | 适配要求 |
|---|---|
| systemd | 提供 `edgedesk-supervisor.service`；以前台模式运行，配置自动重启和适当的启动限制 |
| OpenRC | 提供 OpenRC 服务脚本；在目标版本可用时使用受支持的监督机制，验证 Supervisor 退出后的恢复 |
| BusyBox init | 提供 init/inittab 配置示例，使用可用的 respawn 机制重新启动 Supervisor |

适配层只负责 Supervisor 自身的启动、停止、运行目录和重启，不为每个托管服务生成 init 单元。必须分别记录适配版本和实际验证环境；仅生成脚本不算平台验收通过。

## 11. EdgeDesk API 与 UI

### 11.1 HTTP API（规划契约）

所有接口继续使用现有 EdgeDesk 认证、CSRF 与统一错误响应。建议端点：

| 方法 | 路径 | 行为 |
|---|---|---|
| GET | `/api/supervisor/health` | 查询 Supervisor 连接和健康状态 |
| GET | `/api/supervisor/services` | 列表，可分页/按状态筛选 |
| POST | `/api/supervisor/services` | 创建服务 |
| GET | `/api/supervisor/services/{id}` | 查询详情 |
| PUT | `/api/supervisor/services/{id}` | 更新配置，不隐式重启 |
| DELETE | `/api/supervisor/services/{id}` | 删除已停止服务 |
| POST | `/api/supervisor/services/{id}/start` | 启动服务 |
| POST | `/api/supervisor/services/{id}/stop` | 停止服务 |
| POST | `/api/supervisor/services/{id}/restart` | 重启服务 |
| GET | `/api/supervisor/services/{id}/logs` | 获取日志尾部或游标后的增量内容 |

错误遵循当前 API 格式；建议增加 `supervisor_unavailable`（503）、`service_not_found`（404）、`service_running`（409）、`service_failed`（409）、`invalid_service_config`（400）。这些是拟新增错误码，正式实现前须与当前 `src/core/error.*` 核对。

IPC 调用必须使用有界等待与独立执行上下文，不能无限阻塞 libhv 事件循环。Supervisor 不可用时，EdgeDesk 其他功能必须继续工作。

### 11.2 内置 UI

新增“进程管理”内置应用，复用当前应用注册表、桌面图标、Dock、启动器、窗口生命周期和设计 Token。

- 服务列表：名称、状态、PID、运行时间、CPU/RSS、重启次数、开机自启。
- 创建/编辑表单：可执行文件、参数数组、工作目录、环境变量、运行用户/组、重启策略和时间参数。
- 详情：生命周期状态、退出原因、重启记录。
- 日志：尾部及增量刷新。
- 操作：启动、停止、重启、编辑、删除。

危险操作必须二次确认；root 身份须明确提示；错误显示实际原因。关闭窗口或刷新页面不能停止托管进程。

## 12. 验收标准

### 功能与生命周期

- 可创建、修改、查询、启动、停止、重启、删除服务。
- 参数中含空格、引号和空字符串时，目标进程收到的 argv 与配置一致。
- 未配置的环境变量不从 Supervisor 的 root 环境意外继承。
- 服务按指定 UID/GID 运行；无效身份配置启动失败且不残留子进程。
- SIGTERM 能使服务优雅退出；忽略 SIGTERM 时宽限期结束后发送 SIGKILL。
- 三种重启策略语义准确；退避有上限；超过限制进入 FAILED；稳定运行后按规则重置计数。
- 人工停止在 Supervisor 进程重启后仍保持停止；系统 reboot 后按 autostart 决策。
- 杀死 Supervisor 并保留子服务时，重启后不盲目重复拉起。
- 日志达到上限后文件大小受控且仍可写；慢客户端不会造成无界内存增长。
- Supervisor 不可用时，EdgeDesk 文件、终端、仪表盘、认证和扩展平台继续正常运行。

### 安全

- 无授权 UID/GID 不能连接或调用特权 Socket API。
- 没有 TCP 管理 listener。
- 不存在 Shell 字符串执行、任意 PID 信号和任意文件访问接口。
- Web API 未认证或 CSRF 校验失败时不能执行管理操作。
- 扩展 iframe 无法绕过宿主桥接调用 Supervisor。
- SQLite、Socket、日志文件权限符合约定。
- 任何测试不得通过跳过权限校验来方便集成。

### 平台与回归

- systemd、OpenRC、BusyBox init 分别提供真实验证记录。
- 完成 x86_64、ARM64 与 musl/static-pie 的适用构建验证。
- Buildroot 至少在实际目标环境或明确标注的模拟环境验证启动、停止与 respawn。
- 保留既有文件管理、终端会话、设备仪表盘、认证、设置、桌面布局和扩展平台行为。
- 测试结果必须记录实际执行命令；未执行的平台标记为“未验证”。
