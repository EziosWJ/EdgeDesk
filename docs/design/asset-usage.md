# EdgeDesk 前端素材使用指南

- 状态：设计素材 / 可用于前端，不表示 React 技术选型已批准或页面已实现
- 来源：2026-10-16 的四张 EdgeDesk 原型图（仪表盘、文件管理与浮窗、终端工作区、会话/设置）
- 原则：**可复用图形使用矢量文件；图表、表格、状态数值、浮动窗口全部使用真实 React/HTML/CSS 组件实现，绝不裁切整页截图当 UI。**

## 目录

```text
web/
  public/
    assets/
      brand/           edgedesk-mark.svg / edgedesk-mark-mono.svg
      illustrations/   edge-device.svg / empty-files.svg /
                       empty-terminal.svg / monitor-unavailable.svg /
                       session-disconnected.svg
      icons/           13 个 24x24 导航、监控与操作图标
      decorations/     panel-grid.svg / dot-grid.svg
      asset-manifest.json
  src/
    styles/
      edgedesk-tokens.css

docs/design/
  asset-usage.md
  prototype-guidelines.md
```

## 资产表

| 文件 | 用途 | 建议尺寸 |
| --- | --- | --- |
| `brand/edgedesk-mark.svg` | 侧栏 Logo、登录页标识 | 32x32 或 48x48 |
| `brand/edgedesk-mark-mono.svg` | 单色场景、备用标识 | 24x24 |
| `illustrations/edge-device.svg` | 设备状态卡 / 设备信息页 | 128–256px 宽 |
| `illustrations/empty-files.svg` | 空目录占位图 | 160–240px 宽 |
| `illustrations/empty-terminal.svg` | 无终端会话占位图 | 160–240px 宽 |
| `illustrations/monitor-unavailable.svg` | 采集失败/指标不可用 | 160–240px 宽 |
| `illustrations/session-disconnected.svg` | 会话已断开占位图 | 160–240px 宽 |
| `icons/*.svg` | 导航、指标、工具栏 | 20x20 或 24x24 |
| `decorations/*.svg` | 指标卡/侧栏微弱纹理 | CSS background，平铺 |

完整索引：`web/public/assets/asset-manifest.json`。

## React / Vite 引用示例（React 目前仍是候选）

资源位于 `public/assets` 时，Vite 不需要 `import` 资源文件；运行时 URL 由 `BASE_URL` 决定，根路径部署时通常为 `/assets/...`。

```tsx
const asset = (path: string) =>
  `${import.meta.env.BASE_URL}assets/${path}`;

export function DeviceCard() {
  return (
    <section className="ed-panel">
      <img src={asset("illustrations/edge-device.svg")}
           alt="" width="160" height="110" aria-hidden="true" />
      <strong>设备在线</strong>
    </section>
  );
}

export function SidebarLogo() {
  return <img src={asset("brand/edgedesk-mark.svg")}
              width="36" height="36" alt="EdgeDesk" />;
}
```

```css
@import "./edgedesk-tokens.css";

/* 背景纹理仅作为辅助装饰，不承载状态信息 */
.panel-with-grid {
  background: var(--ed-surface)
    url("/assets/decorations/panel-grid.svg") repeat;
}

/* SVG 通过 mask 做 CSS 动态着色（普通 <img> 的 currentColor 不继承） */
.icon-files {
  --ed-icon-url: url("/assets/icons/files.svg");
  color: var(--ed-primary);
}
```

如果部署在子路径，CSS 中的绝对 `/assets/` 需要改为与应用 `base` 匹配的路径或从组件内联设置 `--ed-icon-url`；不要假设永远部署在域名根目录。

## 单二进制发布

```text
前端源码
  → 前端构建（如 React + Vite）
  → dist/assets/*（包含优化后的 SVG）
  → GCC 15+ #embed 生成资源表
  → EdgeDesk C++ / libhv 可执行文件
```

- 只嵌入**构建产物**，不要同时把 `web/public` 和 `dist` 两份资源打包。
- SVG 即普通静态资源，应设置 `image/svg+xml`、正确的缓存头与 ETag。
- 构建后的资源路径要与 React 的 `base`、C++ 静态资源路由保持一致。
- 不要对不可信的任意上传 SVG 在同源环境以内联脚本方式呈现；本目录是开发时可信的静态素材。
- 静态插画在不同 DPI 下可以无损缩放，不需要一批 1x/2x PNG；未来确需特殊硬件兼容可另导出 PNG/WebP。

## 验收清单

- 资源全部可通过浏览器独立访问；无外部图片、字体或 CDN 依赖。
- SVG 透明底、可缩放；图标提供明确名称和用途。
- 装饰性图片使用 `alt=""`；有含义的图标需要可访问标签。
- UI 实时数据必须来自设备 API，不能用原型图内的演示数值冒充真实状态。
- 资源新增/删除时同步 `asset-manifest.json`、本指南和设计规范。
