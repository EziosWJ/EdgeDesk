# EdgeDesk 编码 Agent 运行规则

在**修改代码之前**先阅读本文件，然后阅读 `docs/spec` 和 `docs/adr` 中相关的文件。

## 单一事实来源

- `docs/spec/` = 已确认的用户可见行为；`docs/adr/` = 已接受的技术决策或提案，注意区分其状态。
- `docs/architecture.md`（模块/线程/资源边界）、`docs/api.md`（HTTP 与 WS 协议）、
  `docs/config.md`（配置项与部署）、`docs/testing.md`（验证矩阵）描述当前实现。
- `docs/phase1.md` 是历史骨架记录；不要将其当作当前功能范围。
- 扩展应用平台核心功能需求基线为 `docs/spec/0007-extension-applications.md`；安全与可靠性加固补充基线为 `docs/spec/0008-extension-platform-hardening.md`。配套决策为 `docs/adr/0011-extension-runtime-and-packages.md`、`docs/adr/0012-extension-security-and-communication.md`、`docs/adr/0013-extension-file-api-and-transfer.md`，风险与实施计划分别见 `docs/security/extension-app-risk-register.md`、`docs/tasks/0007-extension-applications.md`、`docs/tasks/0008-extension-platform-hardening.md`。V1.3 已实现首期静态运行时，但 SPEC-0008 中的修复必须以代码、回归测试与当前 CI 证据验收。数字签名、应用间通信、秘密存储和后端/脚本运行时仍未实现，不得宣称具备。
- Process Supervisor 设计基线为 `docs/spec/0009-process-supervisor.md`，配套决策为 `docs/adr/0015-process-supervisor-architecture.md` 与 `docs/adr/0016-process-supervisor-security.md`，任务拆分见 `docs/tasks/0009-process-supervisor.md`。V1.0 已实现独立 `edgedesk-supervisor` 守护进程与 `/api/supervisor/*`；实际测试与未验证平台以 `docs/testing.md` 为准，未运行的身份切换、init、重启和压力项不得宣称通过。
- React + TypeScript + Vite 与 xterm.js 已确认为前端基线（ADR-0001）；不要再讨论替换。

## 固定的技术边界

- 后端：C++17，锁定 libhv v1.3.4，GCC 15+ Linux Release；musl 静态链接是目标，需要匹配的工具链。
- 资源模式：Release 下使用 GCC 15 `#embed`，本地开发使用磁盘模式。切勿用大量生成的 `0xNN` C++ 数组替代。
- 前端 Phase 1：React + TypeScript + Vite，使用 `web/public/assets` 和来自 `web/src/styles/edgedesk-tokens.css` 的 token。
- 现有核心模块不引入数据库/ORM，设备不运行 Node.js。**SQLite 仅有明确的模块级例外：SPEC-0007 的扩展平台使用独立 SQLite 保存扩展元数据与应用 KV；SPEC-0009 的独立 Process Supervisor 使用自己的 SQLite 保存服务配置和生命周期管理状态。两者数据库不得共用，也不得因此迁移认证、设置、终端或指标模块。** 独立 SDK/CLI 可使用 Node.js 作为开发构建工具，但不得要求设备运行 Node.js。不要引入未获批准的第三方框架。

## 目录归属

| 目录 | 归属 |
| --- | --- |
| `src/core/` | 配置、错误、工具、FD；不得包含 libhv 类型 |
| `src/file/` | PathSandbox、FileService 与统一文件锁；所有路径安全只在这里实现 |
| `src/extensions/` | SemVer/manifest、ZIP 安装、SQLite 扩展元数据/KV、修订生命周期、运行时会话/传输；不得把 SQLite 引入其他模块 |
| `src/terminal/` | PTY、会话、管理器、CommandGuard；路由不得直接 forkpty/kill/waitpid |
| `src/system/` | /proc、/sys、statvfs 采集与采样服务 |
| `src/security/` | 认证、限流、审计 |
| `src/web/` | 静态资源抽象（磁盘 / #embed） |
| `src/http/` | libhv 路由、中间件与协议适配；不包含设备业务逻辑 |
| `src/app/` | 入口与进程级装配；main.cpp 仅作为入口点 |
| `tools/` | 构建期生成器与测试 |
| `web/src/` | React UI；不得调用不安全的系统命令 |
| `web/public/assets/` | 源 SVG 资源；切勿在 Git 中修改生成的 dist |
| `docs/spec/`, `docs/adr/` | 契约 / 决策 |
| `tests/` | 确定性的原生测试 |


已实现：`FileService`（`src/file/`）、`TerminalSessionManager`（`src/terminal/`）、
`SystemMetrics`（`src/system/`）、认证/限流/审计与设置持久化（`src/security/`）、
扩展平台（`src/extensions/`：安装/审批/修订、iframe 运行时、KV、文件传输与锁）、
Process Supervisor（`src/supervisor/` 独立守护进程 + `src/app/supervisor_service.*`
有界 IPC 适配 + `src/http/routes/supervisor_route.*` 与前端 `processes` 应用）。
V1.1 认证由 `AuthStore`（auth.json/锁）、`PasswordHashService`（Argon2id
有界 worker）和 `Auth`（密码登录/会话/Bearer）组成。路由处理器只负责适配协议
输入/输出；不得在路由里实现路径校验、PTY 管理或指标采集。

## 强制性编码约束

1. 保持改动聚焦；绝不在未声明的情况下重写已确认的 API 或决策。
2. 不得使用全局可变业务状态、裸的所有权 new/delete，或无限且无检查的队列/线程。
3. 检查所有系统调用的返回值；对 FD、PTY、socket 和进程生命周期使用 RAII。
4. 所有外部提供的路径都必须校验。未来的特权文件操作需要基于目录 FD、能抵御路径竞态的设计，而不是仅靠 `realpath` 检查。
5. PTY Bash 进程必须在切换标签页、路由变化和刷新后依然存活，且独立于单个 WebSocket 生命周期。不要用 `/bin/sh` 代替 Bash，也不要假设 `.bashrc` 总是被加载。
6. **禁止公网绑定 / 绕过凭据：** Phase 1 仅绑定回环地址。在认证、授权、CSWSH 防护和传输安全经过设计并测试之前，不得暴露 0.0.0.0 或新增特权端点。
7. 切勿用无界的文件系统、进程或监控工作阻塞 libhv 事件循环。
8. 静态资源路由与设备文件管理必须严格分离；扩展静态资源路由也必须与特权 API 路由分离。
9. 扩展应用不得绕过宿主桥接直接访问特权 API；所有扩展文件操作继续通过 `FileService`/`PathSandbox`，不得用前端 `../` 字符串检查替代现有目录 FD、`openat` 和符号链接防护。
10. 区分宿主机构建、交叉编译和真机测试；只声明实际运行过的内容。
11. 新增依赖需要理由说明、版本锁定、许可证检查以及 ARM/musl 影响分析。
12. SQLite 仅允许用于扩展元数据/KV 与独立 Supervisor 数据库（`third_party/sqlite`，公有领域，已固定版本；两库不得共用）；不得据此迁移认证、设置、终端或指标。
13. 扩展特权调用只能经宿主 MessagePort 桥接；不得为 sandbox iframe 开放 Origin:null 直连或把宿主 Cookie/Token 传入 iframe。

## 每个任务的完成标准

测试按改动风险分层。不要每次改动都执行发布级全矩阵；但也不得用快速层代替发布验证。

### L0 编辑循环（每次改动）

- 运行 `scripts/test-fast.sh`。
- 内部执行：`cmake --preset host-offline-tests` + `cmake --build --preset host-offline-tests`
  + `ctest --preset host-offline-tests`；`web/node_modules` 存在时再跑 `npm test`。
- 目标：秒级反馈，只覆盖宿主原生单测和前端单测。

### L1 任务/PR（提交前）

- 运行 `scripts/test-pr.sh`。
- 内部执行：前端 `npm test && npm run build`；`dev-disk` 构建 + `ctest`；
  `scripts/integration_test.sh out/build/dev-disk/bin/edgedesk 19091 web/dist`。
- 所有影响运行行为的改动都必须执行；纯文档改动可只做链接/格式检查。

### L2 全矩阵（main / nightly / release）

- 按 `docs/testing.md` 的“构建与平台矩阵”执行：
  - Embed：`scripts/with-gcc15.sh all release-embed`
  - musl：Bootlin/musl.cc 交叉构建 + `ctest` + 集成测试
  - ARM64：交叉构建 + qemu 集成测试
- CI 当前在 PR 与 main 执行全矩阵；nightly/release 也应执行。若后续改为条件矩阵，
  main/nightly/release 必须保留全量。
- 本地仅在改 `CMakeLists.txt`、`cmake/`、`cmake/toolchains/`、`third_party/`、
  资源模式或准备发布时执行 L2；日常改动交给 L0/L1 与 CI。
- 发布前必须完成 L2；不得用 L0/L1 代替。

- 针对行为或 API 的变化更新文档（`docs/api.md`、`docs/config.md`、`docs/architecture.md`、`docs/testing.md`）。
- 不得编造截图、基准测试或硬件测试结论；未运行的平台在 `docs/testing.md` 中标注为未验证。
- 确保生成的 `out/`、`web/dist/`、`web/node_modules/` 不进入 Git。
- Phase 1 API 契约：`GET /api/health` => HTTP 200 JSON，且 `status=ok`、`service=edgedesk`、`phase=1`。
- 所有后续功能在标记为完成之前，都应有针对具体议题的验收测试。

### 前端样式改动（headless 截图）

仓库直接使用已安装的 `web/node_modules/@playwright/test` 和
`~/.cache/ms-playwright/chromium*`；`scripts/screenshot.sh` 不执行 `npm/npx`，
也不下载浏览器。先启动回环地址上的真实页面（建议使用临时 `--data-dir`），
截图后**实际打开图片检查**；只确认文件生成不算验证。

```bash
# 桌面全页，等待真实内容出现
scripts/screenshot.sh http://127.0.0.1:8080/login \
  /tmp/opencode/login-1440.png --viewport 1440x900 --full-page \
  --wait-selector 'input[type="password"]'

# 移动端
scripts/screenshot.sh http://127.0.0.1:8080/ \
  /tmp/opencode/dashboard-390.png --viewport 390x844 --full-page \
  --wait-selector '.ed-dashboard'
```

- 至少检查 1440×900 与 390×844 两档；等待目标元素或真实数据，避免截到空骨架。
- 检查布局、间距、颜色/token、文字溢出、中文与 emoji 字体；交互态先操作再截图。
- 已登录页面用 `--storage <state.json>` 复用 Playwright storage state；`--color-scheme dark` 可验证深色。
- 截图放 `/tmp` 或 `out/`，不得提交。headless 目视检查是补充，不替代 L0/L1。

## Process Supervisor 约束

- `edgedesk-supervisor` 已实现 V1.0；修改前阅读 SPEC-0009、ADR-0015/0016 和任务计划，平台验证状态以 `docs/testing.md` 为准。
- Supervisor 必须是独立进程，通过受权限保护的 Unix Domain Socket 与 EdgeDesk 后端通信；禁止增加 TCP 管理 listener。
- 服务启动只接受可执行文件和 argv 数组，不使用 Shell 字符串执行。
- root Supervisor 的 IPC 与服务配置属于特权安全边界；不得绕过 `SO_PEERCRED`、现有 Web 认证、CSRF 或审计。
- 进程创建、UID/GID/补充组切换、日志和恢复必须在目标 glibc/musl 与多线程条件下验证；不得用未经验证的 fork 子路径实现。
- 新 Supervisor 数据库与 `extensions.db` 独立；不得迁移认证、设置、终端和指标数据。
