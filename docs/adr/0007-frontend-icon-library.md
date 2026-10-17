# ADR-0007：前端图标库（Lucide）

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
