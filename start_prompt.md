# EdgeDesk V1.0 全量开发任务

你现在负责继续开发 GitHub 仓库 `EziosWJ/EdgeDesk`。

目标：在现有 Phase 1 工程骨架基础上，完成 EdgeDesk V1.0 的全部已确认功能，使其成为一个可实际部署到嵌入式 / 边缘 Linux 设备上的单二进制 Web 运维面板。

不要重新设计项目。必须先阅读现有 ADR、Spec、`AGENTS.md`、`docs/phase1.md` 以及当前代码，再按照已经确定的技术路线实现。

---

## 一、不可改变的技术决策

### 后端

- C++
- C++17 作为代码语言基线
- GCC 15+ 作为正式构建环境
- libhv 作为 HTTP / WebSocket 网络库
- Linux / POSIX
- PTY 使用 Linux `forkpty`
- 终端必须是真实 Shell，不允许使用简单 `sh -c` 模拟
- Bash 登录 Shell：
  - `/bin/bash -l`
  - 正确加载 `/etc/profile`
  - 用户 HOME、PATH、LANG 等环境变量正常
  - profile / alias / 用户环境能够生效

### 前端

- React
- TypeScript
- Vite
- xterm.js
- 使用现有设计素材和设计 Token
- 不引入 Node.js 作为目标设备运行时依赖
- 前端最终构建为静态资源

### 静态资源

正式 Embed 模式必须使用 GCC 15+ 的 C++ `#embed`。

禁止继续使用大量 `"\xNN\xNN..."` 字符串模拟资源。

允许 Python / CMake 生成资源索引、元数据，但实际资源数据必须通过 `#embed` 编译进最终二进制。

支持：

1. 磁盘资源模式
2. `#embed` 单二进制模式

两种模式必须共用统一的 `WebRoot` / 静态资源抽象。

### 数据库

**禁止引入数据库和 ORM。**

EdgeDesk 当前业务不需要 SQLite / PostgreSQL / MySQL / ORM。

运行状态全部使用：

- 内存
- 文件系统
- 配置文件

如果未来确实需要持久化，必须新增 ADR 后再实施。

---

# 二、最终目录架构

保持现有 Phase 1 结构的总体方向，不要为了个人偏好大规模重构。

目标职责结构如下：

```text
EdgeDesk/
├── CMakeLists.txt
├── CMakePresets.json
├── AGENTS.md
├── README.md
│
├── cmake/
│   └── ...
│
├── src/
│   ├── app/
│   │   ├── main.cpp
│   │   └── application.*
│   │
│   ├── core/
│   │   ├── config.*
│   │   ├── error.*
│   │   └── types.*
│   │
│   ├── http/
│   │   ├── server.*
│   │   ├── middleware.*
│   │   └── routes/
│   │       ├── health_route.*
│   │       ├── file_route.*
│   │       ├── terminal_route.*
│   │       └── system_route.*
│   │
│   ├── file/
│   │   ├── file_service.*
│   │   ├── path_sandbox.*
│   │   └── file_types.*
│   │
│   ├── terminal/
│   │   ├── terminal_session.*
│   │   ├── terminal_manager.*
│   │   ├── pty.*
│   │   └── command_guard.*
│   │
│   ├── system/
│   │   ├── system_metrics.*
│   │   ├── cpu_metrics.*
│   │   ├── memory_metrics.*
│   │   ├── disk_metrics.*
│   │   └── network_metrics.*
│   │
│   ├── security/
│   │   ├── auth.*
│   │   ├── rate_limiter.*
│   │   └── audit.*
│   │
│   └── web/
│       ├── web_root.*
│       └── embedded_assets.*
│
├── tests/
│   ├── core/
│   ├── file/
│   ├── terminal/
│   └── system/
│
├── tools/
│   └── ...
│
├── web/
│   ├── src/
│   │   ├── app/
│   │   ├── components/
│   │   ├── features/
│   │   │   ├── files/
│   │   │   ├── terminal/
│   │   │   ├── dashboard/
│   │   │   └── settings/
│   │   ├── services/
│   │   ├── hooks/
│   │   ├── stores/
│   │   └── styles/
│   ├── public/
│   │   └── assets/
│   └── package.json
│
├── docs/
│   ├── adr/
│   ├── spec/
│   ├── design/
│   └── phase1.md
│
└── .github/
    └── workflows/
```

核心原则：

```text
Route
  ↓
Service
  ↓
Platform / POSIX
```

HTTP Route 不允许直接堆积业务逻辑。

例如：

```text
file_route
    ↓
FileService
    ↓
filesystem
```

```text
terminal_route
    ↓
TerminalManager
    ↓
TerminalSession
    ↓
PTY / bash
```

---

# 三、文件管理

完成当前 Spec 中全部 V1.0 文件管理功能。

必须支持：

- 目录浏览
- 文件属性
- 权限
- uid / gid
- 文件大小
- 修改时间
- 隐藏文件
- 排序
- 分页
- 文件下载
- Range / 断点下载
- 流式上传
- 临时文件上传
- 上传成功后 atomic rename
- 新建目录
- 重命名
- 批量删除
- 文本预览
- 文本编辑
- 原子保存
- 保留原文件权限
- 路径沙箱
- 软链接逃逸防护

路径安全必须集中在 `PathSandbox`。

禁止在不同 Route 中重复实现：

```text
realpath
../
symlink
root boundary
```

所有文件操作必须经过统一安全层。

不能允许：

```text
root/../../etc/passwd
```

也不能通过软链接逃逸 root。

文件编辑必须：

- UTF-8
- 有大小限制
- 二进制文件不能直接当文本编辑
- 临时文件写入
- fsync / rename 采用合理的原子保存策略

---

# 四、Web Terminal

这是 EdgeDesk 的核心功能之一。

必须实现真实 PTY：

```text
Browser
  ↓ WebSocket
libhv
  ↓
TerminalSession
  ↓
forkpty()
  ↓
/bin/bash -l
```

禁止：

```text
popen()
system()
sh -c
```

作为 Web Terminal 实现。

每一个 Terminal 都有唯一：

```text
session_id
```

服务器保存：

```text
TerminalSession
```

即使 WebSocket 暂时断开，PTY 和 Bash 进程也不能立即销毁。

---

# 五、Terminal Session 生命周期

必须严格实现 V0.4 已确认行为：

### 切换 Tab

不关闭 Session。

### 页面内部导航

不关闭 Session。

### 浏览器刷新

不关闭 Session。

刷新后前端使用原来的 `session_id` 重新连接。

### WebSocket 短暂断线

Session 保留，允许重新连接。

### 用户显式关闭终端

才真正销毁：

```text
WebSocket
PTY
child process
session
```

### 多 Tab

一个浏览器页面可以同时存在多个 Terminal Session。

前端：

```text
Terminal Tab A
Terminal Tab B
Terminal Tab C
```

每个 Tab 对应独立 Session。

---

# 六、Terminal Manager

实现统一：

```cpp
class TerminalManager
```

负责：

- 创建 Session
- 查询 Session
- 获取 Session
- reconnect
- resize
- 输入
- 输出
- close
- session cleanup
- 并发限制
- 空闲超时

不要让 HTTP Route 直接管理：

```text
forkpty
kill
waitpid
read
write
ioctl
```

这些必须封装在 TerminalSession / PTY 层。

---

# 七、Terminal UI

使用 xterm.js。

必须支持：

- ANSI
- 彩色输出
- 光标
- resize
- Ctrl+C
- Ctrl+D
- 上下键
- Tab
- shell history
- 中文
- Unicode

窗口尺寸变化必须同步：

```text
xterm.js
 ↓
WebSocket resize
 ↓
ioctl(TIOCSWINSZ)
 ↓
SIGWINCH
```

---

# 八、浮动窗口系统

V0.4 已确认：

文件管理页面中的：

- Terminal
- 文件编辑器
- 文件预览

采用浮动窗口。

必须支持：

- 拖动
- 缩放
- 最大化
- 最小化 / 隐藏
- 关闭
- 层级切换
- 多窗口同时存在

不要把浮动窗口做成 iframe。

React 中实现统一：

```text
WindowManager
FloatingWindow
```

每个窗口具有：

```text
id
type
title
x
y
width
height
zIndex
state
```

窗口状态属于前端 UI 状态。

Terminal Session 生命周期属于后端 SessionManager。

两者必须解耦。

---

# 九、硬件仪表盘

实现基础设备监控 Dashboard。

至少包括：

### CPU

- 使用率
- load average
- 核心数

### Memory

- total
- used
- available
- usage %

### Disk

- 文件系统
- total
- used
- available
- usage %

### Network

- 网卡
- RX
- TX
- 当前速率

### Uptime

- 系统运行时间

---

# 十、实时曲线

Dashboard 必须支持实时数据刷新。

不要每次刷新整个页面。

采用：

```text
Browser
   ↓
WebSocket / SSE
   ↓
SystemMetricsService
   ↓
/proc
/sys
POSIX
```

根据当前架构选择 WebSocket 或 SSE，但必须：

- 数据增量更新
- 前端保留有限时间窗口
- 不允许无限制增长内存
- 设备资源受限时仍然稳定

实时曲线至少：

```text
CPU
Memory
Network RX
Network TX
```

磁盘容量可以低频刷新。

---

# 十一、认证与安全

完成当前 V1.0 Spec 中已经规划的认证体系。

要求：

```text
HTTP
 ├── /file
 ├── /terminal
 ├── /system
 └── /ws/terminal
```

统一经过认证中间件。

不要每个 Route 自己认证。

认证方式如果当前 Spec 没有最终确定具体参数：

- 做成 Config
- 使用安全默认值
- 不把具体密码写死在源码
- 不把秘密提交到 Git

同样实现：

- 请求体积限制
- 上传大小限制
- Terminal session 数量限制
- WebSocket 连接限制
- 空闲超时
- 基本安全响应头

危险命令保护必须保留，但不要做成完全不可靠的“字符串黑名单”。

至少对明显危险操作进行保护，并将规则集中到：

```text
CommandGuard
```

---

# 十二、审计日志

记录关键操作：

```text
登录
文件上传
文件删除
文件重命名
文件编辑保存
Terminal 创建
Terminal 关闭
Terminal 命令操作
```

日志不能无限制占用内存。

目标设备上优先采用轻量文件日志。

不要引入数据库。

---

# 十三、前端页面

完成统一 EdgeDesk UI。

至少：

```text
Dashboard
Files
Terminal
```

### Dashboard

展示：

```text
CPU
Memory
Disk
Network
Uptime
实时曲线
```

### Files

支持：

```text
目录树 / 当前路径
文件列表
上传
下载
新建目录
重命名
删除
编辑
预览
打开 Terminal
```

其中：

```text
编辑器
Terminal
预览
```

全部可以作为 FloatingWindow 打开。

### Terminal

支持：

```text
多 Tab
新建 Terminal
切换 Terminal
关闭 Terminal
重新连接 Session
```

---

# 十四、前端状态管理

不要把 Terminal Session 状态全部塞进 React Component。

至少区分：

```text
UI State
  ↓
WindowManager / TerminalTabs

Server State
  ↓
API / WebSocket

Persistent Client State
  ↓
session_id
```

浏览器刷新后必须能够恢复 Terminal Session。

可以使用：

```text
localStorage
```

保存：

```text
session_id
terminal metadata
```

但绝对不要在 localStorage 保存：

```text
密码
Token secret
```

除非现有安全设计明确要求。

---

# 十五、C++ 编码要求

必须遵循：

- RAII
- `std::unique_ptr`
- `std::shared_ptr` 仅在确有共享所有权时使用
- 禁止裸 `new/delete`
- 尽量避免全局变量
- 禁止业务层 singleton
- Config 显式依赖注入
- const correctness
- `std::string_view` 用于只读字符串参数
- 明确所有权
- 明确线程安全边界
- mutex 生命周期必须清晰
- 不允许数据竞争
- 错误必须显式处理

禁止：

```cpp
using namespace std;
```

禁止：

```cpp
catch (...) {}
```

禁止：

```cpp
system(...)
```

禁止：

```cpp
popen(...)
```

禁止通过全局变量保存：

```text
Config
TerminalManager
FileService
```

---

# 十六、线程模型

必须明确线程边界。

HTTP / WebSocket 线程不允许执行长时间阻塞操作。

尤其注意：

```text
文件下载
文件上传
文件搜索
PTY read
waitpid
系统指标采集
```

不能阻塞网络事件循环。

Terminal Session 必须能够并发运行。

所有共享状态必须明确同步策略。

---

# 十七、资源约束

EdgeDesk 面向：

```text
嵌入式 Linux
ARM32
ARM64
x86_64
资源受限设备
```

因此禁止无意义引入大型依赖。

特别禁止：

- 数据库
- ORM
- Boost 全家桶
- 大型日志框架
- 大型监控框架
- Node.js runtime
- Python runtime
- Electron

libhv 是网络核心依赖。

---

# 十八、#embed

这是项目的重要技术特征。

正式 Embed 模式：

```text
React build
    ↓
web/dist
    ↓
C++ #embed
    ↓
embedded_assets.cpp
    ↓
edgedesk
```

要求：

- GCC 15+
- C++26 `#embed`
- 静态资源编译进入最终二进制
- 不需要目标设备存在 web/目录

运行：

```bash
./edgedesk
```

即可启动完整 Web UI。

同时保留：

```text
EDGEDESK_WEB_MODE=disk
```

用于开发调试。

---

# 十九、构建目标

至少保证：

```text
dev-disk
release-disk
release-embed
```

并继续维护：

```text
x64
arm32
arm64
musl
```

正式目标：

```text
GCC 15+
musl
static
static-pie
```

但是不要为了“看起来支持”而伪造交叉编译成功。

每一个 Toolchain 必须真实编译。

---

# 二十、测试

必须补充：

### C++

```text
PathSandbox
Range
FileService
CommandGuard
Config
Terminal Session
System Metrics
```

### HTTP

测试：

```text
/api/health
/api/system/*
/api/file/*
/api/terminal/*
```

### WebSocket

至少验证：

```text
connect
create session
input
output
resize
disconnect
reconnect
close
```

### 前端

至少验证：

```text
build
Dashboard
Files
Terminal
FloatingWindow
Terminal reconnect
```

---

# 二十一、工程质量要求

每完成一个模块：

1. 编译
2. 单元测试
3. 集成测试
4. 更新文档
5. 检查线程安全
6. 检查资源泄漏
7. 检查错误处理

不要最后一次性才测试。

---

# 二十二、文档要求

代码实现过程中同步更新：

```text
README.md
AGENTS.md
docs/architecture
docs/spec
docs/adr
docs/design
```

特别补充：

```text
API endpoint 文档
WebSocket protocol
Terminal Session 生命周期
文件 API
System Metrics API
配置文件
构建方式
交叉编译
#embed 构建
部署方式
安全边界
```

任何新增的重要架构决策，都先更新 ADR，再实现。

---

# 二十三、开发执行方式

不要一次生成大量未经验证的代码。

按照下面顺序执行：

```text
Phase 2
  ↓
文件服务
  ↓
文件 API
  ↓
文件 UI
  ↓
Phase 3
  ↓
PTY
  ↓
TerminalSession
  ↓
TerminalManager
  ↓
WebSocket
  ↓
Terminal UI
  ↓
Session reconnect
  ↓
Phase 4
  ↓
SystemMetrics
  ↓
Dashboard
  ↓
Realtime
  ↓
Phase 5
  ↓
FloatingWindow
  ↓
认证
  ↓
限流
  ↓
审计
  ↓
最终构建
  ↓
多架构验证
```

但不要在每个 Phase 完成后停下来等待我。

**从 Phase 2 开始连续执行，一直做到 V1.0 DoD。**

---

# 二十四、遇到不确定事项时

优先级严格按照：

```text
用户已经确认的决策
    >
ADR
    >
Spec
    >
AGENTS.md
    >
现有代码架构
    >
合理工程默认值
```

对于没有明确确定的：

- 超时
- 数量限制
- 文件大小
- UI 细节
- API 字段

不要擅自破坏架构。

优先：

```text
Config 可配置
合理默认值
文档说明
```

只有涉及架构方向、数据安全或产品边界且无法合理决定时才停止并询问。

**不要因为普通实现细节频繁询问用户。**

---

# 二十五、禁止事项

不要：

- 重写整个项目
- 删除已有 ADR / Spec
- 删除已有测试
- 删除已有素材
- 引入数据库
- 引入 ORM
- 把 C++ 后端改成 Go / Rust
- 把 libhv 替换成其他 Web 框架
- 把 React 改成其他前端框架
- 使用简单 `sh` 代替 Bash 登录 Shell
- 用 `system()` 实现 Terminal
- 用轮询代替已经适合 WebSocket 的实时能力
- 为了通过测试而修改测试预期
- 删除失败测试
- 忽略编译警告
- 提交密码、Token、私钥
- 声称没有实际验证过的平台“构建成功”

---

# 二十六、最终 Definition of Done

只有满足以下条件才算 V1.0 完成：

```text
[ ] Dashboard 可用
[ ] CPU / Memory / Disk / Network / Uptime 正常
[ ] 实时曲线正常
[ ] 文件浏览正常
[ ] 上传正常
[ ] 下载正常
[ ] Range 下载正常
[ ] 新建目录正常
[ ] 重命名正常
[ ] 删除正常
[ ] 文本预览正常
[ ] 在线编辑正常
[ ] 原子保存正常
[ ] 路径沙箱正常
[ ] 软链接逃逸被阻断
[ ] Bash 登录 Shell 正常
[ ] /etc/profile 正常加载
[ ] 环境变量正常
[ ] PTY 正常
[ ] xterm.js 正常
[ ] Terminal resize 正常
[ ] 多 Terminal Tab 正常
[ ] 页面导航 Terminal 不丢失
[ ] 浏览器刷新 Terminal 不丢失
[ ] WebSocket 重连正常
[ ] FloatingWindow 正常
[ ] 窗口可拖动
[ ] 窗口可缩放
[ ] 文件编辑器可浮动
[ ] Terminal 可浮动
[ ] 认证正常
[ ] WebSocket 认证正常
[ ] 限流正常
[ ] 审计日志正常
[ ] 单二进制运行正常
[ ] GCC 15+ #embed 正常
[ ] 磁盘模式正常
[ ] CMake Preset 正常
[ ] C++ 测试全部通过
[ ] HTTP 测试全部通过
[ ] WebSocket 测试通过
[ ] React build 通过
[ ] CI 全部通过
[ ] 文档同步完成
[ ] 无数据库
[ ] 无 ORM
```

最终必须给出一份简短报告：

```text
1. 已完成模块
2. 未完成模块
3. 测试结果
4. 支持的平台
5. 尚未实际验证的平台
6. 关键架构变化
7. 新增依赖
8. 构建命令
9. 运行命令
```

**最重要的要求：不要只写代码。必须实际编译、测试、修复，然后继续推进，直到整个 V1.0 范围完成。**