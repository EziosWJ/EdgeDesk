# SPEC-0007 实施计划：扩展应用平台

- 状态：**Implemented（Phase 0～9 已完成；宿主 L0/L1、集成与真实浏览器已验证；L2 平台矩阵待 CI）**
- 需求基线：[SPEC-0007](../spec/0007-extension-applications.md)
- 架构决策：[ADR-0011](../adr/0011-extension-runtime-and-packages.md)、[ADR-0012](../adr/0012-extension-security-and-communication.md)、[ADR-0013](../adr/0013-extension-file-api-and-transfer.md)
- 风险：[扩展应用风险清单](../security/extension-app-risk-register.md)
- 原则：保留现有 EdgeDesk 业务功能与 V1.2 行为；本文中的 Issue 为建议拆分，尚未创建为 GitHub Issue。

## 0. 实现结果（2026-10-18）

| 阶段 | 交付 | 验收证据 |
| --- | --- | --- |
| Phase 0 | manifest/SemVer、JSON-RPC 方法目录、错误码、SQLite schema、状态机、握手/传输契约 | [实现契约](../extension-platform-contracts.md) |
| Phase 1 | SQLite store、权限/KV/操作日志、启动恢复、0700/0600 | `extension_store_test` |
| Phase 2 | 流式 ZIP、逐文件 SHA-256、原子修订、升级/覆盖/降级、保留/固定/清理、隔离 | `extension_package_test` |
| Phase 3 | `/apps/...` 静态路由、CSP/MIME/SPA/缓存、sandbox iframe、WindowManager 接入 | 浏览器 E2E、集成测试 |
| Phase 4 | postMessage 握手、MessagePort JSON-RPC、权限快照、事件、外部来源 CSP | 浏览器 E2E、`manifest_test`、集成测试 |
| Phase 5 | KV get/set/delete/list/transaction、5 MiB/256 KiB、卸载保留/恢复/删除 | `extension_store_test`、集成测试 |
| Phase 6 | 虚拟路径、游标、1 MiB 分块、原子提交、共享/独占锁、审计 | `file_lock_test`、集成测试 |
| Phase 7 | 应用管理 UI、启动器动态注册、窗口恢复校验、错误/重载、完整性 UI | 浏览器 E2E、前端测试 |
| Phase 8 | SDK、Node CLI、vanilla/React/Vue/Svelte 模板、模板打包 | SDK CLI 测试、vanilla 包安装运行 |
| Phase 9 | 文档、风险证据、L0/L1、真实浏览器检查、崩溃恢复 | [测试矩阵](../testing.md) |

未完成/未验证：L2 平台矩阵（musl/ARM64/qemu/真实硬件）本轮未运行；发布前必须执行。
数字签名、公共市场、应用间通信、秘密存储、后端/脚本运行时按需求保持非目标。

## 1. 实施前强制盘点

编码前先阅读 AGENTS.md、SPEC-0001 至 SPEC-0007、相关 ADR、docs/architecture.md、docs/api.md、docs/config.md、docs/testing.md 和当前实现。必须盘点：

- 前端 AppRegistry、WindowManager/windowStore、DesktopPersistence、AppSurface、应用启动器、Dock、桌面恢复路径。
- C++ HTTP 路由、WebRoot/静态资源路由、认证/CSRF/Origin 中间件、错误映射、维护线程和优雅退出。
- FileService、PathSandbox、上传状态机、原子替换、现有上传大小限制、目录 FD 和文件锁语义。
- TerminalSessionManager 的创建/关闭语义，以及当前 API/WS 会话授权。
- data-dir、state-dir、配置优先级、现有审计和测试脚本。
- 当前没有扩展应用代码，不得把本计划或现有原生 AppRegistry 误认为已具备动态应用包能力。

本次新增 SQLite 是扩展平台的局部例外。不得迁移现有认证、设置、终端或指标模块；不得削弱现有 PathSandbox；不得改变原生应用功能来迁就扩展应用。

## 2. 阶段计划

### Phase 0 — 数据流、威胁模型与接口冻结

**目标：** 将 SPEC 转成可以测试的内部契约，避免在实现过程中自行猜测。

- 画出上传/解压/校验/待审批/激活/升级/回滚/卸载状态机。
- 设计 SQLite schema、事务边界、revisionId、操作日志和启动恢复规则。
- 依据 SPEC-0007 已定义的方法族，冻结 manifest 核心 schema/模块字段、每个 JSON-RPC 方法的参数与结果 schema、错误码、权限快照和 MessageChannel 握手状态机；不得借 Phase 0 重新开启已确认的产品决策。
- 定义文件传输消息、1 MiB 分块、逐块确认、长度检查、原子提交、锁身份和清理状态。
- 对 R-01 至 R-20 完成威胁模型评审；对 opener、网络重定向、无消息上限和延迟权限撤销明确记录残余风险。
- 不增加代码功能，先让接口和测试向量可审查。

**退出条件：** 状态机、manifest 字段表、JSON-RPC 方法目录、数据库 schema、错误码和威胁模型进入文档；没有未说明的接口契约空白。

### Phase 1 — SQLite 应用管理器与状态恢复

**目标：** 建立可事务化的应用索引和状态模型，不执行任何应用代码。

- 在扩展模块内引入 SQLite，不迁移既有数据存储。
- 建立 app、revision、permission_request、permission_grant、operation、integrity_manifest 和 KV 表；使用事务和外键/约束保证状态一致。
- 建立安装/升级/卸载操作状态机与操作日志，定义重启恢复、临时目录清理和幂等处理。
- 数据根目录默认 /var/lib/edgedesk/apps，可通过配置修改；文件权限和目录创建必须符合现有 data-dir 安全约定。
- 应用 ID、SemVer、revisionId 分开存储；相同 SemVer 的不同内容不能复用 revisionId。
- 审计历史与应用数据分开，应用不能直接修改审计记录。

**验收：** SQLite 事务单测；进程在每个操作阶段退出后重启恢复；损坏/缺失数据库的失败关闭行为已定义并测试；现有认证/设置数据不受影响。

### Phase 2 — ZIP 校验、安装、完整性清单与版本管理

**目标：** 安全地把上传包转成不可变修订，但保持待审批状态。

- ZIP 流式解析与解压，校验 manifest、入口、相对路径、重复文件名、符号链接和路径穿越。
- 不设固定包大小或文件数量上限；只按可用磁盘空间判断是否可安装，流式处理并测试压缩炸弹/磁盘耗尽失败路径。
- 每个文件计算 SHA-256，写入修订专属完整性清单；清单不证明发布者身份。
- 安装包先写临时目录，校验完成后原子提交修订，SQLite 记录操作阶段。
- 首次安装状态为待审批，未批准不可运行；未签名警告必须出现在管理 UI 设计契约中。
- 实现手动完整性检查；发现不一致后隔离修订、禁用、关闭窗口、撤销运行访问、保留现场并审计。
- 实现历史修订保留、手动固定、自动回滚引用检查和清理策略。

**验收：** 正常/损坏/重复路径/符号链接/路径穿越/磁盘不足/解压失败/中途崩溃测试；同 SemVer 不同内容生成不同 revisionId；清理不能删除活动或引用修订。

### Phase 3 — 静态资源路由与 iframe 运行时

**目标：** 加载已审批的静态应用，并接入现有窗口系统。

- C++ HTTP 服务新增独立 /apps/{appId}/{version}/{revisionId}/ 静态路由，和特权 API 路由严格分离。
- 处理 MIME、相对资源、SPA 回退、应用专属 CSP、版本不可变缓存和动态状态缓存。
- 实现不授予 allow-same-origin 的 iframe sandbox；允许脚本、表单、下载和弹窗。
- 外部弹窗使用逃离 sandbox 的机制时，限制危险导航和宿主通信；opener 风险需保留在文档与测试中。
- 扩展窗口接入现有 WindowManager，使用 manifest 默认/最小尺寸及实例策略。
- 每次运行生成新的 instance UUID；持久化窗口记录与 runtime UUID 分离。
- 关闭、禁用、升级时关闭端口、取消订阅并释放实例资源；不支持后台任务。

**验收：** 静态资源路径隔离、MIME/SPA/CSP/缓存测试；iframe 无同源授权；窗口拖动缩放和单/多实例行为通过浏览器验收；原生应用无回归。

### Phase 4 — JSON-RPC、MessageChannel 与权限快照

**目标：** 建立扩展与宿主之间唯一的特权通信入口。

- 实现 postMessage 握手：校验 iframe 窗口引用、随机令牌和消息结构，成功后转移专属 MessagePort。
- 实现 JSON-RPC 2.0 request/response、request ID、方法命名空间、通知事件、版本、错误码和超时语义。
- 不实现 JSON-RPC 总消息大小上限；不新增扩展级调用速率、并发数或执行超时限制。必须测试畸形消息、异常和大消息对现有进程的影响，并在风险记录中明确残余风险。
- 权限按模块授权；每个请求检查实例权限快照、方法和参数。新增权限未批准时拒绝。
- 原生特权 API 不允许从 iframe 直接绕过桥接；后端保留现有认证、CSRF/CSWSH 和 API 授权。
- 单项权限撤销对已运行实例延迟生效；应用禁用、升级、卸载或完整性异常立即关闭实例。
- 外部网络使用统一白名单与 CSP；HTTP/HTTPS 均允许，初始 URL 校验后不由 EdgeDesk 逐跳校验重定向。
- 提供有限宿主登录状态；会话复用只能走受控代理/授权，不向 iframe 返回原始宿主凭据。

**验收：** 握手伪造、令牌错配、跨实例端口复用、权限拒绝、无效参数、禁用/升级后旧端口失效、重放旧 request、网络来源匹配和 opener 回调测试。

### Phase 5 — 应用 KV 存储、事务与迁移

**目标：** 给应用提供独立且有配额的数据存储。

- 实现 get、set、delete、list 与 storage.transaction/批量操作。
- 按 appId 隔离，默认 5 MiB/应用、256 KiB/键；超限不覆盖旧值；list 分页/限制结果数。
- 所有 KV 操作由宿主访问 SQLite；扩展不能访问数据库或使用浏览器 localStorage/IndexedDB 作为持久化契约。
- 应用启动时自行迁移数据结构版本；多键变更经 SQLite 原子事务，失败整体回滚。
- 迁移失败保留原始数据并阻止应用正常启动；卸载时按管理员选择保留/删除，保留数据隔离且重装恢复需确认。
- 不实现秘密存储。

**验收：** 配额边界、跨应用隔离、超限不覆盖、事务全成全败、数据库重启恢复、迁移失败、卸载保留/删除与重新安装恢复测试。

### Phase 6 — 文件宿主 API、传输与锁

**目标：** 在不改变现有 FileService 安全边界的前提下开放文件能力。

- 所有文件请求经宿主桥接和现有 FileService/PathSandbox；使用虚拟路径，保留现有目录 FD/openat、符号链接和 TOCTOU 防护。
- 实现游标分页、固定排序、CURSOR_STALE 和文件元数据返回。
- 实现 1 MiB 二进制分块、严格顺序传输、逐块确认、预期长度校验、取消与失败清理。
- 上传先写临时文件，完成后原子替换；同名默认覆盖。锁占用立即返回 FILE_LOCKED。
- 统一共享读锁/独占写锁，结合规范化路径和文件系统对象身份。锁只协调 EdgeDesk 内部操作。
- 文件操作按应用/实例/操作者/类型/规范化路径/时间/结果/失败原因审计。
- 不支持断点续传；失败/取消/实例关闭/服务重启清理临时文件。

**验收：** 大文件有界分块行为、逐块确认、长度不匹配、取消、断连、磁盘满、原子替换失败、同名覆盖、读写锁冲突、路径别名和外部修改冲突测试。现有文件管理 UI/API 不得回归。

### Phase 7 — 应用管理 UI 与桌面恢复

**目标：** 让管理员能完成全生命周期管理。

- 实现 ZIP 上传、安装状态、未签名警告、权限审批、启用/禁用、升级、手动降级、卸载、保留/删除数据、完整性检查和历史版本管理。
- 首次安装权限审批前不允许启动；升级新增权限待审批，原权限保留。
- 启用应用后自动加入启动器；桌面图标和 Dock 固定由用户/管理员决定。
- 自动恢复窗口记录时校验 appId、安装/启用状态、host API 范围和当前权限；每个恢复实例刷新权限快照。
- 运行错误统一提示和重新加载；完整性异常立即隔离和关闭所有窗口。
- 更新桌面持久化 schema，保持现有原生应用窗口布局兼容。

**验收：** 安装到卸载完整浏览器流程；刷新恢复、禁用、升级回滚、降级、数据处置和完整性异常 UI 验收；原生桌面行为保持不变。

### Phase 8 — SDK、模板与 CLI

**目标：** 让开发者可以独立构建兼容应用包。

- 提供基础项目模板与 React、Vue、Svelte、原生 JS 示例。
- SDK 封装 JSON-RPC、事件订阅、权限错误、存储 API 和文件传输状态。
- CLI 提供 manifest 校验、静态资源检查、相对路径检查和 ZIP 打包。
- SDK 不得伪装不存在的宿主能力；后端/脚本运行时扩展点仅提示暂未支持。
- 明确 SDK 与宿主 API 版本的兼容要求。

**验收：** 从空模板构建一个静态应用包，CLI 校验通过，在 EdgeDesk 安装、审批、运行、调用存储并卸载；至少覆盖一种非 React 模板。

### Phase 9 — 安全回归、文档与发布验收

**目标：** 在真实 EdgeDesk 环境验证完整闭环。

- 执行 SPEC-0007 验收矩阵与风险清单中的测试。
- 更新 docs/architecture.md、docs/api.md、docs/config.md、docs/testing.md、AGENTS.md 和文档索引，只描述实际实现的接口和状态。
- 运行 scripts/test-fast.sh；提交前运行 scripts/test-pr.sh。需要时按 docs/testing.md 运行 L2 平台矩阵。
- 使用真实浏览器验证桌面、弹窗、CSP、文件传输和错误状态；记录浏览器/版本/测试步骤，不虚构测试结果。
- 明确未验证的平台、未实现的后端/脚本运行时和仍接受的安全风险。

**退出条件：** 所有 P0 验收项通过；风险清单逐项有测试证据或明确接受记录；没有未解释的权限绕过、数据丢失或恢复不一致问题。

## 3. 建议 Issue 拆分与依赖

| Issue ID（建议） | 标题 | 阶段 | 依赖 | 主要交付 |
| --- | --- | --- | --- | --- |
| EXT-01 | 冻结 manifest、状态机、JSON-RPC 与威胁模型 | Phase 0 | 无 | 内部接口契约、测试向量、状态图 |
| EXT-02 | SQLite 应用管理器与启动恢复 | Phase 1 | EXT-01 | schema、事务、操作日志、幂等恢复 |
| EXT-03 | ZIP 安全校验、安装与完整性清单 | Phase 2 | EXT-02 | 安装事务、SHA-256 清单、隔离状态 |
| EXT-04 | 修订版本、原子升级、回滚与清理 | Phase 2 | EXT-03 | revisionId、保留/固定、升级/降级状态机 |
| EXT-05 | 扩展静态资源路由与 CSP | Phase 3 | EXT-03 | 版本 URL、MIME、SPA 回退、缓存 |
| EXT-06 | iframe Runtime 与 WindowManager 接入 | Phase 3 | EXT-05 | 沙箱、实例 UUID、窗口生命周期 |
| EXT-07 | MessageChannel 握手与 JSON-RPC 桥接 | Phase 4 | EXT-06 | 协议、会话、错误码、事件 |
| EXT-08 | 权限审批、快照与生命周期 | Phase 4 | EXT-02、EXT-07 | 默认拒绝、升级审批、禁用/撤销语义 |
| EXT-09 | 外部网络策略、CSP 与弹窗回调 | Phase 4 | EXT-05、EXT-07 | 来源匹配、opener 回调、风险测试 |
| EXT-10 | 应用 KV、事务、配额与数据迁移 | Phase 5 | EXT-02、EXT-07 | 存储 API、事务、卸载数据处置 |
| EXT-11 | 文件 API 桥接与游标目录列表 | Phase 6 | EXT-07、EXT-08 | 虚拟路径、元数据、分页、审计 |
| EXT-12 | 分块文件传输、原子替换与锁 | Phase 6 | EXT-11 | 1 MiB 顺序传输、锁、取消清理 |
| EXT-13 | 应用管理 UI 与版本操作 | Phase 7 | EXT-03、EXT-04、EXT-08 | 上传、审批、启停、升级/降级/卸载 |
| EXT-14 | 桌面会话恢复与错误/完整性 UI | Phase 7 | EXT-06、EXT-08、EXT-13 | 恢复校验、错误重载、篡改隔离 |
| EXT-15 | SDK、模板与 CLI | Phase 8 | EXT-07、EXT-10、EXT-11、EXT-12 | 开发工具链与示例应用 |
| EXT-16 | 安全、回归与发布验收 | Phase 9 | EXT-01 至 EXT-15 | 测试证据、文档、风险签收 |

以上为文档中的逻辑 Issue ID，不代表 GitHub Issue 已创建。实际拆分时可以再细化，但不能合并掉独立的安全状态机、失败恢复或验收工作。

## 4. 每个 Issue 的完成定义

- 实现与 SPEC-0007、ADR-0011～0013 一致；遇到冲突先记录并停止相关实现，不由编码 Agent 私自改需求。
- 新增行为有自动化测试；安全拒绝路径、失败清理、服务重启恢复和权限变化均需测试。
- 修改运行代码后运行 scripts/test-fast.sh；PR 前运行 scripts/test-pr.sh；纯文档改动至少执行 Markdown/链接检查。
- 无新增无界队列、线程或文件缓存；即使产品决策未设置扩展级调用限流，也必须遵守现有核心资源边界。
- 更新对应 Spec/ADR/API/配置/架构/测试文档；计划文件不得标记为实现完成。
- 不得在 Git 中提交构建产物、web/dist 或 node_modules。
- 不得伪造浏览器截图、测试输出、性能数据或跨平台验收结果。

## 5. 发布门槛

1. 包安装、权限审批、启用/禁用、升级失败回滚、手动降级、卸载数据处置均可重复验证。
2. 任何失败安装/升级不会把半成品修订设为活动版本；服务重启能恢复一致状态。
3. iframe 无 allow-same-origin，特权 API 只通过宿主桥接；禁用/升级/卸载会结束相关运行实例。
4. 现有文件 PathSandbox、认证、CSRF/CSWSH、终端生命周期、指标和桌面窗口功能回归通过。
5. SQLite、应用数据和审计的文件权限、保留与恢复有验证记录。
6. 文件传输覆盖长度错误、磁盘满、取消、锁竞争、原子替换失败和外部修改冲突。
7. R-01 至 R-20 风险逐项被测试或明确标注为接受；不得把接受风险描述为已被技术手段消除。
8. docs/testing.md 只记录实际执行的结果；未验证平台保持未验证。
