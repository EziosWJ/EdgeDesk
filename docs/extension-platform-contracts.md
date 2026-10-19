# EdgeDesk 扩展应用平台实现契约（Host API 1.0）

> 状态：**已实现（2026-10-18，宿主 L0/L1 与真实浏览器验证；跨平台矩阵待 CI）**
> 需求基线：[SPEC-0007](spec/0007-extension-applications.md)
> 决策：[ADR-0011](adr/0011-extension-runtime-and-packages.md)、[ADR-0012](adr/0012-extension-security-and-communication.md)、[ADR-0013](adr/0013-extension-file-api-and-transfer.md)
> 风险：[扩展应用风险清单](security/extension-app-risk-register.md)
>
> 本文把已确认需求固化为可测试的内部契约，不重新开启产品决策。实现状态以代码、测试和 [验证矩阵](testing.md) 为准。

## 1. 版本与身份

- 宿主 API 版本：`1.0.0`，独立于 EdgeDesk 产品版本。
- 应用 ID：稳定反向域名，`^[a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?(\.[a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?)+$`。
- 应用版本：严格 SemVer 2.0.0；pre-release 参与比较，build metadata 不参与。
- 每次安装/覆盖生成 32 字符随机 `revisionId`；相同 SemVer 不重用修订。
- 静态 URL：`/apps/{appId}/{version}/{revisionId}/{relativePath}`。
- 运行实例 UUID 与窗口持久化记录分离；UUID/令牌不是授权凭证。

## 2. manifest 契约

核心字段：

| 字段 | 要求 |
| --- | --- |
| `id` | 必填，反向域名，稳定 |
| `name` | 必填，1..120 字节，无控制字符 |
| `version` | 必填，严格 SemVer |
| `entry` | 必填，首期必须为 `index.html` |
| `minHostApiVersion` / `maxHostApiVersion` | 必填，SemVer，范围包含端点 |
| `permissions` | 可选数组：`storage`、`files.manage`、`terminal.access`、`network` |
| `network.origins` | 可选数组，`http(s)://host[:port]`，支持 `*.domain` |
| `window` | 可选：默认/最小宽高 1..10000，`instances: single|multi` |
| `icon` | 可选，包内规范化相对路径 |
| `description` / `publisher` | 可选展示字段；`publisher` 不受信任 |

- 声明 `network.origins` 会自动把 `network` 加入申请权限集合。
- `backend` / `runtime` / `scripts` 字段只产生“保留且不执行”警告。
- 未知权限、非法来源、路径穿越式 `icon`、反向范围均拒绝安装。
- 宿主 API 范围不兼容时拒绝启用；核心升级后可自动禁用不兼容应用，但保留包与数据。

## 3. 生命周期状态机

```text
install/upload
   │  staging ──> extracted ──> committed ──> done
   │      └────────── failure ──────────────> failed (+ 清理临时目录/修订)
   ▼
pending_review ──approve(enable)──> enabled ──disable──> disabled
   │                                  │  ▲                  │
   │                                  │  └──enable───────────┘
   │                                  ├──upgrade──> enabled/disabled (活动修订原子切换)
   │                                  ├──integrity mismatch──> isolated
   │                                  └──uninstall──> uninstalled (保留/删除数据)
   └──uninstall──> uninstalled
```

- 首次安装即使校验通过也进入 `pending_review`，不能启动。
- 升级保留原授权；新权限保持未授权，需管理员再次批准。
- 单项权限撤销延迟到下一运行实例；禁用立即关闭窗口、端口、事件和该实例创建的终端。
- 完整性检查按清单与实际文件集合**精确比对**：缺失文件、大小/SHA-256 不符、未登记的普通文件、符号链接与特殊文件（以及重复/大小写冲突路径）均判定失败，并给出确定的 `mismatch_path`。静态资源只服务活动修订清单中登记的文件，未登记文件即使物理存在也返回 404。
- 完整性失败范围：**仅活动修订**失败时，修订与应用标记 `isolated`、清除有效授权、关闭运行访问，保留文件与审计；**历史修订**失败只把该修订标记为 `isolated`/`integrity=mismatch` 并禁止再次激活，不改变应用状态、活动修订、授权或现有运行会话。修复/重装后回到 `pending_review`，必须重新审批。
- 服务重启：未完成 operation 的临时目录与未提交修订被幂等清理；活动指针指向的修订存在性会被校验。KV 删除与 app/revision/权限/完整性状态在同一 SQLite 事务提交，恢复后不会出现“KV 已删除但重装未提交”的组合。
- 传输日志：扩展上传在 `apps_root/tmp/transfers/*.journal` 记录临时文件位置；重启时删除孤儿临时文件再删除日志。

## 4. SQLite schema（`apps_root/extensions.db`，schema v2；v1 原地迁移）

| 表 | 关键列 | 约束 |
| --- | --- | --- |
| `apps` | `app_id` PK、`status`、`active_revision_id`、`active_version`、`data_retained` | 应用状态唯一来源 |
| `revisions` | `revision_id` PK、`app_id` FK、`version`、`manifest_json`、`status`、`pinned`、`integrity` | 同 app 多修订；删除 app 级联 |
| `permission_requests` | `(app_id, module)` PK | manifest 申请集合 |
| `permission_grants` | `(app_id, module)` PK、`granted_at` | 管理员有效授权 |
| `network_origins` | `(app_id, origin)` PK、app FK | 管理员批准的来源；与授权在同一事务内更新，删除应用级联 |
| `integrity_entries` | `(revision_id, path)` PK、`size`、`sha256` | 逐文件 SHA-256 清单 |
| `operations` | `operation_id` PK、`type`、`phase`、`temp_path`、`revision_id` | 启动恢复依据 |
| `app_kv` | `(app_id, key)` PK、`value` JSON 文本、`size_bytes` | 无 FK：卸载保留数据 |

- `PRAGMA journal_mode=WAL`、`foreign_keys=ON`、`synchronous=FULL`；数据库文件 `0600`，根目录 `0700`。
- 所有跨表状态变更在 `BEGIN IMMEDIATE` 事务中提交；失败回滚。
- 重装“保留但不恢复”或陈旧保留行的 KV 删除位于安装的最终事务内；ZIP/清单校验、修订目录移动、完整性写入或 SQLite 失败都会回滚并保留旧 KV 与原应用状态。
- 扩展应用不能直接打开数据库，也不能用 localStorage/IndexedDB 作为持久化契约。

## 5. JSON-RPC 与 MessagePort

### 5.1 握手

1. iframe 载入后向 `window.parent` 发送 `{type:"edgedesk:hello", protocol:1}`，每 500 ms 重试。
2. 宿主校验 `event.source === iframe.contentWindow`、`event.origin === "null"`、消息结构，并通过一次 `postMessage` 发送 `edgedesk:init`。
3. `init` 包含随机握手令牌、应用/实例/修订信息、宿主 API 版本、权限快照、来源白名单、窗口策略，并转移 `MessagePort`。
4. 之后只接受该端口上的 JSON-RPC；不再从 `window.postMessage` 接收宿主 API 请求。
5. 15 秒内未完成握手显示统一错误与重新加载入口。

### 5.2 权限映射

| 方法族 | 权限 |
| --- | --- |
| `platform.*`、`auth.getStatus`、`events.*` | 无 |
| `storage.*` | `storage` |
| `files.*` | `files.manage` |
| `terminal.*` | `terminal.access` |

宿主桥接先按运行实例权限快照检查；C++ 运行时端点再按同一快照检查（纵深防御，不削弱既有认证）。禁用/升级/卸载/隔离会使会话立即失效。

### 5.3 方法目录与结果

| 方法 | 参数 | 结果 |
| --- | --- | --- |
| `platform.getInfo` | `{}` | `hostApiVersion, appId, instanceId, revisionId, version` |
| `platform.reportError`（通知） | `type,message,stack` | 写入脱敏审计；字段分别截断（120/1024/2048 字节）并结构化编码，不写原始请求正文 |
| `auth.getStatus` | `{}` | `authenticated, method`（不含 Cookie/Token） |
| `storage.get` | `key` | `key,value,sizeBytes,updatedAt` |
| `storage.set` | `key,value` | `key,sizeBytes,usageBytes` |
| `storage.delete` | `key` | `key,deleted,usageBytes` |
| `storage.list` | `cursor?,limit?` | `entries[{key,sizeBytes,updatedAt}],nextCursor,usageBytes,quotaBytes` |
| `storage.transaction` | `operations[{op:"set"|"delete",key,value?}]` | `applied,usageBytes` |
| `files.list` | `path,cursor?,limit?` | `path,entries[],nextCursor,sort` |
| `files.stat` | `path` | 文件元数据 |
| `files.read` | `path` | `entry,encoding:"utf-8"|"base64",content`（≤1 MiB） |
| `files.mkdir` / `files.rename` / `files.delete` | 与既有文件 API 一致 | `ok` 与删除计数 |
| `files.upload.begin` | `path,size` | `transferId,chunkSize,totalSize,port` |
| `files.download.begin` | `path` | `transferId,chunkSize,totalSize,port` |
| `files.upload.cancel` / `files.download.cancel` | `transferId` | `ok` |
| `terminal.sessions.list` | `{}` | `sessions[],shell,shell_available,max_sessions` |
| `terminal.sessions.create` | `title,cwd,cols,rows` | `session` |
| `terminal.sessions.input` | `sessionId,data` | `ok` |
| `terminal.sessions.resize` | `sessionId,cols,rows` | `ok` |
| `terminal.sessions.close` | `sessionId` | `ok` |
| `events.subscribe` | `events[]`（`visibility`、`theme`、`window.focus`） | `subscribed[]` |
| `events.unsubscribe` | `events[]` | `ok` |

事件通知：`{"jsonrpc":"2.0","method":"event","params":{"name":...}}`；终端输出/退出/拦截/错误使用 `terminal.output`、`terminal.exit`、`terminal.guard`、`terminal.error` 通知。事件不持久化、不重放。

### 5.4 错误码

| code | 含义 |
| --- | --- |
| `-32700` | JSON 解析错误（传输层） |
| `-32600` | 非 JSON-RPC 2.0 请求 |
| `-32601` | 未知方法 |
| `-32602` | 参数无效 |
| `-32603` | 宿主内部错误 |
| `-32000` | 权限拒绝 / 调用方等待超时 |
| `-32001` | 运行会话失效 |
| `-32002` | 文件被锁定 |
| `-32003` | 目录游标失效（`CURSOR_STALE`） |
| `-32004` | 存储配额超限 |
| `-32005` | 未找到 |
| `-32006` | 冲突 |
| `-32007` | 传输错误 |
| `-32008` | 请求过大 |
| `-32010` | 不支持 |
| `-32011` | I/O 错误 |

调用方等待超时只拒绝本次等待，不表示宿主操作已取消。平台不设置 JSON-RPC 总消息大小上限，也不新增扩展级速率/并发/执行超时限制（ADR-0012）。

## 6. 二进制传输

- 每块固定 1 MiB，末块可更小；不使用 JSON Base64。
- `begin` 返回专用 `MessagePort`；应用发送 `{type:"chunk",offset,data:ArrayBuffer}`。
- 宿主写入后回复 `{type:"ack",offset,received}`；应用收到确认后才发送下一块。
- 上传完成：应用发送 `{type:"complete"}`；宿主核对总长度并原子替换，回复 `{type:"done",entry}`。
- 下载：应用发送 `{type:"start"}`；宿主按顺序发送块，应用逐块 `ack`；完成后宿主发送 `{type:"done",totalSize}`。
- 取消/失败/实例关闭/重启：关闭端口、删除临时文件、释放锁；不支持断点续传。
- 普通文件只比较长度，不计算内容哈希；应用 ZIP 仍逐文件 SHA-256。

## 7. 文件语义

- 虚拟路径由 `FileService`/`PathSandbox` 解析；扩展层不拼接服务器绝对路径。
- 目录排序固定：目录优先、文件其次；组内按名称字节序升序、区分大小写、不依赖 locale。
- 游标包含最后条目 `(is_dir,name)` 和目录 `(dev,ino,mtime,nsec)`，并用进程随机 HMAC 绑定 appId；目录变化返回 `CURSOR_STALE`。
- 锁身份：已存在目标用 `(st_dev,st_ino)`；不存在目标用父目录 `(st_dev,st_ino)` + 文件名。
- 读锁共享；写/覆盖/删除/重命名独占；冲突立即 `FILE_LOCKED`（HTTP 423）。
- 上传先写同目录 `.edgedesk-upload-*.tmp`，长度/落盘/fsync 成功后 `renameat` 原子替换；失败保留原文件。
- 上传开始时记录目标 `(dev,ino,size,mtime)`；提交前复查，外部进程造成的变化返回 `conflict` 并保留原文件。检查与 rename 之间的最终竞态无法由内部锁消除。
- 平台内部受保护路径集中定义在 `src/file/protected_paths.{hpp,cpp}`：生效配置的 `data_dir`、`state_dir`、`apps_root`、审计日志及其轮转/清理临时文件、`config_file`，以及设置时的 `auth_token_file`。扩展运行时使用独立于管理员文件管理器的 `FileService`/`PathSandbox`，管理员 `/api/files` 语义不变。
- 受保护路径在目录 FD/openat 解析边界执行，覆盖读、写/创建/覆盖、stat、列目录、mkdir、rename 源与目标、remove、上传 begin/append/finish 和下载打开。判定同时使用规范化绝对路径（阻止尚不存在的受保护目标被创建）和 `(st_dev,st_ino)` 身份（阻止硬链接别名及 follow 模式下的符号链接目标）；受保护目录的现有后代在构造时登记身份。拒绝返回 `permission_denied`，且不修改目标、不暴露受保护内容。

## 8. 静态资源与 CSP

- 仅活动、完整、已启用修订可被服务；URL 必须同时匹配 appId/version/revisionId。
- MIME 由扩展名确定；无扩展名路径回退 `index.html`；带扩展名的缺失资源返回 404。
- 缓存：修订资源 `public, max-age=31536000, immutable`；动态管理 API 不缓存。
- CSP：`default-src 'self'` + **有效来源**；脚本、样式、图片、字体、连接、框架和表单共用同一白名单；`object-src 'none'`、`base-uri 'none'`、`frame-ancestors 'self'`。
- 有效来源 = 当前活动修订 manifest 声明来源 ∩ 管理员已批准来源，且必须同时存在 `network` 授权。缺少授权或批准记录时，所有指令均不产生外部 source。
- 已批准来源保存在扩展 SQLite 的 `network_origins(app_id, origin)`（schema v2），与授权变更在同一事务内写入；`network` 被撤销、卸载、隔离或重装时清除。旧 schema（v1）原地迁移，`CREATE TABLE IF NOT EXISTS` 保留既有数据；旧库中已有 `network` 授权但没有批准来源记录时，视为未批准任何来源（CSP 无外部 source），需管理员重新保存权限后生效。
- 管理员审批（`approve()`/`update_permissions()`）记录当前活动修订声明的来源。升级或切换修订时，若新修订的规范化来源集合不是已批准集合的子集，则移除保留的 `network` 授权（应用仍启用，但网络被拒绝），需管理员重新审批；集合未扩大则继续生效。回滚/降级遵循同一规则。
- 静态资源响应与 `create_session` 都按上述有效来源计算；CSP header 与会话快照不会回退到 manifest 全部来源。
- 由于 sandbox iframe 的源为 opaque，服务端在 CSP 中额外列出精确的 `http://Host` 与 `https://Host`。
- `X-Frame-Options: SAMEORIGIN`；`Cross-Origin-Resource-Policy: cross-origin`。
- CSP 只约束其覆盖的资源类型。`window.open`、`target="_blank"` 和 iframe 自身的顶层导航不受 CSP/网络来源授权限制；`allow-popups-to-escape-sandbox` 与 OAuth 流程保留。该边界见 [ADR-0014](adr/0014-extension-popup-navigation-boundary.md)（Proposed）与风险清单 R-04/R-05，不得宣称弹窗导航已被执行。

## 9. 威胁模型与残余风险

| 风险 | 实现控制 | 证据 |
| --- | --- | --- |
| R-01 未签名代码 | 管理员上传、结构校验、默认拒绝权限、首次审批、UI 警告 | `manifest_test`、`extension_package_test`、集成测试 |
| R-02/R-19 压缩/资源压力 | 流式解压、声明大小磁盘预检、临时目录、原子提交、失败清理 | `extension_package_test` |
| R-03 无消息/速率上限 | 每消息结构校验、异常隔离、保留核心 HTTP 限流 | 代码审查 + 集成测试；残余风险接受 |
| R-04 网络白名单/重定向 | 有效来源 = 声明 ∩ 批准且需 network 授权；升级扩大来源需重新审批；CSP 覆盖资源加载；HTTP/重定向风险仍接受 | `manifest_test`、`extension_store_test`、`extension_package_test`、集成测试、浏览器边界脚本 |
| R-05 opener/顶层导航 | 允许弹窗（含 `allow-popups-to-escape-sandbox`）；宿主只接受已握手 iframe 的桥接消息；弹窗/`target=_blank`/iframe 自身导航不受 CSP 限制 | 浏览器边界脚本；残余风险接受（ADR-0014 Proposed） |
| R-06 桥接执行 | 特权 API 仅通过 MessagePort；后端不开放 Origin:null 直连 | 运行时端点 + 会话令牌 |
| R-07 延迟撤销 | 权限快照；禁用/升级/卸载立刻失效会话并关闭终端 | 集成测试 |
| R-08 路径安全 | 复用 PathSandbox/openat；受保护路径在解析边界按规范化路径与 `(dev,ino)` 身份拒绝；扩展层不实现自有文件系统边界 | `path_sandbox_test`、`file_service_test`、既有 + 扩展测试 |
| R-09 内容完整性 | 长度校验、逐块确认、临时文件、原子替换 | 集成测试（2.5 MiB 往返） |
| R-10 哈希检测 | 安装清单、完整性检查、隔离 | `extension_package_test`、集成测试 |
| R-11 无秘密存储 | KV 文档明确非秘密；不返回宿主凭据 | 文档 + `auth.getStatus` 最小字段 |
| R-12 迁移 | 多键事务 API，全成全败；应用负责 schema 版本 | `extension_store_test` |
| R-13 审计路径 | 只记元数据与规范化路径，不记内容 | 审计检查 |
| R-14 事件不重放 | 通知实时推送，文档要求重新查询 | 代码/文档 |
| R-15 修订混淆 | revisionId/URL/清单/回滚按修订隔离 | `extension_package_test` |
| R-16 SQLite 范围 | 仅 `src/extensions/` 使用；现有模块无迁移 | CMake/代码审查 |
| R-17 应用隔离 | 应用间无 IPC；独立 iframe/端口/KV | 代码/浏览器验证 |
| R-18 会话复用 | 不向 iframe 返回 Cookie/Token；无代理实现 | 代码审查 |
| R-20 外部修改 | 提交前依赖内部锁与原子替换；不宣称消除外部竞态 | 文档 + 锁测试 |

## 10. 实现位置

```text
src/extensions/       manifest/SemVer、ZIP、SQLite store、package manager、
                      runtime sessions、KV、cursor、transfer/lock 协调
src/file/file_lock.*  统一共享读/独占写锁；FileService 集成
src/http/routes/extension_route.cpp
                      /api/extensions、/api/extensions/runtime、/apps 静态资源
web/src/extensions/   管理 UI、iframe runtime、host bridge、动态注册表
tools/ext-sdk/        SDK、CLI、vanilla/React/Vue/Svelte 模板
scripts/extension_test.py
                      扩展平台集成测试（含崩溃恢复模式）
```

未实现/未验证（不得宣称完成）：

- 数字签名、公共应用市场、应用间通信、秘密存储、后台任务。
- 后端/脚本运行时（manifest 字段只保留且不执行）。
- 跨平台 L2 矩阵（ARM64/musl/真实硬件）本轮未重新执行；以 CI 和 `docs/testing.md` 后续记录为准。
