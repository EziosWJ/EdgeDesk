# ADR-0017：Root 运维模式（`--ops`）

- 状态：**Accepted（安装器与暂存测试通过；真机做过手工迁移验证，尚未用 `--ops` 重装）**
- 日期：2026-10-23
- 关联：[ADR-0002：架构与安全](0002-architecture-security.md)、
  [ADR-0015：Process Supervisor 架构](0015-process-supervisor-architecture.md)、
  [ADR-0016：Process Supervisor 安全](0016-process-supervisor-security.md)

## 背景

EdgeDesk 的默认部署以专用服务账号运行 Web 后端，并由 systemd 把文件系统
挂成只读，只放开数据目录。这个姿态适合受限管理面板，但不能完成服务器运维：
文件 API 不能写 `/etc` 或 `/opt`，终端不能 `apt`/`systemctl`，`sudo` 也会被
`NoNewPrivileges=yes` 阻止。

实际操作中，用户会手工把 `User=edgedesk` 改成 `root`，但只改这一项仍然失败：
`ProtectSystem=strict` 对 root 的文件写入同样生效；数据目录属主检查会因为
文件仍属于 `edgedesk` 而拒绝启动；Supervisor 的 `SO_PEERCRED` 允许列表也
不匹配 uid 0。需要把这套切换固化为显式、可测试的部署模式。

## 决策

1. `deploy/install.sh` 新增显式 `--ops` 模式。
2. `--service-user root` 单独使用被拒绝；`--ops` 搭配非 root
   `--service-user` 也被拒绝。root 只能通过 `--ops` 选择，避免隐式提权。
3. `--ops` 生成的 systemd Web unit：
   - `User=root`、`Group=root`、`UMask=0022`、`Environment=HOME=/root`；
   - 保留 `NoNewPrivileges=yes`、`StateDirectory`、`RuntimeDirectory`；
   - 移除 `ProtectSystem`、`ProtectHome`、`PrivateTmp`、`ReadWritePaths`。
4. `--ops` 生成的 Supervisor unit：
   - 增加 `--allow-uid 0`，因为 Web 后端的 `SO_PEERCRED` 是 uid 0；
   - 移除 `PrivateTmp`、`ProtectHome`、`ProtectSystem`、`ReadWritePaths`，
     使托管服务继承一个可访问主机的命名空间；
   - 保留 `KillMode=process`、`NoNewPrivileges=no`、`UMask=0077`。
5. `--file-root` 在 `--ops` 下默认 `/`；显式 `--file-root DIR` 可收窄。
6. 服务身份变化时，安装器在创建/安装文件前对专用 `--data-dir` 和
   `--apps-root` 递归 `chown`；拒绝 `/`、`/var/lib`、`/etc` 等过宽路径。
7. 监听默认值不变：仍为回环；非回环仍要求显式 `--allow-non-loopback`。
8. 安装器在 `--ops` 下输出明确的安全警告。

## 理由

- 显式模式让安装器能一次性生成一致的 unit、配置、Supervisor 允许列表和
  目录属主，避免用户逐项试错。
- `StateDirectory`/`RuntimeDirectory` 的名称改为从路径推导，而不是用户名，
  这样 root 模式下 `/var/lib/edgedesk` 与 `/run/edgedesk` 仍由 systemd 创建
  并设为 `root:root 0700`。
- 递归 `chown` 只作用于平台拥有的专用目录，解决从默认模式迁移到 `--ops`
  （或反向迁移）时 `open_private_dir` 的属主检查失败。

## 后果

正面：

- 一条命令得到可用的 root 运维面板，文件 API、终端和 Supervisor 行为一致；
- 模式切换不再需要手工 `chown`、改 unit、补 `--allow-uid`；
- 默认部署的加固不受影响；不传 `--ops` 时行为与之前一致。

负面：

- 任何能认证到面板的人即获得主机 root；
- 终端命令没有命令级审计，文件操作审计也可被 root 终端修改；
- 扩展宿主桥接以 root 执行，SPEC-0008 的权限模型仍然是应用层约束；
- `UMask=0022` 让新建的审计日志为 `0644`；默认 `--state-dir`（`/run/edgedesk`，
  `0700`）仍能挡住其他用户，但把 `--audit-log` 指到 `0755` 目录时需要自行收紧；
- `file-root=/` 时主文件沙箱没有接入 `platform_protected_paths`
  （`src/app/application.cpp` 只给扩展沙箱传了该选项），误操作可能修改
  `auth.json`、`edgedesk.conf`、审计日志或 Supervisor 数据库；
- L0/L1/CI 仍以非 root 运行，root 模式只有安装器暂存测试与手工真机验证，
  不能宣称自动化覆盖。

## 缓解措施

- 只监听回环，或放在 TLS/VPN/可信网关之后；非回环必须显式确认。
- 保持现有认证、CSRF、限流与审计；root 模式下不要共享管理员账号。
- 可用 `--file-root /srv/ops` 收窄文件 API，把全盘操作留给终端。
- 后续可以给主文件沙箱接入 `platform_protected_paths`，保护认证、配置、
  审计和 Supervisor 状态；该改动需要单独的测试与文档。

## 被否决的方案

- **让 `--service-user root` 隐式切换**：容易在操作者不知情时移除加固，
  也无法给出明确的安全警告。
- **给服务账号配置 sudoers**：`NoNewPrivileges=yes` 会阻止 setuid；关闭它
  再授权等价于 root，且比显式 `--ops` 更难审计。
- **只依赖 Supervisor**：Supervisor 只接受可执行文件与 argv，不能覆盖
  文件编辑、交互式终端和任意系统运维。
