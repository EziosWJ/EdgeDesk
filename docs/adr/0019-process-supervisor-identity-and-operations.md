# ADR-0019：Process Supervisor 进程身份、异步操作与持久化顺序契约

- 状态：**Accepted**（2026-10-23；2026-10-23 补充 anchor 身份哨兵、exec staging 与持久化标记）
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

每个受管服务在启动时记录 `boot_id`、原始 PID、PGID、内核 `start_ticks`，以及一个
**身份 sentinel（anchor）** 的 PID/starttime。anchor 由服务子进程在 `setsid()` 之后、
切换到服务身份之前通过 `clone(CLONE_PARENT|SIGCHLD)` 创建，因此它是 Supervisor 的
直接子进程，但继承服务会话/PGID。它：

- 关闭除 commit 管道外的全部继承 FD（包括 Supervisor 的 IPC socket、SQLite、日志
  和客户端连接），读取 commit 后再关闭该 FD；`PR_SET_NAME("edgedesk-anchor")`；
- 屏蔽全部可捕获信号，只在 commit 管道收到字节后永久 `pause()`；若服务子进程在
  `exec` 之前退出/被回收，commit 管道 EOF 使 anchor 自行退出；
- 是 root 进程（在 `setuid` 前创建），因此非 root 服务不能杀死它；服务为 root 时
  该边界由 `allow_root` 显式信任承担。

所有归属判断统一使用 `probe_service_group()`，返回四态：

| 状态 | 含义 | 允许的动作 |
|---|---|---|
| `verified` | boot ID 一致；原 leader（若仍可见）的 PID/starttime/PGID/SID 与记录一致；anchor 身份一致且存活；所有存活非 anchor 成员满足 `session == pgid`、`start_ticks >= leader_start_ticks` | 允许 SIGTERM/SIGKILL |
| `anchor_only` | 服务进程已全部退出，anchor 仍存活并保留 PGID | 只允许 SIGKILL（回收 anchor） |
| `absent` | 没有存活的非僵尸成员，anchor 也不存活 | 允许清理身份元数据、启动新实例 |
| `uncertain` | boot ID 缺失/变化、`/proc` 不可读、leader/anchor 身份不匹配、anchor 消失但仍有成员、成员越界 | 拒绝信号，进入 `UNKNOWN`，等待人工介入 |

关键点：

- PGID 数字相同、或“组内存在较新进程”不构成归属证明。
- **anchor 存活期间内核保留该 PGID**（进程组成员持有 group `struct pid` 引用），
  因此 `probe → kill(-pgid)` 之间不存在“组变空后被复用”的窗口。anchor 不会自行
  退出，只有 Supervisor 的 SIGKILL 或服务子进程在 exec 前退出（EOF）能结束它。
- 缺少 `start_ticks`、boot ID、`/proc` 证据或 anchor 身份时 fail closed。
- 主进程退出后，在确认整组 `absent` 之前保留 PID/PGID/starttime；三种重启策略、
  人工停止和 Supervisor 关闭都必须先完成清理再清零身份或启动第二份实例。
  `anchor_only` 时立即 SIGKILL 回收 sentinel，不再等待 stop_timeout。
- **旧实例兼容边界**：升级前启动、没有 anchor 记录的实例只在已核验 leader 仍存活时
  允许发信号（leader 本身保留 PGID）；leader 已退出、只剩后代的组返回 `uncertain`，
  不冒险发信号。下一次启动会创建 anchor。

anchor 的创建/回收不依赖 pidfd 或新内核特性；`close_range` 不可用时回退为有界
`close()` 循环。SIGKILL 之后必须再次 probe 确认 `absent` 才完成操作。

### 2. 启动握手与持久化顺序（PSH-04）

服务启动遵循固定顺序，避免 API 谎报成功后留下无身份的孤儿：

1. 持久化 `STARTING` / `desired_state=running`；失败则完全不创建子进程。
2. `fork` 子进程后，子进程在 `setsid` 后、切换身份前创建 anchor，并通过 report
   管道报告其 PID；随后子进程切换身份并阻塞在握手管道上，尚未 `exec`。
3. 父进程读取子进程与 anchor 的 `start_ticks`，**一次写入** PID/PGID/boot ID/starttime
   和 anchor PID/starttime。
4. 持久化成功后才向子进程写入释放字节；失败则关闭握手端、SIGKILL 并 `waitpid`
   回收子进程与 anchor。
5. `exec` 失败通过独立的 error pipe 返回真实 `errno`；子进程退出前不会被报告为成功。
6. anchor 在 `exec` 前收到 commit 字节后转为永久阻塞；若子进程在 commit 前退出，
   commit 管道 EOF 使 anchor 自行退出，不留下孤儿。

停止/重启同样先持久化 `desired_state=stopped`（或重启意图）再发信号。
持久化失败时返回错误、不发送信号、不自动拉起新实例，并在数据目录写入
`.persistence-inconsistent` 持久标记；下一次 Supervisor 启动时按 §4 处理该标记，
成功归一所有记录后才清除。`STARTING` 且没有 PID/starttime 的记录按“中断启动”处理，
不会因陈旧的 `desired_state=running` 自动启动。

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
- 已完成的操作用于查询保留，内存中最多保留 256 条；裁剪跳过 pending 并优先驱逐最旧的
  已完成操作。pending 数量受每个服务至多一个 stop/restart 约束；Supervisor 重启后不保留
  历史操作。
- 操作超时（`operation_timeout`）只表示查询方停止等待，不表示停止/清理已失败或已取消。
  WebSocket/HTTP 断开、页面刷新、后端重启都不会取消守护进程内的清理动作。
- 停止/清理完成前，服务保持 `STOPPING`/`BACKOFF`；成功后才回到 `STOPPED`。

### 4. 恢复优先级（PSH-05）

同一 boot ID 下，Supervisor 重启时按以下顺序处理持久化记录：

1. `desired_state=stopped`：即使原进程仍存活也不恢复为 `RUNNING`；验证身份后继续
   停止/清理流程（包括中断的 `STOPPING`/`BACKOFF`；`anchor_only` 直接 SIGKILL）。
2. `desired_state=running`：只有身份可验证时才接管（`RUNNING`）；否则 `UNKNOWN`，
   不启动第二份实例。
3. PID 存在但没有 `start_ticks`、boot ID 不匹配、anchor 身份不匹配或组内成员无法验证：
   `UNKNOWN`。
4. `STARTING` 且从未持久化 PID/starttime：视为中断启动，标记 `FAILED`，不自动启动。
5. 存在 `.persistence-inconsistent` 标记：禁用本次启动的所有 autostart/自动重启；
   只有身份可验证的实例可被接管（不会新建进程）。全部记录成功归一后清除标记。
6. 新 boot ID：按 `autostart` 决定是否启动，旧身份全部清除。

`UNKNOWN` 是安全的终止状态：不自动信号、不自动重启；只有确认进程组不存在后，
stop 才能清除它。

### 5. shebang 脚本执行（PSH-07）

- 入口文件只打开一次（`O_RDONLY|O_CLOEXEC|O_NONBLOCK`），格式检查和执行使用同一个
  inode；路径在检查与执行之间被替换不会改变执行内容。
- 直接 `fexecve()`/`execveat(AT_EMPTY_PATH)` 会让 `AT_EXECFN` 和 `comm` 变成 FD 号，
  Ubuntu 26.04 的 coreutils multicall 等程序会据此分派 applet 并失败。因此启动前在
  Supervisor 私有的 staging 目录中创建名为**原 basename** 的符号链接，目标是
  `/proc/self/fd/<fd>`，然后 `execve(链接路径)`：`AT_EXECFN` 保留正确文件名，inode
  仍是已验证的 FD。
- staging 基目录为 `/run/edgedesk-supervisor-exec`（root，0711，允许非 root 服务穿越）
  或 `/tmp/edgedesk-supervisor-exec-<uid>`（非 root，0700）；每个进程使用独立的
  `run-<pid>-<seq>` 子目录。链接在实例停止/删除/Supervisor 退出时清理；崩溃遗留的
  链接指向已关闭的 FD，不构成运行依赖。
- 解释器必须存在、是常规可执行文件、绝对路径且不是组/世界可写；shebang 超过内核
  255 字节上限时直接报错。`argv` 始终是数组，未经 Shell 拼接。
- 非 shebang 的 `ENOEXEC`/不可执行文件继续按原有错误路径失败。

### 6. 审计负载（PSH-08）

- 被动只读端点（health/list/get/log tail/cursor）不逐请求写审计事件；日志轮询由
  UI 每约 2 秒触发，逐请求审计会无界增长。
- 创建/修改/删除/启动/停止/重启、认证/授权失败和 root 特权拒绝仍写入审计。
- 审计记录不含日志正文、环境变量或凭据。

### 7. 数据目录、数据库与日志（PSH-03）

- 配置路径必须是绝对路径，不含空分量、`.`、`..`、NUL 或符号链接分量。
- 使用 `openat`/`mkdirat` 从 `/` 逐级打开目录 FD，`O_NOFOLLOW | O_DIRECTORY`，
  后续组件相对已打开的 FD 解析，避免路径替换。
- 只对最终配置目录校验属主并收紧到 `0700`；祖先目录只读打开，绝不 `chmod`。
- SQLite 通过 `/proc/self/fd/<dirfd>/<basename>` 打开，目录 FD 在连接生命周期内保持
  打开，因此启动时的路径替换不能把数据库或其 journal/WAL/SHM 重定向到别处。SQLite 在打开时
  会把路径规范化为真实路径，运行中重命名目录会使后续写事务失败（fail closed），
  绝不会写入替换后的路径。数据库文件要求常规文件、属主为 Supervisor、`0600`。
- 日志目录同样固定为打开时的目录 FD；`openat`/`renameat`/`unlinkat` 全部相对该 FD，
  拒绝符号链接、非普通文件和 FIFO（`O_NONBLOCK` 防止 FIFO 阻塞）。
- `.persistence-inconsistent` 标记也通过目录 FD 创建/删除并 `fsync`。

## 后果

- 身份不确定时服务进入 `UNKNOWN`，需要运维确认进程组不存在后再 stop/start；
  这是有意的可用性取舍。
- 每个运行实例多一个 root sentinel 进程；它不执行任何业务代码、读完 commit 后关闭全部 FD、
  不计入服务进程数/指标。`anchor_only` 状态会让 stop 立即 SIGKILL 回收它。
- 升级前启动的旧实例没有 anchor：只能在其 leader 仍可验证时安全停止；后代单独存活
  时进入 `UNKNOWN`，需要先人工清理，重启后恢复完整保证。
- 可执行文件 staging 目录在 `/run`（root）或 `/tmp`（非 root）；崩溃可能遗留一个
  只含悬空符号链接的小目录，不影响运行，实例停止时正常清理。
- 操作对象不持久化：Supervisor 重启后无法查询旧操作，但服务状态与
  `desired_state` 仍然持久化，清理会被恢复逻辑继续执行。
- 持久化标记存在时，自动启动/自动重启被禁用；重启 Supervisor 只会归一记录并清除标记，
  不会自动启动服务，恢复需要一次成功的手工 start。
- 被动读取不再留下审计痕迹；需要读取审计时只能依赖管理操作事件与认证日志。
