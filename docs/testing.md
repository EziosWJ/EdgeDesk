# EdgeDesk 测试与验证矩阵

> 只记录**实际执行过**的验证。未运行的环境一律标注“未验证”，不得以文档目标代替结果。
> 复现命令附在每节末尾。V1.3 扩展平台新增 5 个原生测试、扩展集成测试、SDK CLI 测试和真实浏览器验收。

## 2026-10-18 V1.3 扩展平台实际执行记录

| 验证 | 命令 | 结果 |
| --- | --- | --- |
| L0 原生 | `scripts/test-fast.sh`（等价 `cmake --preset host-offline-tests` + build + ctest） | **16/16 通过**（新增 `semver_test`、`manifest_test`、`extension_store_test`、`extension_package_test`、`file_lock_test`） |
| L0 前端 | `cd web && npm test` | **144/144 通过**（新增扩展注册表测试；原有测试全部通过） |
| 前端构建 | `cd web && npm run build` | `tsc --noEmit` + Vite 构建通过 |
| SDK/CLI | `cd tools/ext-sdk && npm test` | **4/4 通过**（manifest/资源校验、打包、ZIP 检查、符号链接/重复/绝对路径拒绝） |
| 模板打包 | `node tools/ext-sdk/bin/edgedesk-ext.js pack tools/ext-sdk/templates/vanilla ...` | 通过（5 个文件，ZIP 8.6 KB） |
| L1 集成 | `scripts/integration_test.sh out/build/dev-disk/bin/edgedesk 19094 web/dist` | **全部通过**：auth/api/ws/限流/恢复 + `extension_test` + 崩溃恢复 |
| 浏览器 E2E | Playwright 1.64.1 / Chromium 1248，1440×900 与 390×844；`cd web && EDGEDESK_PASSWORD=... EDGEDESK_SAMPLE_V11_ZIP=... node scripts/extension-e2e.mjs` | 通过：应用管理 UI、安装列表、iframe 握手、KV 计数、终端 `echo hello-e2e`、UI 升级到 1.1.0 并关闭旧窗口、禁用移除启动器并关闭窗口、重新启用、刷新后桌面恢复扩展窗口并新建运行实例；无 console/page error、无 HTTP 4xx、无横向溢出 |
| 崩溃恢复 | 开始 2 MiB 分块上传后 `kill -9`，重启 | 临时文件与 journal 清理，应用仍为 `enabled` |

未执行/未验证：L2 平台矩阵（`release-embed`、musl、ARM64/qemu、真实硬件）本轮未重新运行；
本机无 GitHub Actions 运行记录更新。不得把上表当作跨平台发布验收。


## 2026-10-18 SPEC-0008 复核：当前跨平台 CI 阻断

- 复核提交：`308325a3f5d57e0879b58566cca495406a4b191e`；[GitHub Actions run #39132868799](https://github.com/EziosWJ/EdgeDesk/actions/runs/39132868799)。
- 结果：5 个 job 中 3 个通过（`frontend`、`host-build`、`gcc15-embed`），2 个失败（`x86_64-musl-static`、`arm64-cross`）。
- 两个失败目标在 CMake 配置阶段均未找到目标 ZLIB 头文件/库；跟踪于 [Issue #24](https://github.com/EziosWJ/EdgeDesk/issues/24)。
- 该结果不否定旧 PR #14 当时的 5/5 通过记录，但旧 PR 的通过不能代替当前提交的跨平台验收。SPEC-0008 的安全与可靠性修复另见 [总任务 #16](https://github.com/EziosWJ/EdgeDesk/issues/16)。

## 测试分层（本地入口）

| 层级 | 入口 | 覆盖 | 何时运行 |
| --- | --- | --- | --- |
| L0 编辑循环 | `scripts/test-fast.sh` | `host-offline-tests` 构建 + 16 项原生单测 + 前端单测（`web/node_modules` 存在时；V1.3 为 144 项）+ SDK/CLI 测试（Node 存在时） | 每次改动 |
| L1 任务/PR | `scripts/test-pr.sh` | `dev-disk` 构建 + 16 项原生单测 + 全量集成（含扩展平台）+ 前端 `npm test` / `npm run build` | 提交前 |
| L2 全矩阵 | 见下方“构建与平台矩阵” | `release-embed`、`release-embed-arm64`、`release-disk`、x86_64-musl、arm64-disk、arm64-musl、CI | main/nightly/release；当前 CI 对 PR 也执行全矩阵 |

```bash
# L0（秒级）
scripts/test-fast.sh

# L1（默认集成端口 19091；冲突时用 --port N）
scripts/test-pr.sh
```

- L0/L1 不替代 L2；发布前必须完成 L2。
- `test-fast.sh` 在 `web/node_modules` 缺失时会跳过前端测试并提示 `npm ci`；
  `test-pr.sh` 会自动安装前端依赖。
- `test-pr.sh` 会临时把 `dev-disk` 的 `EDGEDESK_BUILD_WEB` 设为 `OFF`，避免
  CMake 重复调用 npm；退出时恢复原值（需要手动恢复时执行
  `cmake --preset dev-disk -DEDGEDESK_BUILD_WEB=ON`）。
- 集成测试使用 `N..N+5`；默认 `19091`，冲突时用 `--port N` 换一组端口。

> 2026-10-18 V1.2 本机实际执行：`scripts/test-fast.sh` 通过（11/11 原生 + 124 项前端，
> 约 14 s）；`scripts/test-pr.sh` 通过（11/11 原生 + 全量集成 + 前端测试/构建，102 s）。
> 2026-10-17 的 V1.1 记录为：L0 通过（11/11 原生 + 77 项前端）；L1 通过（10/10 原生 +
> 全量集成）。CI 新配置尚未在 GitHub Actions 上运行。
>
> 2026-10-18 V1.2.1 稳定性与安全加固：`scripts/test-fast.sh` 通过（11/11 原生 +
> 140 项前端，约 10 s）。新增回归覆盖：顶部退出登录调用 `POST /api/auth/logout`、
> 未保存编辑器阻止退出、最小化后从图标/Dock/启动器重开保留最大化状态、嵌套相对
> 符号链接（含 `..`）解析、终端订阅回调可重入会话 API。L1 已执行：
> `scripts/test-pr.sh` 通过（11/11 原生 + 140 项前端 + `tsc`/Vite 构建 + 全量集成，126 s）。
> GitHub Actions 全矩阵已在 PR #14 实际运行并通过：`frontend`、`host-build`、
> `gcc15-embed`、`arm64-cross`、`x86_64-musl-static`（5/5，2026-10-18）。

## 自动化测试清单

| 测试 | 覆盖内容 | 环境 | 结果 |
| --- | --- | --- | --- |
| `core_test` | CLI/配置文件/环境变量解析（含 data-dir/cookie-secure/trusted-proxy）、非法参数、SHA-256/HMAC 向量、会话签名与过期、限流桶、审计轮转 | GCC 13.3 host | 通过 |
| `utf8_test` | 合法 UTF-8 原样保留、非法/过长/代理区/超范围序列替换为 U+FFFD、多字节字符跨 `feed()` 边界重组、退出时冲刷不完整尾序列 | GCC 13.3 host | 通过 |
| `auth_store_test` | 未初始化/已初始化/损坏状态、原子写入、0600/0700、flock、恢复重置 | GCC 13.3 host | 通过 |
| `password_hash_test` | 密码策略、Argon2id hash/verify、盐独立、参数升级、有界 worker 队列 | GCC 13.3 host | 通过 |
| `settings_store_test` | 默认值、白名单保存、非法枚举/越界拒绝、损坏文件回退、持久化 | GCC 13.3 host | 通过 |
| `path_sandbox_test` | `..` 穿越、URL 编码、绝对路径、前缀欺骗目录、符号链接逃逸/循环（含 follow 模式）、嵌套相对目标（`dir`、`../..`、逃逸拒绝）、lstat 语义 | GCC 13.3 host | 通过 |
| `file_service_test` | 列表/排序/分页/隐藏文件、`dirs_only` 直接子目录（符号链接不跟随）、文本读写与原子保存保留权限、二进制拒绝、上传/中止/超限、递归删除计数、Range 解析、MIME 判定 | GCC 13.3 host | 通过 |
| `command_guard_test` | 危险命令命中（rm -rf 根、块设备写入、fork 炸弹、关机…）与安全命令不误报 | GCC 13.3 host | 通过 |
| `terminal_session_test` | 真实 Bash 登录 Shell、`~/.profile` 加载、UTF-8/ANSI、TIOCSWINSZ resize、CommandGuard 拦截不执行、回放缓冲有界、订阅断开不杀会话、订阅回调（回放/退出）可重入会话 API 且不死锁、会话上限/空闲回收、缺 Bash 报错而非回退 sh | GCC 13.3 host + /bin/bash 5.2 | 通过 |
| `system_metrics_test` | /proc/stat、loadavg、meminfo（含旧内核回退）、net/dev 速率差分、IPv4/IPv6 地址格式化与 `getifaddrs` 枚举（排除 lo）、mounts 过滤、采样线程与环形缓冲 | GCC 13.3 host | 通过 |
| `semver_test` | 严格 SemVer 解析/拒绝（前导零、v 前缀、非法标识）、pre-release 排序、build 忽略、宿主 API 闭区间 | GCC 13.3 host | 通过 |
| `manifest_test` | 核心字段、权限、来源通配符/CSP、窗口边界、宿主不兼容、保留运行时不执行、非法 ID/版本/来源/图标 | GCC 13.3 host | 通过 |
| `extension_store_test` | SQLite 0700/0600、应用/修订/权限/完整性、操作日志、KV 键限/5 MiB 配额/回滚事务/分页、重启持久化 | GCC 13.3 host | 通过 |
| `extension_package_test` | ZIP 安装、审批/静态资源/SPA、同版本覆盖、升级/降级、完整性隔离与修复、路径穿越/符号链接/重复名/本地头不一致、卸载保留/恢复/删除、修订保留与固定、启动孤儿清理 | GCC 13.3 host | 通过 |
| `file_lock_test` | 共享/独占、硬链接别名、缺失目标父目录身份、多锁原子性、FileService 读写锁、Upload 移动锁转移 | GCC 13.3 host | 通过 |
| `extension_test.py`（L1） | 安装/审批/静态 CSP、运行会话、KV 配额/回滚、目录游标、锁冲突、1 MiB 分块上传/下载 SHA-256、终端创建/关闭、禁用即时失效、升级/同版本覆盖/降级、完整性隔离、修复、卸载保留/恢复/删除、审计事件 | dev-disk + 真实 PTY/文件系统 | 通过 |
| SDK CLI 测试 | manifest/资源/相对路径/符号链接/重复路径校验、ZIP 打包与检查 | Node 24.14 | 通过 |
| `embed_generator_test` | `#embed` 生成器的确定性、路径校验、ETag | Python 3.12 | 通过 |
| 前端单元/DOM 测试 | **V1.2 桌面**：AppRegistry（默认注册、未知 appId、重复注册、元数据/尺寸校验、单/多实例、启动器搜索）、桌面图标布局（网格命中、拖拽换位、边界、去重）、WindowManager（单/多实例、稳定实例 ID、z-index 有界、最小化保持挂载/恢复、最大化几何还原、移动/缩放/视口约束、中等视口完整可见、关闭守卫）、DesktopPersistence（序列化往返、损坏 JSON、版本不兼容、未知 appId、重复单实例、非法几何、敏感字段剥离、localStorage 不可用、恢复时会话失效标记、防抖自动保存）、DesktopShell（单击/双击、Dock 启动/聚焦/多窗口选择、最小化恢复、真全屏与桌面框架恢复、启动器搜索/空态/Esc/焦点返回、Alt 快捷键与输入焦点保护、窄屏强制最大化、退出登录）、终端恢复（session_not_found 不创建替代会话、无 session ID 时才 create、attached 状态）、编辑器生命周期（最小化保留缓冲、未保存关闭确认、外部修改保护、文件消失拒绝保存）、NAS 设备概览（CPU/内存/存储/网卡/系统/服务小组件、多挂载点、多网卡、loopback 排除、缺失值 vs 0、采集中断、加载空态）。**V1.3 扩展平台**：动态应用注册、禁用关闭窗口、待审批应用不可启动、扩展元数据更新。**V1.2.1 回归**：顶部退出登录注销服务端会话并由脏编辑器否决退出、最小化后重开保留最大化状态。**V1.1 回归**：会话持久化、metrics 窗口有界、格式化、FloatingWindow DOM 交互、Sidebar、图表工具、Dashboard 旧断言已迁移、文件管理、认证、设置、偏好。 | Node 24.14 / vitest 3.2 / happy-dom | 通过（144 项）；`npm run build`（tsc + vite）通过 |
| `auth_test.py` | 初始化状态门禁、首次设置密码、重复初始化 409、密码登录、Cookie 属性（含 `--cookie-secure`）、改密后旧 Cookie/旧密码失效、Bearer 不受影响、CSRF、设置白名单、系统信息脱敏、重启后旧 Cookie 失效、损坏状态失败关闭、本机恢复 | dev-disk / host-embed / release-embed / release-embed-arm64 / release-disk / x86_64-musl / arm64-disk / arm64-musl | 全部通过 |
| `api_test.py` | health 契约、密码登录/会话/Bearer、CSRF、目录浏览（含 `dirs_only` 只返回直接子目录）、文本读写、mkdir/rename/delete、流式上传字节一致、Range 206/416、路径沙箱拒绝、JSON 体积限制、系统指标（含 `network[].addresses` 字段） | 见下（三种二进制） | 通过 |
| `ws_test.py` | 未认证/异源握手拒绝、终端 create/input/输出/resize/guard、断线重连（回放）、会话存活、显式关闭、metrics 快照+增量 | 同上 | 通过 |
| `rate_limit_test.py` | 登录限流与通用限流在严格配置下触发 429；伪造 `X-Forwarded-For` 不能绕过 peer-IP 限流 | 同上 | 通过 |
| 审计校验 | `auth.setup`、`login`、`auth.password_change`、`auth.reset`、`file.upload`、`terminal.create` 事件落盘且不含秘密 | 同上 | 通过 |

### 原型还原度与浏览器目视验收

- 逐项对照表：`docs/design/prototype-conformance.md`；快捷键与焦点规则：
  `docs/design/desktop-keyboard-shortcuts.md`。
- 规则：业务功能通过 ≠ 页面验收通过；未在浏览器中实际检查的条目不得标记为通过。
- **2026-10-18 V1.2 实际目视验收（headless Chromium 1.64.1，Playwright，本机）**：
  - 环境：`dev-disk` 二进制 + 未提交的临时 `--data-dir` / `--file-root` / `--state-dir`；
    真实 hostname、真实 `/proc` 指标与真实 Bash PTY；无演示数据。
  - 视口与主题：1440×900 深色为主，1440×900 浅色，390×844 窄屏；浏览器语言 `zh-CN`。
  - 检查页面：桌面、应用启动器、设备概览、文件管理、终端、系统设置，共 6 个页面。
  - 交互结果：登录后进入桌面；单击图标不启动、双击打开设备概览；Dock 打开文件/终端/设置；
    启动器搜索 “设置” 命中、搜索 “zzz” 显示空结果、Esc 关闭并把焦点返回启动器按钮；
    标题栏拖动位移 140×78 px 后标题栏仍可见；最小化后窗口 `display:none` 但 8 个小组件仍挂载，
    Dock 恢复入口可还原；双击标题栏最大化/还原；显式最大化窗口占满整个视口并隐藏顶栏与 Dock，
    还原或最小化后恢复桌面框架；Dock `z-index=400` 高于普通窗口层 `200`；
    刷新后恢复 4 个窗口；浅色主题下文字/图标/卡片可辨识；390×844 下 4 个窗口自动最大化，
    `scrollWidth == clientWidth == 390`，无横向溢出。
  - 2026-10-18 布局回归：1440×900、900×700、800×700、390×844 下逐页检查设置、文件管理、
    设备概览、会话管理、终端、编辑器与预览；应用内容宽度等于窗口主体、无横向滚动；
    设置卡片随窗口宽度为 1/2/3 列；新建窗口在中等视口完整可见。
  - 真实数据核对：CPU 使用率/核心数/负载、内存已用/可用/缓存/Swap、4 个真实挂载点、
    4 个网卡接口（聚合速率排除 loopback）、系统信息与服务状态均来自 API/WS；
    缺失字段显示“—”，未出现原型演示数值。
  - 控制台：无 console error / page error；localStorage 布局存在，且不包含密码、Token 或会话 Cookie。
  - 截图仅保存在临时目录用于人工查看，未提交到 Git；未使用整页截图切片作为生产 UI。

### 端到端套件（`scripts/integration_test.sh`）

| 二进制 | 模式 | 结果 |
| --- | --- | --- |
| `dev-disk`（GCC 13.3，Debug） | 磁盘资源 + React dist | 全部通过（V1.1 初始化/登录/改密/重启/恢复/损坏状态） |
| `host-embed`（GCC 15.2.0，glibc，动态） | 宿主验证用 `#embed` 单二进制 | 全部通过（V1.1 全流程） |
| `release-embed`（Bootlin musl GCC 15.3，`-static-pie`，`#embed`） | 单二进制，无动态依赖 | 全部通过（V1.1 全流程 + 前端外壳与哈希资源校验） |
| `release-embed-arm64`（Bootlin musl GCC 15.3，`-static-pie`，`#embed`，qemu 运行） | 单二进制，无动态依赖，AArch64 Bash PTY | 全部通过（V1.1 全流程） |
| `x86_64-musl`（musl cross GCC 11.2，`-static-pie`） | 磁盘资源，无动态依赖 | 全部通过（V1.1 全流程） |
| `arm64-disk`（gcc-aarch64-linux-gnu 13，qemu-user 8.2.2 运行） | 磁盘资源，AArch64 原生 Bash PTY | 全部通过（V1.1 全流程；9 个原生测试二进制在 qemu 下通过） |
| `arm64-musl`（musl cross GCC 11.2，`-static-pie`，qemu 运行） | 磁盘资源，AArch64 Bash PTY | 全部通过（V1.1 全流程） |

> 2026-10-18 V1.2.0 重新执行了以下 L2 lane，均通过：`host-embed`（GCC 15.2.0 glibc，
> 11/11 原生 + 全量集成）、`release-embed`（Bootlin x86_64 musl GCC 15.3，static-pie +
> `#embed`，11/11 原生 + 全量集成）、`release-embed-arm64`（Bootlin aarch64 musl GCC 15.3，
> static-pie + `#embed`，qemu-user 运行，全量集成）。V1.2 未改 C++ 业务逻辑，但仍按发布门槛
> 复跑了上述平台。`release-disk`、musl.cc GCC 11 磁盘模式、ARM32 与真实硬件本次未重新执行，
> 相关历史结果仅代表 V1.1 状态。
>
> Argon2id 基线参数 `m=64 MiB, t=3, p=1` 的实测：宿主 Debug 库约 0.77 s/次；
> AArch64 Release 库在 qemu-user 下约 0.62 s/次（单次登录哈希，单 worker）。
> 这是本地测量，不是 ARM 真实硬件基准；真实设备仍需复测。
>
> 注：2026-10-17 V1.1.0 在上述目标上实际执行了 `scripts/integration_test.sh`
> 的完整流程（初始化、登录、改密、重启后凭据/设置保留、限速与 XFF 伪造、
> Secure Cookie、服务运行中拒绝恢复、本机恢复、损坏状态、REST/WebSocket/审计）。`arm64-disk` 的原生测试通过
> `qemu-aarch64-static -L ~/opt/arm64/root` 逐个运行；`arm64-musl` 与
> `release-embed-arm64` 构建目录未生成单元测试目标，运行证据来自集成测试。
> Argon2id 依赖在 glibc、musl、x86_64 与 AArch64 上均完成静态链接。

```bash
bash scripts/integration_test.sh out/build/dev-disk/bin/edgedesk 19091 web/dist
bash scripts/integration_test.sh out/build/host-embed/bin/edgedesk 19391 web/dist
bash scripts/integration_test.sh out/build/release-embed/bin/edgedesk 19491 web/dist
bash scripts/integration_test.sh out/build/release-embed-arm64/bin/edgedesk 19591 "" \
  "$HOME/opt/arm64/root/usr/bin/qemu-aarch64-static -L $HOME/opt/arm64/root"
```

## 构建与平台矩阵

| 目标 | 工具链 | 结果 |
| --- | --- | --- |
| x86_64 Linux，磁盘模式（Debug） | GCC 13.3（Ubuntu 24.04 系统编译器） | 通过（`dev-disk`，V1.1 单元测试 + 集成测试） |
| x86_64 Linux，`#embed`（宿主 glibc，动态） | GCC 15.2.0（用户目录安装，见 docs/dev/gcc15-local-toolchain.md） | 通过（`host-embed`，仅本地验证，不作为发布产物） |
| x86_64 **musl static-pie + `#embed`** | Bootlin `x86-64--musl--stable-2026.08-1`（GCC 15.3） | 通过（`release-embed`，V1.1 单元测试 + 集成测试；构建后校验无 `INTERP`/`NEEDED`） |
| x86_64 musl **static-pie**，磁盘模式 | musl.cc `x86_64-linux-musl-cross`（GCC 11.2.1）**与** Bootlin `x86-64--musl--stable-2024.05-1`（GCC 13.3） | 两套工具链均通过（V1.1 单元测试 + 集成测试；Argon2 静态链接） |
| AArch64 **musl static-pie + `#embed`** | Bootlin `aarch64--musl--stable-2026.08-1`（GCC 15.3）+ qemu-user 8.2.2 | 通过（`release-embed-arm64`，V1.1 集成测试） |
| AArch64（ARM64）Linux，磁盘模式（glibc） | Ubuntu cross `gcc-aarch64-linux-gnu` 13 + qemu-user-static 8.2.2 | 通过（V1.1 单元测试 + 集成测试，含 ARM64 Bash PTY） |
| AArch64 musl **static-pie**，磁盘模式 | musl.cc `aarch64-linux-musl-cross`（GCC 11.2.1） | 通过（V1.1 集成测试；Argon2 静态链接） |
| ARM32（armv7 musl） | `arm-linux-musleabihf` 工具链（Bootlin 2026.08 布局已支持） | **未验证**（本机未安装该工具链） |
| GCC 15 + musl + `#embed` 组合 | Bootlin stable-2026.08（GCC 15.3） | 通过（x86_64 与 aarch64，见上） |
| OpenHarmony ARM64 | 需要对应 SDK | **未验证** |
| 真实设备（工控板/网关） | 目标硬件 | **未验证**（当前仅有 QEMU 运行证据） |
| GitHub Actions CI | 本仓库 workflow：frontend + host-build / gcc15-embed / arm64-cross / x86_64-musl-static；npm/ccache/交叉工具链缓存，`web/dist` 通过 artifact 复用 | 通过（PR #14，2026-10-18，5/5 jobs） |

> 2026-10-17 之后 CI 配置增加了 `frontend` 前置 job、npm/ccache/交叉工具链缓存和
> `web/dist` artifact 复用；`x86_64-musl-static` 已改为 musl + `#embed` 静态发布
> lane，`arm64-cross` 改用 Bootlin aarch64 musl + `#embed`，`gcc15-embed` 保留为
> 宿主 GCC 15 兼容性验证。新配置需在下一个 PR 实际运行后回填结果。
> `frontend` 失败时依赖它的 `host-build`/`gcc15-embed` 会显式失败；仍建议把
> `frontend` 加入 required checks。
> 2026-10-18 已在 V1.2.1 PR #14 上实际运行新配置：`frontend`（35 s）、
> `host-build`（2m13s）、`gcc15-embed`（2m6s）、`arm64-cross`（2m5s）、
> `x86_64-musl-static`（1m57s）全部成功，无失败 job。

### 发布产物体积与静态校验

Release/MinSizeRel 构建默认在链接期加 `-s`（`EDGEDESK_STRIP_BINARY=ON`，等价
`strip -s`），并执行 `cmake/verify_static.cmake` 校验产物不含 `INTERP`/`NEEDED`；
Debug 构建和单元测试可执行文件不 strip，保留符号用于调试。
2026-10-17 在 V1.1 上实测（`file` 均报告 `stripped`，对应集成测试通过）：

| 产物 | 大小 | 链接方式 |
| --- | --- | --- |
| `release-embed`（x86_64，musl GCC 15.3 + `#embed`） | 3.5 MiB | static-pie |
| `release-embed-arm64`（aarch64，musl GCC 15.3 + `#embed`） | 3.2 MiB | static-pie |
| `release-disk`（x86_64，musl，磁盘模式） | 2.8 MiB | static-pie |
| `host-embed`（x86_64 glibc，仅本地验证） | 2.5 MiB | 动态（非发布产物） |
| `x86_64-musl`（musl.cc GCC 11.2，磁盘模式） | 2.6 MiB | static-pie |
| `bootlin-check`（Bootlin GCC 13.3，磁盘模式） | 2.8 MiB | static-pie |
| `arm64-musl`（musl.cc GCC 11.2，磁盘模式） | 2.4 MiB | static-pie |
| `arm64-disk`（aarch64 glibc，磁盘模式） | 1.4 MiB | 动态（验证用） |

未 strip 的 static-pie 产物约 13 MiB，其中约 9.4 MB 来自 musl 工具链
`libstdc++.a`/`libgcc.a` 携带的调试段，与业务代码无关。`release-*` 产物均无动态
依赖（`ldd` 报告 `not a dynamic executable`）；磁盘资源模式仍需单独部署 `web/dist`。
strip 只减小分发体积，静态链接与零依赖由 musl 工具链、`EDGEDESK_STATIC_PIE=ON`
和链接后校验共同保证。

### libhv 依赖的已知构建适配

libhv v1.3.4 的部分头文件依赖“隐式包含”（`struct stat`、fcntl 标志、pthread 常量、
`environ`）。glibc 工具链隐式提供这些声明；部分 musl 工具链（如 Bootlin GCC 13）
不提供，且 musl 仅在 `_GNU_SOURCE` 下声明 `environ`。因此 `cmake/Libhv.cmake` 在
**非 glibc** 目标上为 `hv_static` 追加 `-D_GNU_SOURCE` 与若干个 `-include` 标志
（不改动固定的上游源码）；glibc 构建保持原有编译参数不变。

### 已知缺口与诚实声明

- **musl + `#embed` 的发行链路**已用 Bootlin `stable-2026.08`（GCC 15.3）在
  x86_64 与 aarch64 上完成验证；`release-*` 预设均为 static-pie，并由构建后的
  `cmake/verify_static.cmake` 检查无 `INTERP`/`NEEDED`。`host-embed`（宿主 glibc）
  仅用于本地验证，明确不是发布产物。
- **ARM32 与真实设备**没有运行证据；`cmake/toolchains/arm-linux-musleabihf.cmake`
  已支持 musl.cc 与 Bootlin 两种布局，但未安装工具链、未构建、未运行。
  不是“已成功”的声明。
- Playwright headless Chromium 已完成 V1.2 桌面、启动器、设备概览、文件管理、终端、设置
  六个页面的实际目视验收（1440×900 深/浅色与 390×844 窄屏），详见上文。Chrome/Firefox
  的非 Chromium 手工交互、真实移动设备与 Safari 仍未验证；不声明通过。
- 原型还原度：V1.2 页面已按对照表完成浏览器目视核对并回填；V1.1 历史条目中仍未复跑的
  细节保持原状态，不因本轮 V1.2 验收自动标记为通过。
- 性能与资源占用没有做基准测试，不提供数字。

## ARM64 QEMU 运行环境（复现说明）

```bash
# 1) 无 root 获取交叉工具链与 qemu-user
mkdir -p ~/opt/arm64/debs && cd ~/opt/arm64/debs
apt-get download $(apt-cache depends --recurse --no-recommends --no-suggests \
  --no-conflicts --no-breaks --no-replaces --no-enhances g++-aarch64-linux-gnu qemu-user-static \
  | grep '^\w' | sort -u)
for d in *.deb; do dpkg -x "$d" ~/opt/arm64/root; done

# 2) host 侧交叉 binutils 需要的库（避免污染系统 LD_LIBRARY_PATH）
mkdir -p ~/opt/arm64/hostlibs && cd ~/opt/arm64/hostlibs
ln -sf ~/opt/arm64/root/usr/lib/x86_64-linux-gnu/lib{opcodes-2.42-arm64,bfd-2.42-arm64,ctf-arm64,ctf-nobfd-arm64}.so .
ln -sf libctf-arm64.so.0.0.0 libctf-arm64.so.0
ln -sf libctf-nobfd-arm64.so.0.0.0 libctf-nobfd-arm64.so.0

# 3) 运行期布局（glibc 三连目录 + usrmerge + ARM64 bash 便于 PTY 测试）
cd ~/opt/arm64/root
mkdir -p lib usr/lib
ln -sfn ../usr/aarch64-linux-gnu/lib lib/aarch64-linux-gnu
ln -sfn ../aarch64-linux-gnu/lib usr/lib/aarch64-linux-gnu
ln -sf ../usr/aarch64-linux-gnu/lib/ld-linux-aarch64.so.1 lib/ld-linux-aarch64.so.1
ln -sfn usr/bin bin
# ARM64 bash（选择测试用 shell）
apt-get download -o Dir::Etc::sourcelist=<arm64 sources.list> ... bash:arm64  # 见下方脚本说明
dpkg -x bash_*_arm64.deb ~/opt/arm64/root && dpkg -x libtinfo6_*_arm64.deb ~/opt/arm64/root

# 4) 交叉构建（磁盘模式；#embed 需要 GCC 15+ 交叉工具链）
LD_LIBRARY_PATH=~/opt/arm64/hostlibs cmake -S . -B out/build/arm64-disk -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DEDGEDESK_WEB_MODE=disk -DEDGEDESK_BUILD_WEB=OFF \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/aarch64-linux-gnu.cmake \
  -DEDGEDESK_TOOLCHAIN_ROOT=$HOME/opt/arm64/root
LD_LIBRARY_PATH=~/opt/arm64/hostlibs cmake --build out/build/arm64-disk -j 6
file out/build/arm64-disk/bin/edgedesk   # ELF 64-bit ... ARM aarch64
```
