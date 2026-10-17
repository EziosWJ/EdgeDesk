# EdgeDesk 编码 Agent 运行规则

在**修改代码之前**先阅读本文件，然后阅读 `docs/spec` 和 `docs/adr` 中相关的文件。

## 单一事实来源

- `docs/spec/` = 已确认的用户可见行为；`docs/adr/` = 已接受的技术决策或提案，注意区分其状态。
- `docs/architecture.md`（模块/线程/资源边界）、`docs/api.md`（HTTP 与 WS 协议）、
  `docs/config.md`（配置项与部署）、`docs/testing.md`（验证矩阵）描述当前实现。
- `docs/phase1.md` 是历史骨架记录；不要将其当作当前功能范围。
- React + TypeScript + Vite 与 xterm.js 已确认为前端基线（ADR-0001）；不要再讨论替换。

## 固定的技术边界

- 后端：C++17，锁定 libhv v1.3.4，GCC 15+ Linux Release；musl 静态链接是目标，需要匹配的工具链。
- 资源模式：Release 下使用 GCC 15 `#embed`，本地开发使用磁盘模式。切勿用大量生成的 `0xNN` C++ 数组替代。
- 前端 Phase 1：React + TypeScript + Vite，使用 `web/public/assets` 和来自 `web/src/styles/edgedesk-tokens.css` 的 token。
- 不使用数据库、ORM、设备上的 Node 运行时，也不引入不必要的第三方框架。

## 目录归属

| 目录 | 归属 |
| --- | --- |
| `src/core/` | 配置、错误、工具、FD；不得包含 libhv 类型 |
| `src/file/` | PathSandbox 与 FileService；所有路径安全只在这里实现 |
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
`SystemMetrics`（`src/system/`）、认证/限流/审计与设置持久化（`src/security/`）。
V1.1 认证由 `AuthStore`（auth.json/init-code/锁）、`PasswordHashService`（Argon2id
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
8. 静态资源路由与设备文件管理必须严格分离。
9. 区分宿主机构建、交叉编译和真机测试；只声明实际运行过的内容。
10. 新增依赖需要理由说明、版本锁定、许可证检查以及 ARM/musl 影响分析。

## 每个任务的完成标准

- 运行相关的测试/构建命令，如实报告确切的成功与失败：
  - 原生：`cmake --preset host-offline-tests && cmake --build --preset host-offline-tests && ctest --preset host-offline-tests`
  - 服务端：`cmake --build --preset dev-disk` + `ctest --preset dev-disk`
  - 端到端：`bash scripts/integration_test.sh out/build/dev-disk/bin/edgedesk 19091 web/dist`
  - 前端：`cd web && npm run build && npm test`
  - Embed：`scripts/with-gcc15.sh all release-embed`
- 针对行为或 API 的变化更新文档（`docs/api.md`、`docs/config.md`、`docs/architecture.md`、`docs/testing.md`）。
- 不得编造截图、基准测试或硬件测试结论；未运行的平台在 `docs/testing.md` 中标注为未验证。
- 确保生成的 `out/`、`web/dist/`、`web/node_modules/` 不进入 Git。
- Phase 1 API 契约：`GET /api/health` => HTTP 200 JSON，且 `status=ok`、`service=edgedesk`、`phase=1`。
- 所有后续功能在标记为完成之前，都应有针对具体议题的验收测试。
