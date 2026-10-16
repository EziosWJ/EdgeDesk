# ADR-0006：系统指标采集与实时推送

- 状态：**Accepted（已确认）**
- 日期：2026-10-16
- 关联：[仪表盘 Spec](../spec/0004-device-dashboard.md)、[ADR-0003](0003-v1-auth-api-protocol.md)

## 背景

仪表盘需要 CPU、内存、磁盘、网络与运行时间的当前值与短时曲线；设备资源受限、内核接口可能缺失；不允许数据库，内存占用必须固定有界。

## 决策

1. 采集实现为一个独立后台线程（`SystemMetrics`），按 `--metrics-interval`（默认 1000 ms）采样，数据源为 Linux 基础接口：
   - CPU：`/proc/stat`（累计 jiffies，两次采样差分）；负载 `<sys/loadavg>`；核心数 `sysconf(_SC_NPROCESSORS_ONLN)`。
   - 内存：`/proc/meminfo`（total/available/free/buffers/cached/swap）。
   - 磁盘：`statvfs`（逐个挂载点，默认读取 `/proc/mounts` 中真实块设备挂载；可配置 `--disk-mount` 列表覆盖）。
   - 网络：`/proc/net/dev`（累计 RX/TX 字节，差分得到速率；排除 lo，无接口时返回空列表）。
   - 运行时间：`/proc/uptime`；主机名 `gethostname`；内核 `uname`。
2. 内存中维护固定长度环形缓冲（`--metrics-history`，默认 300 点），只保存标量样本，不落盘、不跨重启。
3. 传输：`GET /api/system/summary`（当前值 + 可选 `history=N` 返回最近 N 点快照）与 `WS /ws/metrics` 增量推送（见 ADR-0003、API 文档）。
4. 接口缺失（例如容器内没有某些挂载、没有非 lo 网卡）时字段为 `null` 并在 `unavailable[]` 中列出原因，页面显示“不可用”，不虚构为 0。
5. 采样线程只读小文件、单次采样复杂度 O(核数 + 挂载数 + 网卡数)，且采样周期 ≥ 500 ms，避免影响受限设备；磁盘容量默认按 `--disk-interval`（默认每 10 个采样周期）低频刷新。
6. 首帧缺少差分基准时速率字段为 `null`，由前端显示“采样中”。

## 影响

- 不依赖 `top/free/df` 等外部命令，不依赖数据库与监控框架。
- 单元测试注入固定文件内容（解析函数与文件路径解耦）验证差分与缺失字段行为。
