# SPEC-0011 实施计划：原生应用包与后端运行时

- 状态：**已实施（APPX-01～06 与宿主 x86_64 的 APPX-07 验收完成；ARM64/ARM32、musl、root 身份与 init/reboot 未验证）**
- 需求基线：[SPEC-0011](../spec/0011-native-application-packages.md)
- 架构决策：[ADR-0018](../adr/0018-native-application-packages.md)
- 既有基线：[SPEC-0007 Web 扩展](../spec/0007-extension-applications.md)、[SPEC-0009 Process Supervisor](../spec/0009-process-supervisor.md)
- 安全前置依赖：[SPEC-0010 Supervisor 加固](../spec/0010-process-supervisor-hardening.md)
- 原则：只实现已确认的 V1 能力；保留纯 Web 应用兼容性；不直接把未验证的平台标记为通过。

## 1. 独立 Ticket

| Ticket | GitHub Issue | 优先级 | 内容 | 依赖 |
|---|---|---:|---|---|
| APPX-01 | [#57](https://github.com/EziosWJ/EdgeDesk/issues/57) | P1 | manifest backend 契约与 ELF 架构校验 | 无 |
| APPX-02 | [#58](https://github.com/EziosWJ/EdgeDesk/issues/58) | P1 | 原生包安装、覆盖升级与数据保留 | APPX-01 |
| APPX-03 | [#59](https://github.com/EziosWJ/EdgeDesk/issues/59) | P1 | Process Supervisor 生命周期集成 | APPX-01、APPX-02、SPEC-0010 安全前置依赖 |
| APPX-04 | [#60](https://github.com/EziosWJ/EdgeDesk/issues/60) | P1 | HTTP over UDS 宿主代理 | APPX-03 |
| APPX-05 | [#61](https://github.com/EziosWJ/EdgeDesk/issues/61) | P1 | 应用管理 UI 与生命周期操作 | APPX-02、APPX-03、APPX-04 |
| APPX-06 | [#62](https://github.com/EziosWJ/EdgeDesk/issues/62) | P2 | SDK/CLI、开发模板与后端示例 | APPX-01；端到端示例依赖 APPX-03、APPX-04 |
| APPX-07 | [#63](https://github.com/EziosWJ/EdgeDesk/issues/63) | P1 | 集成、安全、兼容性与跨架构验收 | APPX-01～APPX-06 |

## 2. 实施前置门槛

原生后端会通过 Process Supervisor 执行本地 ELF，可能由管理员选择 root。实现前必须先处理 SPEC-0010 中影响身份验证、目录权限和生命周期一致性的阻塞项：

- PSH-01 #46：进程组身份校验与安全发信号。
- PSH-02 #47：主进程退出后的残留后代清理。
- PSH-03 #48：数据目录路径与权限保护。
- PSH-04 #49：SQLite 持久化失败与生命周期一致性。
- PSH-05 #50：Supervisor 恢复时尊重 desired_state。

生产发布前还必须完成适用的 PSH-09 #54 root 身份切换测试和 PSH-10 #55 init/reboot/autostart 验收。APPX-01 的 manifest 契约工作可以先行；在上述 P0/P1 安全缺陷未修复前，不得把原生后端功能标记为可生产使用。

## 3. 阶段与依赖

### Phase 1 — 契约与包校验

**Ticket：APPX-01**

- 扩展现有 manifest schema，backend 可选。
- 仅允许单个 ELF 后端、argv 数组和目标架构匹配。
- 更新 SDK/CLI 的校验契约，但不改变现有 Web-only 包格式。
- 先为路径穿越、符号链接、脚本和架构不匹配建立负向测试。

**退出条件：** 旧包回归通过；无效后端包均在安装前拒绝。

### Phase 2 — 安装与文件/数据生命周期

**Ticket：APPX-02**

- 复用现有 ZIP、SHA-256、SQLite 和操作恢复机制。
- 原生程序文件放在应用专属稳定目录；持久化数据独立存放。
- 安装不执行；卸载默认保留数据。
- 原生后端采用原目录覆盖升级，不自动回滚；Web 资源继续沿用现有 revision 机制。

**退出条件：** 安装、覆盖、失败状态、卸载和数据保留有自动化测试；旧 Web 包无回归。

### Phase 3 — Supervisor 生命周期

**Ticket：APPX-03**

- 创建一个 appId 到 serviceId 的稳定映射。
- 复用现有服务定义、用户/用户组、root 显式约束、autostart 与状态机。
- 默认不开机自启；安装不启动；Socket 与数据目录由宿主分配。
- 不生成 per-app systemd/OpenRC/BusyBox unit。

**退出条件：** 启停、退出、恢复、用户切换和 Socket 权限有测试；SPEC-0010 阻塞项关闭。

### Phase 4 — 后端 HTTP 代理

**Ticket：APPX-04**

- 在现有宿主 JSON-RPC/MessagePort 上增加 backend.request。
- 宿主根据当前实例选择自己的 UDS；前端不能传任意 Socket/URL。
- V1 支持 JSON HTTP 请求/响应，不实现 WebSocket、流式代理和跨应用调用。

**退出条件：** 请求/响应成功；未运行、权限错误、错误路径和跨应用访问均有明确失败结果。

### Phase 5 — 管理界面

**Ticket：APPX-05**

- 展示后端状态、架构、运行身份与自启状态。
- 提供运行用户/组配置、启停/重启、自启开关和升级失败信息。
- root 运行显示主机级权限警告。
- 卸载默认保留数据；彻底清理独立操作。

**退出条件：** 浏览器 E2E 覆盖主要生命周期；原有 Web-only 管理行为不回归。

### Phase 6 — 开发体验

**Ticket：APPX-06**

- 更新 manifest schema、CLI 与模板说明。
- 提供最小 HTTP over UDS 示例，读取 EDGEDESK_APP_SOCKET 和 EDGEDESK_APP_DATA_DIR。
- 按 ARM32、ARM64、x86_64 分别构建；不增加后端语言运行时依赖。

**退出条件：** 现有模板继续可用；示例包可安装并通过宿主代理返回 JSON。

### Phase 7 — 集成验收与发布

**Ticket：APPX-07**

- 运行现有 Web 扩展全量回归与新原生包测试。
- 注入 ZIP、哈希、ELF、权限、Socket、SQLite、Supervisor 和覆盖失败。
- 验证升级/卸载的数据保留策略。
- 更新 docs/testing.md、SPEC 状态和风险记录。

**退出条件：** L0/L1 与浏览器集成测试通过；x86_64、ARM64 有实际运行证据；ARM32 未验证时明确记录；SPEC-0010 发布阻塞项关闭。

## 4. 通用完成定义

每个 Ticket 必须：

- 先添加可复现问题的测试，再实现功能。
- 不通过放宽断言或跳过测试制造通过结果。
- 更新相关 SPEC/ADR/API/config/testing 文档。
- 记录真实执行的命令、环境、结果和未覆盖项。
- 不把交叉编译等同于目标设备运行成功。
- 保持文件管理、终端、认证、设置、桌面和纯 Web 扩展功能不回归。
- 在实现过程中不得自行增加已被 Grill 排除的多进程、签名、沙箱、健康检查、跨应用调用或自动回滚机制。

## 5. 发布门槛

- [x] APPX-01～APPX-07 在宿主 x86_64（glibc）完成实现与自动化验收。
- [x] PSH-01～PSH-05 相关阻塞项关闭（见 `docs/testing.md` 的 SPEC-0010 记录）。
- [ ] root 身份切换与 init/reboot/autostart 有实际验证证据（本轮未执行；同 uid 普通用户已验证）。
- [x] 安装不执行后端；后端路径与参数不经 Shell。
- [x] UDS 访问仅绑定对应应用；没有新增后端 TCP 监听。
- [x] 升级失败保持停止且可诊断；不声称自动回滚。
- [x] 卸载默认保留数据，彻底清理有独立确认。
- [x] Web-only 包和现有桌面功能回归通过（L0/L1）。
- [ ] ARM64/ARM32 与 musl 实际运行验证；`docs/testing.md` 已如实标记未验证。

实际执行证据见 `docs/testing.md` 的“2026-10-23 SPEC-0011 原生应用包实际执行记录”和
`docs/tasks/0011-acceptance.md`。不得据此表声称已满足生产发布矩阵。