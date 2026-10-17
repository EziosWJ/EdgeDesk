# 本地 GCC 15 工具链（免 sudo）

> 状态：**已验证的宿主机构建环境说明**。仅描述 Ubuntu x86_64 开发机上的本地工具链安装方式，
> 不改变任何产品行为，也不替代 [ADR-0001](../adr/0001-technology-stack.md) 中“GCC 15+”的目标基线。

## 为什么需要单独一份本地工具链

| 事实 | 说明 |
| --- | --- |
| `EDGEDESK_WEB_MODE=embed` 强制 GCC 15+ | 见 `CMakeLists.txt`：不满足直接 `FATAL_ERROR`，提示改用 `disk` 模式 |
| Ubuntu 24.04 (noble) 官方源只有 `gcc-13` / `gcc-14` | 没有 `gcc-15`，需要 PPA 或自建 |
| 装系统级 `gcc-15` 需要 root | 无 sudo 密码时无法安装，因此这里采用“解压到用户目录”的方案 |
| 产物只使用 `#embed` 这一项 GCC 15 扩展 | 业务代码仍是 C++17；宿主 `libstdc++` 未升级也能运行构建产物 |

本方案**不修改系统**：deb 包只通过 `dpkg -x` 解压到 `$HOME/opt/gcc-15`，不写入 `/usr`，
也不改变系统默认 `gcc`/`g++`（仍为 13.3），因此不会影响既有构建。

## 已验证环境（2026-10-16 实测）

| 项目 | 值 |
| --- | --- |
| 系统 | Ubuntu 24.04.4 LTS (noble)，x86_64 |
| 系统编译器 | GCC 13.3.0（保持不变） |
| 本地工具链 | **GCC 15.2.0**（`15.2.0-14ubuntu1~24~ppa1`），来自 `ppa:ubuntu-toolchain-r/test` |
| 安装位置 | `$HOME/opt/gcc-15/root/usr/bin/{gcc-15,g++-15}`（解压后约 196 MB；含 `debs/` 缓存约 254 MB） |
| 运行时库 | `libstdc++6` / `libgcc-s1` 使用该 PPA 当前的 gcc-16 构建（该 PPA 已不再提供 15.x 运行时） |
| 目标 ABI | `x86_64-linux-gnu`（glibc） |

## 一次性安装

```bash
# 默认安装到 $HOME/opt/gcc-15，可用 GCC15_PREFIX 或 --prefix 覆盖
scripts/install-gcc15-local.sh
```

脚本行为：

1. 按锁定版本与 sha256 从 `ppa:ubuntu-toolchain-r/test` 下载 deb 到 `$PREFIX/debs/`，校验失败即中止；
2. 全部校验通过后，用 `dpkg -x` 解压到 `$PREFIX/root`（不调用 `dpkg -i`，不需要 root）；
3. 用新工具链编译并运行一个 C++17 `#embed` 探针，确认扩展真实可用；
4. 打印版本与后续命令。

重复执行是幂等的：已缓存的 deb 直接复用（可离线重装）。若 PPA 之后清理了锁定版本导致 404，
把已下载的 deb 目录用 `--from-dir DIR` 传入即可继续安装。

## 日常使用

```bash
# 配置 + 构建 + 测试（默认预设 host-embed，仅用于宿主 glibc 验证）
scripts/with-gcc15.sh all

# 分步执行
scripts/with-gcc15.sh configure host-embed
scripts/with-gcc15.sh build host-embed -j20
scripts/with-gcc15.sh test host-embed

# 只打印环境变量（解压树里只有 gcc-15/g++-15 这类带后缀的可执行文件，不会覆盖系统 gcc/g++）
scripts/with-gcc15.sh env
```

等价的手工命令：

```bash
cmake --preset host-embed \
  -DCMAKE_C_COMPILER="$HOME/opt/gcc-15/root/usr/bin/gcc-15" \
  -DCMAKE_CXX_COMPILER="$HOME/opt/gcc-15/root/usr/bin/g++-15"
cmake --build --preset host-embed -j"$(nproc)"
ctest --test-dir out/build/host-embed --output-on-failure
```

`dev-disk` 与 `host-offline-tests` 预设同样可用，但它们在 GCC 13 下也能构建，是否切换到 GCC 15 由需要决定。

> `release-embed` / `release-embed-arm64` / `release-disk` 是静态发布预设，使用
> Bootlin GCC 15.3 musl 交叉工具链（`EDGEDESK_TOOLCHAIN_ROOT`），**不能**用本节的
> 宿主编译器配置；宿主 GCC 15 的 `#embed` 验证统一走 `host-embed`。

## 验收证据（本次实测结果）

| 检查项 | 命令 | 结果 |
| --- | --- | --- |
| 编译器版本 | `g++-15 --version` | `g++-15 (Ubuntu 15.2.0-14ubuntu1~24~ppa1) 15.2.0` |
| `#embed` 探针 | `g++-15 -std=c++17 -Wall -Wextra -Wpedantic` + `#embed "..." suffix(,)` | 编译、运行通过 |
| 宿主 `#embed` 构建（实测时预设名为 release-embed） | `cmake --preset host-embed` + GCC 15 | 72/72 目标成功，产出 `out/build/host-embed/bin/edgedesk` |
| 单元测试 | `ctest --test-dir out/build/host-embed` | 2/2 通过（`core_test`、`embed_generator_test`；当时测试数量） |
| 离线测试预设 | `cmake --preset host-offline-tests` + GCC 15 | 构建 0 warning，测试 2/2 通过 |
| Phase 1 契约 | `GET /api/health` | `{"status":"ok","service":"edgedesk","phase":1}` |
| 嵌入资源路由 | `GET /` | HTTP 200，返回嵌入的 `index.html` |
| 运行时依赖 | `ldd out/build/host-embed/bin/edgedesk` | 使用宿主 `libstdc++.so.6`（14.2）正常启动；该预设按设计是动态链接，仅用于本地验证 |

## 已知警告与限制

- **`-Wc++26-extensions`**：`#embed` 是 GCC 扩展，在 `-std=c++17` + `-Wpedantic` 下对每个 `#embed` 产生一条警告。
  项目未启用 `-Werror`，不影响构建。是否屏蔽属于产品/构建决策，尚未确认。
- **本工具链不用于发布**：`host-embed` 是 glibc 动态链接，仅用于本地验证 `#embed` 管线。
  发布产物使用 Bootlin GCC 15.3 musl 静态预设（`release-embed`、`release-embed-arm64`、
  `release-disk`），已在 x86_64 与 aarch64 上验证。
- **ARM32 未覆盖**：需要 armv7 musl GCC 15+ 工具链；`cmake/toolchains/arm-linux-musleabihf.cmake`
  已支持 Bootlin 布局，但本机未安装、未验证，见 ADR-0001。
- **仅限 x86_64 宿主**：锁定的 deb 是 `_amd64`，脚本在其他架构上会直接报错退出。
- **系统级安装（可选）**：若希望 `gcc-15` 全局可用，需要 root 执行
  `add-apt-repository ppa:ubuntu-toolchain-r/test && apt install gcc-15 g++-15`；本方案未采用。

## 相关文件

- `scripts/install-gcc15-local.sh` —— 免 sudo 安装与自检
- `scripts/with-gcc15.sh` —— 用本地 GCC 15 配置/构建/测试
- `CMakeLists.txt` ——`EDGEDESK_WEB_MODE` 与编译器版本约束
- [ADR-0001 技术路线](../adr/0001-technology-stack.md)、[Phase 1 实现与测试](../phase1.md)
