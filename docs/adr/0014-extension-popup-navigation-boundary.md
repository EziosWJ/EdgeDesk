# ADR-0014：扩展弹窗与 iframe 顶层导航的执行边界

- 状态：**Proposed（待确认；确认前不得宣称弹窗/顶层导航已被网络来源授权限制）**
- 日期：2026-10-19
- 关联：[SPEC-0007](../spec/0007-extension-applications.md)、[SPEC-0008](../spec/0008-extension-platform-hardening.md) T-02、[ADR-0012](0012-extension-security-and-communication.md) §3/§4、风险清单 R-04/R-05、[扩展平台契约 §8](../extension-platform-contracts.md#8-静态资源与-csp)

## 背景

Issue #18 修复了扩展网络来源授权未被执行的问题：CSP 外部来源现在只来自
“当前修订声明来源 ∩ 管理员已批准来源”，且必须同时存在 `network` 授权。
修复后用真实 headless Chromium（Playwright 缓存的 Chromium 1248，通过 CDP 驱动）
验证了 sandbox iframe 的实际行为，证据脚本为 `web/scripts/extension-network-boundary.mjs`。

实测结论（2026-10-19，本机 Linux/WSL2，Chromium 1248）：

- CSP 能阻止未批准来源的 `fetch`、`<img>`、`<iframe>` 和表单提交。
- CSP **不能**阻止 `window.open()`、`<a target="_blank">` 以及 iframe 自身的
  `location.href` 顶层导航；这三种路径在“网络未授权”和“网络已授权”两种场景下
  都实际到达了未声明来源。
- 现有 iframe sandbox 含 `allow-popups-to-escape-sandbox`，弹窗因此脱离 sandbox，
  其后续导航不受扩展 CSP 约束。
- SDK 的 `openExternal()` 参数校验只是开发者辅助，无法拦截应用直接调用
  `window.open`、创建 `target="_blank"` 链接或在 iframe 内自行导航，因此不是安全边界。
- 弹窗不持有宿主 `MessagePort`、运行时令牌、宿主 Cookie 或扩展 KV 访问权；宿主桥接
  仍按 `event.source === iframe.contentWindow`、opaque origin 与随机握手校验消息。

## 决策（提案）

1. **保留现有 CSP 强制。** 网络授权的有效来源为声明来源与已批准来源的交集；
   没有 `network` 授权或没有批准记录时，所有外部资源指令不产生外部 source。
   这属于已修复缺陷，不回退。
2. **不把 SDK 校验当作安全边界。** `openExternal()` 可以继续做 URL 白名单提示，
   但文档、测试和 UI 不得据此声称弹窗或顶层导航被限制。
3. **V1.3 保留弹窗与 OAuth 能力。** 继续使用
   `allow-popups-to-escape-sandbox` 和标准 `window.opener` 回调；不删除弹窗、
   不改用仅宿主中介的外部打开方式，以免破坏已确认的第三方登录流程。
4. **将弹窗/顶层导航逃逸记录为残余风险。** 在风险清单 R-05（以及 R-04 的
   “网络来源不覆盖顶层导航”说明）中明确：网络来源授权只约束 CSP 覆盖的资源类型，
   不约束应用发起的顶层导航；这不是待修复的实现缺陷，而是当前浏览器边界与
   已确认弹窗决策的组合结果。
5. **宿主安全依赖隔离而非导航限制。** 弹窗与外部页面不能获得宿主 DOM、宿主凭据、
   应用 KV 或既有 MessagePort；宿主只接受来自已握手 iframe 的桥接消息。若未来要求
   限制顶层导航，必须在不破坏 OAuth 的前提下另行设计并更新本 ADR。

## 备选方案

- **移除 `allow-popups-to-escape-sandbox`。** 会让外部 OAuth 页面停留在 opaque
  sandbox 中，通常无法完成登录；属于产品行为回退，未采纳。
- **在 SDK/宿主中统一拦截 `window.open` 与 `target=_blank`。** iframe 内脚本可直接
  调用浏览器 API，宿主无法可靠覆盖所有调用点；字符串校验不能构成边界，未采纳。
- **移除全部弹窗，改为宿主中介打开外部页面。** 可作为未来高安全模式，但会改变
  ADR-0012 已确认的 OAuth 流程，需要单独的产品/安全决策；当前仅作为后续选项。

## 后果

- 管理员批准 `network` 来源后，CSP 覆盖的资源请求按契约工作；未批准来源的
  `fetch`/图片/iframe/表单被浏览器阻止。
- 即使未授予 `network`，扩展仍可通过 `window.open`、`target="_blank"` 或自身
  `location.href` 把用户带到任意外部地址。该行为不得被描述为“网络权限已完全执行”。
- 该能力不会把宿主凭据或桥接令牌带入外部页面，但可能造成用户困惑、钓鱼式跳转或
  `window.opener` 反向导航（R-05）。
- 管理员界面需继续展示 network 来源与 HTTP 风险；来源审批只影响 CSP 资源加载。

## 验证

- `web/scripts/extension-network-boundary.mjs`（Node 内置模块 + 已安装 Chromium，
  不依赖 npm 包）覆盖两种 CSP 场景，断言：
  - 未授权：批准/未声明来源的 `fetch`、图片、iframe 均无外部请求；
  - 已授权：批准来源的 `fetch`、图片、iframe 各命中一次；未声明来源的
    `fetch`/图片/iframe/表单均被阻止；
  - 两种场景下 `window.open`、`target=_blank`、iframe 自身导航均到达外部来源，
    作为已确认的残余风险而非通过项。
- 服务端与本机原生测试见 `docs/testing.md` 的 2026-10-19 记录。
- 未验证：Firefox/WebKit、真实硬件、L2 平台矩阵。不得据本 ADR 宣称这些环境通过。
