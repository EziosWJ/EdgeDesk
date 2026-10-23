# SPEC-0011 验收矩阵（APPX-07）

- 状态：**宿主 x86_64 已执行（2026-10-23）；跨架构、musl、root 与 init 未验证**
- 实际证据：[docs/testing.md](../testing.md) 的“2026-10-23 SPEC-0011 原生应用包实际执行记录”。
- 需求：[SPEC-0011](../spec/0011-native-application-packages.md) 第 11 节
- 共享契约：[0011-workstreams.md](0011-workstreams.md)
- 原则：只记录**实际执行**的命令、环境与结果；未执行项明确标记未验证。

## 1. 环境记录模板

每次执行填写：

```text
日期：
主机 / 内核：uname -a
libc：ldd --version
编译器：g++ --version / scripts/with-gcc15.sh
构建预设：
命令：
结果：通过/失败（附 ctest 摘要）
未覆盖：
```

## 2. 兼容性（SPEC 11.1）

| 用例 | 证据来源 | 状态 |
| --- | --- | --- |
| 现有 Web-only 包安装/审批/启用/打开/升级/降级/卸载 | `extension_package_test`、`extension_runtime_test`、`scripts/integration_test.sh` | 待执行 |
| iframe/MessagePort/权限/WindowManager/KV/文件 API/缓存回归 | web 单测 + 浏览器集成 | 待执行 |
| Web-only 包不生成 Supervisor 服务、不创建 native/run 目录 | 新增包测试 | 待执行 |

## 3. 包与安装（SPEC 11.2）

| 用例 | 预期 | 状态 |
| --- | --- | --- |
| 有效 ELF 后端包 | 安装成功，`state=stopped`、`autostart=false`，未启动进程 | 待执行 |
| 脚本 / 非 ELF / 不可执行 / 错误架构 | 安装前拒绝，现有应用不变 | 待执行 |
| 绝对路径 / 路径穿越 / 符号链接 backend | 拒绝 | 待执行 |
| SHA-256 不符 / ZIP 损坏 / 重复路径 / 磁盘不足 / 安装中断 | 失败且不登记半成品 | 待执行 |
| 安装阶段不执行后端 | 无子进程、无审计执行记录 | 待执行 |

## 4. 生命周期（SPEC 11.3）

| 用例 | 预期 | 状态 |
| --- | --- | --- |
| 配置身份后启动/停止/重启 | Supervisor 状态变化，扩展管理器不缓存进程状态 | 待执行（需 Supervisor） |
| 服务配置使用绝对路径 + argv 数组 | `service.get` 中无 shell 字符串 | 待执行 |
| `EDGEDESK_APP_SOCKET` / `EDGEDESK_APP_DATA_DIR` 由宿主传入 | backend 读取到宿主路径；manifest 无法覆盖 | 待执行 |
| autostart 独立、默认关闭 | 启停不改 autostart；安装不打开 | 待执行 |
| 身份未配置 / 解析失败 / 程序不可执行 / 架构不匹配 / Socket 权限失败 / Supervisor 不可用 | 启动返回明确错误，不报告成功 | 待执行 |
| 崩溃恢复尊重 desired_state | 依赖 SPEC-0010 PSH-05；引用其证据 | 前置已完成（见 docs/testing.md） |
| root 身份切换、init/reboot/autostart | 需真实环境；未执行即标记未验证 | 未执行 |

## 5. 通信与隔离（SPEC 11.4）

| 用例 | 预期 | 状态 |
| --- | --- | --- |
| 前端 `backend.request` → 宿主 → UDS HTTP | 返回 status/允许头/JSON body | 待执行 |
| app A 传参访问 app B / 任意 URL | 拒绝；身份只取自 session | 待执行 |
| 未启动 / Socket 权限错误 / 无效响应 | `backend_not_running` / permission / `backend_protocol_error` | 待执行 |
| 浏览器不能直接访问 Socket/TCP | 无新增 TCP listener；前端无 socket 参数 | 待执行 |
| 关闭/禁用/升级后旧实例请求 | `runtime_session_invalid` | 待执行 |

## 6. 升级、卸载与数据（SPEC 11.5）

| 用例 | 预期 | 状态 |
| --- | --- | --- |
| 升级校验失败 | 旧应用、旧二进制、数据不变 | 待执行 |
| 覆盖/元数据提交失败 | 应用保持停止，标记 `upgrade_failed`，无自动回滚/启动 | 待执行 |
| 升级不删除数据 | 数据目录与 KV 保留 | 待执行 |
| 卸载默认保留数据 | `data_retained=true`，程序与 service 配置删除 | 待执行 |
| 独立清理 | 只删除指定 appId 数据 + KV，需确认与审计 | 待执行 |
| 路径安全 | 程序/数据/运行目录不跟随符号链接 | 待执行 |

## 7. 发布门槛（SPEC 11.6）

- [ ] PSH-01～PSH-05 关闭（已有证据，引用 `docs/testing.md`）。
- [ ] x86_64 真机运行验证。
- [ ] ARM64 目标环境运行验证（交叉编译不算）。
- [ ] ARM32 未执行时明确标记未验证。
- [ ] 现有文件、终端、认证、设置、桌面、Web-only 扩展回归通过。
- [ ] `scripts/test-pr.sh` 通过；记录输出摘要。
- [ ] 不得在文档中宣称未实现能力（签名、沙箱、健康检查、跨应用、自动回滚）。

## 8. 集成执行顺序

1. 合并五个工作流分支到 `feat/spec-0011-native-app-packages`，解决文档/声明冲突。
2. `scripts/test-fast.sh`（L0）→ 失败逐项修复。
3. `scripts/test-pr.sh`（L1，含集成测试与前端构建）。
4. 手工/脚本场景：安装有效后端包 → 配置身份 → 启动 → backend.request → 停止 → 升级 → 卸载保留数据 → 独立清理。
5. 浏览器检查（1440×900、390×844）：后端状态、身份警告、启停、自启、清理确认。
6. 更新 `docs/testing.md`、SPEC-0011/ADR-0018 状态与 `docs/tasks/0011-native-application-packages.md` 勾选项；未验证平台如实标注。
