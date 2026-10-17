# V1.2 实施计划：桌面工作区与应用窗口管理

- 状态：**Ready for implementation planning**
- 需求基线：[SPEC-0006](../spec/0006-desktop-workspace.md)
- 架构决策：[ADR-0009](../adr/0009-desktop-workspace-architecture.md)、[ADR-0010](../adr/0010-desktop-persistence-and-recovery.md)
- 原则：保留现有业务能力；本文件用于规划，不代表功能已经实现。

## 1. 实施约束

1. 先阅读 AGENTS.md、SPEC-0001 至 SPEC-0006、ADR-0001/0002/0005/0007/0008/0009/0010，并阅读 docs/design/prototypes/README.md、prototype-guidelines.md、prototype-conformance.md，再修改代码。
2. 不重写 C++ 后端、不新增 API/数据库、不改变认证和授权契约、不引入新的窗口组件库；复用 web/public/assets/desktop/ 的壁纸与 SVG sprite、现有设计 Token 和 Icon 封装。
3. 不把原型截图裁切成生产 UI；窗口、图表、表格、指标卡必须是实际 React/HTML/CSS 组件。原型中的演示数据不得进入生产逻辑。
4. 不删除现有文件管理、终端多 Tab、Bash 登录环境、会话刷新重连、指标实时曲线、设置和主题能力。
5. 窗口关闭、窗口最小化、WebSocket 断开和 Bash 会话结束必须保持语义区分。
6. 每个 Issue 独立实现、独立测试；完成标准以测试和浏览器实际行为为依据，不能只凭 Agent 自述。
7. 修改行为代码后执行 scripts/test-fast.sh；提交前执行 scripts/test-pr.sh。纯文档改动可做 Markdown/链接检查。

## 2. 推荐实施阶段

### Phase 0 — 基线盘点与测试保护

**目标：** 固定当前行为，避免桌面化过程中丢失已有能力。

- 梳理 App.tsx、Sidebar、WindowLayer、FloatingWindow、windowStore、sessionStore、terminalSocket、文件工作区和偏好设置的依赖。
- 为现有终端多 Tab、文件管理多视图/操作、窗口拖拽缩放和设置偏好补齐缺失的回归测试。
- 明确应用注册和窗口实例的数据模型；提交前不得更改后端 API。

**退出条件：** 当前测试基线通过；需求和模块边界与 SPEC-0006 一致。

### Phase 1 — DesktopShell、AppRegistry 与窗口管理

**目标：** 建立桌面壳层和统一窗口入口。

- 新增 DesktopShell、AppRegistry、WindowManager 接口和统一窗口注册元数据。
- 将设备概览、文件管理、终端工作区和设置接入应用窗口体系，移除以 active 页面切换为主导航的旧壳层。
- 保留现有业务组件和窗口拖拽/缩放实现；整理单实例、多实例、窗口焦点和层级状态。
- 修复最小化导致子组件卸载的问题：隐藏窗口但保持业务组件挂载。

**退出条件：** 所有首期应用都通过注册表启动；单/多实例规则、窗口状态转换和组件挂载行为有自动化测试。

### Phase 2 — 桌面图标、Dock 与应用启动器

**目标：** 实现桌面化入口和窗口切换。

- 实现可拖拽图标网格：单击选中、双击启动。
- 实现底部居中 Dock、固定应用、运行状态指示、最小化窗口恢复和多窗口选择。
- 实现应用网格启动器、名称搜索、应用说明和空状态。
- 确保 Dock/启动器的焦点与 z-index 行为正确，不遮挡窗口关键操作。

**退出条件：** 桌面图标、Dock 和启动器交互通过前端测试及浏览器目视检查。

### Phase 2.5 — NAS 风格设备概览小组件

**目标：** 将现有设备概览整理为可复用、数据可信的资源小组件，视觉参考 `docs/design/prototypes/dashboard.html` 与 `components.html`。

- 实现 CPU、内存、存储、网卡等资源卡片和短时趋势图；字段只使用现有 API/WebSocket 实际提供的数据。
- 多挂载点和多网卡按真实实体分别展示；服务状态只显示后端能够确认的状态，不虚构 Docker/systemd 管理能力。
- 加载、数据缺失、采集断开和部分不可用状态清晰可辨；演示数值不得进入生产代码。
- 复用 `web/src/styles/edgedesk-tokens.css`、`web/public/assets/desktop/app-icons.svg` 和现有 Icon 封装，不引入新的图标库。

**退出条件：** 组件状态、单位、缺失值与趋势图有测试；视觉按原型目视验收。

### Phase 3 — 桌面持久化与恢复

**目标：** 刷新后恢复完整工作区。

- 新增 DesktopPersistence，定义带 schemaVersion 的 localStorage 布局结构。
- 保存图标位置、Dock 配置、窗口应用/实例、几何状态和最小化/最大化状态。
- 在认证完成后按顺序恢复布局；校验 appId、实例策略、几何值和目录路径。
- 实现损坏数据、存储不可用、未知版本和单个窗口恢复失败时的安全回退。

**退出条件：** 序列化/迁移/异常恢复有单元测试；刷新后窗口与桌面布局恢复可验证。

### Phase 4 — 终端、编辑器与会话生命周期集成

**目标：** 确保桌面操作不会破坏已有业务状态。

- 终端窗口关闭只分离 UI；会话仍存在时可以从终端工作区或窗口恢复入口重新连接。
- 恢复前读取真实终端会话列表；已结束会话展示明确状态，不创建替代会话。
- 保留终端多 Tab、独立 PTY、Bash 登录语义、resize、重放缓冲和显式关闭标签行为。
- 编辑器最小化时保留组件内存状态；关闭未保存内容时确认；不保存文件正文草稿。
- 对文件路径失效、文件外部修改、服务重启和会话过期进行错误状态处理。

**退出条件：** 终端会话恢复和编辑器未保存提醒有自动化/集成测试，且没有资源或会话泄漏。

### Phase 5 — 响应式、快捷键、回归与发布验收

**目标：** 让桌面在目标浏览器尺寸下稳定工作。

- 窄屏自动最大化应用窗口，核心操作仍可用。
- 实现基础窗口/菜单快捷键，并避开浏览器保留组合和输入控件焦点。
- 验证深色/浅色/跟随系统主题、键盘可访问性、窗口边界和异常状态。
- 更新设计规范、文档索引、测试矩阵和人工目视验收记录。

**退出条件：** scripts/test-pr.sh 通过；目标浏览器目视验收完成；未验证的平台明确标记。

## 3. GitHub Issue 拆分

以下 Issue 应在仓库创建，并使用 SPEC-0006 作为需求基线。依赖关系以 Issue 正文为准。

| 顺序 | Issue | 目标 | 依赖 |
| --- | --- | --- | --- |
| 1 | [#4 DesktopShell 与 AppRegistry](https://github.com/EziosWJ/EdgeDesk/issues/4) | 桌面壳层和应用注册基础 | 无 |
| 2 | [#5 WindowManager 与窗口生命周期](https://github.com/EziosWJ/EdgeDesk/issues/5) | 窗口操作、实例策略、最小化不卸载 | #4 |
| 3 | [#6 桌面图标与 Dock](https://github.com/EziosWJ/EdgeDesk/issues/6) | 桌面入口、固定应用、运行状态、恢复最小化窗口 | #4、#5 |
| 4 | [#12 NAS 风格设备资源小组件](https://github.com/EziosWJ/EdgeDesk/issues/12) | CPU、内存、存储、网卡和系统信息卡片 | #4、#5 |
| 5 | [#7 应用启动器与快捷键](https://github.com/EziosWJ/EdgeDesk/issues/7) | 搜索、应用说明、菜单和窗口快捷操作 | #4、#5、#6 |
| 6 | [#8 DesktopPersistence 与工作区恢复](https://github.com/EziosWJ/EdgeDesk/issues/8) | 本地布局 schema、恢复、回退 | #4、#5、#6 |
| 7 | [#9 终端会话与编辑器生命周期](https://github.com/EziosWJ/EdgeDesk/issues/9) | 保留 Bash 会话、多 Tab、编辑器状态和关闭语义 | #5、#8 |
| 8 | [#10 窄屏、快捷键边界与可访问性](https://github.com/EziosWJ/EdgeDesk/issues/10) | 响应式和键盘交互 | #5、#6、#7 |
| 9 | [#11 回归测试、文档与目视验收](https://github.com/EziosWJ/EdgeDesk/issues/11) | 发布门槛与回归证据 | #4–#10、#12 |

## 4. 通用完成定义

- Issue 正文中的验收项逐条验证；有测试的行为必须添加/更新自动化测试。
- scripts/test-fast.sh 通过；在提交/PR 前 scripts/test-pr.sh 通过。
- 无新增未解释依赖；无后端 API 或认证安全边界的非计划变更。
- 更新相关 Spec/ADR/README/设计说明；不把计划文档标记为实现完成。
- 浏览器人工验收提供真实检查记录；不得伪造截图、测试输出或跨平台验证结果。
- 所有终端会话恢复和关闭行为都以服务端真实状态为准，不允许本地 UI 状态绕过授权。

## 5. 发布门槛

1. SPEC-0006 的功能验收全部完成，或明确记录经产品批准的延期项。
2. 设备概览 NAS 风格小组件按真实 API 字段显示 CPU、内存、挂载点、网卡和系统信息；演示数据不进入生产 UI。
3. 现有 V1.1 功能回归通过，尤其是认证、文件路径沙箱、上传/下载、终端 PTY 生命周期和实时指标。
4. localStorage 数据损坏/升级、会话过期、服务重启、文件路径失效、窄屏和键盘焦点场景有验证结果。
5. scripts/test-pr.sh 通过；发布前按 AGENTS.md 和 docs/testing.md 执行适用的 L2 构建/平台验证。
6. docs/README.md 中 SPEC/ADR/实施计划索引更新；人工目视验收记录可追溯。