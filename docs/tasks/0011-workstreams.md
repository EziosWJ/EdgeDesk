# SPEC-0011 并行实施契约（Workstream Handoff）

- 状态：**实施中**
- 需求：[SPEC-0011](../spec/0011-native-application-packages.md)
- 决策：[ADR-0018](../adr/0018-native-application-packages.md)
- 任务拆分：[0011 实施计划](0011-native-application-packages.md)
- 集成分支：`feat/spec-0011-native-app-packages`
- 集成 worktree：`.claude/worktrees/spec-0011`

本文只固定并行工作流之间的**共享接口与文件归属**，不新增产品决策。实现细节必须服从 SPEC-0011 与 ADR-0018。

## 1. 共享基础层（集成分支已提交，勿重复设计）

| 文件 | 作用 |
| --- | --- |
| `src/extensions/backend_spec.hpp` | manifest 可选 `backend` 字段模型 |
| `src/extensions/elf_info.hpp/.cpp` | ELF 架构探测接口（APPX-01 实现） |
| `src/extensions/native_backend.hpp/.cpp` | `BackendRecord`、状态、稳定路径与服务 ID 推导 |
| `src/extensions/native_backend_installer.hpp/.cpp` | 程序目录/安装/覆盖/删除接口（APPX-02 实现） |
| `src/extensions/native_backend_lifecycle.hpp/.cpp` | Supervisor 服务参数、运行目录、身份与自启（APPX-03 实现） |
| `src/extensions/backend_proxy.hpp/.cpp` | 有界 HTTP-over-UDS 代理（APPX-04 实现） |
| `src/extensions/extension_store.hpp/.cpp` | schema v3 `app_backends` 表与 CRUD（已完成） |
| `src/http/routes/native_backend_route.*` | 管理 API 路由（APPX-03 实现） |
| `src/http/routes/backend_proxy_route.*` | 运行时代理路由（APPX-04 实现） |
| `src/app/application.*`、`src/http/server.cpp`、`CMakeLists.txt` | 进程装配与路由注册；已包含全部新模块 |

- `PackageManager` 已增加 `set_backend_installer()` 与 `set_backend_stop_hook()`；APPX-02 在 install/upgrade/uninstall 中调用。
- 应用根目录约定：
  - 程序：`<apps_root>/native/<app_id>/backend`
  - 数据：`<apps_root>/data/<app_id>`（通过 `EDGEDESK_APP_DATA_DIR`）
  - 运行目录：`<apps_root>/run/<app_id>`，Socket：`<apps_root>/run/<app_id>/backend.sock`
  - Supervisor serviceId：首次创建前用 `app-<sha256(appId)[:32]>` 占位；Supervisor 在 `service.create` 时分配实际 id，宿主立即回写 `app_backends.service_id`，升级保留该 id。所有后续 start/stop/restart/logs 使用已保存的 id。
- 环境变量名固定为 `EDGEDESK_APP_SOCKET` / `EDGEDESK_APP_DATA_DIR`，manifest 不能覆盖。
- `BackendRecord.state` 只保存扩展管理器状态：`stopped`、`upgrading`、`upgrade_failed`。进程存活状态一律读取 Supervisor，不得复制。

## 2. 管理 API 契约（APPX-03 / APPX-05 共用）

| 方法 | 路径 | 请求 | 成功响应 |
| --- | --- | --- | --- |
| GET | `/api/extensions/{app_id}/backend` | — | `{"backend":{...}}` |
| PUT | `/api/extensions/{app_id}/backend/identity` | `{"run_user","run_group","allow_root"}` | `{"ok":true,"backend":{...}}` |
| PUT | `/api/extensions/{app_id}/backend/autostart` | `{"enabled":bool}` | `{"ok":true,"backend":{...}}` |
| POST | `/api/extensions/{app_id}/backend/start` | `{}` | `{"ok":true,"backend":{...}}` |
| POST | `/api/extensions/{app_id}/backend/stop` | `{}` | `{"ok":true,"backend":{...}}` |
| POST | `/api/extensions/{app_id}/backend/restart` | `{}` | `{"ok":true,"backend":{...}}` |
| POST | `/api/extensions/{app_id}/data/purge` | `{"confirm":true,"app_id":"<same>"}` | `{"ok":true,"purged":true}` |

`backend` 对象字段（稳定）：

```json
{
  "present": true,
  "service_id": "app-…",
  "arch": "x86_64",
  "host_arch": "x86_64",
  "executable": "/abs/apps/native/<id>/backend",
  "program_dir": "/abs/apps/native/<id>",
  "data_dir": "/abs/apps/data/<id>",
  "socket_path": "/abs/apps/run/<id>/backend.sock",
  "run_user": "", "run_group": "", "allow_root": false,
  "autostart": false,
  "state": "stopped|upgrading|upgrade_failed",
  "error": "",
  "supervisor": { "state": "running|stopped|…", "pid": 1, "last_exit_code": 0 }
}
```

- 纯 Web 应用返回 `{"backend":{"present":false}}`，管理 UI 不展示后端操作。
- 启动前置条件不满足（身份未配置、程序不可执行、架构不匹配、运行目录权限无法安全建立、Supervisor 不可用）必须返回明确错误，不能返回成功。
- 关闭/禁用/升级/卸载后旧实例的 `backend.request` 必须失败（会话失效优先）。
- 现有 `/api/extensions/{app_id}` 详情应附带同一 `backend` 对象。

## 3. 运行时代理契约（APPX-04）

- 路径：`POST /api/extensions/runtime/backend/request`
- 身份：只从请求头 `X-EdgeDesk-Runtime` 的 runtime session token 得到 appId（与其他 runtime 端点一致）；请求体里的 `token` 字段被忽略，任何 `app_id`/`socket`/`url`/`host` 字段一律拒绝。
- 请求体：`{"method":"GET|POST|PUT|PATCH|DELETE","path":"/relative","body":<optional JSON>,"contentType":"application/json"}`
- 成功响应（宿主到浏览器）固定 `200`：`{"status":<backend HTTP status>,"headers":{...},"body":<JSON|null>}`
- 宿主不得透传 `Host`、`Connection`、`Upgrade`、`Transfer-Encoding`、`Content-Length` 等 hop-by-hop 头；响应只允许 `content-type` 与 `x-*`。
- 稳定错误 slug：`backend_not_configured`、`backend_not_running`、`backend_unavailable`、`backend_timeout`、`backend_protocol_error`、`invalid_argument`、`runtime_session_invalid`。
- 前端新增 JSON-RPC `backend.request`（无需新增 manifest 权限；仅访问本应用后端）。错误映射：未配置/未运行 → `notFound`/`conflict`，超时 → `internal` 并带 `{slug}`。
- 不实现 WebSocket、流式响应、任意 TCP 或跨应用调用。

## 4. 文件归属（避免并行冲突）

| 工作流 | Ticket | 独占文件 |
| --- | --- | --- |
| A | APPX-01/02 | `src/extensions/elf_info.*`、`src/extensions/manifest.*`、`src/extensions/native_backend_installer.*`、`src/extensions/package_manager.*`、`tests/extensions/elf_info_test.cpp`、`tests/extensions/native_backend_test.cpp`、相关测试改动 |
| B | APPX-03 | `src/extensions/native_backend_lifecycle.*`、`src/http/routes/native_backend_route.*`、`tests/extensions/native_backend_lifecycle_test.cpp` |
| C | APPX-04/05 | `src/extensions/backend_proxy.*`、`src/http/routes/backend_proxy_route.*`、`web/src/extensions/*`、`web/src/features/processes/*`（如需要）、`tests/extensions/backend_proxy_test.cpp` |
| D | APPX-06 | `tools/ext-sdk/**`、`examples/**`、`docs/` 中 SDK/模板说明 |

- 不要修改 `src/app/application.*`、`src/http/server.cpp`、`CMakeLists.txt` 的共享区域；如需新源文件，先在集成分支登记。
- 测试文件若需新增，使用上表已登记的路径；CMake 已登记测试目标。
- 需要 store 新方法时先在本文追加，避免同一文件并行修改。

## 5. 验收证据

- 每个工作流必须运行对应单测；集成后统一运行 `scripts/test-fast.sh` 与 `scripts/test-pr.sh`。
- 平台矩阵（musl/ARM64/ARM32）由集成阶段按 `docs/testing.md` 执行并如实标注；不得把交叉编译当作运行验证。
- 安装阶段不得执行后端；测试必须断言安装后 `state=stopped`、`autostart=false`。
- 未实现或未验证的能力不得在文档中标记为完成。
