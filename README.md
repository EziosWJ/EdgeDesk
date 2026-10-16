# EdgeDesk

面向嵌入式与边缘 Linux 设备的轻量 Web 运维工作台：单二进制部署，浏览器即可管理
设备文件、使用真实的 Bash 登录终端并查看设备实时资源占用。设备端不需要安装
Node/Python/Java 运行时，也不使用数据库。

> 当前仓库已实现 **V1.0 范围**：文件管理、Bash PTY 终端（多 Tab / 刷新重连）、
> 设备仪表盘与实时曲线、浮动窗口工作区、访问密钥认证、限流与审计日志。
> 逐项验收结果见 [docs/testing.md](docs/testing.md)。

## 功能一览

| 模块 | 能力 |
| --- | --- |
| 文件管理 | 目录浏览（权限/uid/gid/大小/时间/隐藏文件/排序/分页）、流式上传（临时文件+原子替换）、下载（Range 断点续传）、在线文本编辑（原子保存、保留权限）、新建/重命名/批量删除、路径沙箱 |
| Web 终端 | `/bin/bash -l` 真实登录 Shell、多 Tab、多独立会话、刷新/导航/断线重连不中断、CommandGuard 危险命令提示 |
| 浮动工作区 | 终端 / 编辑器 / 预览均可作为可拖动、可缩放、可最大化/最小化的浮动窗口打开 |
| 仪表盘 | CPU（使用率、load、核心数）、内存、磁盘、网络速率、运行时间 + 实时曲线（WebSocket 增量推送） |
| 安全 | 统一认证（Bearer / 会话 Cookie）、CSRF/CSWSH 校验、每 IP 限流、请求体积与并发限制、JSON Lines 审计日志 |
| 资源模式 | GCC 15 `#embed` 单二进制 / 磁盘模式，共用同一套静态资源抽象 |

## 快速开始

```sh
git clone https://github.com/EziosWJ/EdgeDesk.git
cd EdgeDesk

# 1) 前端（Node 只用于构建，不进入设备）
cd web && npm install && npm run build && cd ..

# 2) 后端：开发用磁盘资源模式
cmake --preset dev-disk
cmake --build --preset dev-disk
ctest --preset dev-disk

# 3) 运行（首次会自动生成一次性访问密钥并打印）
./out/build/dev-disk/bin/edgedesk --port 8080 --file-root /srv
# 浏览器打开 http://127.0.0.1:8080，输入启动日志中的密钥
```

正式发布使用 GCC 15+ 的 `#embed` 单二进制（运行期不需要 `web/`）：

```sh
cmake --preset release-embed     # 需要 GCC 15+；见 scripts/with-gcc15.sh
cmake --build --preset release-embed
./out/build/release-embed/bin/edgedesk --port 8080 --file-root /srv
```

完整集成测试（HTTP + WebSocket + 限流）：

```sh
bash scripts/integration_test.sh out/build/dev-disk/bin/edgedesk 19091 web/dist
```

### 认证与安全默认值

- 认证始终开启；默认只监听 `127.0.0.1`。非回环地址必须显式提供访问密钥
  （`--auth-token-file` / `EDGEDESK_AUTH_TOKEN` / `--auth-token`）。
- 未配置密钥时服务生成一次性密钥并只打印一次；重启即使其失效。
- Web 会话 Cookie 为 `HttpOnly; SameSite=Strict`，签发基于进程内随机密钥。
- TLS 不在 V1.0 二进制内终止；远程访问请置于 TLS/VPN 网关之后。

详见 [配置与部署](docs/config.md)。

## 构建与验证

| 命令 | 用途 |
| --- | --- |
| `cmake --preset dev-disk` | Debug，磁盘资源模式（任意 C++17 编译器） |
| `cmake --preset release-disk` | Release，磁盘资源模式 |
| `cmake --preset release-embed` | Release，GCC 15+ `#embed` 单二进制 |
| `cmake --preset host-offline-tests` | 仅核心库与单元测试（不需要 libhv / npm） |
| `scripts/with-gcc15.sh all release-embed` | 使用 `~/opt/gcc-15` 的本地 GCC 15 一键构建 |
| `scripts/integration_test.sh` | 端到端：REST + WebSocket + 限流 + 审计 |
| `cd web && npm test` | 前端单元测试（浮窗存储 / 会话持久化 / 格式化） |

单元测试覆盖：配置解析、SHA-256/HMAC 与会话签名、限流、审计轮转、
`PathSandbox`（穿越、符号链接逃逸、前缀欺骗、循环）、`FileService`
（列表/排序/分页、原子保存、保留权限、上传/中止、删除计数、Range 解析）、
`CommandGuard`、终端会话（真实 Bash、登录语义、resize、重放、上限、空闲回收）、
系统指标解析与采样。

## 技术基线

| 范围 | 决策 |
| --- | --- |
| 后端 | C++17 + libhv v1.3.4（HTTP/WebSocket） |
| 编译器 | GCC 15+（正式构建）；开发可用较旧 GCC 走磁盘模式 |
| 资源打包 | GCC `#embed`（前端产物编入 ELF） |
| 前端 | React 19 + TypeScript + Vite + xterm.js |
| 目标平台 | x86_64 / ARM32 / ARM64（分别交叉构建；见 docs/testing.md 的真实验证状态） |
| 数据 | 无数据库、无 ORM |

## 文档

- [架构与线程模型](docs/architecture.md)
- [API 与 WebSocket 协议](docs/api.md)
- [配置与部署](docs/config.md)
- [测试与验证矩阵](docs/testing.md)
- [文档索引 / ADR / Spec](docs/README.md)
- [Coding Agent 强制约束](AGENTS.md)
- [GitHub Actions](https://github.com/EziosWJ/EdgeDesk/actions)

## 前端设计资源

- [素材库（Logo、图标、空状态插画）](web/public/assets/)
- [素材预览画廊](docs/design/asset-gallery.html)
- [前端素材使用说明](docs/design/asset-usage.md)
- [原型视觉与组件实现规范](docs/design/prototype-guidelines.md)
- [设计 Token](web/src/styles/edgedesk-tokens.css)
