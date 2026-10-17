# EdgeDesk 测试与验证矩阵

> 只记录**实际执行过**的验证。未运行的环境一律标注“未验证”，不得以文档目标代替结果。
> 复现命令附在每节末尾。

## 自动化测试清单

| 测试 | 覆盖内容 | 环境 | 结果 |
| --- | --- | --- | --- |
| `core_test` | CLI/配置文件/环境变量解析、非法参数、SHA-256/HMAC 向量、会话签名与过期、限流桶、审计轮转 | GCC 13.3 host | 通过 |
| `path_sandbox_test` | `..` 穿越、URL 编码、绝对路径、前缀欺骗目录、符号链接逃逸/循环（含 follow 模式）、lstat 语义 | GCC 13.3 host | 通过 |
| `file_service_test` | 列表/排序/分页/隐藏文件、`dirs_only` 直接子目录（符号链接不跟随）、文本读写与原子保存保留权限、二进制拒绝、上传/中止/超限、递归删除计数、Range 解析、MIME 判定 | GCC 13.3 host | 通过 |
| `command_guard_test` | 危险命令命中（rm -rf 根、块设备写入、fork 炸弹、关机…）与安全命令不误报 | GCC 13.3 host | 通过 |
| `terminal_session_test` | 真实 Bash 登录 Shell、`~/.profile` 加载、UTF-8/ANSI、TIOCSWINSZ resize、CommandGuard 拦截不执行、回放缓冲有界、订阅断开不杀会话、会话上限/空闲回收、缺 Bash 报错而非回退 sh | GCC 13.3 host + /bin/bash 5.2 | 通过 |
| `system_metrics_test` | /proc/stat、loadavg、meminfo（含旧内核回退）、net/dev 速率差分、mounts 过滤、采样线程与环形缓冲 | GCC 13.3 host | 通过 |
| `embed_generator_test` | `#embed` 生成器的确定性、路径校验、ETag | Python 3.12 | 通过 |
| 前端单元测试 | windowStore（开关/层级/拖动/缩放/最大化/关闭、不可变快照）、会话持久化（仅 id/title）、metrics 窗口有界、字节/百分比/运行时间格式化、FloatingWindow DOM 交互（指针拖动、缩放把手、按钮）、Sidebar（导航选中、收起/展开与 localStorage 持久化恢复、服务状态）、仪表盘图表工具（窗口切片、变化值、速率单位、时间刻度、开机时刻）、DashboardView DOM 渲染（KPI 六卡、三图表面板、磁盘环形图与挂载点切换、在线/数据中断状态、空数据不造假）、文件管理（路径助手/视图偏好、目录树按需加载与缓存/祖先展开/失败重试/刷新/不跟随链接、工作区单双击语义、树面板收起展开与宽度调整持久化、列表↔网格共享选择、本地筛选、隐藏文件联动、分页排序、失败态与重试、批量删除确认与失败不报成功、浮动终端/编辑器窗口） | Node 24.14 / vitest 3.2 / happy-dom | 通过（62 项） |
| `api_test.py` | health 契约、认证/会话/Bearer、CSRF、目录浏览（含 `dirs_only` 只返回直接子目录）、文本读写、mkdir/rename/delete、流式上传字节一致、Range 206/416、路径沙箱拒绝、JSON 体积限制、系统指标 | 见下（三种二进制） | 通过 |
| `ws_test.py` | 未认证/异源握手拒绝、终端 create/input/输出/resize/guard、断线重连（回放）、会话存活、显式关闭、metrics 快照+增量 | 同上 | 通过 |
| `rate_limit_test.py` | 登录限流与通用限流在严格配置下触发 429 | 同上 | 通过 |
| 审计校验 | `login`、`file.upload`、`terminal.create` 事件落盘 | 同上 | 通过 |

### 原型还原度（视觉）验收

- 逐项对照表：`docs/design/prototype-conformance.md`（每个原型页一份）。
- 规则：业务功能通过 ≠ 页面验收通过；表中“待目视核对”的条目必须由人工在浏览器中对照原型图确认。
- 当前状态：**未执行**（开发环境无浏览器）。代码级结构与样式已按对照表落地；仪表盘
  数据全部来自 `/api/system/summary` 与 `/ws/metrics` 的真实采样，文件管理页的目录树、
  列表/网格数据全部来自 `/api/files`（目录树使用 `dirs_only=1` 按需加载），不使用原型演示数值。
- 核对步骤见该文档“人工核对步骤”一节；核对结论必须回填表格，不能口头声明通过。

### 端到端套件（`scripts/integration_test.sh`）

| 二进制 | 模式 | 结果 |
| --- | --- | --- |
| `dev-disk`（GCC 13.3，Debug） | 磁盘资源 + React dist | 全部通过 |
| `release-embed`（GCC 15.2.0） | `#embed` 单二进制（27 个 `#embed` 指令） | 全部通过（含前端外壳与哈希资源校验） |
| `x86_64-musl`（musl cross GCC 11.2，`-static-pie`） | 磁盘资源，无动态依赖 | 全部通过 |
| `arm64-disk`（gcc-aarch64-linux-gnu 13，qemu-user 8.2.2 运行） | 磁盘资源，AArch64 原生 Bash PTY | 全部通过 |
| `arm64-musl`（musl cross GCC 11.2，`-static-pie`，qemu 运行） | 磁盘资源，AArch64 Bash PTY | 全部通过 |

```bash
bash scripts/integration_test.sh out/build/dev-disk/bin/edgedesk 19091 web/dist
bash scripts/integration_test.sh out/build/release-embed/bin/edgedesk 19391 web/dist
bash scripts/integration_test.sh out/build/arm64-disk/bin/edgedesk 19591 "" \
  "$HOME/opt/arm64/root/usr/bin/qemu-aarch64-static -L $HOME/opt/arm64/root"
```

## 构建与平台矩阵

| 目标 | 工具链 | 结果 |
| --- | --- | --- |
| x86_64 Linux，磁盘模式（Debug/Release） | GCC 13.3（Ubuntu 24.04 系统编译器） | 通过（`dev-disk`） |
| x86_64 Linux，`#embed` 单二进制 | GCC 15.2.0（用户目录安装，见 docs/dev/gcc15-local-toolchain.md） | 通过（`release-embed`，27 个 `#embed` 指令） |
| x86_64 musl **static-pie** | musl.cc `x86_64-linux-musl-cross`（GCC 11.2.1）**与** Bootlin `x86-64--musl--stable-2024.05-1`（GCC 13.3），磁盘模式 | 通过（两套工具链各自：7 个单元测试可执行文件 + 全量集成测试） |
| AArch64（ARM64）Linux，磁盘模式 | Ubuntu cross `gcc-aarch64-linux-gnu` 13 + qemu-user-static 8.2.2 | 通过（编译 + 运行 + 全量集成测试，含 ARM64 Bash PTY） |
| AArch64 musl **static-pie** | musl.cc `aarch64-linux-musl-cross`（GCC 11.2.1），磁盘模式 | 通过（qemu 下全量集成测试） |
| ARM32（armv7 musl） | 需要 `arm-linux-musleabihf` 工具链 | **未验证**（本机未安装该工具链） |
| GCC 15 + musl + `#embed` 组合 | 需要 GCC 15 的 musl 交叉工具链 | **未验证**（预编译 musl 工具链当前最高 GCC 13.3；`#embed` 管线已在 glibc GCC 15.2 上验证，musl 静态链路已在 GCC 11.2 / 13.3 上验证，两者交集未验证） |
| OpenHarmony ARM64 | 需要对应 SDK | **未验证** |
| 真实设备（工控板/网关） | 目标硬件 | **未验证**（当前仅有 QEMU 运行证据） |
| GitHub Actions CI | 本仓库 workflow：host-build / gcc15-embed / arm64-cross / x86_64-musl-static 四个 job | **已执行并通过**（2026-10-16，main 分支；见仓库 Actions） |

### libhv 依赖的已知构建适配

libhv v1.3.4 的部分头文件依赖“隐式包含”（`struct stat`、fcntl 标志、pthread 常量、
`environ`）。glibc 工具链隐式提供这些声明；部分 musl 工具链（如 Bootlin GCC 13）
不提供，且 musl 仅在 `_GNU_SOURCE` 下声明 `environ`。因此 `cmake/Libhv.cmake` 在
**非 glibc** 目标上为 `hv_static` 追加 `-D_GNU_SOURCE` 与若干个 `-include` 标志
（不改动固定的上游源码）；glibc 构建保持原有编译参数不变。

### 已知缺口与诚实声明

- **musl/static-pie 的最终发行验证**需要匹配的 GCC 15 musl 工具链；本机与 CI 用
  musl.cc（GCC 11.2）和 Bootlin（GCC 13.3）验证，预编译工具链当前最高 GCC 13，
  因此 musl 目标目前只能构建磁盘模式。
- **ARM32 与真实设备**没有运行证据；`cmake/toolchains/` 提供模板，
  不是“已成功”的声明。
- 浏览器（Chrome/Firefox）手工交互未在本机执行；前端行为通过单元测试
  （含 happy-dom DOM 交互测试）与 WebSocket/REST 集成测试间接覆盖，
  浮动窗口的拖拽/缩放由 DOM 测试断言状态变化。
- 原型还原度的**目视核对未执行**（本机无浏览器）；对照表见
  `docs/design/prototype-conformance.md`，未完成的条目不视为通过。
- 性能与资源占用没有做基准测试，不提供数字。

## ARM64 QEMU 运行环境（复现说明）

```bash
# 1) 无 root 获取交叉工具链与 qemu-user
mkdir -p ~/opt/arm64/debs && cd ~/opt/arm64/debs
apt-get download $(apt-cache depends --recurse --no-recommends --no-suggests \
  --no-conflicts --no-breaks --no-replaces --no-enhances g++-aarch64-linux-gnu qemu-user-static \
  | grep '^\w' | sort -u)
for d in *.deb; do dpkg -x "$d" ~/opt/arm64/root; done

# 2) host 侧交叉 binutils 需要的库（避免污染系统 LD_LIBRARY_PATH）
mkdir -p ~/opt/arm64/hostlibs && cd ~/opt/arm64/hostlibs
ln -sf ~/opt/arm64/root/usr/lib/x86_64-linux-gnu/lib{opcodes-2.42-arm64,bfd-2.42-arm64,ctf-arm64,ctf-nobfd-arm64}.so .
ln -sf libctf-arm64.so.0.0.0 libctf-arm64.so.0
ln -sf libctf-nobfd-arm64.so.0.0.0 libctf-nobfd-arm64.so.0

# 3) 运行期布局（glibc 三连目录 + usrmerge + ARM64 bash 便于 PTY 测试）
cd ~/opt/arm64/root
mkdir -p lib usr/lib
ln -sfn ../usr/aarch64-linux-gnu/lib lib/aarch64-linux-gnu
ln -sfn ../aarch64-linux-gnu/lib usr/lib/aarch64-linux-gnu
ln -sf ../usr/aarch64-linux-gnu/lib/ld-linux-aarch64.so.1 lib/ld-linux-aarch64.so.1
ln -sfn usr/bin bin
# ARM64 bash（选择测试用 shell）
apt-get download -o Dir::Etc::sourcelist=<arm64 sources.list> ... bash:arm64  # 见下方脚本说明
dpkg -x bash_*_arm64.deb ~/opt/arm64/root && dpkg -x libtinfo6_*_arm64.deb ~/opt/arm64/root

# 4) 交叉构建（磁盘模式；#embed 需要 GCC 15+ 交叉工具链）
LD_LIBRARY_PATH=~/opt/arm64/hostlibs cmake -S . -B out/build/arm64-disk -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DEDGEDESK_WEB_MODE=disk -DEDGEDESK_BUILD_WEB=OFF \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/aarch64-linux-gnu.cmake \
  -DEDGEDESK_TOOLCHAIN_ROOT=$HOME/opt/arm64/root
LD_LIBRARY_PATH=~/opt/arm64/hostlibs cmake --build out/build/arm64-disk -j 6
file out/build/arm64-disk/bin/edgedesk   # ELF 64-bit ... ARM aarch64
```
