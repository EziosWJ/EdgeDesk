# EdgeDesk UI 设计资产

- [浏览器素材画廊](asset-gallery.html)：无需构建即可查看既有插画素材
- [V1.2 独立页面原型](prototypes/README.md)：桌面、启动器、设备概览、文件管理、终端、设置与组件素材
- [素材使用指南](asset-usage.md)：路径、资源清单、React 引用、打包与验收
- [原型实现规范](prototype-guidelines.md)：四页原型拆解、视觉 Token、功能/展示边界
- [原型还原度对照表](prototype-conformance.md)：V1.2 已实现页面与目视验收记录
- [桌面快捷键与焦点规则](desktop-keyboard-shortcuts.md)：Alt 快捷键、输入/终端焦点保护和窄屏行为

实际 SVG 素材：[`web/public/assets/`](../../web/public/assets/)，新增桌面壁纸与应用图标 sprite 位于 [`web/public/assets/desktop/`](../../web/public/assets/desktop/)。

原型总览：[prototypes/index.html](prototypes/index.html)。从仓库根目录运行 `python3 -m http.server 8000`，再打开 `http://127.0.0.1:8000/docs/design/prototypes/`。
设计 Token：[`web/src/styles/edgedesk-tokens.css`](../../web/src/styles/edgedesk-tokens.css)。

> UI 原型图是视觉参考；这里的独立 SVG 为按其配色与图形语言重绘的可复用素材，而非带有原型文字和模拟数据的截图切片。
