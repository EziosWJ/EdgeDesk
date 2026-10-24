# ADR-0020：原生后端程序隔离、禁用生命周期与升级一致性

- 状态：**Accepted（2026-10-23，宿主单元与 L1 集成已执行；root Supervisor + 跨 UID 运行、ARM/musl 与 init 未验证）**
- 日期：2026-10-23
- 关联：[SPEC-0011](../spec/0011-native-application-packages.md)、[SPEC-0010](../spec/0010-process-supervisor-hardening.md)、[ADR-0015](0015-process-supervisor-architecture.md)、[ADR-0016](0016-process-supervisor-security.md)、[ADR-0018](0018-native-application-packages.md)、[ADR-0019](0019-process-supervisor-identity-and-operations.md)

## 背景

ADR-0018 的 V1 实现把“程序目录 0755、可执行文件 0755”当作隔离手段，但进程属主可以自行 `chmod`；在 EdgeDesk 与后端同 UID 时，后端能够修改、替换或删除自身及其他应用的安装程序。升级流程也只在下次显式启动时才刷新 Supervisor 服务定义，且禁用应用不会停止后端或关闭开机自启。本次代码审查要求在不引入沙箱/namespace 的前提下给出可靠边界。

## 决策

1. **属主是边界，模式位只是纵深防御。** 程序文件归 EdgeDesk 主机身份所有：稳定可执行文件 `0555`，程序目录 `0755`（保留宿主原子替换能力），`native/` 根 `0711`。升级时由宿主身份临时打开目录、写临时文件并 `renameat`，失败不改变旧程序。
2. **拒绝同 UID 后端。** 非 root EdgeDesk 与后端 `run_user` 相同 UID 时，双方都无法用 DAC 阻止对方改写程序文件；`set_identity`、`service_params`、`prepare_runtime` 返回 `backend_program_not_isolated`。可靠的部署是 root EdgeDesk/Supervisor + 非 root `run_user`。
3. **Supervisor 追加最后一道门。** 带有 `EDGEDESK_APP_SOCKET` 的服务在 `fork` 前校验可执行文件与工作目录：属主不得是后端 UID，且不得对组/其他用户可写。该检查覆盖 autostart、崩溃恢复和通过通用 Supervisor API 修改过的服务；普通非应用服务不受影响。
4. **root 后端是显式的主机级例外。** `allow_root=true` 的后端无法被 DAC 约束，不能宣称“不可修改程序文件”。这一点继续保持为接受风险，不能把 root 扩展视为受限插件。
5. **禁用先停止、先关自启。** 禁用含后端应用时：先持久化 `autostart=false` 并在 Supervisor 中关闭自启，再失效运行会话，然后提交 `service.stop` 并等待操作终态；只有确认 `STOPPED/FAILED` 且自启已关闭后才写入 `disabled`。任一步失败返回非 2xx，应用保持 enabled。重新启用不会隐式启动后端。
6. **升级同步服务定义。** 在停止旧后端之后、提交新修订/覆盖二进制之前，先以 `suppress_autostart` 关闭 Supervisor 自启；安装成功后用当前记录（绝对路径、argv、env、身份、自启）`service.update` 同步已存在的服务。同步失败标记 `upgrade_failed`、自启保持关闭并尝试 `service.delete`/安全状态，不自动回滚、不自动启动。
7. **版本一致性由完整包保证。** 含后端的应用禁止只切换 Web 历史修订；`activate_revision` 返回冲突并要求上传完整包。降级同样走完整包流程。
8. **ELF 执行位来自包。** ZIP 解压保留包的 Unix 执行位（仅保留 `0111`，其余规范化为 `0600`）；安装器不再补执行位，`0644/0600/无模式` 的 ELF 在安装前拒绝。最终稳定文件由宿主写成 `0555`。
9. **服务 ID 失败补偿。** `service.create` 成功后若扩展数据库无法持久化 id：立即关闭自启并 `service.delete`；清理失败写入 `operations`（`type=backend_orphan`）并在下次启动重试。创建请求期间强制 `autostart=false`，映射成功后才恢复管理员偏好，避免孤儿服务开机自启。
10. **代理查询串。** 后端请求目标支持 origin-form 的 `path[?query]`；拒绝 `#`、绝对 URL、协议相对路径、`.`/`..`（含百分号编码形式）、编码路径分隔符和控制字符，目标按原样转发。已排队请求在执行前重新校验运行时会话与应用状态。

## 后果与风险

- 非 root 同 UID 部署不再可用；这是有意的安全收紧，文档与 UI 必须提示 root Supervisor + 非 root 运行身份。
- root 后端仍具有主机级权限，属于接受风险。
- 代理的“校验 → 建立连接”窗口仍可能让已开始执行的请求触达后端；这是残余竞态，禁用会关闭会话并停止进程。
- Supervisor 服务同步失败时没有二进制自动回滚；管理员必须重新安装包。
- 所有结论仍需在 root/跨 UID、ARM64/ARM32、musl 与 init/reboot 环境验证后才能用于生产发布声明。
