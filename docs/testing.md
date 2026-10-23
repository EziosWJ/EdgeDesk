# EdgeDesk 测试与验证矩阵

> 只记录**实际执行过**的验证。未运行的环境一律标注“未验证”，不得以文档目标代替结果。
> 复现命令附在每节末尾。V1.3 扩展平台新增 6 个原生测试、扩展集成测试、SDK CLI 测试和真实浏览器验收。
> Process Supervisor V1.0 的宿主 L0/L1、musl/static-pie 与 ARM64/QEMU 记录见文末
> “Process Supervisor V1.0 实际执行记录”。

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
该本地执行记录不包含 L2；本轮跨平台 CI 的实际结果见下方 SPEC-0008 复核记录。不得把上表当作跨平台发布验收。

## 2026-10-19 SPEC-0008 T-01（Issue #17）实际执行记录

| 验证 | 命令 | 结果 |
| --- | --- | --- |
| L0 原生 + SDK | `JOBS=10 CTEST_JOBS=6 scripts/test-fast.sh --no-frontend` | **16/16 原生通过**；SDK/CLI **4/4 通过**（5 s） |
| L1 原生 + 集成 | `JOBS=10 CTEST_JOBS=6 scripts/test-pr.sh --no-frontend --port 19101` | **16/16 原生通过**；auth/api/ws/限流/扩展/崩溃恢复集成全部通过（125 s） |

- 本次未运行前端测试/构建（`web/node_modules` 不在该工作树）；`--no-frontend` 复用了既有 `web/dist` 产物。
- 新增回归：`path_sandbox_test` 与 `file_service_test` 覆盖受保护路径的直连/规范化变体/嵌套/符号链接（follow）/硬链接别名/延迟创建目标，以及 list、stat、read、write、mkdir、rename、remove、递归删除、上传和下载；失败后目标内容与元数据保持不变，管理员服务不受策略影响。
- 未验证：L2 平台矩阵。不得将本表用于跨平台发布验收。

## 2026-10-19 #21 包安装与静态资源内存边界实际执行记录

- L0：`JOBS=8 CTEST_JOBS=4 scripts/test-fast.sh --no-frontend`，**17/17 原生测试通过**
  （新增 `extension_memory_test`），SDK CLI 4/4 通过。本轮 `web/node_modules` 不存在，
  未运行前端单测。
- L1：`JOBS=8 CTEST_JOBS=4 scripts/test-pr.sh --no-frontend --port 19221`，**17/17 原生 +
  全量集成通过**（auth/api/ws/限流/恢复/扩展/崩溃恢复）。仓库无 `web/node_modules` 与
  `web/dist`，为执行 L1 临时创建了仅含 `EdgeDesk` 标记的最小 `web/dist/index.html`
  （无哈希资源），运行后删除、未提交；本轮未运行前端测试与 `npm run build`。
- `extension_memory_test` 在 `fork`+`exec` 的独立子进程中读取 `/proc/self/status`
  的 `VmHWM`，避免父进程历史峰值污染基线。实测（64 MiB stored/deflate 资产，
  另有 manifest 与 `index.html`，每次运行数值略有波动）：
  - 安装：baseline 5724 KiB，peak 6192 KiB，**delta 468 KiB**；完整性清单精确覆盖
    3 个文件，`total_bytes` 等于 64 MiB + manifest + index 的实际大小。
  - 静态服务：baseline 5844 KiB，peak 6108 KiB，**delta 264 KiB**；完整读取
    64 MiB 资产，分块 128 KiB。两次 delta 均远低于 24 MiB 断言上限，且不随资产大小线性增长。
  - 失败清理：`RLIMIT_FSIZE` + 忽略 `SIGXFSZ` 注入 `EFBIG`（替代不可移植的真实 ENOSPC），
    安装失败后临时目录与修订目录为空、无挂起操作、无应用记录；`recover()` 后仍为空。
- 未执行/未验证：真实 ENOSPC（磁盘写满）注入、L2 平台矩阵、前端单测与构建。
  不得将本记录视为跨平台或前端验收。

## 2026-10-19 SPEC-0008 集成验收记录（#17–#24 合并树）

本机 Linux/WSL2（x86_64 host），集成分支 `worktree-v13-hardening` 合并全部八个 Ticket 后执行。
命令与结果均为合并树上的实际运行记录：

| 验证 | 实际命令 | 结果 |
| --- | --- | --- |
| L0 原生 + SDK | `JOBS=8 CTEST_JOBS=4 scripts/test-fast.sh --no-frontend` | **19/19 原生通过**（含 `audit_test`、`extension_runtime_test`、`extension_memory_test`）；SDK/CLI **4/4 通过**，11 s |
| L1 前端 | `JOBS=8 CTEST_JOBS=4 scripts/test-pr.sh --port 19181`（内部 `npm test` + `npm run build`） | 前端 **144/144 通过**，`tsc --noEmit` + Vite 构建通过 |
| L1 原生 + 集成 | 同上 | dev-disk `ctest` **19/19 通过**；auth/api/ws/限流/恢复/扩展/崩溃恢复/审计 **ALL TESTS PASSED**，137 s |
| 浏览器边界 | `node web/scripts/extension-network-boundary.mjs` | **PASS**：CSP 阻止未批准/未声明资源；弹窗、`target=_blank`、iframe 自身导航逃逸稳定复现（残余风险，ADR-0014 Proposed） |
| embed x86_64 | `EDGEDESK_TOOLCHAIN_ROOT=.../x86-64--musl--stable-2026.08-1 cmake --preset release-embed && cmake --build --preset release-embed -j 8` | 构建通过；`file` 报告 `static-pie linked, stripped` |
| embed x86_64 单测/集成 | `ctest --test-dir out/build/release-embed -j 4`；`scripts/integration_test.sh out/build/release-embed/bin/edgedesk 19151 web/dist` | **19/19 通过**；集成 **ALL TESTS PASSED** |
| embed aarch64 | `EDGEDESK_TOOLCHAIN_ROOT=.../aarch64--musl--stable-2026.08-1 cmake --preset release-embed-arm64 && cmake --build --preset release-embed-arm64 -j 8` | 构建通过；`static-pie linked`；qemu 下 `--version` 输出 `EdgeDesk 1.3.0` |
| embed aarch64 集成 | `scripts/integration_test.sh out/build/release-embed-arm64/bin/edgedesk 19161 "" "<qemu-aarch64-static> -L <root>"` | **ALL TESTS PASSED** |
| disk musl x86_64 | disk 模式重新配置/构建 + `ctest` + 集成（端口 19131） | **19/19 通过**；集成 **ALL TESTS PASSED**；`static-pie linked` |
| disk musl aarch64 | disk 模式重新配置/构建 + `ctest` + 集成（端口 19141） | **19/19 通过**（qemu-user）；集成 **ALL TESTS PASSED**；`static-pie linked` |

- 交叉验收中发现并修复：`extension_memory_test` 的子进程直接 `execv("/proc/self/exe")`，在 qemu-user 下
  `/proc/self/exe` 指向宿主 emulator，导致子进程退出 127。现从 `argv[0]` 取真实被测二进制路径，并由
  CMake 通过 `EDGEDESK_TEST_EXEC_PREFIX`（来自 `CMAKE_CROSSCOMPILING_EMULATOR`）把 emulator 前缀传给子进程。
  修复后两个 musl 目标的该测试均通过；原生行为不变。
- 合并树测试计数为 19：原 V1.3 的 16 个测试 + `extension_runtime_test`（#22）+ `audit_test`（#23）+
  `extension_memory_test`（#21）。
- 未验证：**GitHub Actions 全矩阵尚未在对应提交上运行**；真实 ARM64 硬件、ARM32、非 Linux 宿主未验证。
  本地 L2 证据不能替代 CI 发布验收。


## 2026-10-18 SPEC-0008 复核：当前跨平台 CI 阻断

- 复核提交：`308325a3f5d57e0879b58566cca495406a4b191e`；[GitHub Actions run #39132868799](https://github.com/EziosWJ/EdgeDesk/actions/runs/39132868799)。
- 结果：5 个 job 中 3 个通过（`frontend`、`host-build`、`gcc15-embed`），2 个失败（`x86_64-musl-static`、`arm64-cross`）。
- 两个失败目标在 CMake 配置阶段均未找到目标 ZLIB 头文件/库；跟踪于 [Issue #24](https://github.com/EziosWJ/EdgeDesk/issues/24)。
- 该结果不否定旧 PR #14 当时的 5/5 通过记录，但旧 PR 的通过不能代替当前提交的跨平台验收。SPEC-0008 的安全与可靠性修复另见 [总任务 #16](https://github.com/EziosWJ/EdgeDesk/issues/16)。

## 2026-10-19 Issue #24：musl ZLIB 依赖修复的本地验证

- 修复方式：zlib **1.3.1** 固定版本 vendored 到 `third_party/zlib/`（来源与
  SHA-256 见 `third_party/README.md`），由静态目标 `edgedesk_zlib` 构建并从
  `edgedesk_core` `PUBLIC` 链接；`find_package(ZLIB)` 已移除，宿主与交叉构建
  使用同一份源码，不再依赖工具链 sysroot 中的 ZLIB 或宿主 glibc zlib。
- 工具链：Bootlin `x86-64--musl--stable-2026.08-1` 与
  `aarch64--musl--stable-2026.08-1`（GCC 15.3）。构建目录为 `out/build/x86_64-musl`
  与 `out/build/aarch64-musl`，资源模式为 disk（本次未重跑 `#embed` lane）。
- 同时修复了两个在 musl 目标上暴露的既有问题：四个测试缺少 `<unistd.h>`
  （`getpid`）；`remove_tree_at` 的 RAII closer 在手动关闭后对 `nullptr` 调用
  `closedir`（glibc 容忍、musl 段错误）。均为构建/测试阻塞项，未改变产品行为。

| 验证 | 实际命令 | 结果 |
| --- | --- | --- |
| 宿主 L0 | `JOBS=12 CTEST_JOBS=6 scripts/test-fast.sh --no-frontend` | **16/16 原生通过**；SDK/CLI **4/4 通过**。当时 `web/node_modules` 不存在，前端按脚本规则跳过。 |
| 宿主 L1 | `JOBS=12 CTEST_JOBS=6 scripts/test-pr.sh --port 19121` | 前端 **144/144 通过**，`tsc --noEmit` + Vite 构建通过；dev-disk `ctest` **16/16 通过**；完整集成（auth/api/ws/限流/扩展/崩溃恢复）**ALL TESTS PASSED**，共 157 s。 |
| x86_64 musl 配置/构建 | `EDGEDESK_TOOLCHAIN_ROOT=$HOME/opt/musl/x86-64--musl--stable-2026.08-1 cmake -S . -B out/build/x86_64-musl -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/x86_64-linux-musl.cmake -DEDGEDESK_WEB_MODE=disk -DEDGEDESK_BUILD_WEB=OFF -DEDGEDESK_STATIC_PIE=ON && cmake --build out/build/x86_64-musl -j 12` | 配置与编译通过；`file` 报告 `static-pie linked, stripped`；`x86_64-linux-objdump -p` 无 `INTERP`/`NEEDED`。 |
| x86_64 musl 单测 | `ctest --test-dir out/build/x86_64-musl -j 6 --output-on-failure` | **16/16 通过**。 |
| x86_64 musl 集成 | `bash scripts/integration_test.sh out/build/x86_64-musl/bin/edgedesk 19131 web/dist` | **ALL TESTS PASSED**。 |
| aarch64 musl 配置/构建 | `EDGEDESK_TOOLCHAIN_ROOT=$HOME/opt/musl/aarch64--musl--stable-2026.08-1 cmake -S . -B out/build/aarch64-musl -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/aarch64-linux-musl.cmake -DEDGEDESK_WEB_MODE=disk -DEDGEDESK_BUILD_WEB=OFF -DEDGEDESK_STATIC_PIE=ON -DCMAKE_CROSSCOMPILING_EMULATOR="$HOME/opt/arm64/root/usr/bin/qemu-aarch64-static;-L;$HOME/opt/arm64/root" && cmake --build out/build/aarch64-musl -j 12` | 配置与编译通过；`file` 报告 `ARM aarch64, static-pie linked, stripped`；`aarch64-linux-objdump -p` 无 `INTERP`/`NEEDED`。 |
| aarch64 musl 运行 | `qemu-aarch64-static -L $HOME/opt/arm64/root out/build/aarch64-musl/bin/edgedesk --version` | 输出 `EdgeDesk 1.3.0`。 |
| aarch64 musl 单测 | `ctest --test-dir out/build/aarch64-musl -j 4 --output-on-failure`（CMake 已设置 qemu emulator） | **16/16 通过**（qemu-user；`terminal_session_test` 使用 ARM64 rootfs 中的 Bash）。 |
| aarch64 musl 集成 | `bash scripts/integration_test.sh out/build/aarch64-musl/bin/edgedesk 19141 web/dist "$HOME/opt/arm64/root/usr/bin/qemu-aarch64-static -L $HOME/opt/arm64/root"` | **ALL TESTS PASSED**。 |

未验证（本次提交不能声明通过）：

- **GitHub Actions 全矩阵尚未在本分支/提交上运行。** 上文的失败结论仍对应
  run #39132868799；只有合并请求实际跑绿后才能关闭 Issue #24。
- x86_64 / aarch64 的 `#embed` + static-pie 发布 lane 本次未重跑（本次使用 disk 模式）。
- ARM32、真实 ARM64 硬件、非 Linux 宿主未验证。
- 上述结果为本地宿主 + 交叉 + qemu 证据，不替代 L2 发布验收。

## 2026-10-19 SPEC-0008 T-02（#18）网络权限与来源策略执行记录

本机 Linux/WSL2，GCC 13.3 host（`host-offline-tests`/`dev-disk`），提交前执行：

| 验证 | 命令 | 结果 |
| --- | --- | --- |
| L0 原生 + SDK | `JOBS=10 CTEST_JOBS=6 scripts/test-fast.sh --no-frontend` | 原生 **16/16 通过**，SDK/CLI **4/4 通过**；前端测试按参数跳过 |
| L1 构建/单测/集成 | `JOBS=10 CTEST_JOBS=6 scripts/test-pr.sh --no-frontend --port 19111` | `dev-disk` 构建通过，原生 **16/16 通过**，全量集成通过 |
| 集成测试单独复跑 | `bash scripts/integration_test.sh out/build/dev-disk/bin/edgedesk 19111 web/dist` | 全部通过（含扩展 `io.example.network` 场景） |
| 浏览器边界 | `node web/scripts/extension-network-boundary.mjs` | **通过**（headless Chromium 1248，CDP；断言见下） |

新增/更新的确定性覆盖：

- `manifest_test`：无授权/撤销时 CSP 不产生外部 source；仅批准来源进入 CSP；
  声明集合与批准集合的交集/子集判定；全部批准后集合扩大生效。
- `extension_store_test`：`network_origins` 持久化、替换、清除与外键级联；
  手工构造 schema v1 数据库后原地迁移到 v2，保留应用/授权数据并可使用新表。
- `extension_package_test`：批准 `network` 后 CSP 出现外部来源，撤销后消失；
  升级新增来源移除 `network` 授权且应用保持启用；重新审批后生效；来源未扩大
  时保留授权；回滚按同一规则复核；卸载保留数据同时清除批准来源。
- `scripts/extension_test.py`：安装/审批后无外部 CSP；授予 `network` 后 9 类指令
  均出现批准来源且会话快照一致；撤销后无外部来源且新会话为空；禁用/启用后新
  会话恢复；升级扩大来源要求重新审批；卸载保留清除状态。

浏览器边界脚本实际结果（不得外推到其他浏览器）：

- 网络未授权：批准来源与未声明来源的 `fetch`、图片、iframe 均无外部命中；
- 网络已授权：批准来源的 `fetch`、图片、iframe 各命中一次；未声明来源的
  `fetch`/图片/iframe/表单均被阻止。此处“命中”以外部服务器收到请求为准；
  `fetch` 仍可能因测试服务器未配置 CORS 在页面内被拒绝，不能用页面内结果
  反推 CSP；
- 两种场景下 `window.open`、`target="_blank"` 和 iframe 自身导航均到达未声明
  来源，确认 CSP 不约束顶层导航。该行为按残余风险记录（ADR-0014 Proposed），
  不作为通过项，也不得宣称“网络权限已完全执行”。
- 集成分支复跑时发现脚本在发出 self-navigation 请求后立即关闭页面，命中记录存在
  竞态（同一脚本可能间歇性地报缺失）。补充 1.5 s 等待后，denied/approved 两种
  场景均稳定复现上述导航逃逸；该等待仅用于消除测试竞态，不改变产品行为。

未执行/未验证：`web/node_modules` 不存在，前端单元、Vite 构建及
`web/scripts/extension-e2e.mjs` 本轮未运行；本轮集成测试使用的 `web/dist`
是仅用于静态路由门禁的最小占位页（非前端构建产物）；Firefox/WebKit、L2 平台矩阵
（Embed/musl/ARM64/qemu/真实硬件）未运行。以上均不得标记为通过。

## 2026-10-19 T-07 审计脱敏本地执行记录

- 变更：新增 `security/redaction.{hpp,cpp}`；`AuditLog::record` 对 detail/path 统一脱敏并限制为 8192 字节；扩展错误上报改为脱敏 + UTF-8 安全截断 + 结构化编码。
- L0：`JOBS=8 CTEST_JOBS=4 scripts/test-fast.sh --no-frontend` 通过（17/17 原生测试，含新增 `audit_test`；SDK/CLI 4/4；`web/node_modules` 缺失，前端单测未运行）。
- L1：`JOBS=8 CTEST_JOBS=4 scripts/test-pr.sh --no-frontend --port 19161` 通过（17/17 原生测试 + 全量集成；`extension_test` 新增错误上报脱敏与 JSONL 逐行解析断言）。`--no-frontend` 复用既有 `web/dist`，本轮未重新构建前端。
- 未执行：L2 平台矩阵（embed/musl/ARM64）；本轮结论不构成跨平台发布验收。

## 测试分层（本地入口）

| 层级 | 入口 | 覆盖 | 何时运行 |
| --- | --- | --- | --- |
| L0 编辑循环 | `scripts/test-fast.sh` | `host-offline-tests` 构建 + 17 项原生单测 + 前端单测（`web/node_modules` 存在时；V1.3 为 144 项）+ SDK/CLI 测试（Node 存在时） | 每次改动 |
| L1 任务/PR | `scripts/test-pr.sh` | `dev-disk` 构建 + 17 项原生单测 + 全量集成（含扩展平台）+ 前端 `npm test` / `npm run build` | 提交前 |
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
| `audit_test` | Authorization/Cookie/Set-Cookie/API Key/密码脱敏、JSON 与表单键、URL query/fragment/userinfo、裸 Bearer、大小写与 URL 编码、幂等性、空值/多行/超长输入、UTF-8 安全截断、8192 字节 sink 上限、JSONL 逐行有效 | GCC 13.3 host | 通过 |
| `path_sandbox_test` | `..` 穿越、URL 编码、绝对路径、前缀欺骗目录、符号链接逃逸/循环（含 follow 模式）、嵌套相对目标（`dir`、`../..`、逃逸拒绝）、lstat 语义、受保护路径（直连/`//`/`.`/合法 `..` 变体、嵌套、符号链接目标、硬链接别名、构造后创建的受保护目标） | GCC 13.3 host | 通过 |
| `file_service_test` | 列表/排序/分页/隐藏文件、`dirs_only` 直接子目录（符号链接不跟随）、文本读写与原子保存保留权限、二进制拒绝、新建空文件与同名拒绝、上传/中止/超限、递归删除计数、Range 解析、MIME 判定、受保护路径上的 list/stat/read/write/mkdir/rename/remove/递归删除/上传/下载拒绝且不修改目标、管理员服务不受策略影响 | GCC 13.3 host | 通过 |
| `command_guard_test` | 危险命令命中（rm -rf 根、块设备写入、fork 炸弹、关机…）与安全命令不误报 | GCC 13.3 host | 通过 |
| `terminal_session_test` | 真实 Bash 登录 Shell、`~/.profile` 加载、UTF-8/ANSI、TIOCSWINSZ resize、CommandGuard 拦截不执行、回放缓冲有界、订阅断开不杀会话、订阅回调（回放/退出）可重入会话 API 且不死锁、会话上限/空闲回收、缺 Bash 报错而非回退 sh | GCC 13.3 host + /bin/bash 5.2 | 通过 |
| `system_metrics_test` | /proc/stat、loadavg、meminfo（含旧内核回退）、net/dev 速率差分、IPv4/IPv6 地址格式化与 `getifaddrs` 枚举（排除 lo）、mounts 过滤、采样线程与环形缓冲 | GCC 13.3 host | 通过 |
| `semver_test` | 严格 SemVer 解析/拒绝（前导零、v 前缀、非法标识）、pre-release 排序、build 忽略、宿主 API 闭区间 | GCC 13.3 host | 通过 |
| `manifest_test` | 核心字段、权限、来源通配符/CSP、声明∩批准来源（无授权/撤销不产生外部 source）、窗口边界、宿主不兼容、保留运行时不执行、非法 ID/版本/来源/图标 | GCC 13.3 host | 通过 |
| `extension_store_test` | SQLite 0700/0600、应用/修订/权限/完整性、操作日志、KV 键限/5 MiB 配额/回滚事务/分页、重启持久化、`network_origins` 持久化/替换/级联、schema v1→v2 原地迁移、同一事务内删除 KV 后触发真实 SQLite 约束失败时行与配额全部回滚 | GCC 13.3 host | 通过 |
| `extension_package_test` | ZIP 安装、审批/静态资源/SPA、同版本覆盖、升级/降级、网络授权驱动 CSP、升级来源扩大需重新审批/回滚复核、完整性清单与实际文件集合精确比对（缺失/改内容/改大小/新增未登记文件/清单条目缺失/符号链接/FIFO/大小写冲突）、活动修订隔离与修复、历史修订失败不误伤活动修订、未登记资源拒绝服务、路径穿越/符号链接/重复名/本地头不一致、卸载保留/恢复/删除、修订保留与固定、启动孤儿清理、重装恢复/不恢复与陈旧保留行的 KV 删除仅在最终事务提交、升级失败保留 KV 与原活动修订、崩溃恢复保持 KV 与已提交修订一致（SQLite 触发器注入提交前后失败） | GCC 13.3 host | 通过 |
| `extension_memory_test` | 64 MiB 资产安装/静态服务峰值 `VmHWM` 增量（子进程隔离）、完整性精确集合、`RLIMIT_FSIZE`/`EFBIG` 中断清理、无挂起操作与应用记录 | GCC 13.3 host | 通过（安装 delta 468 KiB、静态 delta 264 KiB） |
| `file_lock_test` | 共享/独占、硬链接别名、缺失目标父目录身份、多锁原子性、FileService 读写锁、Upload 移动锁转移 | GCC 13.3 host | 通过 |
| `extension_test.py`（L1） | 安装/审批/静态 CSP（授权/撤销/升级复核）、运行会话、KV 配额/回滚、目录游标、锁冲突、1 MiB 分块上传/下载 SHA-256、2 MiB 静态资源流式 body/HEAD/ETag 304、裸 `/apps` 404、终端创建/关闭、扩展错误上报脱敏（伪造 Bearer/Cookie/API Key/密码/query/fragment 不落盘、JSONL 逐行可解析）、禁用即时失效、升级/同版本覆盖/降级、未登记资源 404、活动修订完整性隔离、历史修订失败不误伤活动应用/会话/授权、完整性失败修订拒绝激活、修复、卸载保留/恢复/删除、审计事件 | dev-disk + 真实 PTY/文件系统 | 通过 |
| SDK CLI 测试 | manifest/资源/相对路径/符号链接/重复路径校验、ZIP 打包与检查 | Node 24.14 | 通过 |
| `embed_generator_test` | `#embed` 生成器的确定性、路径校验、ETag | Python 3.12 | 通过 |
| 前端单元/DOM 测试 | **V1.2 桌面**：AppRegistry（默认注册、未知 appId、重复注册、元数据/尺寸校验、单/多实例、启动器搜索）、桌面图标布局（网格命中、拖拽换位、边界、去重）、WindowManager（单/多实例、稳定实例 ID、z-index 有界、最小化保持挂载/恢复、最大化几何还原、移动/缩放/视口约束、中等视口完整可见、关闭守卫）、DesktopPersistence（序列化往返、损坏 JSON、版本不兼容、未知 appId、重复单实例、非法几何、敏感字段剥离、localStorage 不可用、恢复时会话失效标记、防抖自动保存）、DesktopShell（单击/双击、Dock 启动/聚焦/多窗口选择、最小化恢复、真全屏与桌面框架恢复、启动器搜索/空态/Esc/焦点返回、Alt 快捷键与输入焦点保护、窄屏强制最大化、退出登录）、终端恢复（session_not_found 不创建替代会话、无 session ID 时才 create、attached 状态）、编辑器生命周期（最小化保留缓冲、未保存关闭确认、外部修改保护、文件消失拒绝保存）、NAS 设备概览（CPU/内存/存储/网卡/系统/服务小组件、多挂载点、多网卡、loopback 排除、缺失值 vs 0、采集中断、加载空态）。**V1.3 扩展平台**：动态应用注册、禁用关闭窗口、待审批应用不可启动、扩展元数据更新。**V1.2.1 回归**：顶部退出登录注销服务端会话并由脏编辑器否决退出、最小化后重开保留最大化状态。**V1.1 回归**：会话持久化、metrics 窗口有界、格式化、FloatingWindow DOM 交互、Sidebar、图表工具、Dashboard 旧断言已迁移、文件管理、认证、设置、偏好。 | Node 24.14 / vitest 3.2 / happy-dom | 通过（155 项）；`npm run build`（tsc + vite）通过 |
| `auth_test.py` | 初始化状态门禁、首次设置密码、重复初始化 409、密码登录、Cookie 属性（含 `--cookie-secure`）、改密后旧 Cookie/旧密码失效、Bearer 不受影响、CSRF、设置白名单、系统信息脱敏、重启后旧 Cookie 失效、损坏状态失败关闭、本机恢复 | dev-disk / host-embed / release-embed / release-embed-arm64 / release-disk / x86_64-musl / arm64-disk / arm64-musl | 全部通过 |
| `api_test.py` | health 契约、密码登录/会话/Bearer、CSRF、目录浏览（含 `dirs_only` 只返回直接子目录）、文本读写、mkdir/create/rename/delete、流式上传字节一致、Range 206/416、路径沙箱拒绝、JSON 体积限制、系统指标（含 `network[].addresses` 字段） | 见下（三种二进制） | 通过 |
| `ws_test.py` | 未认证/异源握手拒绝、终端 create/input/输出/resize/guard、断线重连（回放）、会话存活、显式关闭、metrics 快照+增量 | 同上 | 通过 |
| `rate_limit_test.py` | 登录限流与通用限流在严格配置下触发 429；伪造 `X-Forwarded-For` 不能绕过 peer-IP 限流 | 同上 | 通过 |
| 审计校验 | `auth.setup`、`login`、`auth.password_change`、`auth.reset`、`file.upload`、`terminal.create`、`extension.error` 事件落盘且不含秘密；错误上报注入的 Bearer/Cookie/API Key/密码/query/fragment 样本不出现在审计文件，且每行均为有效 JSON | 同上 | 通过 |

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
| GitHub Actions CI | 本仓库 workflow：frontend + host-build / gcc15-embed / arm64-cross / x86_64-musl-static；npm/ccache/交叉工具链缓存，`web/dist` 通过 artifact 复用 | PR #14 曾 5/5 通过；复核 run #39132868799 为 3/5（两个 musl job 在 ZLIB 配置阶段失败）。#24 已改为 vendored zlib，#17–#24 合并树在本地 embed/musl/ARM64 四条 lane 均通过；**对应提交尚未在 CI 运行**，不能声明 CI 通过 |

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

## Process Supervisor V1.0 实际执行记录（2026-10-19）

执行环境：WSL2 Linux 6.18.54.1-microsoft-standard-WSL2，x86_64；集成测试以普通用户
`uid=1000` 运行（非 root）。以下结果均在当前工作树实际执行，不是计划项。

| 验证 | 命令 | 实际结果 |
| --- | --- | --- |
| L0 原生 | `scripts/test-fast.sh` | **24/24 通过**（新增 `supervisor_types_test`、`supervisor_store_test`、`supervisor_log_store_test`、`supervisor_engine_test`、`supervisor_ipc_test`），13 s |
| L0 前端 | 同上（`cd web && npm test`） | **27 个文件 / 153 个测试通过** |
| SDK/CLI | 同上 | **4/4 通过** |
| L1 全量 | `scripts/test-pr.sh` | 前端测试 + 构建、`dev-disk` 构建、24/24 ctest、完整集成套件通过，**155 s**；快进合并到本地 `main` 后再次运行通过，**176 s** |
| L2 host-embed | `scripts/with-gcc15.sh all host-embed` | GCC 15 构建通过，**24/24 ctest**，12.05 s |
| L2 x86_64 musl | `EDGEDESK_TOOLCHAIN_ROOT=~/opt/musl/x86-64--musl--stable-2026.08-1 cmake --preset release-embed && cmake --build --preset release-embed && ctest --preset release-embed` | GCC 15.3、static-pie 构建通过，**24/24 ctest**；`readelf` 无 `INTERP`/`NEEDED` |
| L2 x86_64 运行冒烟 | 以 `release-embed/bin/edgedesk-supervisor` 启动并执行 Python IPC 客户端 | `health`、create/start/stop/delete 全部成功；PID/PGID 正确，停止后进程消失 |
| L2 x86_64 Web 冒烟 | `release-embed/bin/edgedesk --no-supervisor` + curl | `/api/health` 200（`status=ok`、`phase=1`），`/` 返回 200（embed 资源） |
| L2 ARM64 构建 | `EDGEDESK_TOOLCHAIN_ROOT=~/opt/musl/aarch64--musl--stable-2026.08-1 cmake --preset release-embed-arm64 && cmake --build --preset release-embed-arm64` | GCC 15.3、static-pie 构建通过；`file` 显示 ARM aarch64，无 `INTERP` |
| L2 ARM64 QEMU | `qemu-aarch64-static` 运行 `edgedesk-supervisor` + Python IPC 客户端 | `health`、create/start/stop/delete 全部成功；PID/PGID 正确，停止后进程消失 |
| L2 ARM64 Web 冒烟 | `qemu-aarch64-static edgedesk --no-supervisor` + curl | `/api/health` 200，`/` 返回 200（embed 资源） |
| 前端目视 | Playwright headless Chromium，`processes-1440.png` / `processes-390.png` | 1440×900 与 390×844 均已打开检查：进程列表、状态、PID、指标、配置与日志无溢出；窄屏 ID/时间行已修复为单行 |
| init 静态检查 | `sh -n deploy/openrc/...`、`sh -n deploy/busybox/...`、`systemd-analyze verify ...` | 两个 shell 脚本语法通过；systemd 单元仅报告“二进制尚未安装”，未发现语法错误 |

### 安装器实际执行记录（2026-10-23）

本地暂存与自动化测试：

| 验证 | 命令 | 实际结果 |
| --- | --- | --- |
| 语法 | `sh -n deploy/install.sh`、`dash -n deploy/install.sh` | 两种 shell 均通过 |
| init 探测 | `deploy/install.sh --detect` | 本机输出 `init=systemd`、`pid1=systemd`、`uid=1000`、`systemd=yes` |
| 暂存安装测试 | `ctest -R install_script_test` | **25 项断言通过**（无 `script(1)` 的平台上交互用例跳过）：systemd/OpenRC/BusyBox 暂存安装、配置与 unit 内容、inittab `--force` 幂等、覆盖保护、`--no-supervisor`、端口/host/路径校验、dry-run 不落盘、uninstall/`--purge`、过宽 prefix 拒绝 |
| 交互提示可见性 | `tests/install/install_test.sh`（伪终端）与 `script -qec` 手工复现 | 提示经 stderr 显示在终端，四个问题均可回答；答案不再混入 `Listen host [...]` 文本。修复前提示被 `$(...)` 捕获，终端无任何输出且 `read` 永久阻塞 |
| script + bin/ 布局 | 测试用例与真机 dry-run | 无需 `--source`/`--force`；预置在目标目录的二进制被就地采用 |
| L0 全量 | `scripts/test-fast.sh` | **25/25 通过**（含 `install_script_test`） |
| L1 全量 | `scripts/test-pr.sh` | 前端 **153 个测试** + 构建、`dev-disk` 构建、**25/25 ctest**、完整集成套件通过 |
| systemd 静态校验 | `systemd-analyze verify`（暂存二进制） | 生成的 web/supervisor unit 无告警 |
| OpenRC/BusyBox 生成物 | `sh -n` 暂存的 init 脚本与 wrapper | 通过；`/etc/inittab` 仅生成一个受管块 |

真机验证（Ubuntu 26.04 LTS，systemd，`root@192.168.1.74`）：

| 验证 | 命令 | 实际结果 |
| --- | --- | --- |
| 真实安装 | `sh /opt/edgedesk/install.sh --force --yes --source /opt/edgedesk/bin --host 127.0.0.1 --port 8080` | 创建 `edgedesk` 系统账号；生成配置文件与两个 unit；两个服务 `enabled` 且 `active (running)` |
| init 探测 | `install.sh --detect` | 主机装有 busybox 但 `inittab=no`，仍正确选择 `init=systemd` |
| 健康检查 | `curl http://127.0.0.1:8080/api/health`；`curl /` | 200，`status=ok`、`service=edgedesk`、`phase=1`、`version=1.3.0`；`/` 返回 200（assets: embed） |
| 运行身份 | `ps -o user,pid,cmd` | Web 后端为 `edgedesk`；Supervisor 为 `root` |
| 权限与监听 | `stat`、`ss -ltnp` | 配置 `0640 root:edgedesk`；data/apps/state `0700 edgedesk`；Supervisor 状态 `0700 root`；socket `srw-rw---- root edgedesk`；仅监听 `127.0.0.1:8080` |
| 重启韧性 | `systemctl restart edgedesk`、`systemctl restart edgedesk-supervisor` | 两次重启后 `/api/health` 均 200，两个服务均 `active` |
| 安装后 unit 校验 | `systemd-analyze verify /etc/systemd/system/edgedesk*.service` | exit 0，无告警 |
| 布局修复真机验证 | 上传新版脚本到 `/tmp/edgedesk-fix` 后 `--dry-run --init none` | 输出 `binary already at target: ...`，未写系统文件 |

仍未执行：OpenRC/BusyBox 真机启动、停止与 respawn；systemd 真机 `--uninstall`
（测试机保持安装状态）；ARM64/musl 目标上的安装器运行。安装器本身是 POSIX shell，
但仍依赖目标机的 `useradd`/`addgroup`、`awk`、`sed` 等命令集，不能由 x86_64 结果外推。

### 优雅退出耗时修复（2026-10-23）

现象：前台 Ctrl+C 或 `systemctl stop/restart` 需要约 30 秒才退出。根因是
`Application::start_maintenance()` 使用**局部** `condition_variable` 等待 30 秒维护周期，
退出时只设置 `stopping_` 而没有 `notify`，`maintenance_.join()` 必须等满整个周期。

修复：维护线程改用 `Application` 成员 `maintenance_mutex_`/`maintenance_wakeup_`，
`request_stop()` 置位并 `notify_all`；周期工作不持锁执行，避免关停等待 sweep。
启动横幅同时改为 “starting on”，绑定失败时额外打印
“EdgeDesk failed to listen on <host>:<port> (error <n>)”。

| 验证 | 命令 | 实际结果 |
| --- | --- | --- |
| 修复前复现（测试机） | 发送 `SIGINT` 并计时 | `shutdown_ms=29903` |
| 修复后（宿主） | 同一脚本、`127.0.0.1:18081` | `shutdown_ms=9`～`10` |
| 修复后（测试机 Ubuntu 26.04） | 上传新二进制到 `/tmp` 后同一脚本 | `shutdown_ms=13` |
| 集成回归守卫 | `scripts/integration_test.sh`（新增 “graceful shutdown is prompt”） | 优雅退出 < 3 s 断言通过；`ctest` 25/25 |
| 绑定失败诊断 | 非 root 绑定 `127.0.0.1:80` | 输出 `bind: Permission denied` 与新的失败说明；进程退出 |

30 秒等待问题与低端口绑定无关：端口 80 失败是因为生成的服务以 `User=edgedesk`
运行，Linux 要求 root 或 `CAP_NET_BIND_SERVICE` 才能绑定 1024 以下端口。
安装器仍只允许回环地址；`0.0.0.0` 不会在未经安全设计评审时启用。

Supervisor 集成测试（`scripts/supervisor_test.py`）实际覆盖：

| 场景 | 实际断言与结果 |
| --- | --- |
| Web 认证/CSRF | 未认证 `GET /api/supervisor/*` → 401；Cookie 写请求缺少同源 Origin → 403 `csrf_rejected` |
| 配置校验 | 缺少 `run_user`/`run_group` → 400 `invalid_service_config`；root 身份未确认 → 403 `root_confirmation_required` |
| CRUD | 创建、重名 409、列表、详情、部分更新保留未提供字段、删除运行中服务 409、缺失服务 404 |
| 生命周期 | start → RUNNING，PID 存活且 `pgid == pid`；restart 生成新 PID；stop 后 `/proc/<pid>` 消失；再次 start 成功 |
| 无 Shell 执行 | `/bin/echo` 参数包含字面量 `$(touch <marker>)`、`semi;colon`、`space value`；日志原样保留，marker 文件不存在 |
| 日志 | 尾部读取返回上述字面量，`truncated=false`；删除后再次删除 404 |
| 后端重启 | 停止并重启 EdgeDesk 后端后，服务 PID 不变且继续存活 |
| Supervisor 崩溃恢复 | `SIGKILL` Supervisor 后重启，接管原 PID（不重复拉起）；显式 stop 能结束该进程 |
| 人工停止持久化 | 服务已 STOPPED 时再次重启 Supervisor，服务仍 STOPPED，随后可删除 |
| 不可用隔离 | Supervisor 不可达时 `/api/supervisor/*` → 503 `supervisor_unavailable`；`/api/health`、认证会话、文件 API 仍 200，托管进程继续存活 |
| 审计 | 集成脚本检查 `supervisor.create/update/start/restart/stop/logs/delete` 与 `root_confirmation_required` 事件已写入审计 |

`supervisor_engine_test` 额外覆盖：`always` 对退出码 0 也重启、`on-failure` 重启上限与
`FAILED` 锁定、`never` 不重启、指数退避（约 200 ms → 400 ms）、稳定运行后重置重启窗口、
忽略 SIGTERM 时超时升级为 SIGKILL、Supervisor 重启后人工停止保持、崩溃接管、
运行中更新不重启。`supervisor_ipc_test` 覆盖授权/未授权 `SO_PEERCRED`、错误 JSON、
未知方法、异步 stop。静态 musl 运行暴露了测试中“socket 文件出现即已 listen”的时序假设，
已改为重试连接至监听就绪；产品行为未修改。

### 明确未验证项

- **root 身份切换**：集成测试以 `uid=1000` 运行，未实际以 root 启动 Supervisor，也未验证
  跨 UID/GID 的 `setgroups`/`setgid`/`setuid` 失败清理。相关配置校验与拒绝路径有原生测试，
  但运行时身份切换没有证据。
- **init 真机**：systemd 已在 Ubuntu 26.04 测试机完成真实安装、启停与重启验证
  （见“安装器实际执行记录”）；OpenRC 与 BusyBox 仍只有生成物和语法检查，未在真实
  init 环境执行启动、停止、Supervisor 崩溃后的 respawn。不得视为平台验收通过。
- **真实 reboot**：未执行系统重启；boot ID 变化后的 `autostart` 行为只有代码路径和单元级
  构造测试，没有真实断电/重启证据。
- **Buildroot/真实硬件、ARM32**：未构建、未运行。
- **压力/高频日志/慢客户端/消息洪流**：只有单元级边界测试，没有长时间压力测试或基准数据。
- **systemd/OpenRC/BusyBox 之外的发行版与容器环境**：未验证。

实施期间仍按本文件前述 L0/L1/L2 规则执行。发布前须实际运行适用的完整矩阵；静态编译成功不能代替进程生命周期、权限和 init 行为的运行验证。
