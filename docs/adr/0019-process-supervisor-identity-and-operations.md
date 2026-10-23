# ADR-0019：Process Supervisor 进程身份、异步操作与持久化顺序契约

- 状态：**Accepted**（2026-10-23）
- 适用版本：Process Supervisor V1.0 加固（SPEC-0010 PSH-01～PSH-08）
- 需求基线：[SPEC-0010](../spec/0010-process-supervisor-hardening.md)、[SPEC-0009](../spec/0009-process-supervisor.md)
- 相关决策：[ADR-0015](0015-process-supervisor-architecture.md)、[ADR-0016](0016-process-supervisor-security.md)
- 验证依据：[测试与验证矩阵](../testing.md)

## 背景

SPEC-0010 修复了进程组身份校验、后代清理、持久化失败语义、崩溃恢复、长操作 API、
脚本执行与审计负载。这些修复共享一组跨模块契约：什么时候可以向进程组发信号、
子进程何时可见、stop/restart 的结果如何表达、Supervisor 重启后如何恢复。
本 ADR 记录这些已接受的决定与其边界，避免后续实现重新引入同一类竞态。

## 决策

### 1. 进程组身份模型（PSH-01/PSH-02）

每个受管服务在启动时记录 `boot_id`、原始 PID、PGID、内核 `start_ticks`。
所有归属判断统一使用 `probe_service_group()`，返回三态：

| 状态 | 含义 | 允许的动作 |
|---|---|---|
| `verified` | boot ID 一致，原 leader（若仍可见）的 PID/starttime/PGID/SID 与记录一致，且所有存活组成员满足 `session == pgid`、`start_ticks >= leader_start_ticks` | 允许 SIGTERM/SIGKILL |
| `absent` | 组内没有任何存活的非僵尸成员 | 允许清理身份元数据、启动新实例 |
| `uncertain` | boot ID 缺失/变化、`/proc` 不可读、leader 身份不匹配、成员越界或缺少可验证身份 | 拒绝信号，进入 `UNKNOWN`，等待人工介入 |

关键点：

- PGID 数字相同、或“组内存在较新进程”不构成归属证明。
- `setsid()` 使服务成为会话首进程，SID == PGID == 原始 leader PID。只要会话仍有成员，
  内核就保留该 SID 数字，因此“成员 `session == pgid`”把后代锚定到原实例。
- 缺少 `start_ticks`、boot ID 或 `/proc` 证据时 fail closed，不得回退为仅比较 PGID。
- 主进程退出后，在确认整组 `absent` 之前保留 PID/PGID/starttime；三种重启策略、
  人工停止和 Supervisor 关闭都必须先完成清理再清零身份或启动第二份实例。
- 不依赖 pidfd：目标内核可能较旧；pidfd 可作为未来的无竞态增强，但不是当前前提。

**残余竞态（已接受）**：`probe → kill(-pgid)` 之间进程组可能变为空，随后数字 PGID
被新进程组复用。同一类风险还出现在原 leader 已退出、数字 SID 被新的 `setsid()`
进程复用时：内核此时只能提供“会话号 == 记录值”的数字证据。当前实现无法在旧内核上
原子地消除这些窗口，缓解措施是：

1. 组为空只在最后一个成员退出后发生；内核按 PID 分配顺序复用，窗口极短；
2. leader 仍可见时要求其 starttime/PGID/SID 与记录完全一致；
3. 发信号失败后立即重新 probe，不使用旧结论重试；
4. SIGKILL 后必须再次 probe 确认 `absent` 才完成操作，否则进入 `UNKNOWN`；
5. 绝不以数字 PGID 单独判断归属。

更严格的方案（`pidfd_open` + `pidfd_send_signal`）需要新内核与 PGID 级 pidfd 支持，
留作后续增强；在此之前以 fail-closed 的三态判断为契约。

### 2. 启动握手与持久化顺序（PSH-04）

服务启动遵循固定顺序，避免 API 谎报成功后留下无身份的孤儿：

1. 持久化 `STARTING` / `desired_state=running`；失败则完全不创建子进程。
2. `fork` 子进程后，子进程在 `setsid`/身份切换后阻塞在握手管道上，尚未 `exec`。
3. 父进程读取子进程 `start_ticks`，持久化 PID/PGID/boot ID/starttime。
4. 持久化成功后才向子进程写入释放字节；失败则关闭握手端、SIGKILL 并 `waitpid` 回收。
5. `exec` 失败通过独立的 error pipe 返回真实 `errno`；子进程退出前不会被报告为成功。

停止/重启同样先持久化 `desired_state=stopped`（或重启意图）再发信号。
持久化失败时返回错误、不发送信号、不自动拉起新实例，并记录可诊断错误。

### 3. stop/restart 异步操作契约（PSH-06）

`service.stop` 与 `service.restart` 立即返回操作对象，不再阻塞 IPC 客户端或 Web 工作线程：

```json
{
  "operation_id": "op-12-1792421400000",
  "service_id": "…",
  "kind": "stop",
  "state": "pending",
  "result": null,
  "error": null,
  "created_at": 1792421400,
  "finished_at": null
}
```

- `state` 为 `pending`、`succeeded` 或 `failed`；`result` 为成功后的服务对象，
  `error` 为 `{slug, message}`。
- 通过 `service.operation.get` / `GET /api/supervisor/operations/{id}` 查询。
- 同一服务同一类型的 pending 操作重复提交返回同一个 `operation_id`；
  人工 stop 会取代 pending restart。
- 已完成的操作用于查询保留，内存中按 256 条上限裁剪；Supervisor 重启后不保留历史操作。
- 操作超时（`operation_timeout`）只表示查询方停止等待，不表示停止/清理已失败或已取消。
  WebSocket/HTTP 断开、页面刷新、后端重启都不会取消守护进程内的清理动作。
- 停止/清理完成前，服务保持 `STOPPING`/`BACKOFF`；成功后才回到 `STOPPED`。

### 4. 恢复优先级（PSH-05）

同一 boot ID 下，Supervisor 重启时按以下顺序处理持久化记录：

1. `desired_state=stopped`：即使原进程仍存活也不恢复为 `RUNNING`；验证身份后继续
   停止/清理流程（包括中断的 `STOPPING`/`BACKOFF`）。
2. `desired_state=running`：只有身份可验证时才接管（`RUNNING`）；否则 `UNKNOWN`，
   不启动第二份实例。
3. PID 存在但没有 `start_ticks`、boot ID 不匹配或组内成员无法验证：`UNKNOWN`。
4. 新 boot ID：按 `autostart` 决定是否启动，旧身份全部清除。

`UNKNOWN` 是安全的终止状态：不自动信号、不自动重启；只有确认进程组不存在后，
stop 才能清除它。

### 5. shebang 脚本执行（PSH-07）

- 入口是脚本时，Supervisor 以 `O_RDONLY | O_CLOEXEC` 打开被检查的同一 inode，
  通过 `/proc/self/fd/<fd>` 作为 `execve` 的路径；文件在检查与执行之间被替换不会
  改变执行内容。
- 解释器必须存在、是常规可执行文件且为绝对路径；错误信息指出实际原因，
  不返回误导性的 `ENOENT`。
- 其余 FD 保持 `O_CLOEXEC`，只继承解释器所需的脚本 FD；argv 始终是数组，
  不经过 Shell 拼接。
- 非 shebang 的 `ENOEXEC`/不可执行文件继续按原有错误路径失败。

### 6. 审计负载（PSH-08）

- 被动只读端点（health/list/get/log tail/cursor）不逐请求写审计事件；日志轮询由
  UI 每约 2 秒触发，逐请求审计会无界增长。
- 创建/修改/删除/启动/停止/重启、认证/授权失败和 root 特权拒绝仍写入审计。
- 审计记录不含日志正文、环境变量或凭据。

### 7. 数据目录（PSH-03）

- 配置路径必须是绝对路径，不含空分量、`.`、`..`、NUL 或符号链接分量。
- 使用 `openat`/`mkdirat` 从 `/` 逐级打开目录 FD，`O_NOFOLLOW | O_DIRECTORY`，
  后续组件相对已打开的 FD 解析，避免路径替换。
- 只对最终配置目录校验属主并收紧到 `0700`；祖先目录只读打开，绝不 `chmod`。
- 失败时启动失败并输出可诊断错误，不自动修改不属于 Supervisor 的目录。

## 后果

- 身份不确定时服务进入 `UNKNOWN`，需要运维确认进程组不存在后再 stop/start；
  这是有意的可用性取舍。
- 操作对象不持久化：Supervisor 重启后无法查询旧操作，但服务状态与
  `desired_state` 仍然持久化，清理会被恢复逻辑继续执行。
- 旧内核上的 probe→kill 竞态未能完全消除，已作为残余风险记录；后续可引入 pidfd
  作为增强，不改变三态判断和 fail-closed 语义。
- 被动读取不再留下审计痕迹；需要读取审计时只能依赖管理操作事件与认证日志。
