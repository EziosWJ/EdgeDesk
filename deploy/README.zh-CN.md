# EdgeDesk 部署适配器与安装器

> 英文版：[README.md](README.md)

`deploy/install.sh` 是在 Linux 主机上安装 EdgeDesk 的受支持方式。它是一个
POSIX `sh` 脚本（可在 dash、bash 和 BusyBox ash 下运行），只做四类事情：

1. 检测可用的服务管理器；
2. 收集并校验监听地址与路径设置；
3. 安装二进制、前端（仅磁盘构建）和配置文件；
4. 将 `edgedesk` 与 `edgedesk-supervisor` 注册到该 init 系统。

它从不修改业务数据。管理员密码仍需在首次登录时通过 Web 界面设置；安装器
不会生成或记录任何凭据。

## 快速开始

```sh
# 交互模式：询问 host、port、file root 和 prefix（仅 TTY）
sudo deploy/install.sh

# 非交互模式
sudo deploy/install.sh --host 127.0.0.1 --port 8080 \
  --source /opt/edgedesk-dist --file-root /srv/edgedesk-files

# 完整服务器运维（root 后端）；见下方“Root 运维模式”
sudo deploy/install.sh --ops --host 127.0.0.1 --port 8080

# 只检查环境，不做任何修改
deploy/install.sh --detect

# 打印完整计划和将生成的文件，不写入任何内容
deploy/install.sh --dry-run --source out/build/release-embed/bin

# 暂存一棵安装包目录树，不接触正在运行的系统
deploy/install.sh --destdir /tmp/edgedesk-stage \
  --source out/build/release-embed/bin --file-root /srv
```

`--source` 指向包含 `edgedesk` 和 `edgedesk-supervisor` 的目录。省略时，脚本
依次查找：脚本自身所在目录、其 `bin/` 子目录、仓库的构建目录。因此发布包可以
直接以 `install.sh` + `bin/` 的形式上传，无需 `--source` 即可安装；安装目标处
已存在的二进制会被原地采用，而不是要求 `--force`。`--web-root` 仅磁盘构建需要；
`release-embed` 已内嵌前端。

## 环境检测

`--detect` 输出机器可读的多行结果后退出：

```text
init=systemd
pid1=systemd
uid=0
systemd=yes
openrc=no
busybox=no
inittab=no
```

检测的是服务管理器，而不只是 PID 1——因为 Alpine 可能在 BusyBox `init` 之上
运行 OpenRC。顺序为：systemd（`systemctl` + `/run/systemd/system`）、
OpenRC（`rc-update` + `rc-service`）、BusyBox（`busybox` + `/etc/inittab`）。
在容器或非标准镜像中，可用 `--init systemd|openrc|busybox|none` 覆盖。

## 安装内容

| 路径 | 内容 |
| --- | --- |
| `/opt/edgedesk/bin/edgedesk` | Web 后端 |
| `/opt/edgedesk/bin/edgedesk-supervisor` | 进程 Supervisor（除非指定 `--no-supervisor`） |
| `/opt/edgedesk/web/` | 前端，仅在传入 `--web-root` 时安装 |
| `/etc/edgedesk/edgedesk.conf` | 生成的 `key = value` 配置，权限 `0640 root:<service-group>` |
| `/var/lib/edgedesk` | 持久化数据，权限 `0700 <service-user>` |
| `/var/lib/edgedesk/apps` | 扩展平台根目录，权限 `0700 <service-user>` |
| `/run/edgedesk` | 运行时状态；由 systemd 的 `RuntimeDirectory` 或启动包装脚本重建 |
| `/var/lib/edgedesk-supervisor` | Supervisor 的 SQLite 与日志，权限 `0700 root` |

Init 适配器：

- **systemd**：在 `/etc/systemd/system` 下写入 `edgedesk.service` 和
  `edgedesk-supervisor.service`，两者均启用并启动。默认 Web unit 使用
  `ProtectSystem=strict`、`NoNewPrivileges=yes`、`StateDirectory` 和
  `RuntimeDirectory`。`--ops` 下改为以 `root` 运行，设置 `UMask=0022`、
  `Environment=HOME=/root`，不再包含 `ProtectSystem`/`ProtectHome`/
  `PrivateTmp`/`ReadWritePaths`；Supervisor 增加 `--allow-uid 0` 并移除
  挂载加固。Supervisor 始终保留 `KillMode=process` 和 `NoNewPrivileges=no`，
  因为它必须切换服务身份。
- **OpenRC**：`/etc/init.d/edgedesk` 和 `/etc/init.d/edgedesk-supervisor`，
  两者均使用 `supervise-daemon`（OpenRC >= 0.21），并加入 `default` 运行级。
- **BusyBox init**：`edgedesk-boot` 与 `edgedesk-supervisor-boot` 包装脚本，
  外加 `/etc/inittab` 中一个带标记的块；通过 `SIGHUP` 重载 init。Web 包装脚本
  先用 `setpriv`、再用 `su` 降权到服务账号，仅当两者都不存在时才回退为以 root
  运行（并在 stderr 给出警告）。

生成的配置复用现有的 `--config` 文件支持：

```ini
host = 127.0.0.1
port = 8080
file-root = /srv/edgedesk-files
data-dir = /var/lib/edgedesk
state-dir = /run/edgedesk
apps-root = /var/lib/edgedesk/apps
audit-log = /var/lib/edgedesk/audit.log
supervisor-socket = /run/edgedesk-supervisor.sock
```

## Root 运维模式（`--ops`）

默认安装是刻意受限的管理面板：后端、终端和文件 API 以 `edgedesk` 服务账号
运行，systemd 把数据目录以外的文件系统挂为只读。该姿态无法编辑 `/etc`、
执行 `apt`、管理 `systemctl` 或维护 `/opt`。

`--ops` 是完整运维控制台的显式选择：

```sh
sudo deploy/install.sh --ops --host 127.0.0.1 --port 8080
```

它改变的是整个权限模型，而不只是一个命令行参数：

- `User=root`、`Group=root`、`UMask=0022`、`Environment=HOME=/root`；
- Web unit 不再包含 `ProtectSystem`、`ProtectHome`、`PrivateTmp` 与
  `ReadWritePaths`，因此终端和文件 API 可以写入主机文件系统；
- `--file-root` 默认 `/`（可用 `--file-root DIR` 收窄）；
- Supervisor 增加 `--allow-uid 0`（后端的 `SO_PEERCRED` 为 uid 0），并移除
  `ProtectSystem`/`ProtectHome`/`PrivateTmp`；托管服务继承该命名空间，因此
  可以访问 `/etc`、`/home` 与主机 `/tmp`；
- 认证、CSRF、限流与审计不变：该模式改变的是进程身份和 systemd 命名空间，
  不是 API。

此时面板等价于主机 root，应按管理平面对待：只监听回环，或在前置 TLS/VPN 与
可信网络边界之后。非回环 `--host` 仍需 `--allow-non-loopback`。不要多人共用
一个管理员账号。

不带 `--ops` 的 `--service-user root` 会被拒绝，`--ops` 搭配其他
`--service-user` 也会被拒绝，因此不会误选该模式。

### 迁移已有安装

应用会拒绝属于其他 UID 的数据目录。服务身份变化时，安装器会在安装文件前对
`--data-dir` 和 `--apps-root` 递归应用新的属主。只接受专用目录；`/`、
`/var/lib`、`/etc` 等过宽路径会被拒绝。因此手工迁移通常只需要：

```sh
sudo deploy/install.sh --ops --force --source /opt/edgedesk/bin
sudo systemctl restart edgedesk-supervisor edgedesk
```

`--force` 会保留 unit 与配置文件的 `.bak` 副本。

## 安装器强制执行的安全规则

- **默认仅回环地址。** 除非传入 `--allow-non-loopback`（或在交互模式下确认），
  否则拒绝非回环的 `--host`。该警告要求前置 TLS 或可信 VPN/网关，并为 HTTPS
  设置 `--cookie-secure`。
- 安装器不写入任何凭据、密码或令牌。
- 会进入 unit 文件的值先经过校验：端口为数字、host 限定字符集、路径为不含
  `..` 的绝对路径、不含空白字符、引号或 `#`。
- 未指定 `--force` 时不覆盖已有文件；覆盖时保留 `.bak` 副本。
- `--dry-run` 和 `--destdir` 不需要 root，也从不修改正在运行的系统。

## 卸载

```sh
sudo deploy/install.sh --uninstall              # 保留 /var/lib/edgedesk*
sudo deploy/install.sh --uninstall --purge      # 同时删除数据和 Supervisor 状态
```

卸载会停止并注销检测到的 init 系统、删除二进制、init 文件和 `/etc/edgedesk`，
并保留持久化数据，除非指定 `--purge`。服务账号不会被自动删除。`--purge` 会
拒绝 `/`、`/usr`、`/opt`、`/var/lib` 这类过于宽泛的 prefix。

## 手动安装

生成的文件有意与仓库中签入的参考 unit 保持接近。当主机布局不寻常时，仍可手动
安装。

先安装二进制，例如放到 `/opt/edgedesk/bin`。持久化状态目录
`/var/lib/edgedesk-supervisor` 必须位于可写的持久化文件系统上；在只读 rootfs
上，请把 `--database` 和 `--log-dir` 移到可写的数据分区，并保持相同权限
（目录 0700，文件 0600）。

### systemd

```sh
install -m 0644 deploy/systemd/edgedesk-supervisor.service \
  /etc/systemd/system/edgedesk-supervisor.service
systemctl daemon-reload
systemctl enable --now edgedesk-supervisor
systemctl status edgedesk-supervisor
```

`KillMode=process` 是有意为之：`systemctl stop` 向 Supervisor 发送 SIGTERM，
Supervisor 将其转发给每个受管进程组，等待各服务停止超时，升级为 SIGKILL，
之后才退出。`Restart=always` 会在 Supervisor 崩溃后重启它。受管进程在这段
空档内继续运行，如果身份可验证，会在重启时被重新接管。

让 EdgeDesk Web 服务指向同一个 socket：
`--supervisor-socket /run/edgedesk-supervisor.sock`（默认值）。如果 Web 后端
先于 Supervisor 启动，进程管理类 API 会返回 `503 supervisor_unavailable`；
EdgeDesk 其余部分仍可正常使用。

### OpenRC

```sh
install -m 0755 deploy/openrc/edgedesk-supervisor \
  /etc/init.d/edgedesk-supervisor
rc-update add edgedesk-supervisor default
rc-service edgedesk-supervisor start
rc-service edgedesk-supervisor status
```

该脚本要求带 `supervise-daemon` 的 OpenRC（OpenRC >= 0.21），由它提供前台
进程监控与重启。如果二进制、socket、数据库或日志路径不同，请在
`/etc/conf.d/edgedesk-supervisor` 中覆盖 `EDGEDESK_SUPERVISOR_*` 变量。

### BusyBox init

```sh
install -m 0755 deploy/busybox/edgedesk-supervisor-boot \
  /usr/sbin/edgedesk-supervisor-boot
cat deploy/busybox/inittab >> /etc/inittab
kill -HUP 1
```

`::respawn:` 让 BusyBox 在 Supervisor 崩溃后重新启动它。包装脚本在 exec 守护
进程之前，以 0700 权限创建 `/var/lib/edgedesk-supervisor` 及其日志目录。在没有
控制台的设备上，请把包装脚本的 stdout/stderr 重定向到可用的日志系统。

## 验证

只有在某个平台实际执行过的文件和命令才算验证记录；安装了脚本并不等于验收。
`docs/testing.md` 记录了实际运行过的命令和仍然存在的缺口。`tests/install/install_test.sh`
中的宿主机测试会暂存全部三种适配器、对生成的脚本做语法检查并断言安全规则；
它们不能替代真实运行 init 系统。
