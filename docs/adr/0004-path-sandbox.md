# ADR-0004：文件路径沙箱（目录 FD + openat 解析）

- 状态：**Accepted（已确认）**
- 日期：2026-10-16
- 关联：[文件管理 Spec](../spec/0002-file-management.md)、[ADR-0002](0002-architecture-security.md)

## 背景

文件 API 必须只允许访问 `--file-root` 下的内容，阻断 `..` 穿越、URL 编码穿越、符号链接逃逸以及 check-then-use（TOCTOU）竞态。单纯 `realpath()` 之后再 `open()` 存在竞态窗口，不能满足 SPEC-0002 的验收要求。

## 决策

1. 所有文件操作统一经过 `PathSandbox`，由它持有**根目录 FD**（`open(root, O_PATH|O_DIRECTORY|O_CLOEXEC)`，启动时校验并一次性解析根路径）。
2. 路径解析采用**逐组件 `openat` 下降**，每个中间组件使用 `O_PATH|O_NOFOLLOW|O_DIRECTORY`：
   - 任何组件的 `lstat`/`openat` 发现符号链接，默认返回 `403 symlink_not_allowed`（`--follow-symlinks` 默认关闭；开启时在沙箱内受深度限制地重新解析，仍不允许逃出根）。
   - 拒绝 `..`、空组件、绝对路径、反斜杠、NUL 及控制字符；路径长度上限 4096。
   - 组件级下降天然不受字典序前缀欺骗影响（`/data/../data2` 无法逃出 `/data`）。
3. 最终组件操作用已持有的目录 FD 加 `openat/mkdirat/renameat2/unlinkat/fstatat`，**不使用字符串拼接再做二次解析**，因此没有 TOCTOU 窗口：
   - 读：`openat(dirfd, name, O_RDONLY|O_NOFOLLOW|O_CLOEXEC)`
   - 新建/覆盖：`O_WRONLY|O_CREAT|O_EXCL` 或原子替换（临时文件 + `renameat2`）
   - 删除：`unlinkat(..., 0)` / `unlinkat(..., AT_REMOVEDIR)`
   - 属性：`fstatat(..., AT_SYMLINK_NOFOLLOW)`
4. 读操作（浏览、下载、预览）在中间组件上允许“已存在的目录”；写操作在打开目标父目录时要求 `O_DIRECTORY` 且调用者对父目录具有相应权限，错误映射为 `403/404/409`。
5. 沙箱是**文件 API 的边界，不是终端沙箱**：Bash 会话的权限由运行用户、设备系统权限和外部隔离机制决定（与 ADR-0002 一致）。
6. 递归删除在 FD 基础上实现，带深度上限（默认 64）和条目上限（默认 100000），不跟随符号链接（符号链接本身被删除）。
7. 目录 FD 在请求处理期间持有并随 RAII 关闭；`PathSandbox` 自身线程安全（根 FD 只读共享）。

## 影响

- 不支持把“跟随符号链接”作为默认行为；需要该能力的部署可显式打开并在文档中说明风险。
- 所有文件 HTTP 路由只做参数解析与错误映射，安全判断集中在 `PathSandbox`。
- 单元测试必须覆盖：`..`、URL 编码、绝对路径、符号链接逃逸、边界外重命名/删除、前缀欺骗目录、并发替换目标文件。

## 非决策

- 不实现 `openat2` 的 `RESOLVE_BENEATH` 依赖（内核版本不确定）；组件级 `openat` 已提供等价语义。
- 不实现配额、ACL、SELinux/AppArmor 集成。
