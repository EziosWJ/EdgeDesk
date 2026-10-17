# EdgeDesk V1.2 独立页面原型

这里的页面是**可在浏览器中打开的静态交互原型**，用于评审桌面结构、NAS 风格资源组件和窗口视觉，不连接真实设备 API，也不代表功能已经实现。

## 页面

- [原型总览](index.html)
- [桌面与 Dock](desktop.html)
- [应用启动器](launcher.html)
- [设备概览与 NAS 风格资源小组件](dashboard.html)
- [文件管理](files.html)
- [终端工作区](terminal.html)
- [系统设置](settings.html)
- [可复用组件与素材库](components.html)

## 本地预览

从仓库根目录启动一个静态 HTTP 服务：

```bash
python3 -m http.server 8000
```

打开 `http://127.0.0.1:8000/docs/design/prototypes/`。外部 SVG sprite 和桌面壁纸通过相对路径引用，因此不要直接双击 HTML 使用 `file://` 打开。

## 素材目录

- 可复用 SVG：`web/public/assets/desktop/`
- 既有品牌与插画：`web/public/assets/brand/`、`web/public/assets/illustrations/`
- 设计 Token：`web/src/styles/edgedesk-tokens.css`
- 共享原型样式与交互：`prototype.css`、`prototype.js`

## 使用边界

1. 页面中的 CPU、内存、磁盘、网卡和服务数值都是示例数据，禁止直接复制到正式 UI 作为设备状态。
2. 生产 UI 的图表、表格、窗口和状态组件必须由真实 React/HTML/CSS 实现；禁止裁剪原型截图作为功能控件。
3. 优先使用现有 Lucide 图标封装和本目录的 SVG 素材；不要引入新的图标库或桌面框架。
4. 这些静态页面是设计交付物，不替代 SPEC-0006 和 Issues #4–#12 的功能验收。
