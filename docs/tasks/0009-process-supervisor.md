# SPEC-0009 实施计划：EdgeDesk Process Supervisor

- 状态：**V1.0 implemented（Phase 0–4 完成；Phase 5 平台矩阵部分未验证）**
- 需求基线：[SPEC-0009](../spec/0009-process-supervisor.md)
- 架构决策：[ADR-0015](../adr/0015-process-supervisor-architecture.md)、[ADR-0016](../adr/0016-process-supervisor-security.md)
- 工程规范：[AGENTS.md](../../AGENTS.md)
- 测试基线：[测试与验证矩阵](../testing.md)

> 本文保留最初的分阶段计划。PS-01～PS-09 的功能代码、宿主原生测试、集成测试和前端
> 单元测试已落地；PS-10 的 systemd/OpenRC/BusyBox 真机、musl/static-pie 与
> ARM64/QEMU 验收尚未在当前环境执行，必须按 `docs/testing.md` 标记为未验证。
> 建议的 GitHub Issue 拆分没有实际创建，工作直接在分支 `feat/process-supervisor-v1.0` 完成。

## 1. 实施原则

- 先实现独立守护进程和可靠生命周期，再接入 Web UI。
- 保留 EdgeDesk 现有文件、终端、指标、认证、设置、桌面与扩展功能。
- 不把 Supervisor 实现为扩展应用，也不允许扩展 iframe 直接访问特权 IPC。
- 不在 libhv 事件循环中执行无界等待或阻塞的进程/日志操作。
- 每个阶段先增加失败测试，再实现功能。
- 所有平台验证必须记录实际执行命令和环境；未验证的平台必须明确标注。
- 不直接创建 GitHub Issues；需要用户确认实施节奏后再建立 Issue/PR。

## 2. 阶段计划

### Phase 0 — 文档与工程边界

交付：

- SPEC-0009、ADR-0015/0016 和本实施计划。
- 更新文档索引、AGENTS.md、产品范围、架构、API、配置和测试文档。
- 定义 IPC、服务模型、错误码和独立构建目标。
- 确认 SQLite 复用现有 vendored 依赖的方式，不引入新的设备端运行时。

验收：

- 文档链接与编号有效。
- Supervisor SQLite 是独立模块的明确局部例外；不迁移认证、设置、终端或指标。
- 规划中的能力没有被描述为已实现。
- 既有 API 与现有功能范围不被无意更改。

### Phase 1 — Supervisor 骨架与存储

- PS-01：独立二进制、CLI、配置校验、健康检查、日志。
- PS-02：SQLite schema/迁移、服务配置 CRUD、原子事务和错误处理。

验收：

- Supervisor 可独立启动、停止。
- 服务配置跨 Supervisor 重启持久化。
- 数据目录与数据库权限符合规范。
- 数据库损坏、权限不足和 schema 版本不兼容时不静默清空数据。
- 原有 EdgeDesk 构建和测试不回归。

### Phase 2 — 进程生命周期

- PS-03：直接执行 argv、运行用户/组、工作目录、受控环境、进程组、SIGTERM/SIGKILL。
- PS-04：状态机、重启策略、指数退避、重启限流、稳定计数与 Supervisor 崩溃恢复。

验收：

- 参数边界准确；不经过 Shell。
- 服务按配置 UID/GID 运行；身份切换失败时无残留子进程。
- 服务停止按 SIGTERM → 超时 → SIGKILL 工作。
- always/on-failure/never、FAILED 锁定与手动恢复符合 SPEC。
- 人工停止状态在 Supervisor 进程重启后保持。
- 杀死 Supervisor 而保留服务进程时，恢复逻辑不盲目启动第二份实例。

### Phase 3 — IPC、日志、指标和 init

- PS-05：有界 stdout/stderr 日志、增量读取、日志上限、主进程 CPU/RSS。
- PS-06：Unix Socket、JSON-RPC 2.0/NDJSON、peer credentials、请求限制。
- PS-07：systemd、OpenRC、BusyBox init 适配。

验收：

- 没有 TCP 管理监听。
- 未授权 UID/GID 无法调用管理接口。
- 日志上限生效且服务输出不会因 Web 页面断开而停止。
- Socket/数据库异常不会导致资源泄漏或无界等待。
- 每种 init 适配都有独立验证记录；只生成脚本不算通过。

### Phase 4 — EdgeDesk 集成

- PS-08：新增后端 IPC Client、认证 REST 路由、错误映射和审计。
- PS-09：新增内置进程管理应用与表单、列表、操作确认和日志视图。

验收：

- 未认证或 CSRF 校验失败的请求不能执行特权操作。
- UI 可以完成服务 CRUD、启停、重启、查看状态和日志。
- Supervisor 不可用时只影响进程管理功能，其他 EdgeDesk 功能继续可用。
- 刷新/关闭进程管理窗口不影响托管服务。
- 桌面、Dock、启动器和窗口状态继续通过现有注册表与 Store 管理。

### Phase 5 — 安全、可靠性和发布

- PS-10：综合回归、压力/故障注入、跨平台构建、init 真机验证和部署文档。
- 完成适用的 L0/L1/L2 测试矩阵。
- 复核特权边界、进程恢复、日志限制、数据库迁移和权限失败路径。

验收：

- 核心行为有自动化回归测试。
- 既有文件、终端、指标、认证、桌面和扩展平台回归通过。
- 发布所需的 musl/static-pie 与 ARM64/QEMU 测试通过。
- 未执行的平台清楚标记“未验证”，不得用宿主构建代替真实平台验收。

## 3. 建议 Issue 拆分

| ID | 建议标题 | 优先级 | 依赖 | 主要交付物 |
|---|---|---|---|---|
| PS-01 | 建立独立 Supervisor 二进制与配置入口 | P0 | 无 | 独立目标、CLI、健康检查 |
| PS-02 | 实现 Supervisor SQLite 服务存储 | P0 | PS-01 | schema、迁移、服务 CRUD |
| PS-03 | 实现服务启动、身份切换与进程组停止 | P0 | PS-01 | argv 执行、UID/GID、信号管理 |
| PS-04 | 实现生命周期状态机与自动恢复 | P0 | PS-02、PS-03 | 重启策略、退避、失败锁定、崩溃恢复 |
| PS-05 | 实现有界日志与进程指标 | P1 | PS-03、PS-04 | stdout/stderr、日志读取、CPU/RSS |
| PS-06 | 实现安全 Unix Socket IPC | P0 | PS-02、PS-04 | JSON-RPC、peer credentials、限流 |
| PS-07 | 实现 systemd/OpenRC/BusyBox init 适配 | P0 | PS-01、PS-04 | 启动、停止、respawn |
| PS-08 | 集成 EdgeDesk 后端 API 与审计 | P0 | PS-06 | 认证路由、错误映射、审计 |
| PS-09 | 实现进程管理桌面应用 | P1 | PS-08 | 服务列表、表单、操作、日志 UI |
| PS-10 | 完成可靠性、安全和平台矩阵验收 | P0 | PS-01～PS-09 | 自动化测试、平台记录、发布门槛 |

PS-01～PS-07 中，进程执行与 IPC 的具体实现可以在代码结构稳定后适度并行，但不得在接口契约未定时各自定义冲突的数据模型。

## 4. 通用 Issue 完成定义

1. 先补充能够复现失败场景的测试。
2. 变更聚焦，不重写既有模块或公开 API。
3. 检查所有系统调用与错误路径；FD、Socket、SQLite 连接和子进程均有明确所有权。
4. 不允许无界队列、线程或日志缓存。
5. 更新受影响的 SPEC/ADR/architecture/API/config/testing 文档。
6. L0：执行 `scripts/test-fast.sh`。
7. L1：影响运行行为时执行 `scripts/test-pr.sh`。
8. 影响 CMake、工具链、依赖或发布资源模式时执行项目规定的 L2。
9. 任何失败测试、未运行测试和未验证平台均如实记录。
10. 不创建与本计划无关的 Issue；不把全部工作塞入一个大型 PR。

## 5. 发布门槛

- 功能：服务 CRUD、启动/停止/重启、三种重启策略、重启限流、日志、状态与手动恢复全部通过。
- 安全：无 Shell 字符串执行、无任意 PID 接口、无未授权 Socket 访问、无未认证 Web 特权操作。
- 可靠性：Supervisor 重启不盲目重复启动服务；数据库损坏不导致静默丢配置；日志大小受控。
- 回归：文件管理、Bash 终端、仪表盘、认证、设置、桌面窗口与扩展应用全部通过。
- 平台：systemd、OpenRC、BusyBox init 各自记录实际测试环境；Buildroot、musl、ARM64 按发布目标完成验证。
