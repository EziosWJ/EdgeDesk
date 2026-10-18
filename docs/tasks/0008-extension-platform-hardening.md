# SPEC-0008 实施计划：扩展应用平台安全与可靠性加固

- 状态：**Planned（GitHub Issues 已创建；实现尚未开始）**
- 需求基线：[SPEC-0008](../spec/0008-extension-platform-hardening.md)
- 前置基线：[SPEC-0007](../spec/0007-extension-applications.md)
- 架构决策：[ADR-0011](../adr/0011-extension-runtime-and-packages.md)、[ADR-0012](../adr/0012-extension-security-and-communication.md)、[ADR-0013](../adr/0013-extension-file-api-and-transfer.md)
- 风险清单：[扩展应用风险清单](../security/extension-app-risk-register.md)
- 测试基线：[测试与验证矩阵](../testing.md)
- 原则：只修复已识别的实现缺陷和构建问题；保留 EdgeDesk 既有业务功能。改变已确认的产品取舍必须先走 ADR，不得由编码 Agent 静默决定。

## 1. Ticket 拆分

本计划拆分为一个总任务和八个可独立验收的实现 Ticket。总任务为 [#16](https://github.com/EziosWJ/EdgeDesk/issues/16)，实现 Ticket 为 [#17](https://github.com/EziosWJ/EdgeDesk/issues/17)～[#24](https://github.com/EziosWJ/EdgeDesk/issues/24)。

| ID | 建议标题 | 优先级 | 依赖 | 主要范围 |
| --- | --- | --- | --- | --- |
| T-00 | [#16 — 扩展平台安全与可靠性加固总任务](https://github.com/EziosWJ/EdgeDesk/issues/16) | P0 | 无 | 汇总修复、追踪依赖与发布门槛 |
| T-01 | [#17 — 阻止扩展文件 API 访问平台内部数据](https://github.com/EziosWJ/EdgeDesk/issues/17) | P0 | 无 | 文件安全边界、受保护路径与别名测试 |
| T-02 | [#18 — 修复扩展网络权限与来源策略执行](https://github.com/EziosWJ/EdgeDesk/issues/18) | P0 | 无 | 授权驱动 CSP、升级来源复核、实际浏览器导航验证 |
| T-03 | [#19 — 完整性清单覆盖实际资源并修正隔离范围](https://github.com/EziosWJ/EdgeDesk/issues/19) | P1 | 无 | 未登记文件拒绝服务、实际文件集合比对、历史修订隔离语义 |
| T-04 | [#20 — 保证重装失败时应用 KV 数据不丢失](https://github.com/EziosWJ/EdgeDesk/issues/20) | P1 | 无 | 数据删除与安装状态提交事务化、失败/崩溃恢复 |
| T-05 | [#21 — 控制扩展包安装与静态资源服务的内存占用](https://github.com/EziosWJ/EdgeDesk/issues/21) | P1 | 无 | 流式处理、减少元数据副本、静态资源传输设计与峰值内存验证 |
| T-06 | [#22 — 统一清理过期扩展会话及关联资源](https://github.com/EziosWJ/EdgeDesk/issues/22) | P1 | 无 | 终端、传输、临时文件、锁与事件订阅的幂等清理 |
| T-07 | [#23 — 对扩展错误上报与审计信息进行脱敏](https://github.com/EziosWJ/EdgeDesk/issues/23) | P2 | 无 | 敏感值脱敏、URL 参数处理、日志结构与长度限制 |
| T-08 | [#24 — 修复 x86_64-musl 与 ARM64-musl 的 ZLIB 依赖](https://github.com/EziosWJ/EdgeDesk/issues/24) | P0 | 无 | 目标 sysroot、CMake 查找、静态链接和完整 CI |

> T-00 是协调/验收总任务；T-01 至 T-08 已分别建立独立 GitHub Issue，实施时各自使用独立 PR。不要把所有改动塞进一个大 PR。

## 2. 建议实施顺序

### 第一批：发布阻断项

1. T-01 文件 API 内部路径保护。
2. T-02 网络权限和来源策略执行。
3. T-08 musl 交叉构建依赖恢复。

三个任务可以并行，但每个任务都必须先补充失败用例，再实现修复。T-01 和 T-02 属于安全边界问题，必须通过负向测试证明绕过路径已关闭；T-08 必须由 CI 实际跑绿证明。

### 第二批：数据完整性与资源生命周期

4. T-03 完整性清单和隔离范围。
5. T-04 重装失败的数据保留。
6. T-06 运行会话及关联资源清理。

这些任务均需增加可重复的故障注入或状态机回归测试，不接受仅在正常路径上测试成功。

### 第三批：资源占用与审计质量

7. T-05 包处理与静态资源内存占用。
8. T-07 审计脱敏。

T-05 先基于现有 libhv API 评估可用的流式响应机制。若需要改变现有资源限制决策，提交 ADR，不得自行引入固定 ZIP 总大小/文件数上限或 JSON-RPC 消息上限。

## 3. 每个 Ticket 的通用交付要求

- 在实现前阅读 AGENTS.md、SPEC-0007、SPEC-0008、相关 ADR、风险清单和当前测试。
- 先写失败测试，确认测试确实能复现目标缺陷，再修改实现。
- 改动保持聚焦，优先复用 PathSandbox、FileService、既有授权模型、操作日志和统一运行时清理机制。
- 不修改无关 UI、桌面窗口行为、原生应用功能或公开 API。
- 代码与测试完成后运行 L0；涉及安全、文件系统、数据事务、运行时生命周期的任务必须运行 L1。
- 修改 CMake、工具链、依赖或静态链接配置的任务必须运行对应 L2；合并前全矩阵必须通过。
- 更新测试文档及风险状态；未实际执行的测试标记为“未验证”，不得沿用旧记录声称当前提交全矩阵通过。

## 4. 分项验收清单

### T-01：平台内部数据保护

- 受保护路径列表集中维护并有文档说明。
- 读、写、创建、覆盖、列举、重命名、删除和别名访问均被阻止。
- 路径解析使用既有目录 FD/openat/PathSandbox 安全机制；不能只用字符串前缀或 realpath。
- 管理员原有文件管理器与可配置 file root 的行为不回退。
- 增加原生或集成回归测试，验证请求失败后目标文件内容和元数据不变。

### T-02：网络权限和来源策略

- CSP 的外部 source 与有效 network 授权及当前修订 manifest 一致。
- 升级新增来源进入明确的管理员复核流程。
- 直接 window.open、target=_blank、表单导航和 iframe 导航有真实浏览器测试。
- 保留已确认的 HTTP、重定向和 opener 风险决策；不能修复的浏览器边界必须记录残余风险并提交 ADR。
- 无授权时拒绝外部资源；已授权来源按契约工作。

### T-03：完整性与隔离

- 清单与实际文件集合完全一致。
- 未登记文件不能通过静态资源服务返回。
- 活动修订被篡改会触发既有隔离行为。
- 历史修订校验失败不误伤健康的活动修订。
- 修复后状态、审计与管理 UI 一致。

### T-04：KV 数据事务

- 不在安装/重装最终提交前删除原 KV。
- 任一提交前失败都保留原 KV 和原活动修订。
- “保留并恢复”“保留但不恢复”“删除”三种用户选择均有测试。
- 故障注入覆盖 ZIP、文件系统、SQLite 事务和进程恢复。

### T-05：内存与资源占用

- 解压与哈希按块进行，不缓存全部解压文件内容。
- 减少 ZIP 元数据与完整性清单的重复副本。
- 静态资源优先使用流式/分块响应；若受 HTTP 层限制，提供技术证据、可复现的峰值内存测试和 ADR/后续方案。
- 不增加未经批准的固定 ZIP 大小或文件数限制。
- 磁盘不足、失败中断和临时目录清理均有测试。

### T-06：运行资源清理

- 所有失效路径使用统一、幂等的资源清理语义。
- 终端关闭不在持有运行时互斥锁时触发可能重入的回调。
- 过期会话关联的上传/下载、临时文件和锁被清理。
- 并发过期请求与重复关闭无死锁、崩溃或资源残留。

### T-07：审计脱敏

- 不记录原始 Cookie、Bearer Token、API Key、密码或完整请求正文。
- URL query/fragment 的敏感值被移除或脱敏。
- 错误消息与堆栈有长度限制并进行结构化编码。
- 测试同时验证秘密不存在和日志仍可正常解析、排障信息仍可用。

### T-08：musl 构建

- x86_64-musl 与 ARM64-musl 使用目标工具链匹配的 ZLIB。
- CMake 不误用宿主机头文件或库。
- 两个目标成功配置、编译和链接。
- frontend、host-build、gcc15-embed、x86_64-musl-static、arm64-cross 全部通过。

## 5. 发布门槛

扩展平台硬化完成前，不得将本轮标记为发布验收通过。总任务 T-00 关闭前，必须满足：

- T-01 至 T-08 全部关闭，或存在经明确审批的 ADR/风险接受记录；安全发布阻断项不得以普通风险接受替代修复。
- L0、L1 与相关 L2 均实际执行并通过。
- 当前 main 的 GitHub Actions 全矩阵为绿色；不能用之前 PR 的通过记录替代当前提交结果。
- docs/testing.md、docs/extension-platform-contracts.md 和 docs/security/extension-app-risk-register.md 与最终实现状态一致。
