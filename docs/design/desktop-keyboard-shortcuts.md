# 桌面快捷键与焦点规则（V1.2）

- 状态：**已实现**（2026-10-18）
- 实现：`web/src/desktop/DesktopShell.tsx`（全局监听）、`Launcher.tsx`（Esc/焦点返回）、
  `web/src/desktop/useNarrowScreen.ts`（可编辑目标判断）
- 需求基线：[SPEC-0006](../spec/0006-desktop-workspace.md) §4.4、§6

## 按键映射

| 快捷键 | 行为 | 复用接口 |
| --- | --- | --- |
| `Alt` + `K` | 打开/关闭应用启动器 | `Launcher` 状态；应用来自 `AppRegistry` |
| `Alt` + `M` | 最大化/还原当前聚焦窗口 | `windowStore.toggleMaximize` |
| `Alt` + `H` | 最小化当前聚焦窗口 | `windowStore.minimize` |
| `Alt` + `,` | 聚焦上一个可见窗口 | `windowStore.cycleFocus(-1)` |
| `Alt` + `.` | 聚焦下一个可见窗口 | `windowStore.cycleFocus(1)` |
| `Esc` | 关闭启动器、窗口选择器或非破坏性菜单 | 各组件自身的关闭逻辑 |

窗口按钮、Dock 与启动器同时提供鼠标操作；快捷键不是唯一入口。

## 不抢占的输入

以下情况全局快捷键不处理（`isEditableTarget`）：

- `input`、`textarea`、`select`、`contenteditable` 元素；
- xterm.js 终端区域（`.xterm`，其内部隐藏 textarea 需要接收字符与 Alt 组合）；
- 编辑器文本域（`.ed-editor-textarea`）。

此外：

- 不注册任何 `Ctrl`/`Cmd` 组合，因此浏览器刷新、地址栏、标签页、开发者工具等保留快捷键不受影响。
- 编辑器保存使用 `Ctrl`/`Cmd` + `S`，监听在编辑器 textarea 上，只在该编辑器获得焦点时生效；不注册全局保存。
- 启动器内的 `Esc` 正常关闭弹层；关闭后焦点返回打开启动器前的元素（通常是 Dock 启动器按钮）。

## 窄屏行为

- 断点：`max-width: 768px`（`NARROW_BREAKPOINT`），仅用于布局，不是后端协议。
- 窄屏下所有应用窗口强制以最大化样式呈现，禁用标题栏自由拖动与缩放把手；最大化/还原按钮禁用。
- Dock 缩紧间距并保留启动器、运行指示、最小化恢复入口；窗口内容在自身容器内滚动。
- 已在实际浏览器 390×844 视口验证无页面横向溢出（见 `docs/testing.md`）。

## 窗口焦点与层级

- 点击窗口任意位置或标题栏都会聚焦该窗口；标题栏、内容区有可见焦点/前景反馈。
- 聚焦顺序由 `windowStore` 的数组顺序决定，`z-index = 10 + index`，不会无限增长。
- 最小化窗口从可见焦点序列中移除，但组件保持挂载；Dock 中的恢复入口可还原原窗口及几何状态。
- 同一应用有多个窗口时，点击 Dock 项显示窗口选择器（标题 + 状态），不随机聚焦实例。

## 浏览器验证记录

| 日期 | 视口 | 结果 |
| --- | --- | --- |
| 2026-10-18 | 1440×900 | 启动器、窗口快捷键、焦点保护、最小化恢复、刷新恢复均通过 |
| 2026-10-18 | 390×844 | 自动最大化、无横向溢出、Dock 入口可用 |
