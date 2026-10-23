# ADR-0016：Process Supervisor 权限与进程执行安全

- 状态：**Accepted（V1.0 已实现；Ubuntu 26.04 systemd 上 root 身份切换、respawn 与真实 reboot 已验证；OpenRC/BusyBox 待验证）**
- 日期：2026-10-19
- 关联规格：[SPEC-0009：Process Supervisor](../spec/0009-process-supervisor.md)

## 背景

Supervisor 需要创建和终止系统进程，目标部署又包含 root 管理的 Linux 设备。其 API 是特权边界，不能把前端表单校验当作安全控制，也不能通过任意 Shell 命令字符串实现便利性。

## 决策

1. Supervisor 以 root 运行，以支持系统级服务管理。
2. 每个服务必须显式配置运行用户和用户组；root 运行需用户明确确认。
3. 启动输入采用绝对可执行文件路径和参数数组，不执行 Shell 解析。可执行文件只打开一次，
   并通过私有 staging 目录中指向该已验证 FD 的同名符号链接执行，既保留 `AT_EXECFN`
   语义又避免检查/执行路径替换。
4. Supervisor IPC 只使用 Unix Domain Socket；默认 root 所有、0660 权限，并使用专用组授权 EdgeDesk 后端。
5. 除文件系统权限外，必须检查 Linux `SO_PEERCRED` 的客户端 UID/GID，并显式配置允许的对端身份。
6. Web 管理 API 沿用 EdgeDesk 现有认证、CSRF、限流和审计机制。
7. 所有操作绑定服务 ID；不开放任意 PID/PGID 信号、任意文件读写或 Shell 执行接口。
8. 环境变量和参数中的秘密不得无差别写入审计日志。
9. 扩展 iframe 不得直接访问 Socket 或获得等价的特权执行能力。
10. 服务的路径、工作目录、运行身份、Socket 目录和持久化目录必须校验；不得依赖纯字符串路径前缀判断安全。

## 备选方案

- **Supervisor 以普通用户运行**：权限边界更小，但无法覆盖系统服务管理场景；不作为默认方案。
- **普通用户核心 + 特权辅助程序**：权限隔离更细，但增加跨发行版部署和 IPC 复杂度；可在后续版本评估。
- **Shell 命令字符串**：可表达管道和重定向，但容易产生解析歧义和命令注入风险；不采用。

## 风险与缓解

- **root 守护进程影响面大**：Socket 权限、peer credentials、Web 认证、服务 ID 校验和审计共同构成边界。
- **服务配置可导致任意 root 代码执行**：这是授予管理员创建 root 服务的固有能力；UI 必须明确警示，不能把 root 作为默认运行身份。
- **路径竞态与可执行文件替换**：在启动前验证路径和权限，只打开一次 inode；执行经私有
  staging 目录中的同名符号链接指向该 FD。staging 由 Supervisor 所有、不可被服务用户
  写入，避免仅依赖一次 `stat`。
- **身份哨兵（anchor）**：每个实例有一个关闭全部继承 FD、屏蔽可捕获信号、在 `setuid`
  前创建的 root sentinel；它不执行服务代码，服务用户无法终止它，从而消除 PGID 复用窗口。
- **多线程进程创建安全**：必须在目标 libc/musl 环境验证 spawn、补充组设置和 UID/GID 切换；不得在 fork 子路径执行未经验证的复杂 C++ 逻辑。
- **Socket 伪造或越权**：安全创建父目录、校验 Socket 类型/所有权，并同时校验 peer credentials。
- **日志泄密**：对环境变量、凭据和敏感参数进行脱敏或只记录变更元数据。

## 后果

安全策略比“本机可访问即可”更严格；部署时必须正确配置 EdgeDesk 后端的 UID/GID 与 Socket 访问组。实现不得为了测试便利绕过身份验证。
