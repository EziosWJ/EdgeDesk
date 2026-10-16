# ADR-0002：服务架构、会话生命周期与安全边界

- 状态：**Proposed（建议架构，待实施时确认细节）**
- 日期：2026-10-16
- 关联：[终端与工作区](../spec/0003-terminal-workspace.md)、[文件管理](../spec/0002-file-management.md)

## 已确认的架构约束

1. 服务端是 C++ 单进程应用，以 libhv 提供 HTTP 和 WebSocket。
2. 文件管理、Bash 终端和硬件监控应能独立组织代码，不因 UI 路由变化而中断后台作业。
3. **PTY 会话生命周期不得绑定到 React 组件挂载、终端 Tab 可见性或单条 WebSocket 连接。**
4. 页面导航或刷新后，前端必须能够通过会话标识重新关联存活的终端；具体认证与恢复协议待设计。
5. 设备端 V1.0 无数据库、无 ORM；实时监控数据可在内存中维护短时缓存。

## 推荐分层（尚未作为实现定案）

```text
React（候选）/ 浏览器
       │ HTTP + WebSocket
HTTP Router / Auth / WS Transport（libhv）
       │
FileService   TerminalSessionManager   HardwareMonitorService
       │              │                     │
Filesystem/OS APIs   Bash/PTY              /proc, /sys
```

- Route 负责输入校验、认证和 HTTP/WS 适配，不负责业务实现。
- Service 不直接暴露 libhv 内部类型，减少网络库对文件与终端逻辑的耦合。
- OS/Platform 封装不同系统的文件描述符、PTY、信号和指标采集。
- 优先使用 RAII 管理 Socket、FD、子进程与线程/任务资源，所有缓存和并发数量必须有上限。

## 必须遵循的安全约束

- 对文件 API、终端 API、WebSocket 握手实行统一认证、会话授权；校验 WebSocket Origin。
- 文件路径限定在配置的根目录内；单纯 `realpath` 再打开可能存在 TOCTOU 风险，设计时采用基于目录 FD 的安全解析，必要时结合可用内核的 `openat2`。
- 文件 API 的根目录限制**不能**被解释为 Bash 终端的系统级沙箱。终端实际权限由执行用户和可选隔离机制决定。
- CmdGuard 黑名单只能辅助减少误操作，**不能**构成执行命令的安全边界。
- 默认最小权限、默认不直接暴露公网；远程访问应通过 TLS/VPN/可信网关保护。
- 限制上传体积、HTTP 请求和 WS 连接数量、PTY 会话数及输出缓存；禁止在事件循环线程中执行无界阻塞任务。
- `/terminal/exec` 如保留，应默认关闭；严格处理参数，禁止通过字符串拼接调用 shell。
- 结构化日志不得泄露口令、令牌或输入中的敏感信息；审计不能声称完整还原 Bash 实际命令。
- 在只读根文件系统下必须能使用明确的可写临时/运行目录；不要假设根目录可写。

## 待决定

- 认证方式（Token、HTTP Basic over TLS 或其他）、部署监听默认值。
- PTY 会话的最大数量、断线保留超时、浏览器彻底关闭后的行为。
- TLS 采用内置实现还是仅由外部安全网关终止。
- 不支持 Bash 的设备是否直接报错（建议）还是允许显式兼容模式。

上述“待决定”不得由 Coding Agent 直接当作已经批准的产品决策。
