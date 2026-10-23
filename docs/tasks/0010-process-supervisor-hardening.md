# SPEC-0010 实施计划：Process Supervisor 可靠性与安全加固

- 状态：**PSH-01～PSH-11 已实现；宿主、L1、root/systemd/真实 reboot 与隔离压力验收已完成；OpenRC/BusyBox/ARM64 未验证**
- 需求基线：[SPEC-0010](../spec/0010-process-supervisor-hardening.md)
- 原始功能基线：[SPEC-0009](../spec/0009-process-supervisor.md)
- 总跟踪 Issue：[#45](https://github.com/EziosWJ/EdgeDesk/issues/45)
- 验证基线：[测试与验证矩阵](../testing.md)

> 本计划只拆分修复和验收任务，不代表代码已经修改或测试已经通过。所有验收证据必须来自实际执行。

## 1. Ticket 列表

| Ticket | GitHub Issue | 优先级 | 主题 | 依赖 |
|---|---|---:|---|---|
| PSH-01 | [#46](https://github.com/EziosWJ/EdgeDesk/issues/46) | P0 | 进程组身份校验与安全发信号 | 无 |
| PSH-02 | [#47](https://github.com/EziosWJ/EdgeDesk/issues/47) | P1 | 主进程退出后的残留后代清理 | PSH-01 |
| PSH-03 | [#48](https://github.com/EziosWJ/EdgeDesk/issues/48) | P1 | 数据目录路径与权限保护 | 无 |
| PSH-04 | [#49](https://github.com/EziosWJ/EdgeDesk/issues/49) | P1 | SQLite 持久化失败与生命周期一致性 | 无 |
| PSH-05 | [#50](https://github.com/EziosWJ/EdgeDesk/issues/50) | P1 | Supervisor 恢复时尊重 desired_state | PSH-04 |
| PSH-06 | [#51](https://github.com/EziosWJ/EdgeDesk/issues/51) | P1 | 长时间 stop/restart 异步操作契约 | PSH-04；PSH-05 可并行 |
| PSH-07 | [#52](https://github.com/EziosWJ/EdgeDesk/issues/52) | P2 | shebang 脚本安全执行 | 无 |
| PSH-08 | [#53](https://github.com/EziosWJ/EdgeDesk/issues/53) | P2 | 日志轮询审计限频/聚合 | 无 |
| PSH-09 | [#54](https://github.com/EziosWJ/EdgeDesk/issues/54) | P1 | root Supervisor 身份切换真实测试 | PSH-04 |
| PSH-10 | [#55](https://github.com/EziosWJ/EdgeDesk/issues/55) | P1 | systemd/OpenRC/BusyBox/reboot 验收 | PSH-04、PSH-05 |
| PSH-11 | [#56](https://github.com/EziosWJ/EdgeDesk/issues/56) | P2 | IPC、日志、多服务压力与故障注入 | PSH-01、02、04、06 |

## 2. 建议实施顺序

### Phase A — 安全边界先行

1. PSH-01：解决进程组身份验证与信号发送安全；不确定时必须 fail closed。
2. PSH-03：修复数据目录逐级路径校验，确保不会修改祖先目录权限。
3. 为以上两项先添加能复现原问题的回归测试，再实现修复。

### Phase B — 生命周期与持久化

4. PSH-04：定义持久化失败语义、启动握手、停止意图先落盘和故障恢复行为。
5. PSH-02：主进程退出后保留组身份并清理后代，依赖 PSH-01 的身份判断。
6. PSH-05：Supervisor 重启后依据持久化期望状态恢复；人工停止必须优先。
7. PSH-06：将长时间 stop/restart 改为非阻塞操作 ID 与查询，更新 API、前端和测试。

### Phase C — 运行兼容性与审计

8. PSH-07：安全支持 shebang 脚本，保持 inode 安全和 argv 边界。
9. PSH-08：减少被动日志轮询产生的审计噪声，保留关键安全事件。

### Phase D — 实际环境验证

10. PSH-09：在隔离环境真实测试 root Supervisor 的 UID/GID 切换与失败清理。
11. PSH-10：执行 init 启停、Supervisor respawn 和 reboot/autostart 验收。
12. PSH-11：执行有明确资源阈值的压力和故障注入测试。

PSH-01 与 PSH-03 可以并行；PSH-07 与 PSH-08 可独立实施。涉及进程身份、持久化协议或 HTTP API 的变更，必须先更新接口契约，避免实现之间发生状态语义冲突。

## 3. 每个 Ticket 的通用完成定义

- 先添加可以稳定复现问题的测试，再修复实现。
- 代码审查覆盖错误路径、并发竞态、FD/子进程所有权和失败清理。
- 不删除安全检查，不用放宽断言或只改测试来制造通过。
- 更新相关 SPEC/ADR/API/config/testing 文档。
- L0 执行项目规定的快速原生测试；涉及运行行为时执行完整 L1 集成测试。
- 影响 CMake、脚本执行、静态链接或平台适配时，执行适用的 musl/static-PIE 与 ARM64/QEMU 验证。
- 每次记录实际命令、环境、结果和未覆盖项；不将未执行项目写成通过。
- 每个 Issue 独立提交或 PR，便于回滚和定位回归。

## 4. 总体发布门槛

- [x] PSH-01～PSH-08 所有功能/回归验收通过（2026-10-23；证据见 SPEC-0010 §6）。
- [x] PSH-09 在 Ubuntu 26.04 虚拟机以 root 运行 Supervisor，验证 UID/GID/补充组、root
  确认、无效用户、SIGTERM/SIGKILL、子进程清理与 socket/DB/日志权限（46/46 检查）。
- [x] PSH-10 systemd 启停、graceful restart、SIGKILL respawn 接管、真实 reboot 后的
  autostart/人工停止语义（24/24 + 19/19 检查）。OpenRC/BusyBox 仍未验证。
- [x] PSH-11 在隔离实例完成 1,000+ 生命周期操作与 30 分钟持续压力；资源、SQLite、
  日志、孤儿清理与操作记录有界（详见 SPEC-0010 §6）。
- [x] 最终合并树 L0/L1 和适用的 x86_64 musl static-pie 构建/ctest 全部通过。
- [x] 文件管理、终端、仪表盘、认证、设置、桌面、扩展平台回归通过（L1 套件）。
- [x] 更新 docs/testing.md，所有未运行平台和未完成测试明确标记“未验证”。
- [x] 将 SPEC-0010 和本计划状态更新为与实际证据一致的状态。

## 5. 当前已知验证缺口

以下内容仍不可宣称完成：

- OpenRC、BusyBox init 的真实启动、停止、Supervisor 崩溃 respawn 与 reboot 语义。
- ARM64/QEMU 与 Bootlin musl 交叉构建在本轮未重跑；本轮仅验证 x86_64 musl static-pie。
- 真实硬件断电、文件系统损坏与磁盘满故障注入（只做了 SQLite 写入失败的注入测试）。
