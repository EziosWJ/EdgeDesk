# SPEC-0010：Process Supervisor 可靠性与安全加固

- 状态：**部分实现**（PSH-01～PSH-08 已实现并有自动化证据；PSH-09～PSH-11 未执行）
- 适用版本：EdgeDesk Process Supervisor V1.0 后续加固
- 基线：[SPEC-0009：本机进程托管](0009-process-supervisor.md)
- 相关决策：[ADR-0015](../adr/0015-process-supervisor-architecture.md)、[ADR-0016](../adr/0016-process-supervisor-security.md)
- 实施计划：[SPEC-0010 实施计划与 Ticket 拆分](../tasks/0010-process-supervisor-hardening.md)
- 总跟踪 Issue：[ #45 Process Supervisor 安全与可靠性加固](https://github.com/EziosWJ/EdgeDesk/issues/45)
- 验证依据：[测试与验证矩阵](../testing.md)

## 1. 目标

修复进程管理代码审查发现的进程组身份、后代清理、目录权限、状态持久化、崩溃恢复、长停止操作、脚本执行和审计日志问题；补齐尚未实际验证的高权限及平台行为。

本 SPEC 仅加固现有 Process Supervisor，不扩展产品功能，不重写架构。必须保留现有文件管理、终端、仪表盘、认证、设置、桌面和扩展平台功能。

## 2. 范围与非目标

### 范围

1. 避免对无法证明属于目标服务的进程组发送信号。
2. 主进程退出后，仍跟踪并清理其进程组内存活的后代。
3. 安全验证 Supervisor 数据目录路径，不修改路径祖先的权限。
4. 将 SQLite 持久化错误纳入启动、停止、重启和恢复状态机。
5. 恢复时尊重当前系统启动周期内持久化的期望状态。
6. 使长时间 stop/restart 操作可查询、可取消等待且不占用 Web 后端工作线程数分钟。
7. 明确支持 shebang 脚本的安全执行行为。
8. 避免日志轮询以高频率写入审计事件。
9. 补齐 root 身份切换、init、reboot 和压力场景的验证证据。

### 非目标

- 不替代 systemd、OpenRC 或 BusyBox init，也不实现完整 PID 1。
- 不保证清理主动调用 `setsid()` 或脱离原进程组的全部后代；此边界继续由 SPEC-0009 明确声明。
- 不引入不兼容旧目标 Linux 内核的强制依赖。需要使用新内核特性的方案必须提供旧内核兼容路径。
- 不修改现有认证模型或开放 TCP 管理 Socket。

## 3. 强制要求

### PSH-01：进程组身份与信号安全

当前 `process_group_has_members()` 仅以 PGID 和 `start_ticks >= minimum_start_ticks` 判断成员；该条件不足以证明扫描到的进程组仍属于原服务实例。检查成员与 `kill(-pgid, signal)` 之间还存在 TOCTOU 窗口。

要求：

- 进程组信号操作必须绑定到可验证的服务实例身份，至少考虑 boot ID、原始 PID/starttime、PGID、SID 和当前组成员证据；不得把“同一数字 PGID 下存在较新进程”作为归属证明。
- 设计必须明确处理进程组最后一个成员退出、PGID 可能被复用以及检查/发信号之间的竞态。不得仅增加一次相同的 `/proc` 扫描就宣称消除竞态。
- 如果在目标内核上无法可靠证明归属，必须安全拒绝信号并进入 `UNKNOWN`/人工介入路径，不得冒险向不确定的进程组发送 SIGTERM/SIGKILL。
- 保持对旧 Linux 内核的兼容性；不得无条件依赖 pidfd 等较新内核能力。
- 对原组存活、原组已消失、PGID 被复用、Supervisor 重启接管和扫描/信号竞态增加回归测试。

### PSH-02：主进程退出后的后代清理

当服务主进程退出，但同一受管进程组仍有成员存活时：

- 不得先清空 PID/PGID/starttime 并丢失清理目标。
- `restart_policy=never`、`on-failure` 不重启分支及人工停止分支均必须处理残留后代。
- 先按既定停止策略对可验证的原服务组执行 SIGTERM、宽限期、SIGKILL 和清理确认。
- 只有确认目标组已清理，才将运行身份元数据清零；需要自动重启时，必须先完成清理，再创建新实例。
- 如果无法安全识别或清理，保留可诊断状态并进入 `UNKNOWN`，不得启动第二份服务。
- 测试主进程退出、子进程继续运行的组合，覆盖三种重启策略及 Supervisor 重启。

### PSH-03：数据目录路径与权限安全

修复 `ensure_private_directory()` 的路径校验与 chmod 行为：

- 拒绝非绝对路径、空路径、`.`/`..` 路径分量和符号链接路径分量。
- 使用目录文件描述符及 `openat`/`mkdirat` 等安全方式逐级校验，避免路径替换竞态；若项目现有工具链限制实现方式，必须记录等效的安全方案。
- 只允许对明确配置的最终专用数据目录设置 `0700`；不得对 `/`、`/var`、`/var/lib` 等祖先目录调用 chmod。
- 检查最终目录的类型、属主和权限；失败时启动失败并输出可诊断错误，不得自动修改不属于 Supervisor 的目录。
- 测试 `/`、`/var`、`/var/..`、包含点路径分量、符号链接、错误属主、普通文件冒充目录和并发路径替换；测试必须断言系统目录权限没有变化。

### PSH-04：状态持久化失败的 fail-safe 语义

- `save()` 必须向调用方返回持久化结果；SQLite 写入失败不得只打印日志后继续报告操作成功。
- 服务启动前先持久化启动意图/STARTING 状态。父进程必须在子进程正式执行目标程序前获得启动握手，并成功持久化 PID、PGID、boot ID、starttime 等身份信息；若持久化失败，必须阻止子进程继续执行并回收它。
- 停止前先持久化 `desired_state=stopped`；持久化失败时不得报告停止意图已可靠提交。
- 发生数据库写入失败时，不得继续自动拉起新实例或覆盖更可信的旧身份数据。必须进入明确的降级/错误状态，记录原始错误并提供恢复诊断。
- 不能保证跨进程与 SQLite 的真正原子事务时，必须通过握手、补偿清理和幂等恢复建立可验证的安全顺序，并在 SPEC/ADR 中说明边界。
- 使用注入式 SQLite 写入失败/只读数据库/磁盘错误测试启动、停止、自动重启和 Supervisor 崩溃窗口；断言无孤儿进程、无重复实例且 API 不谎报成功。

### PSH-05：恢复时尊重期望状态

- 同一 boot ID 下恢复时必须优先读取并遵守持久化的 `desired_state`。
- `desired_state=stopped` 或持久化状态为停止进行中时，即使原进程仍存活，也不得直接恢复为稳定的 `RUNNING`；应验证身份并继续停止/清理流程。
- `desired_state=running` 时，只有验证原实例身份后才可接管；无法验证时进入 `UNKNOWN`，不重复启动。
- 新 boot ID 的行为继续按 SPEC-0009 的 `autostart` 规则执行。
- 增加在 STOPPING、BACKOFF 清理期间杀死 Supervisor，再启动 Supervisor 的集成回归测试。

### PSH-06：长操作的异步 API 与超时契约

当前服务允许 `stop_timeout_ms` 最大 600000 ms，而 IPC 客户端等待超时为 65000 ms。不得让合法停止操作超过客户端固定超时后表现为失败，同时后台继续执行。

- stop/restart 应采用明确的异步操作 ID/ticket 和查询接口（或提供同等可靠的非阻塞契约）。
- Web API 不应为等待最长数分钟的停止流程占用有限的工作线程；前端应显示进行中、成功、失败或超时状态。
- 客户端断开或页面刷新不得取消实际的服务停止/清理动作。
- 超时只表示调用方停止等待，不得被解释为操作已经失败或服务已停止。
- 明确操作 ID 的生命周期、结果保留、重复提交和幂等行为；既有接口字段能兼容时保持兼容，否则更新 API 文档和 UI。
- 测试短/最长停止超时、忽略 SIGTERM、IPC 客户端断开、后端线程池饱和和操作重复查询。

### PSH-07：shebang 脚本执行契约

- 支持带有效 shebang 的可执行脚本作为服务入口，同时保持 argv 数组执行，不经 Shell 拼接命令。
- 保留可执行文件 inode 校验，避免为支持脚本引入可执行路径的 TOCTOU 漏洞。
- 正确处理解释器不可用、无效 shebang、脚本权限不足和启动失败；错误信息必须指出实际原因，不得只返回误导性的 `ENOENT`。
- 审查 `O_CLOEXEC` 与 `fexecve()` 对脚本解释器的交互；任何需要继承文件描述符的方案必须限定继承范围，并测试服务不能访问非预期的 Supervisor FD。
- 增加 ELF 程序、`#!/bin/sh` 脚本、带参数解释器、缺失解释器、恶意参数字面量和失败清理测试。

### PSH-08：日志轮询与审计负载

- 被 UI 高频轮询的只读日志尾部/增量读取，不得每次请求都生成独立的持久化审计事件。
- 保留认证、授权和 CSRF 校验；保留服务创建/修改/启动/停止/重启/删除及 root 特权拒绝等安全事件审计。
- 如需审计日志读取，应按服务/会话做限频或聚合，不能随轮询频率无界增长。
- 明确记录审计事件的保留与聚合语义；不得记录日志正文或敏感环境变量。
- 增加模拟长时间 UI 轮询的测试，验证审计表增长受控且关键管理操作仍被记录。

### PSH-09：root 身份切换测试

- 在隔离测试环境以 root 真实启动 Supervisor，验证普通 UID/GID、补充组、root 特例及无效用户/组的行为。
- 注入 `setgroups`、`setgid`、`setuid`、工作目录切换和 exec 失败，验证错误可见且没有遗留子进程/FD。
- 验证普通服务确实以指定 UID/GID 运行，root 服务只有经过明确确认才允许启动。
- 测试必须不依赖关闭权限校验；宿主非 root 测试继续保留。

### PSH-10：init 与 reboot 平台验证

- systemd、OpenRC、BusyBox init 分别记录实际测试环境、版本、命令和结果。
- 验证安装、启动、停止、Supervisor 异常退出后的 respawn、卸载/回滚以及系统 reboot 后的 `autostart` 语义。
- OpenRC/BusyBox 仅通过 shell 语法或生成文件检查不算验收通过；必须在真实目标环境或明确标注的可复现虚拟环境运行。
- 不能完成的平台明确标记“未验证”，不得从 systemd 结果外推。
- 保持安装器最小依赖与旧 Linux/BusyBox 兼容性。

### PSH-11：压力与故障注入

- 覆盖高频 stdout/stderr、持续日志轮转、慢 IPC 客户端、断开连接、消息大小边界、队列饱和和多服务同时启停。
- 验证内存、线程、FD、SQLite 锁等待和日志磁盘占用有明确上界。
- 测试持续运行时无死锁、无无界队列、无丢失必要生命周期状态；记录负载参数、运行时间、峰值资源与结果。
- 对性能指标设定可重复的基线；没有数据时不得宣称通过压力验收。

## 4. 优先级与依赖

- **P0：PSH-01**。特权 Supervisor 不得向身份不确定的进程组发送信号。
- **P1：PSH-02～PSH-06、PSH-09、PSH-10**。影响服务隔离、数据完整性、生命周期正确性和生产发布证据。
- **P2：PSH-07、PSH-08、PSH-11**。需在本次发布候选完成前关闭；优先级较低不代表可以忽略。

建议依赖顺序：

1. PSH-01 与 PSH-03 先行。
2. PSH-02 依赖 PSH-01 的安全进程组判定。
3. PSH-04 先于 PSH-05、PSH-06；PSH-05 可与 PSH-06 并行。
4. PSH-07、PSH-08 可独立实施。
5. PSH-09～PSH-11 在相关代码修复合入后执行，期间可先建立测试环境。

## 5. 总体验收门槛

- 所有 PSH Ticket 的验收标准与自动化测试通过。
- L0/L1 测试在最终合并树执行并记录真实命令与结果。
- 相关 CMake、静态链接、脚本执行或平台适配变更必须运行适用的 musl/static-PIE、ARM64/QEMU 构建与集成测试。
- root 身份切换、真实 reboot、init respawn 和压力测试分别提供证据；未执行项目标记为“未验证”。
- 回归确认文件管理、终端、仪表盘、认证、设置、桌面与扩展平台不受影响。
- 不允许通过跳过权限校验、降低安全断言或只修改测试使其通过。
- 所有代码修改由独立 Ticket 跟踪；合入后更新本 SPEC、实施计划与 `docs/testing.md`，并把状态从 Proposed 改为对应实际状态。

## 6. 实施状态与证据（2026-10-23）

下列状态只反映实际执行过的命令与测试。宿主为 WSL2 Linux 6.18.54.1，x86_64，
集成测试以普通用户 `uid=1000` 运行；本轮没有以 root 启动 Supervisor，也没有执行
init/reboot/压力测试。

| Ticket | 状态 | 实现要点 | 已执行证据 |
|---|---|---|---|
| PSH-01 | 已实现 | `probe_service_group()` 三态判断（boot ID、leader starttime/PGID/SID、成员 session）；仅对 `verified` 发信号，`uncertain` 进入 `UNKNOWN` | `process_control_test`（leader 存活、starttime 不匹配、boot ID 不匹配、缺少 starttime、拒绝信号、组消失）；`supervisor_engine_test::test_recovery_unverifiable_identity` |
| PSH-02 | 已实现 | leader 退出后保留组身份；`never`/`on-failure`/`always`、人工 stop 与关闭均先清理后代；清理完成前不重启 | `test_descendant_cleanup_before_restart`（孤儿后代忽略 SIGTERM → SIGKILL → 新实例）；stop 路径由 `test_operation_contract`、`test_stop_escalates_to_sigkill` 覆盖 |
| PSH-03 | 已实现 | 绝对路径/空分量/`.`/`..`/NUL/符号链接拒绝；`openat`+`O_NOFOLLOW` 逐级打开；仅最终目录校验属主并 `fchmod 0700` | `supervisor_store_test`：root/相对路径、点分量、符号链接、普通文件、嵌套创建、系统祖先不被收紧、错误属主；未测试并发路径替换 |
| PSH-04 | 已实现 | 启动意图 → 阻塞子进程 → 持久化身份 → 释放；失败中止并回收；停止先持久化意图；持久化失败不自动拉起 | `test_persistence_failure_is_fail_closed`（身份写入失败无遗留子进程；停止意图失败不发送信号）；自动重启持久化失败路径已实现，未单独注入测试 |
| PSH-05 | 已实现 | 同 boot 恢复遵守 `desired_state`；中断的 `STOPPING`/`BACKOFF` 继续清理；身份不可验证 → `UNKNOWN` | `test_interrupted_stop_recovery`、`test_recovery_unverifiable_identity`、`test_manual_stop_survives_supervisor_restart`；集成 `after-supervisor-restart` 接管原 PID |
| PSH-06 | 已实现 | stop/restart 返回 `operation_id`；`service.operation.get` 与 `GET /api/supervisor/operations/{id}`；重复提交幂等；操作上限 256；客户端断开不取消清理 | `test_operation_contract`（pending/重复 ID/成功/未知 ID/后续新操作）、`test_operation_retention_bound`（300 次后最旧操作被裁剪）、`supervisor_ipc_test` 异步 stop、集成 stop/restart 轮询；未测试后端线程池饱和与 IPC 断开 |
| PSH-07 | 已实现 | 检查与执行使用同一 `O_RDONLY` inode，脚本经 `/proc/self/fd/<fd>`；解释器缺失/非法/非绝对路径 fail closed | `test_shebang_script_start`（`#!/bin/sh` 脚本与缺失解释器）；带参数解释器与 FD 继承范围未单独测试 |
| PSH-08 | 已实现 | 被动日志尾部/游标读取不写审计；管理操作与 root 拒绝仍审计 | 集成 `supervisor_test.py` 的 40 次轮询断言审计事件不增长，`integration_test.sh` 禁止 `supervisor.logs` 事件，`audit log sanity` 通过 |
| PSH-09 | **未验证** | 代码路径存在 | 未以 root 运行，未注入 `setgroups`/`setgid`/`setuid`/cwd/exec 失败 |
| PSH-10 | **未验证** | systemd 安装/启停此前已在测试机验证（见 `docs/testing.md`） | OpenRC/BusyBox 未真机执行；真实 reboot 后的 `autostart` 未验证 |
| PSH-11 | **未验证** | 队列、日志与连接均有代码上限 | 未执行长时间高频日志、慢 IPC 客户端、队列饱和或多服务并发压力测试 |

本轮实际执行的命令与结果：

- `scripts/test-fast.sh`：26/26 原生测试通过（含新增 `process_control_test`）。
- `scripts/test-pr.sh`：前端 27 个文件 / 155 个测试通过、生产构建成功、
  `dev-disk` 26/26 ctest 通过、完整集成套件通过，87 s。
- 集成套件中的 supervisor 全部阶段（crud、after-backend-restart、
  after-supervisor-restart、after-manual-stop-restart、unavailable）通过。

未执行：musl/static-PIE 与 ARM64/QEMU 重建（本轮未改 CMake、第三方库或平台适配），
root 身份切换，OpenRC/BusyBox 真机，真实 reboot，压力/故障注入。上述项目仍按
`docs/testing.md` 标注为未验证，不得视为通过。
