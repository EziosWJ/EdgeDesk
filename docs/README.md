# EdgeDesk 文档索引

> 所有文档用于记录已确认的决策、需求与待确认问题。文档完成 != 功能完成。严禁 Agent 将原项目中已实现的功能视为 EdgeDesk 仓库已实现。

## ADR（为什么这样做）

| 文档 | 内容 | 状态 |
| --- | --- | --- |
| [ADR-0001](adr/0001-technology-stack.md) | C++17、GCC 15+、libhv、musl、`#embed` | 已确认基线 |
| [ADR-0002](adr/0002-architecture-security.md) | 业务模块分层、PTY 生命期与安全边界 | 设计提案 |

## Spec（要做什么）

| 文档 | 内容 |
| --- | --- |
| [SPEC-0001](spec/0001-product-scope.md) | 项目定位、V1.0 范围及验收 |
| [SPEC-0002](spec/0002-file-management.md) | 文件管理与文件工作区 |
| [SPEC-0003](spec/0003-terminal-workspace.md) | Bash、终端 Tab、刷新重连、浮动窗口 |
| [SPEC-0004](spec/0004-device-dashboard.md) | 设备基本资源与实时曲线 |

## 关键待确认项（不要擅自定案）

- 前端是否正式使用 React，是否采用 xterm.js。
- OpenHarmony ARM64 是否属于 V1.0 正式发行平台。
- 认证方式、HTTPS/TLS 接入策略、默认监听地址。
- 浏览器彻底关闭、显式关闭终端 Tab 后的后端会话保留策略；会话上限和空闲超时。
- 窗口最小化、跨刷新布局记忆等增强交互。
- 仪表盘刷新周期、曲线窗口和详细统计维度。
- CMake/Ninja 以及不支持 `#embed` 的工具链是否必须提供资源嵌入回退。

## 变更约定

1. 产品决策有变化：更新相关 Spec 与 ADR 的状态和日期。
2. 编程 Agent 实现时应提供可验证的验收证据，未验证的平台不得声明通过。
3. 将建议项改为“已确认”之前必须取得明确的产品决策。
4. V1.0 不引入数据库、ORM、云端设备管理控制平面或完整 SSH/SFTP 兼容。
