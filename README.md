# EdgeDesk

面向嵌入式与边缘 Linux 设备的轻量 Web 运维工作台。

> 当前仓库处于**需求与架构设计阶段**。本文及 docs 下的 Spec/ADR 记录已确认的产品决策和实现约束，**不表示代码已经实现**。

## V1.0 核心能力

- 文件管理：目录浏览、上传/下载、在线文本编辑、重命名、删除等。
- Web Bash 终端：交互式登录环境、多终端 Tab，切换 Tab、页面导航和浏览器刷新不结束后端会话。
- 设备仪表盘：CPU、内存、磁盘、网络基础指标及短时间实时曲线。
- 文件管理工作区：终端和编辑器可作为多个可拖拽、可缩放的浮动窗口打开。

## 已确认技术基线

| 范围 | 决策 |
| --- | --- |
| 后端 | C++17 + libhv (HTTP/WebSocket) |
| 编译 | GCC 15+；Linux 使用 musl 静态链接 |
| 资源打包 | GCC `#embed` 扩展（前端构建产物嵌入 ELF） |
| 目标 Linux | x86_64 / ARM32 / ARM64（分别交叉构建并验证） |
| 数据 | V1.0 不引入数据库和 ORM |
| 前端 | React 为候选，**尚未正式确认** |

注意：`#embed` 是 GCC 15+ 提供的扩展，可在 C++17 源码中使用；其他编译器和 OpenHarmony SDK 的适配需要单独验证。

## 文档入口

- [产品与版本范围](docs/spec/0001-product-scope.md)
- [文件管理需求](docs/spec/0002-file-management.md)
- [终端与多窗口需求](docs/spec/0003-terminal-workspace.md)
- [设备仪表盘需求](docs/spec/0004-device-dashboard.md)
- [技术路线 ADR](docs/adr/0001-technology-stack.md)
- [架构与安全 ADR](docs/adr/0002-architecture-security.md)
- [文档索引 / 待确认事项](docs/README.md)

## 前端设计资源

已从 EdgeDesk 四张原型中提炼可直接使用的 SVG 素材与 CSS 设计变量：

- [素材库（Logo、图标、空状态插画）](web/public/assets/)
- [素材预览画廊](docs/design/asset-gallery.html)
- [前端素材使用说明](docs/design/asset-usage.md)
- [原型视觉与组件实现规范](docs/design/prototype-guidelines.md)
- [设计 Token](web/src/styles/edgedesk-tokens.css)

> 资源库可直接供后续 React 前端使用；当前不表示 React 已正式选型或 UI 已完成实现。

## 开发原则

先以文档为基线建立最小可运行闭环，再分模块迁移或实现功能；不得将历史项目的“已实现”状态直接视作本仓库的完成状态。
