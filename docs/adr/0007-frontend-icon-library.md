# ADR-0007：前端图标库（Lucide + 文件类型图标）

- 状态：**Accepted（已确认）**（2026-10-17 产品确认全站迁移）
- 日期：2026-10-17
- 关联：[ADR-0001](0001-technology-stack.md)、[素材使用指南](../design/asset-usage.md)

## 背景

界面此前只使用 `web/public/assets/icons` 下 20 个手写 SVG，通过 CSS mask + `--ed-icon-url`
着色。图标种类不足以覆盖文件类型、操作与状态表达，且每新增一个图标都要手工绘制与维护；
文件管理页希望按扩展名区分文件类型并丰富图标展示。

## 决策

| 项目 | 决策 |
| --- | --- |
| 图标库 | `lucide-react`，精确锁定 `1.58.0`（package.json 精确版本 + lockfile） |
| 许可证 | ISC（宽松许可，允许商业使用，保留版权声明） |
| 迁移范围 | 全站统一：侧栏、仪表盘、终端工作区、设置、文件管理、浮动窗口与任务栏 |
| 静态资产 | 删除 `web/public/assets/icons/*.svg`；`asset-manifest.json` 不再收录 icons |
| 统一封装 | `web/src/components/Icon.tsx` 固定尺寸、线宽与无障碍默认值（装饰性图标 `aria-hidden`） |

## 理由

- 图标以 React 组件按需导入，Vite 构建期 tree-shake，只打包实际用到的字形；
  运行时无 CDN、无外部请求，符合离线与单二进制部署约束。
- 24x24 线性风格与原型视觉规范（`docs/design/prototype-guidelines.md`）一致；
  `currentColor` 直接继承现有 CSS 颜色，不再需要 mask + CSS 变量机制。
- ISC 为宽松许可证，无 copyleft 义务；资产清单与素材文档记录版本与来源。
- ADR-0001 明确“不指定 React UI 组件库”，本决策不与既有 ADR 冲突。

## ARM / musl 影响

- 依赖只存在于开发/CI 构建期；设备端没有 Node 运行时，不新增动态库、系统调用或配置项。
- 产物仍是 Vite 构建后的静态资源，由磁盘模式或 `#embed` 单二进制提供，路径、缓存与 ETag 策略不变。
- 交叉编译（ARM32/ARM64、musl）不受影响，前端产物与 C++ 构建解耦。

## 影响（本机实测，2026-10-17）

- `web` 生产构建：`dist/assets/index-*.js` 563.94 kB → 592.11 kB（gzip 158.25 kB → 165.23 kB）；
  CSS 33.86 kB → 33.73 kB（gzip 8.13 kB → 8.11 kB）。
- 删除 20 个手写 SVG 与 `.ed-icon-mask` / `--ed-icon-url` 机制，新增一条前端依赖。
- 首次安装或升级依赖需要访问 npm registry，与现有前端依赖一致。

## 补充（2026-10-17）：文件类型图标

Lucide 是通用界面图标集，不提供语言/格式专属字形（没有 Go、C++、Markdown 等图标），
仅靠 `FileCode` / `FileText` 无法区分具体格式。因此文件列表/网格的**普通文件**改用
VS Code 图标集，其余界面与操作图标继续使用 Lucide。

| 项目 | 决策 |
| --- | --- |
| 文件类型图标 | `@iconify-json/vscode-icons` **1.2.90**（MIT，VS Code Icons） |
| 编译方式 | `unplugin-icons` **24.0.0**（MIT）在 Vite/Vitest 构建期按需编译为 React 组件 |
| 编译依赖 | `@svgr/core` **8.1.0**、`@svgr/plugin-jsx` **8.1.0**（MIT，仅构建期） |
| 适用范围 | 文件列表与网格中的普通文件（按扩展名/特殊文件名映射）；目录、符号链接继续用 Lucide |
| 离线约束 | `autoInstall: false`；图标数据来自本地 npm 包，无 CDN、无运行时请求 |
| 静态资产 | 图标编译进 JS bundle，`web/public/assets/icons` 仍保持删除状态 |

处理方式：`fileUtils.ts` 的 `fileIconKey` 负责把文件名解析为图标键（Go/C/C++/Rust/
Python/...、JSON/YAML/TOML、Shell/PowerShell、压缩包/音视频/字体等，含 Dockerfile、
Makefile、CMakeLists.txt、LICENSE、package.json 等特殊文件名），`FileTypeIcon.tsx`
把键映射到 86 个按需编译的图标。未知文本回退 `text`，其余回退 `default-file`。

**实测包体积**：`dist/assets/index-*.js` 592.11 kB → 782.40 kB
（gzip 165.23 kB → 227.05 kB）；CSS 不变（33.73 kB / gzip 8.11 kB）。
增量来自 86 个多色图标的 SVG 字形；相比 `material-file-icons` 全量打包
（约 +480 kB min / +170 kB gzip）仍显著更小，且只包含实际使用的图标。

**ARM / musl 影响**：`unplugin-icons`、SVGR 与图标集合都只在开发/CI 构建期使用，
设备端不新增运行时依赖；产物仍是 Vite 构建后的 JS，磁盘模式与 `#embed` 单二进制路径不变。
