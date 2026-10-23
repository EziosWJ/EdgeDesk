# ADR-0018：原生应用包与后端运行时集成

- 状态：**Accepted（宿主 x86_64 已实现；跨架构、musl、root 身份与 init 验证未完成）**
- 日期：2026-10-23
- 关联：[SPEC-0011](../spec/0011-native-application-packages.md)、[SPEC-0007](../spec/0007-extension-applications.md)、[ADR-0011](0011-extension-runtime-and-packages.md)、[ADR-0012](0012-extension-security-and-communication.md)、[ADR-0015](0015-process-supervisor-architecture.md)、[ADR-0016](0016-process-supervisor-security.md)、[ADR-0017](0017-root-operations-profile.md)

## 背景

EdgeDesk 已有静态 Web 应用包、沙箱 iframe、JSON-RPC/MessagePort、SQLite 应用管理与独立 Process Supervisor。用户希望扩展应用可携带本地原生后端，但保持单二进制、低依赖、跨架构与旧 Web 包兼容，且 V1 不过度设计。

## 决策

1. **扩展现有 ZIP 与 manifest**：backend 为可选字段；没有该字段的 Web-only 包继续使用 SPEC-0007 的既有行为。
2. **一个应用最多一个后端**：每个后端包只包含一个目标架构的 ELF；ARM32、ARM64、x86_64 分别打包。
3. **不执行脚本**：可执行路径与 argv 数组传给 Process Supervisor，不经 Shell。运行身份由管理员配置。
4. **复用 Process Supervisor**：服务配置由应用管理器生成；安装不启动；默认不开机自启；root 可由管理员指定，但继续遵循 Supervisor 现有显式 allow_root 约束。
5. **HTTP over UDS**：宿主生成 Socket 路径，以 EDGEDESK_APP_SOCKET 传给后端；持久化数据目录通过 EDGEDESK_APP_DATA_DIR 传入。前端只调用宿主 backend.request，由宿主连接当前应用自己的 Socket。
6. **默认应用内访问**：前端默认只能访问本应用后端；V1 不实现跨应用调用、任意 URL 代理、TCP 后端监听或 WebSocket 代理。
7. **SHA-256，不强制签名**：复用现有逐文件完整性清单并提示未签名包的发布者身份未经验证。
8. **原生后端覆盖升级**：先完整校验包，再停止后端并覆盖稳定程序路径；不保留原生后端历史版本，不自动回滚。升级失败保持停止并报告。Web 静态资源继续使用 SPEC-0007 的不可变 revision URL 与缓存机制。
9. **程序与数据分离**：升级覆盖程序不删除数据；卸载默认保留用户数据和平台 KV，彻底清理是独立的管理员操作。
10. **不新增沙箱或健康检查**：复用 Linux 用户/组/文件权限与 Supervisor 状态；RUNNING 只表示进程存在，不代表应用 API 健康。
11. **最小兼容优先**：不为应用创建 systemd/OpenRC/BusyBox 服务单元，不新增多进程、应用间调用、秘密存储、公共应用市场或通用数据迁移框架。

## 理由

- 复用现有 Web 扩展平台和 Supervisor，避免两套安装器、权限系统与生命周期管理。
- UDS 不需要为每个后端开放 TCP 端口；宿主代理可依据当前运行实例固定路由到对应应用。
- 以管理员指定的 OS 身份运行，兼容不同设备与部署环境；不引入无法在所有嵌入式内核上保证可用的容器/namespace 依赖。
- 覆盖升级、默认保留数据和单后端约束控制 V1 实现量。
- 旧 Web 包保持原有运行时、静态资源版本与缓存语义。

## 后果与风险

- SHA-256 不能认证发布者。未签名包只能作为管理员明确接受来源风险的本地软件包。
- 允许 root 后端意味着主机级权限；由于 V1 不增加进程沙箱，宿主无法阻止其访问主机资源或自行监听 TCP。
- 原生后端不自动回滚。覆盖或元数据提交失败时必须保持停止并明确标记失败，管理员通过重新安装/上传包恢复。
- UDS 文件与父目录权限必须允许后端绑定且只允许 EdgeDesk 宿主连接；权限配置不安全时不得启动。
- 不提供健康检查协议，Supervisor 的 RUNNING 状态不能作为后端就绪的保证；代理请求失败需返回明确错误。
- 应用级后端能力不得绕过现有 iframe/MessagePort、宿主 API 权限和路径沙箱。
- SPEC-0010 的 P0/P1 进程身份、后代清理、目录权限、持久化和恢复缺陷是原生后端生产使用的阻塞依赖。

## 被否决的方案

- **独立的新应用包格式**：重复已有 ZIP/manifest/安装流程，且会破坏统一开发体验。
- **多后端进程**：V1 复杂度过高，先限制一个后端。
- **强制数字签名**：需要额外密钥管理与发布基础设施；V1 先沿用 SHA-256 完整性校验并明确来源风险。
- **直接由 iframe 访问后端端口**：增加 CORS、端口暴露和访问控制复杂度。
- **每个应用生成系统 init unit**：重复 Process Supervisor 已承担的跨 init 生命周期能力。
- **版本目录切换与自动回滚**：原生后端 V1 采用稳定路径覆盖；不为此新增历史二进制管理系统。