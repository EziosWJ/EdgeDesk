# ADR-0015：独立 Process Supervisor 架构

- 状态：**Accepted（V1.0 已实现；init/musl/ARM64 平台矩阵部分未验证）**
- 日期：2026-10-19
- 关联规格：[SPEC-0009：Process Supervisor](../spec/0009-process-supervisor.md)

## 背景

EdgeDesk 需要在 systemd、OpenRC、BusyBox init 等 Linux 环境中统一管理后台服务。若将自动拉起逻辑直接放进 Web 后端，Web 服务重启或崩溃就可能中断进程监控；若把每个业务服务映射成各发行版的原生服务单元，又会把核心业务逻辑绑定到特定 init 系统。

## 决策

1. 新增独立的 `edgedesk-supervisor` 守护进程。
2. EdgeDesk 后端通过本机 Unix Domain Socket 与其通信，不暴露 Supervisor TCP API。
3. Supervisor 独占自己的 SQLite 数据库，持久化服务定义、期望状态、重启计数和恢复元数据。
4. Supervisor 核心不依赖特定 init；通过 systemd、OpenRC、BusyBox init 适配层负责自身的开机启动和故障恢复。
5. 被托管服务由 Supervisor 直接启动，不为每个服务生成 systemd/OpenRC/BusyBox init 单元。
6. 以独立进程组实现 V1.0 的服务停止与基本清理，不要求 cgroups v2。
7. 不引入设备端 Node.js、Python、D-Bus 或新的常驻数据库服务。

## 备选方案

- **集成到 EdgeDesk 后端**：部署简单，但后端异常时会影响进程监督；不采用。
- **每个业务进程使用原生 init 单元**：可利用各系统的服务机制，但配置与行为不一致，无法形成统一 API；不采用。
- **依赖 systemd**：不适用于典型 BusyBox init 的 Buildroot 设备；不采用。
- **只提供 shell 启动脚本**：缺少统一生命周期、状态和恢复语义；不采用。

## 后果

优点：

- Web 后端升级/重启不影响托管进程。
- 业务进程管理行为统一，减少发行版差异。
- 可以为 Mihomo 和 EdgeDesk 后续本机服务提供共用基础设施。

代价：

- 需要实现状态机、持久化、重启策略、日志与恢复识别。
- 需要维护并实际验证多种 init 适配。
- 进程组不是完整的进程树隔离；cgroups 与复杂服务依赖留待后续版本。

## 约束

- SPEC-0009 是用户可见行为的单一事实来源。
- Supervisor 不可用不能导致 EdgeDesk 其他功能退出。
- 数据库错误不能通过静默清空数据库恢复。
- 不得将尚未实现或未验证的能力标记为已实现。
