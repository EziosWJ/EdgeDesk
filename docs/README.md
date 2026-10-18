# EdgeDesk 文档索引

> 所有文档用于记录已确认的决策、需求、实现状态与待确认问题。文档完成 != 功能完成。
> 功能完成情况以代码、自动化测试与 [验证矩阵](testing.md) 的实际结果为准。

## 当前实现（V1.2）

- [架构、线程模型与资源边界](architecture.md)
- [API 与 WebSocket 协议](api.md)
- [配置与部署](config.md)
- [测试与验证矩阵](testing.md)（含各平台真实验证结果与未验证项）
- [Coding Agent 开发规范](../AGENTS.md)
- [Phase 1 工程骨架（历史记录）](phase1.md)

## V1.2 桌面工作区

- [SPEC-0006：桌面工作区与应用窗口管理](spec/0006-desktop-workspace.md)（已实现）
- [ADR-0009：桌面工作区与应用窗口管理架构](adr/0009-desktop-workspace-architecture.md)（已实现）
- [ADR-0010：桌面布局持久化与终端会话恢复](adr/0010-desktop-persistence-and-recovery.md)（已实现）
- [V1.2 实施计划与 Issue 拆分](tasks/0006-desktop-workspace.md)
- [桌面快捷键与焦点规则](design/desktop-keyboard-shortcuts.md)

## 扩展应用平台（计划中，尚未实现）

- [SPEC-0007：扩展应用平台与静态 Web 应用运行时](spec/0007-extension-applications.md)：完整记录第 1～98 轮已确认决策及验收标准。
- [ADR-0011：扩展应用包、运行时与数据存储](adr/0011-extension-runtime-and-packages.md)
- [ADR-0012：扩展应用权限、通信与网络安全边界](adr/0012-extension-security-and-communication.md)
- [ADR-0013：扩展文件 API、分块传输与并发一致性](adr/0013-extension-file-api-and-transfer.md)
- [扩展应用安全风险清单](security/extension-app-risk-register.md)：已接受风险、强制安全边界和发布门槛。
- [扩展应用分阶段实施计划](tasks/0007-extension-applications.md)：阶段、建议 Issue 依赖、验收与发布门槛。

> 本节文档仅冻结需求与技术决策；扩展运行时、安装器、SQLite 管理器和文件桥接尚未因此实现。不得将计划文档当作功能完成证明。

## ADR（为什么这样做）

| 文档 | 内容 | 状态 |
| --- | --- | --- |
| [ADR-0001](adr/0001-technology-stack.md) | C++17、GCC 15+、libhv、musl、#embed、React + xterm.js | 已确认基线 |
| [ADR-0002](adr/0002-architecture-security.md) | 业务模块分层、PTY 生命周期与安全边界 | 已确认 |
| [ADR-0003](adr/0003-v1-auth-api-protocol.md) | V1.0 认证（密钥 + 会话 Cookie）、API 面、实时协议、限流与审计 | 历史契约（V1.1 由 ADR-0008 取代登录方式） |
| [ADR-0004](adr/0004-path-sandbox.md) | 目录 FD + openat 路径沙箱、符号链接策略 | 已确认 |
| [ADR-0005](adr/0005-terminal-architecture.md) | PTY、线程边界、会话生命周期与重连 | 已确认 |
| [ADR-0006](adr/0006-system-metrics.md) | 指标采集、内存窗口与增量推送 | 已确认 |
| [ADR-0007](adr/0007-frontend-icon-library.md) | 前端图标库：Lucide（ISC，全站 UI）+ VS Code Icons 按需编译（文件类型） | 已确认（2026-10-17） |
| [ADR-0008](adr/0008-authentication-and-settings.md) | V1.1 单管理员密码认证、首次初始化、独立 API Token 与系统设置 | 已实现（V1.1.0） |
| [ADR-0009](adr/0009-desktop-workspace-architecture.md) | 桌面壳层、应用注册表、统一窗口管理与自研交互 | 已实现（V1.2.0） |
| [ADR-0010](adr/0010-desktop-persistence-and-recovery.md) | 浏览器本地桌面布局、窗口恢复与终端会话重连 | 已实现（V1.2.0） |
| [ADR-0011](adr/0011-extension-runtime-and-packages.md) | 扩展应用包、静态 iframe 运行时、SQLite 局部存储与版本修订 | 已确认；待实施 |
| [ADR-0012](adr/0012-extension-security-and-communication.md) | 扩展权限快照、JSON-RPC/MessagePort、CSP 与网络安全边界 | 已确认；含已接受风险 |
| [ADR-0013](adr/0013-extension-file-api-and-transfer.md) | 扩展文件 API、1 MiB 分块、原子上传与文件锁 | 已确认；待实施 |

## Spec（要做什么）

| 文档 | 内容 |
| --- | --- |
| [SPEC-0001](spec/0001-product-scope.md) | 项目定位、V1.0 范围及验收 |
| [SPEC-0002](spec/0002-file-management.md) | 文件管理与文件工作区 |
| [SPEC-0003](spec/0003-terminal-workspace.md) | Bash 单页面终端、会话管理、刷新重连、浮动窗口 |
| [SPEC-0004](spec/0004-device-dashboard.md) | 设备基本资源与实时曲线 |
| [SPEC-0005](spec/0005-authentication-and-settings.md) | V1.1 管理员认证、首次初始化、系统设置与验收 |
| [SPEC-0006](spec/0006-desktop-workspace.md) | V1.2 桌面图标、Dock、启动器、窗口管理、持久化与恢复 |
| [SPEC-0007](spec/0007-extension-applications.md) | 扩展应用 ZIP 包、安装/审批/升级/卸载、iframe 沙箱、宿主 API、KV 与文件传输；含第 1～98 轮决策台账 |

## 实施计划

| 文档 | 内容 |
| --- | --- |
| [SPEC-0005 开发任务拆分](tasks/0005-authentication-and-settings.md) | V1.1 认证与设置的实现决策、任务依赖、测试矩阵与发布门槛 |
| [SPEC-0006 V1.2 实施计划](tasks/0006-desktop-workspace.md) | 桌面化分阶段实施、Issue 拆分、通用完成定义与发布门槛 |
| [SPEC-0007 扩展应用平台实施计划](tasks/0007-extension-applications.md) | Phase 0～9、建议 Issue 依赖、验收标准与发布门槛（待实施） |

## 设计素材

- [V1.2 独立页面原型总览](design/prototypes/index.html)：桌面、启动器、设备概览、文件管理、终端、设置与组件素材
- [原型本地运行说明](design/prototypes/README.md)
- [浏览器素材画廊](design/asset-gallery.html)：品牌、插画和装饰 SVG
- [素材使用指南](design/asset-usage.md)：资源清单、React 引用、打包与验收
- [原型实现规范](design/prototype-guidelines.md)：视觉 Token、NAS 风格资源小组件和功能/展示边界
- [原型还原度对照表](design/prototype-conformance.md)：视觉验收清单
- [桌面快捷键与焦点规则](design/desktop-keyboard-shortcuts.md)：按键映射、输入/终端焦点保护和窄屏行为

可复用素材位于 [web/public/assets](../web/public/assets/)，其中新增桌面壁纸与应用图标 sprite 位于 [web/public/assets/desktop](../web/public/assets/desktop/)；设计 Token 位于 [web/src/styles/edgedesk-tokens.css](../web/src/styles/edgedesk-tokens.css)。

> 静态原型使用演示数据，不连接设备 API，也不代表功能已实现。生产 UI 必须使用真实 API/WebSocket 数据。

## 开发环境

| 文档 | 内容 | 状态 |
| --- | --- | --- |
| [本地 GCC 15 工具链（免 sudo）](dev/gcc15-local-toolchain.md) | Ubuntu 24.04 上用 scripts/install-gcc15-local.sh 安装 GCC 15 到用户目录 | 已验证（2026-10-16） |
| [测试与验证矩阵](testing.md) | 各平台构建/运行/交叉验证的真实结果 | 持续更新 |

## 仍然待确认（不影响 V1.1 已实现范围）

- TLS 是否内置终止（当前由外部网关终止）。
- OpenHarmony ARM64 是否作为正式发行平台。
- V1.1 首期是否开放高级部署配置的网页写入（当前决定为只读展示，后续逐项评估）。
- 多用户/角色不在当前规划内；会话不跨服务进程重启持久化。
- V1.2 桌面布局已在浏览器 localStorage 中恢复（schema v1，见 ADR-0010）；跨浏览器同步不在范围内。

## 变更约定

1. 产品决策有变化：更新相关 Spec 与 ADR 的状态和日期。
2. 编程 Agent 实现时应提供可验证的验收证据，未验证的平台不得声明通过。
3. 将建议项改为已确认之前必须取得明确的产品决策。
4. V1.0 不引入数据库、ORM、云端设备管理控制平面或完整 SSH/SFTP 兼容。