# ADR-0005：终端会话架构（PTY、线程边界与生命周期）

- 状态：**Accepted（已确认）**
- 日期：2026-10-16
- 关联：[终端 Spec](../spec/0003-terminal-workspace.md)、[ADR-0002](0002-architecture-security.md)、[API 文档](../api.md)

## 背景

终端必须连接真实 Bash 登录 Shell，会话生命周期不得绑定 React 组件、Tab 可见性或单条 WebSocket 连接；刷新、导航、短暂断线后必须可以重连；设备资源受限，输出缓存、线程和进程数量都必须有界。

## 决策

### 1. 进程创建

- 使用 `forkpty(3)` 创建 PTY master + 子进程，子进程 `execve("/bin/bash", {"/bin/bash", "-l", ...})`。
- 若 `/bin/bash` 不存在（`access(X_OK)` 检查）：创建会话返回明确错误，**绝不回退到 `/bin/sh`**。
- 子进程设置：`setsid`（由 forkpty 完成）、工作目录为请求且经沙箱校验的目录（无沙箱配置时为运行用户 HOME），继承服务进程环境并显式设置 `TERM=xterm-256color`、`COLORTERM=truecolor`、`LANG`（若缺失则 `C.UTF-8`）。
- 不加载 `~/.bashrc` 的假设：`bash -l` 的登录语义按 Bash 官方规则加载 `/etc/profile` 与第一个存在的 `~/.bash_profile|~/.bash_login|~/.profile`。

### 2. 线程模型

- `TerminalManager` 持有会话注册表（`std::unordered_map<std::string, std::shared_ptr<TerminalSession>>`，mutex 保护）。
- 每个 `TerminalSession` 拥有**一个专用读取线程**：`poll(master_fd, 200ms)` → `read()` → 有界回放缓冲 + 广播给已连接 WebSocket channel。
  - 读取线程是 master FD 的唯一读取者；`write` / `resize` / 关闭信号可由任意线程发起。
  - 短超时轮询使关闭/空闲检查不需要额外的唤醒机制，并且 `poll` 不阻塞网络事件循环。
- 网络事件循环线程只做：解析 WS 消息、投递到会话（`write` 为小数据量且 PTY 主端写缓冲足够）、发送输出（`WebSocketChannel::send` 内部加锁且线程安全）。
- `TerminalManager` 不创建额外后台线程；空闲回收由 libhv 事件循环上的定时器触发（默认 30 s 检查一次，只做标记，实际终止由读取线程完成）。

### 3. 会话生命周期

| 事件 | 行为 |
| --- | --- |
| WS 断开 / 页面刷新 / 切换 Tab | 会话与 Bash 继续运行，回放缓冲保留最近输出 |
| WS 重连（同一 session_id） | 先补发回放缓冲（截断到上限），再继续实时输出 |
| 用户显式关闭（HTTP close 或 WS `close` 消息） | 终止 Bash（SIGHUP，超时后 SIGKILL）、回收 PTY FD、移除会话 |
| Bash 自行退出 | 会话标记 `exited`，保留元数据直到显式关闭或空闲回收；重连会收到退出状态 |
| 无客户端连接且无 IO 超过 `--terminal-idle-timeout`（默认 1800 s） | 自动关闭并审计 |
| 服务关闭 | `TerminalManager::shutdown()` 终止全部会话并 join 所有读取线程 |

### 4. 有界资源

- 每会话回放缓冲上限 `--terminal-backlog`（默认 256 KiB，环形丢弃最旧数据并在重连流首部插入丢弃提示）。
- 会话总数上限 `--max-terminal-sessions`（默认 8），超出返回 `409`。
- WS 发送：单次 `send` 前检查 `channel->writeBufsize()`，超过阈值（2 MiB）时断开该连接（避免慢客户端拖垮设备内存）。
- 会话 ID 为 128 位随机十六进制，不可预测；所有权绑定到同一认证主体（V1.0 为单一部署密钥，因此所有已认证客户端可访问同一会话集合，这是单用户产品的明确取舍）。

### 5. 协议

- 见 [API 文档](../api.md) 的 WebSocket 章节：`attach/create/input/resize/close/ping` ↔ `attached/output/exit/error/pong`。

## 影响

- `command_guard` 只作为“明显危险操作”的提示层（回显警告），不构成安全边界；权限边界是运行用户与系统策略。
- 测试需要真实 `/bin/bash`；CI 环境已具备。PTY 相关测试在无 `forkpty` 的平台上会被跳过（当前目标平台均支持）。
