# EdgeDesk 文档索引

> 所有文档用于记录已确认的决策、需求、实现状态与待确认问题。文档完成 != 功能完成。
> 功能完成情况以代码、自动化测试与 [验证矩阵](testing.md) 的实际结果为准。

## 当前实现（V1.0）

- [架构、线程模型与资源边界](architecture.md)
- [API 与 WebSocket 协议](api.md)
- [配置与部署](config.md)
- [测试与验证矩阵](testing.md)（含各平台真实验证结果与未验证项）
- [Coding Agent 开发规范](../AGENTS.md)
- [Phase 1 工程骨架（历史记录）](phase1.md)

## ADR（为什么这样做）

| 文档 | 内容 | 状态 |
| --- | --- | --- |
| [ADR-0001](adr/0001-technology-stack.md) | C++17、GCC 15+、libhv、musl、`#embed`、React + xterm.js | 已确认基线 |
| [ADR-0002](adr/0002-architecture-security.md) | 业务模块分层、PTY 生命周期与安全边界 | 已确认 |
| [ADR-0003](adr/0003-v1-auth-api-protocol.md) | 认证（密钥 + 会话 Cookie）、API 面、实时协议、限流与审计 | 已确认 |
| [ADR-0004](adr/0004-path-sandbox.md) | 目录 FD + openat 路径沙箱、符号链接策略 | 已确认 |
| [ADR-0005](adr/0005-terminal-architecture.md) | PTY、线程边界、会话生命周期与重连 | 已确认 |
| [ADR-0006](adr/0006-system-metrics.md) | 指标采集、内存窗口与增量推送 | 已确认 |
| [ADR-0007](adr/0007-frontend-icon-library.md) | 前端图标库：lucide-react（ISC，版本锁定，全站迁移） | 已确认（2026-10-17） |

## Spec（要做什么）

| 文档 | 内容 |
| --- | --- |
| [SPEC-0001](spec/0001-product-scope.md) | 项目定位、V1.0 范围及验收 |
| [SPEC-0002](spec/0002-file-management.md) | 文件管理与文件工作区 |
| [SPEC-0003](spec/0003-terminal-workspace.md) | Bash、终端 Tab、刷新重连、浮动窗口 |
| [SPEC-0004](spec/0004-device-dashboard.md) | 设备基本资源与实时曲线 |

## 设计素材

- [设计资源索引](design/README.md)
- [浏览器素材画廊](design/asset-gallery.html)
- [素材引用与 #embed 打包说明](design/asset-usage.md)
- [四页原型拆解及视觉规范](design/prototype-guidelines.md)

## 开发环境

| 文档 | 内容 | 状态 |
| --- | --- | --- |
| [本地 GCC 15 工具链（免 sudo）](dev/gcc15-local-toolchain.md) | Ubuntu 24.04 上用 `scripts/install-gcc15-local.sh` 安装 GCC 15 到用户目录 | 已验证（2026-10-16） |
| [测试与验证矩阵](testing.md) | 各平台构建/运行/交叉验证的真实结果 | 持续更新 |

## 仍然待确认（不影响 V1.0 已实现范围）

- TLS 是否内置终止（当前由外部网关终止）。
- OpenHarmony ARM64 是否作为正式发行平台。
- 多用户/角色、会话持久化是否需要。
- 浮动窗口布局在跨刷新后的自动恢复策略（当前仅恢复会话列表，不自动重开窗口）。

## 变更约定

1. 产品决策有变化：更新相关 Spec 与 ADR 的状态和日期。
2. 编程 Agent 实现时应提供可验证的验收证据，未验证的平台不得声明通过。
3. 将建议项改为“已确认”之前必须取得明确的产品决策。
4. V1.0 不引入数据库、ORM、云端设备管理控制平面或完整 SSH/SFTP 兼容。
