# SPEC-0005 开发任务拆分：V1.1 管理员认证与系统设置

- 状态：**Implemented（V1.1.0，2026-10-17）**
- 依据：[SPEC-0005](../spec/0005-authentication-and-settings.md)、[ADR-0008](../adr/0008-authentication-and-settings.md)、[ADR-0003](../adr/0003-v1-auth-api-protocol.md)、[ADR-0002](../adr/0002-architecture-security.md)
- 目标版本：V1.1.0
- 关联实现：`src/security/`、`src/http/`、`src/app/`、`src/core/`、`web/src/`、`tests/`、`scripts/`

> 本文把 SPEC-0005 的 P0 范围拆成可独立实现、独立验证的任务。
> 任务编号用于提交信息、PR 标题和测试报告引用，例如 `auth(T04): ...`。
> 第 2 节的实现决策已随 V1.1.0 实施确认；实际验证结果见 [验证矩阵](../testing.md)。

## 1. 完成定义（Definition of Done）

V1.1 认证与设置只有同时满足以下条件才算完成：

1. SPEC-0005 第 9 节 12 条验收标准全部有自动化或可复现证据。
2. 全新数据目录可以完成“设置密码 → 登录 → 修改设置”的完整流程。
3. 已初始化实例在重启后保留管理员密码和应用设置；旧 Cookie 会话因进程重启失效。
4. 未初始化、未登录、已登录、认证损坏四种状态在服务端和前端都得到明确处理。
5. 旧版 `POST /api/auth/login {"token": ...}` 不再签发 Cookie；Bearer Token 仍可直接访问受保护 API。
6. 认证文件损坏、数据目录不可写、密码过短/确认不一致、服务运行中执行恢复命令等失败路径都有测试。
7. Argon2id、密码策略、会话 Cookie、CSRF/CSWSH、登录限流、审计脱敏均通过安全测试。
8. 下列命令全部通过，且 `docs/testing.md` 记录真实结果：

   ```bash
   cmake --preset host-offline-tests && cmake --build --preset host-offline-tests && ctest --preset host-offline-tests
   cmake --build --preset dev-disk && ctest --preset dev-disk
   bash scripts/integration_test.sh out/build/dev-disk/bin/edgedesk 19091 web/dist
   cd web && npm run build && npm test
   scripts/with-gcc15.sh all release-embed
   ```

9. 未运行的平台（ARM64、musl、真实设备）在文档中明确标注“未验证”，不得用目标代替结果。
10. `docs/api.md`、`docs/config.md`、`docs/architecture.md`、`docs/testing.md`、`README.md` 与实现一致。

## 2. 开工前必须确认的实现决策

以下是 SPEC-0005 未完全展开、但会直接影响实现的点。表中“建议值”是本文的推荐方案，需要产品或架构负责人确认后写回 SPEC/ADR。

| ID | 问题 | 建议值 | 影响 |
| --- | --- | --- | --- |
| D1 | `--data-dir` 默认路径 | 优先 `$XDG_DATA_HOME/edgedesk`（绝对路径），否则 `$HOME/.local/share/edgedesk`；两者都不可用时启动失败并提示显式 `--data-dir`。禁止回退到 `state-dir`、`/tmp`、`XDG_RUNTIME_DIR`。systemd 示例使用 `StateDirectory=edgedesk` + `--data-dir=/var/lib/edgedesk`。 | 配置、部署、恢复命令 |
| D2 | 首次初始化 | 未初始化实例只要求设置管理员密码和确认密码；接口限流；已初始化后重复 setup 返回 409。不生成、不校验本机初始化码。 | 初始化流程、前端、审计 |
| D3 | 密码长度与编码 | 最少 6 个 Unicode 码点，最多 1024 个 UTF-8 字节；允许空格和任意可打印 Unicode；不强制组合规则；不做隐式 Unicode 归一化，前端提示“按输入原样处理”。超长返回 400/413，不进入哈希。 | 登录/初始化/改密、前端校验 |
| D4 | Argon2id 参数与并发 | 基线 `m=64 MiB, t=3, p=1`，盐 16 字节，输出 32 字节；在 x86_64 与 ARM64 目标实测后定稿。哈希在**有界 worker** 中执行（默认 1 个并发，最大 2），不得阻塞 libhv 事件循环；队列满时快速返回 503，不无限排队。 | 安全、性能、线程模型 |
| D5 | Cookie `Secure` 与可信代理 | 新增 `--cookie-secure`（默认关闭）；HTTPS 部署必须显式开启。新增 `--trusted-proxy <IP/CIDR>`（可重复，默认空）；只有来自可信代理的请求才解析 `X-Forwarded-For`/`X-Forwarded-Proto`。未配置时一律使用 peer IP。 | 认证、限流、审计、部署 |
| D6 | 设置的服务端/浏览器边界 | 写入 `settings.json`：`theme`、`files.view`、`files.show_hidden`、`terminal.font_size`、`dashboard.metrics_window`。继续放 `localStorage`：侧栏折叠、目录树宽度、浮窗位置/尺寸、当前 Tab、最近路径、终端会话 id。高级配置只读。 | 设置 API、前端状态 |
| D7 | 认证文件损坏时的运行状态 | 服务可以启动，但进入 `corrupt` 降级态：只提供静态资源、健康检查和通用故障页；`setup-status` 返回 `state=corrupt`；业务 API 与 WS 返回 `503 auth_state_corrupt`；禁止远程重新初始化。启动日志给出本机恢复命令。 | 启动流程、中间件、前端 |
| D8 | `GET /api/auth/session` 是否公共 | 建议保持公共：未认证时返回 `200 {"authenticated":false,"method":"none"}`，便于前端启动时判断；已认证返回 `method=cookie\|bearer` 和过期时间。若改为 401，必须同步修改前端启动逻辑和审计噪声测试。 | 前端、中间件、API 文档 |
| D9 | 旧 Token 登录与自动生成 Token | 删除“未配置 token 时自动生成 token”；未配置 `--auth-token-file`/`EDGEDESK_AUTH_TOKEN`/`--auth-token` 时 Bearer 不可用。删除 `POST /api/auth/login {"token":...}` 的 Cookie 交换；保留 Bearer 直接认证。非回环监听仍必须显式 `--host`，部署文档要求 TLS/VPN。 | 配置、迁移、脚本 |
| D10 | 核心层 JSON 序列化 | `src/core` 不允许 libhv 类型，`host-offline-tests` 也不构建 libhv。`auth.json`/`settings.json` 需要选择一个可离线、可静态链接的 JSON 实现：优先评估固定版本的 header-only `nlohmann/json`（MIT）或小型严格解析器；不得手写不受测试覆盖的通用 JSON 解析器。 | 依赖、构建、单元测试 |

## 3. 任务依赖与交付顺序

```text
T01 决策定稿
 ├── T02 Argon2id 依赖与基准
 └── T03 配置与 data-dir
      └── T04 认证持久化与状态机
           ├── T05 密码哈希服务（也依赖 T02）
           ├── T08 本机恢复命令
           └── T06 会话与认证服务
                └── T07 HTTP 认证端点与状态门禁
                     └── T09 设置与系统信息 API
                          ├── T10 前端认证状态与页面
                          └── T11 前端设置页与偏好
                               ├── T12 单元测试与安全边界
                               ├── T13 集成/E2E 与迁移测试
                               └── T14 跨平台构建、文档与发布
```

> **分支策略：** V1.1 认证是破坏性迁移。建议创建长期分支 `feat/v1.1-auth-settings`，T03–T11 合并到该分支；T01/T02 若不改变运行行为可直接进 `main`。最终由 T14 以单个 PR 合并到 `main`，避免中途出现“新后端 + 旧前端”或“旧测试 + 新登录”的不可用状态。每个任务仍需独立提交、独立测试、可回滚。

## 4. 任务清单

### T01 — 决策定稿与契约补充（P0，S）

- **依赖：** 无。
- **目标：** 把第 2 节 D1–D10 的结论写回 SPEC-0005/ADR-0008（或新增一份决策记录），消除“首次初始化”“设置字段边界”“损坏状态 API”等歧义。
- **改动：**
  - `docs/spec/0005-authentication-and-settings.md`
  - `docs/adr/0008-authentication-and-settings.md`
  - 必要时更新 `docs/config.md` 的目标配置表（标注“V1.1 目标”）。
- **验收：**
  - 10 个决策都有明确结论；没有“待定”进入实现。
  - SPEC 的验收标准与 ADR 的 API 表一致。
  - 文档链接检查通过，`git diff --check` 通过。
- **验证：** 文档审阅；不涉及代码。

### T02 — Argon2id 依赖与基准（P0，M）

- **依赖：** T01（D4/D10）。
- **目标：** 选定并锁定密码哈希实现，证明它能在目标工具链上静态链接，且不会把设备内存打爆。
- **改动：**
  - `CMakeLists.txt`、新增 `cmake/Argon2.cmake`（或等价模块）。
  - 依赖来源二选一：固定版本的 PHC reference `phc-winner-argon2`（CC0/Apache-2.0）源码，或发行版 `libargon2`；优先可离线、可交叉编译的源码方式。
  - 新增最小验证程序/测试（可放在 `tests/core/` 或独立的 `tests/security/`）。
- **验收：**
  - Argon2id hash/verify 通过标准测试向量或自生成向量。
  - x86_64 glibc、x86_64 musl static-pie、ARM64 交叉编译均成功；`file`/`ldd` 证明无动态依赖（musl）。
  - 记录 `m=64 MiB, t=3, p=1` 与 `m=32 MiB` 在宿主和 ARM64（QEMU）上的耗时/内存；若 64 MiB 不可接受，提交替代参数，不得静默降级。
  - 依赖版本/提交、许可证、ARM/musl 影响写入 ADR-0008 或 `docs/testing.md`。
- **验证：**
  ```bash
  cmake --preset host-offline-tests && cmake --build --preset host-offline-tests && ctest --preset host-offline-tests
  cmake --build --preset dev-disk && ctest --preset dev-disk
  # 交叉构建命令见 docs/testing.md
  ```

### T03 — 配置与持久化数据目录（P0，S）

- **依赖：** T01（D1/D5/D9）。
- **目标：** 增加 `--data-dir`、`--cookie-secure`、`--trusted-proxy` 及对应配置文件/环境变量；把运行期 `state-dir` 与持久化 `data-dir` 分开。
- **改动：**
  - `src/core/config.hpp`、`src/core/config.cpp`：新增字段、解析、帮助文本、校验。
  - `src/core/config.*`：`default_data_dir()`、`ensure_data_dir()`（0700，绝对路径，不跟随符号链接）。
  - `src/app/application.cpp`：启动时创建/检查 data-dir；日志打印路径但不打印秘密。
  - `tests/core/core_test.cpp`：默认值、优先级、相对路径、权限、不可写场景。
  - `docs/config.md`：V1.1 配置项（可先标注目标）。
- **验收：**
  - 优先级仍为“默认值 < 配置文件 < 环境变量 < 命令行”。
  - 未显式配置时使用 D1 的持久默认；`HOME`/`XDG_DATA_HOME` 都不可用时启动失败并给出可操作错误。
  - data-dir 不存在时以 0700 创建；已存在但非目录/不可写时失败。
  - `host-offline-tests` 通过，`src/core` 仍不包含 libhv 类型。
- **验证：** 单元测试 + `--help` 人工检查。

### T04 — 认证持久化与状态机（P0，M）

- **依赖：** T03、T01（D2/D7/D10）。
- **目标：** 实现 `auth.json`、锁文件和四种认证状态；为上层提供稳定的读写/恢复原语。
- **改动：**
  - 新增 `src/security/auth_store.{hpp,cpp}`（或等价命名）。
  - 新增 `src/core/atomic_file.{hpp,cpp}`：目录 FD、`O_NOFOLLOW`、临时文件、`fsync`、`renameat`、目录 `fsync`、权限设置。
  - `auth.json` schema（示例）：
    ```json
    {
      "schema": 1,
      "algorithm": "argon2id",
      "params": {"m": 65536, "t": 3, "p": 1},
      "salt": "<base64>",
      "hash": "<base64>",
      "credential_version": 1,
      "created_at": "2026-10-17T00:00:00Z"
    }
    ```
  - 状态枚举：`uninitialized`、`initialized`、`corrupt`；启动时读取并校验，损坏时进入 D7 降级态。
  - `.lock`（`flock`）生命周期；旧版本遗留的 `init-code` 文件在启动时尽力清理。
  - 严格限制文件大小（例如 64 KiB），拒绝未知 schema/算法/参数、重复键和越界值。
- **验收：**
  - 新目录 → `uninitialized`；成功写入 `auth.json` → `initialized`。
  - `auth.json` 缺失、损坏、不可读、版本不支持分别有测试；损坏不会自动变成未初始化。
  - 写入是原子的：模拟中途失败不会留下半写文件；目录权限 0700、文件 0600。
  - 恢复原语只重置认证状态，不动 `settings.json`、审计日志和业务文件。
  - 锁被占用时恢复操作返回明确错误。
- **验证：** 新增 `auth_store_test`（可加入 `core_test` 或独立目标）+ `ctest --preset host-offline-tests`。

### T05 — 密码哈希服务（P0，M）

- **依赖：** T02、T04、T01（D3/D4）。
- **目标：** 把 Argon2id 封装成可测试、可限流、不会阻塞事件循环的服务。
- **改动：**
  - 新增 `src/security/password_hash.{hpp,cpp}`：`hash_password`、`verify_password`、`needs_rehash`、参数序列化。
  - 16 字节随机盐、32 字节输出；密码最大 1024 UTF-8 字节；哈希前后清零临时缓冲区（`explicit_bzero`/等价实现）。
  - 有界 worker 池（默认 1 并发、最大 2；队列有上限），提供异步完成回调；HTTP 层通过 `HttpResponseWriterPtr` 写回响应。
  - 新增 `tests/security/password_hash_test.cpp`。
- **验收：**
  - 正确密码通过、错误密码失败、相同密码两次哈希不同（盐独立）。
  - 参数记录可解析；`needs_rehash` 在参数升级时返回 true。
  - 超长密码在进入哈希前被拒绝；worker 队列满时明确返回 503/429，不无限排队。
  - 事件循环线程不执行 Argon2；通过代码审查/测试确认。
- **验证：** 单元测试 + 集成测试中的登录耗时/并发场景。

### T06 — 会话与认证服务（P0，M）

- **依赖：** T04、T05。
- **目标：** 用密码登录替换 Token 登录，同时保留可选 Bearer；实现会话签发、校验、登出和改密后的全局失效。
- **改动：**
  - 重构 `src/security/auth.{hpp,cpp}`：保留 `verify_bearer`；移除 `verify_token` 作为 Cookie 登录入口；新增 `login_password`、`change_password`、`rotate_session_key`、`session_info`。
  - 会话 Cookie：`edgedesk_session`、`HttpOnly; SameSite=Strict; Path=/`、`Max-Age`，按 D5 加 `Secure`；默认 12 小时。
  - 进程内会话密钥继续使用 CSPRNG；改密时在互斥保护下轮换，使旧 Cookie 全部失效。
  - 线程安全：认证状态读多写少；Argon2 校验期间不持锁；提交改密前校验 `credential_version` 未变。
  - 删除“未配置 token 时自动生成 token”的逻辑；Bearer 未配置时返回不可用。
- **验收：**
  - 正确密码登录成功；错误密码返回通用失败；未初始化/损坏状态不能登录。
  - 改密成功后旧 Cookie 全部 401，新密码可登录，Bearer 行为不变。
  - 进程重启后旧 Cookie 失效；`auth.json` 中的密码仍可登录。
  - 无 token 配置时 Bearer 不可用，网页密码登录不受影响。
- **验证：** 新增 `auth_service_test` + 集成测试。

### T07 — HTTP 认证端点与状态门禁（P0，L）

- **依赖：** T06、T03（D5/D8）。
- **目标：** 落地 SPEC-0005 第 8 节认证 API，并保证未初始化/未登录/损坏状态不能访问业务 API。
- **改动：**
  - `src/http/routes/auth_route.{hpp,cpp}`：
    - `GET /api/auth/setup-status`
    - `POST /api/auth/setup`
    - `POST /api/auth/login`
    - `POST /api/auth/logout`
    - `GET /api/auth/session`
    - `POST /api/auth/password`
  - `src/http/middleware.{hpp,cpp}`：按状态维护公共路径。始终公共：静态资源、`GET /api/health`、`GET /api/auth/session`（D8）、`POST /api/auth/logout`。未初始化额外允许 `setup-status`、`setup`；已初始化额外允许 `login`。损坏状态允许静态资源、健康检查、`session`、`logout` 和通用故障响应，拒绝 `setup`/`login` 与全部业务 API/WS，返回 `503 setup_required` 或 `503 auth_state_corrupt`。
  - `src/http/http_util.*`：`client_ip()` 默认使用 peer IP；仅当 peer 在 `--trusted-proxy` 中才解析 `X-Forwarded-For`；补测试。
  - 请求体限制、JSON 解析失败、统一错误结构、`Retry-After`。
  - 审计事件：`auth.setup`、`auth.login`、`auth.logout`、`auth.password_change`、`auth.state_corrupt`、`auth.rate_limit`；不记录密码/Cookie/Token。
  - 删除旧 `{"token":...}` 登录分支；保留 Bearer 直接认证。
- **验收：**
  - 未初始化时直接访问 `/api/files`、`/api/terminal/*`、`/api/system/*`、`/api/settings`、`/ws/*` 全部被拒绝，且不泄露数据。
  - 密码过短、确认不一致和超限分别返回明确但无秘密的错误；成功初始化后再次 setup 返回 409。
  - 密码登录失败统一错误，5 次/分钟限流实际触发；伪造 `X-Forwarded-For` 不能绕过。
  - Cookie 非安全方法必须同源 Origin；WS 握手继续校验认证和 Origin。
  - `GET /api/auth/session` 行为符合 D8。
- **验证：** `scripts/api_test.py`/新增认证测试 + 手工 `curl`。

### T08 — 本机密码恢复命令（P0，S）

- **依赖：** T04、T03。
- **目标：** 实现 `edgedesk --reset-admin-password`，作为认证损坏或忘记密码时的唯一恢复路径。
- **改动：**
  - `src/app/main.cpp`：在启动服务前识别 `--reset-admin-password`，加载配置、创建/检查 data-dir，执行恢复后退出（不启动 HTTP）。
  - `src/security/auth_store.*`：`reset_for_recovery()`：持有 `.lock`（`LOCK_EX|LOCK_NB`），删除/替换 `auth.json`，保留其他文件。
  - 输出：明确提示“服务已停止、数据未删除、重启后进入初始化页设置新密码”。
  - 审计：若审计日志可用，追加 `auth.reset`（不含秘密）。
  - `docs/config.md`：恢复命令与停止服务要求。
- **验收：**
  - 服务运行中执行 → 非零退出，提示先停止服务，不修改文件。
  - 服务停止时执行 → 认证状态回到未初始化，生成新码；`settings.json`、审计日志、文件数据保留。
  - 新码可用于初始化，旧密码/旧 Cookie 全部失效。
  - 命令不会启动 HTTP 服务。
- **验证：** shell 集成测试（启动/停止/恢复/重新初始化）。

### T09 — 设置与系统信息 API（P0，M）

- **依赖：** T04（原子写入）、T07（认证门禁）。
- **目标：** 实现 `GET/PUT /api/settings` 与 `GET /api/system/info`，并建立白名单/脱敏边界。
- **改动：**
  - 新增 `src/security/settings_store.{hpp,cpp}`（或 `src/core/settings.*`）：`settings.json` schema 1、白名单字段、类型/枚举/范围校验、原子写入、0600。
  - 新增 `src/http/routes/settings_route.{hpp,cpp}`；在 `src/http/server.cpp` 注册。
  - `src/http/routes/system_route.cpp`：新增 `/api/system/info`；返回版本、构建信息、主机名、架构、内核、运行时间、监听地址/端口、文件根目录、Bash 路径、审计状态、API Token 是否配置；不返回 Token、密码哈希、Cookie、环境变量全集、完整命令行。
  - `PUT /api/settings` 只接受 D6 白名单；未知字段 400，不静默忽略；返回完整设置。
  - 设置文件损坏时回退默认值并返回可诊断状态，不影响认证。
- **验收：**
  - 重启后设置保留；非法 JSON/未知字段/越界值被拒绝；部分更新语义明确。
  - 系统信息不含秘密；`api_token_configured` 只返回布尔值。
  - 部署级配置只读，不能被 `settings.json` 覆盖。
- **验证：** 单元测试 + `scripts/api_test.py` 新增用例。

### T10 — 前端认证状态与页面（P0，M）

- **依赖：** T07。
- **目标：** 前端正确处理 `checking`、`setup`、`anonymous`、`authenticated`、`corrupt` 五种状态。
- **改动：**
  - `web/src/app/App.tsx`：启动时先请求 `setup-status`，再请求 `session`；按状态渲染 `SetupView`/`LoginView`/`CorruptStateView`/主界面。
  - `web/src/features/auth/LoginView.tsx`：改为密码登录；更新文案，移除“访问密钥/启动日志”描述。
  - 新增 `web/src/features/auth/SetupView.tsx`：密码、确认密码、客户端长度校验、错误展示。
  - 新增 `web/src/features/auth/CorruptStateView.tsx`：通用故障提示与本机恢复命令，不提供网页重置。
  - `web/src/services/api.ts`：新增 setup/login/password/logout/session 类型；统一处理 401（回到登录页，避免循环）。
  - 更新前端测试：状态切换、setup 成功/失败、登录失败、401 处理、登出。
- **验收：**
  - 未初始化实例只显示初始化页，不请求业务 API。
  - 已初始化未登录实例只显示登录页；登录后进入主界面。
  - 认证损坏实例显示故障页，不显示文件/终端/仪表盘。
  - 刷新后会话保持；登出后回到登录页；改密后要求重新登录。
- **验证：** `cd web && npm test && npm run build`。

### T11 — 前端设置页与偏好（P0，L）

- **依赖：** T09、T10。
- **目标：** 落地 SPEC-0005 第 6 节的设置页，并把 D6 的偏好接到现有界面。
- **改动：**
  - `web/src/features/settings/SettingsView.tsx`：分组导航“安全 / 系统信息 / 基础偏好 / 高级配置（只读）”。
  - 安全：修改密码（当前密码、新密码、确认）、当前会话/过期时间、退出登录、API Token 是否配置（不显示值）。
  - 系统信息：调用 `/api/system/info`，展示版本、架构、内核、运行时间、监听地址、文件根目录、Bash、审计状态。
  - 基础偏好：主题（浅色/深色/跟随系统）、默认文件视图、隐藏文件、终端字体大小、仪表盘时间窗口；保存到 `settings.json`，失败可回退。
  - 新增 `web/src/stores/preferencesStore.ts`（或 context）；`FileWorkspace`、`TerminalPane`、`DashboardView` 读取偏好；纯布局继续用 `localStorage`。
  - 主题：在 `web/src/styles/edgedesk-tokens.css` 增加浅色 token；`app.css` 使用变量；`document.documentElement` 设置 `data-theme`。
  - 高级配置：只读展示监听地址、端口、文件根目录、符号链接策略、上传上限、终端并发/空闲、指标周期、审计路径；不提供写入。
- **验收：**
  - 设置修改后刷新/重启仍生效（服务端偏好）；浏览器布局偏好仍按浏览器保存。
  - 非法枚举/越界值在前端阻止，后端仍二次校验。
  - 浅色/深色/跟随系统可切换，无不可读对比度问题（至少代码级 token 检查）。
  - 修改密码成功后旧会话失效，当前页面回到登录页。
- **验证：** `cd web && npm test && npm run build`；人工目视核对记录到 `docs/design/prototype-conformance.md` 或 `docs/testing.md`（若执行）。

### T12 — 单元测试与安全边界（P0，M）

- **依赖：** T04–T09。
- **目标：** 补齐跨任务的对抗性测试，避免“正常路径通过、边界失败”。
- **覆盖：**
  - 配置：data-dir 默认/优先级/权限/不可写；`--cookie-secure`；`--trusted-proxy` 解析。
  - AuthStore：未初始化/已初始化/损坏/版本不支持/不可读；原子写入失败；符号链接；文件权限；锁。
  - PasswordHash：正确/错误/超长/盐独立/参数升级/worker 队列上限。
  - AuthService：登录/登出/改密/会话轮换/重启失效/Bearer 不受影响/无 token 配置。
  - 中间件：未初始化门禁、损坏门禁、CSRF、WS Origin、`X-Forwarded-For` 伪造、限流。
  - SettingsStore：未知字段/非法枚举/越界/损坏文件/原子写入。
  - 审计：不含密码/Token/Cookie。
- **验收：** 新增测试全部通过；`ctest --preset host-offline-tests` 与 `ctest --preset dev-disk` 通过；关键安全用例在 CI 中运行。
- **验证：**
  ```bash
  cmake --preset host-offline-tests && cmake --build --preset host-offline-tests && ctest --preset host-offline-tests
  cmake --build --preset dev-disk && ctest --preset dev-disk
  ```

### T13 — 集成/E2E 与迁移测试（P0，M）

- **依赖：** T07–T11。
- **目标：** 用真实二进制覆盖完整生命周期，并更新 V1.0 脚本。
- **改动：**
  - 新增 `scripts/auth_test.py`（或扩展 `api_test.py`）：setup-status/setup/login/logout/session/password/settings/system-info。
  - 更新 `scripts/integration_test.sh`：
    - 使用临时 `--data-dir`，直接设置密码完成 setup，再登录。
    - 保留 Bearer Token 测试（启动参数提供 token）。
    - 增加重启后凭据/设置保留、旧 Cookie 失效。
    - 增加恢复命令：停止服务 → `--reset-admin-password` → 重启 → 新码初始化。
    - 增加损坏 `auth.json` → 503/降级态 → 本机恢复。
    - 增加伪造 `X-Forwarded-For` 不绕过登录限流。
    - 增加改密后旧 Cookie 401、Bearer 仍可用。
  - 更新 `scripts/ws_test.py`、`scripts/rate_limit_test.py` 的登录方式。
  - CI 四个 job 全部执行新套件（host、gcc15-embed、x86_64-musl、arm64-qemu）。
- **验收：**
  - 完整流程在 dev-disk 与至少一个交叉目标通过。
  - 失败路径有明确断言，不仅检查 HTTP 200。
  - 审计日志包含 `auth.setup`/`auth.login`/`auth.password_change`/`auth.reset`，且不含秘密。
- **验证：**
  ```bash
  bash scripts/integration_test.sh out/build/dev-disk/bin/edgedesk 19091 web/dist
  ```

### T14 — 跨平台构建、文档与发布（P0，M）

- **依赖：** T12、T13。
- **目标：** 完成 V1.1.0 发布门槛，确保文档与真实验证一致。
- **改动：**
  - `docs/api.md`：V1.1 认证/设置/系统信息协议；旧 Token 登录标注已移除；错误码/状态码。
  - `docs/config.md`：`--data-dir`、`--cookie-secure`、`--trusted-proxy`、恢复命令、systemd `StateDirectory` 示例、迁移说明。
  - `docs/architecture.md`：认证状态机、AuthStore、PasswordHash worker、data-dir/state-dir 边界。
  - `docs/testing.md`：单元/集成/交叉/未验证项真实结果。
  - `README.md`：V1.1 快速开始、首次初始化、Bearer Token、恢复。
  - `docs/README.md`、ADR-0003 状态（V1.0 历史契约）、ADR-0008 状态（Implemented + 日期）。
  - `CMakeLists.txt` 版本号 `1.1.0`；`src/app/main.cpp` 版本输出。
  - 更新 `.github/workflows/ci.yml`（Argon2 源码缓存/离线构建、端到端步骤）。
- **验收：**
  - `dev-disk`、`release-embed`、`x86_64-musl`、`arm64-disk` 的构建/测试结果如实记录；未运行的平台标注未验证。
  - `#embed` 单二进制仍不含运行期前端目录依赖。
  - 升级说明明确：V1.0 用户首次启动进入初始化；旧 API Token 只能作 Bearer，不能换 Cookie。
  - 发布前完成一次从空 data-dir 开始的完整手工/脚本验收。
- **验证：**
  ```bash
  scripts/with-gcc15.sh all release-embed
  bash scripts/integration_test.sh out/build/release-embed/bin/edgedesk 19391 web/dist
  # musl / ARM64 命令见 docs/testing.md
  ```

## 5. 安全验收用例（必须进入自动化）

| 编号 | 场景 | 期望 |
| --- | --- | --- |
| S01 | 未初始化访问 `/api/files`、`/api/terminal/sessions`、`/api/system/summary`、`/ws/metrics` | 拒绝，不返回业务数据 |
| S02 | 密码过短、确认不一致、重复初始化 | 拒绝；不泄露密码/哈希 |
| S03 | 初始化成功后再次 `POST /api/auth/setup` | 409；原密码不变 |
| S04 | 认证文件损坏/不可读/版本不支持 | 降级态；不自动进入初始化；恢复命令可修复 |
| S05 | 密码错误、超长密码、空密码 | 通用失败/400；不进入无限哈希 |
| S06 | 登录限流 + 伪造 `X-Forwarded-For` | 仍触发 429；伪造头不改变 peer IP（除非可信代理） |
| S07 | 旧 Cookie、篡改 Cookie、过期 Cookie、重启后 Cookie | 401；不泄露会话内容 |
| S08 | 改密后旧 Cookie、Bearer Token | 旧 Cookie 401；Bearer 不变 |
| S09 | Cookie 非安全方法缺少 Origin、异源 WS 握手 | 403；WS 不进入 101 |
| S10 | 审计日志、错误响应、系统信息 | 不含密码、哈希、Token、Cookie |
| S11 | `--reset-admin-password` 在服务运行时执行 | 非零退出；auth 文件不变 |
| S12 | 恢复/重启后 `settings.json`、审计日志、业务文件 | 保留；认证状态按预期重置/保留 |

## 6. 迁移与回滚

- **V1.0 → V1.1：** 没有 `auth.json` 的实例进入未初始化状态，直接在 Web UI 设置管理员密码；已有 `--auth-token` 配置继续作为 Bearer，不再用于网页登录。
- **脚本迁移：** 自动化客户端改用 `Authorization: Bearer <token>`；依赖 `POST /api/auth/login {"token":...}` 的脚本必须修改。
- **数据备份：** 升级前备份 `data-dir/auth.json`、`data-dir/settings.json`（如有）；`state-dir` 的审计日志按现有策略保留。
- **回滚：** 回退到 V1.0 二进制时，V1.0 不读取 `auth.json`/`settings.json`，仍按其 Token 逻辑运行；升级后的数据文件保留，不自动删除。回滚前应停止服务并备份。
- **恢复：** 忘记密码或认证文件损坏时，停止服务 → `edgedesk --reset-admin-password --data-dir ...` → 重启 → 在 Web UI 设置新密码。

## 7. 风险与缓解

| 风险 | 影响 | 缓解 |
| --- | --- | --- |
| Argon2 内存/耗时超出嵌入式目标 | 登录慢、OOM、事件循环卡顿 | T02 基准；有界 worker；参数可升级；必要时经 ADR 调整 |
| Argon2 在 musl/ARM 静态链接失败 | 无法发布目标平台 | T02 早期验证；源码构建；CI 覆盖 |
| `X-Forwarded-For` 伪造 | 登录限流被绕过、审计 IP 失真 | D5 可信代理白名单；默认 peer IP；S06 |
| 未初始化实例被不可信访问者抢先设置密码 | 远程首次访问被劫持 | 默认回环；远程部署在初始化完成前限制访问；文档明确提示 |
| auth.json 损坏被当成新安装 | 认证绕过 | T04 失败关闭；S04；本机恢复 |
| 升级后管理员被锁在门外 | 无法使用 Web UI | 恢复命令；升级文档 |
| 前后端迁移不同步 | 登录/设置不可用 | 长期分支；最终单 PR；T13 全流程 |
| 设置覆盖部署配置 | 远程/文件边界被改变 | 白名单 + 只读高级配置 + 后端校验 |

## 8. 明确不在本拆分内

- API Token 的网页创建、撤销、权限细分（P1）。
- 活跃会话列表与单会话撤销（P1）。
- 高级配置的网页写入、变更预览、回滚（P1/P2）。
- 审计日志网页检索、诊断包导出（P1/P2）。
- 多用户、角色、OIDC/OAuth、邮件/短信找回。
- 数据库/ORM、TLS 内置终止。
