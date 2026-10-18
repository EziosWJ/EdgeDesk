import { useEffect, useState, type ReactNode } from 'react';
import { Activity, Cpu, HardDrive, MemoryStick } from 'lucide-react';
import { Icon } from '../../components/Icon';
import { MetricChart } from '../../components/MetricChart';
import { Sparkline } from '../../components/Sparkline';
import { AppSprite } from '../../desktop/AppIcon';
import { api, formatBytes, formatPercent, formatRate, formatUptime } from '../../services/api';
import { asset } from '../../services/assets';
import { preferencesStore, usePreferences } from '../../stores/preferencesStore';
import { sessionStore, useMetricsSamples, useTerminals } from '../../stores/sessionStore';
import type { MetricsSample, SystemInfo } from '../../types';
import {
  bootMoment,
  deltaTone,
  formatDateTime,
  formatDelta,
  kpiPercent,
  rateScale,
  sliceWindow,
  usageDelta,
} from './chartUtils';

/** Wall clock for the dashboard header (real time, not sample time). */
function useClock(): Date {
  const [now, setNow] = useState(() => new Date());
  useEffect(() => {
    const timer = window.setInterval(() => setNow(new Date()), 1000);
    return () => window.clearInterval(timer);
  }, []);
  return now;
}

function clockText(now: Date): string {
  const two = (value: number) => String(value).padStart(2, '0');
  return `${now.getFullYear()}年${now.getMonth() + 1}月${now.getDate()}日 ${two(now.getHours())}:${two(now.getMinutes())}:${two(now.getSeconds())}`;
}

type WidgetStatus = 'ok' | 'warning' | 'unavailable' | 'loading';

function Widget({
  sprite,
  title,
  status = 'ok',
  badge,
  children,
  footer,
  className = '',
}: {
  sprite: string;
  title: string;
  status?: WidgetStatus;
  badge?: ReactNode;
  children: ReactNode;
  footer?: ReactNode;
  className?: string;
}) {
  return (
    <article className={`ed-nas-widget ${className}`.trim()} data-status={status}>
      <header className="ed-nas-widget-head">
        <h2>
          <span className="ed-nas-widget-glyph" aria-hidden="true">
            <AppSprite app={{ sprite, tone: 'blue' }} size={17} />
          </span>
          {title}
        </h2>
        <span className={`ed-nas-status ${status}`}>
          <span className="ed-nas-status-dot" aria-hidden="true" />
          {badge ?? (status === 'ok' ? '正常' : status === 'warning' ? '采样中' : status === 'loading' ? '加载中' : '不可用')}
        </span>
      </header>
      <div className="ed-nas-widget-body">{children}</div>
      {footer && <div className="ed-nas-widget-foot">{footer}</div>}
    </article>
  );
}

function StatRow({ label, value, title }: { label: string; value: ReactNode; title?: string }) {
  return (
    <div className="ed-nas-stat" title={title}>
      <span>{label}</span>
      <strong>{value}</strong>
    </div>
  );
}

function DeltaChip({ delta, lowerIsBetter = true }: { delta: number | null; lowerIsBetter?: boolean }) {
  if (delta === null) return null;
  return (
    <span className={`ed-kpi-delta ${deltaTone(delta, lowerIsBetter)}`} title="与曲线窗口前段对比">
      {formatDelta(delta)}
    </span>
  );
}

function Progress({ value, tone = 'primary', label }: { value: number | null; tone?: 'primary' | 'accent'; label: string }) {
  const width = value === null ? 0 : Math.min(100, Math.max(0, value));
  return (
    <div
      className="ed-nas-progress"
      role="progressbar"
      aria-label={label}
      aria-valuemin={0}
      aria-valuemax={100}
      aria-valuenow={value === null ? undefined : Number(width.toFixed(1))}
    >
      <span className={`tone-${tone}`} style={{ width: `${width}%` }} />
    </div>
  );
}

function primaryAddress(addresses: readonly string[]): string | null {
  return addresses.find((address) => !address.includes(':')) ?? addresses[0] ?? null;
}

/** Loopback traffic must not be double-counted in device throughput. */
export function isLoopbackInterface(name: string): boolean {
  return /^(lo|loopback)/i.test(name);
}

function RangeSelect({ value, onChange }: { value: number; onChange: (seconds: number) => void }) {
  return (
    <select className="ed-range-select" value={value} onChange={(event) => onChange(Number(event.target.value))} aria-label="全部曲线时间范围">
      <option value={60}>1 分钟</option>
      <option value={300}>5 分钟</option>
      <option value={600}>10 分钟</option>
      <option value={1800}>30 分钟</option>
    </select>
  );
}

function ServiceStatusWidget({
  system,
  systemError,
  latest,
  intervalMs,
  now,
}: {
  system: SystemInfo | null;
  systemError: string | null;
  latest: MetricsSample | null;
  intervalMs: number;
  now: Date;
}) {
  const terminals = useTerminals();
  const stale = latest ? (now.getTime() - latest.ts) / 1000 > Math.max(5, (intervalMs * 3) / 1000) : false;
  const shell = sessionStore.shellInfo;
  const apiStatus: WidgetStatus = system ? 'ok' : systemError ? 'unavailable' : 'loading';
  const metricsStatus: WidgetStatus = !latest ? 'loading' : stale ? 'unavailable' : 'ok';
  const terminalStatus: WidgetStatus = shell.available ? 'ok' : 'unavailable';

  const rows: Array<{ name: string; status: WidgetStatus; text: string; detail: string }> = [
    {
      name: 'EdgeDesk API',
      status: apiStatus,
      text: system ? '运行中' : systemError ? '不可用' : '检测中',
      detail: system ? `版本 ${system.version}` : systemError ?? '正在读取 /api/system/info',
    },
    {
      name: '指标采集',
      status: metricsStatus,
      text: !latest ? '等待数据' : stale ? '数据中断' : '运行中',
      detail: stale ? '超过 3 个采样周期未更新' : `采样间隔 ${intervalMs} ms`,
    },
    {
      name: '终端服务',
      status: terminalStatus,
      text: shell.available ? `${terminals.length} 个会话` : 'Bash 不可用',
      detail: shell.available ? shell.shell : '未找到登录 Shell',
    },
    {
      name: '审计日志',
      status: system ? (system.audit_enabled ? 'ok' : 'warning') : 'loading',
      text: system ? (system.audit_enabled ? '已启用' : '已关闭') : '检测中',
      detail: system ? (system.audit_enabled ? system.audit_log : '部署配置中关闭') : '—',
    },
  ];

  return (
    <Widget
      sprite="service"
      title="服务状态"
      status={apiStatus === 'unavailable' || metricsStatus === 'unavailable' ? 'unavailable' : 'ok'}
      badge={apiStatus === 'unavailable' || metricsStatus === 'unavailable' ? '部分不可用' : '运行中'}
      className="ed-nas-service-widget"
      footer={<span>仅展示后端确实可以确认的 EdgeDesk 服务状态；不提供 Docker/systemd 管理。</span>}
    >
      <div className="ed-nas-service-list">
        {rows.map((row) => (
          <div className="ed-nas-service-row" key={row.name}>
            <span className="ed-nas-service-name">{row.name}</span>
            <span className={`ed-nas-status ${row.status}`}>
              <span className="ed-nas-status-dot" aria-hidden="true" />
              {row.text}
            </span>
            <span className="ed-nas-service-detail" title={row.detail}>
              {row.detail}
            </span>
          </div>
        ))}
      </div>
    </Widget>
  );
}

export function DashboardView() {
  const samples = useMetricsSamples();
  const preferences = usePreferences();
  const terminals = useTerminals();
  const intervalMs = sessionStore.intervalMs;
  const [rangeSeconds, setRangeSeconds] = useState(preferences.dashboard_metrics_window);
  const [system, setSystem] = useState<SystemInfo | null>(null);
  const [systemError, setSystemError] = useState<string | null>(null);
  const now = useClock();

  useEffect(() => {
    setRangeSeconds(preferences.dashboard_metrics_window);
  }, [preferences.dashboard_metrics_window]);

  useEffect(() => {
    let cancelled = false;
    api
      .systemInfo()
      .then((info) => {
        if (!cancelled) setSystem(info);
      })
      .catch((caught: unknown) => {
        if (!cancelled) setSystemError(caught instanceof Error ? caught.message : String(caught));
      });
    return () => {
      cancelled = true;
    };
  }, []);

  const changeRange = (seconds: number) => {
    setRangeSeconds(seconds);
    void preferencesStore.save({ dashboard_metrics_window: seconds }).catch(() => undefined);
  };

  useEffect(() => {
    // Prime charts with server-side history; the WebSocket keeps them current.
    if (sessionStore.metricsSnapshot().length > 0) return;
    api
      .systemSummary(120)
      .then((data) => {
        sessionStore.setMetricsHistory(data.history ?? [data.sample], data.interval_ms);
      })
      .catch(() => undefined);
  }, []);

  const latest = samples.length > 0 ? samples[samples.length - 1] : null;
  const visible = sliceWindow(samples, rangeSeconds, intervalMs);
  const timestamps = visible.map((sample) => sample.ts);
  const unavailable = new Set(latest?.unavailable ?? []);
  const lookback = Math.max(2, Math.round(visible.length / 2));
  const staleSeconds = latest ? Math.max(0, (now.getTime() - latest.ts) / 1000) : null;
  const stale = staleSeconds !== null && staleSeconds > Math.max(5, (intervalMs * 3) / 1000);
  const hardUnavailable = [...unavailable].filter((code) => code !== 'network_rate_initial');

  const cpuValues = visible.map((sample) => sample.cpu.usage_percent);
  const memoryUsed = visible.map((sample) => (sample.memory ? sample.memory.used_percent : null));
  const memoryFree = memoryUsed.map((value) => (value === null ? null : Math.max(0, 100 - value)));
  const networkValues = visible.map((sample) => {
    const usable = sample.network.filter((item) => !isLoopbackInterface(item.name));
    return usable.length > 0
      ? usable.reduce((sum, item) => sum + item.rx_bytes_per_second + item.tx_bytes_per_second, 0)
      : null;
  });
  const rxValues = visible.map((sample) => {
    const usable = sample.network.filter((item) => !isLoopbackInterface(item.name));
    return usable.length > 0 ? usable.reduce((sum, item) => sum + item.rx_bytes_per_second, 0) : null;
  });
  const txValues = visible.map((sample) => {
    const usable = sample.network.filter((item) => !isLoopbackInterface(item.name));
    return usable.length > 0 ? usable.reduce((sum, item) => sum + item.tx_bytes_per_second, 0) : null;
  });
  const networkScale = rateScale([...rxValues, ...txValues]);

  const disks = latest?.disks ?? [];
  const primaryDisk = disks.find((disk) => disk.mount === '/') ?? disks[0] ?? null;
  const diskValues = primaryDisk
    ? visible.map((sample) => sample.disks.find((disk) => disk.mount === primaryDisk.mount)?.used_percent ?? null)
    : [];
  const loadText =
    latest && latest.cpu.load1 !== null
      ? `${latest.cpu.load1.toFixed(2)}/${latest.cpu.load5?.toFixed(2) ?? '—'}/${latest.cpu.load15?.toFixed(2) ?? '—'}`
      : '—';

  const physicalInterfaces = latest?.network.filter((item) => !isLoopbackInterface(item.name)) ?? [];
  const rxTotal =
    physicalInterfaces.length > 0
      ? physicalInterfaces.reduce((sum, item) => sum + item.rx_bytes_per_second, 0)
      : null;
  const txTotal =
    physicalInterfaces.length > 0
      ? physicalInterfaces.reduce((sum, item) => sum + item.tx_bytes_per_second, 0)
      : null;
  const throughput = rxTotal !== null && txTotal !== null ? rxTotal + txTotal : null;

  const status = !latest
    ? { tone: 'warning', text: '等待数据', detail: '尚未收到指标采样，正在连接服务。' }
    : stale
      ? { tone: 'warning', text: '数据中断', detail: '最近的采样已超过 3 个周期未更新，请检查 Metrics WebSocket 连接。' }
      : hardUnavailable.length > 0
        ? { tone: 'warning', text: '部分不可用', detail: `以下指标不可用：${hardUnavailable.join('、')}。` }
        : { tone: 'online', text: '在线', detail: '设备运行正常，指标持续采集中。' };

  const criticalDisk = disks.find((disk) => disk.used_percent >= 90) ?? null;
  const memoryUnavailable = !latest?.memory || unavailable.has('memory');
  const cpuUnavailable = unavailable.has('cpu_usage');

  return (
    <div className="ed-view ed-dashboard ed-nas-dashboard">
      <div className="ed-view-header">
        <div>
          <h1>设备概览</h1>
          <p>实时监控设备资源与服务状态；所有数值来自后端采样，不使用演示数据。</p>
        </div>
        <div className="ed-toolbar ed-dashboard-meta">
          <span className="ed-clock">{clockText(now)}</span>
          <span className={`ed-status-pill ${status.tone}`}>
            <span className="ed-status-dot" />
            {status.text}
          </span>
          <span className="ed-live">
            <span className="ed-live-dot" />
            <span className="ed-live-label">
              实时 · {samples.length} 个样本 / 窗口 {Math.round((samples.length * intervalMs) / 1000)} 秒
            </span>
          </span>
          <div className="ed-range-control">
            <span>曲线范围</span>
            <RangeSelect value={rangeSeconds} onChange={changeRange} />
          </div>
        </div>
      </div>

      {!latest ? (
        <>
          <div className="ed-panel ed-empty small">
            <img src={asset('illustrations/monitor-unavailable.svg')} alt="" />
            <p>等待指标数据…首次采样到达后自动刷新，不使用模拟数值。</p>
          </div>
          <section className="ed-nas-grid ed-nas-grid-loading" aria-hidden="true">
            {['CPU 使用率', '内存使用率', '存储空间', '网络吞吐'].map((title) => (
              <article className="ed-nas-widget" data-status="loading" key={title}>
                <header className="ed-nas-widget-head">
                  <h2>
                    <span className="ed-nas-widget-glyph">
                      <Icon icon={title === '内存使用率' ? MemoryStick : title === '存储空间' ? HardDrive : title === '网络吞吐' ? Activity : Cpu} size={17} />
                    </span>
                    {title}
                  </h2>
                  <span className="ed-nas-status loading">
                    <span className="ed-nas-status-dot" />
                    加载中
                  </span>
                </header>
                <div className="ed-nas-widget-body">
                  <strong className="ed-nas-value">—</strong>
                  <p className="ed-nas-placeholder">等待真实采样…</p>
                </div>
              </article>
            ))}
          </section>
        </>
      ) : (
        <>
          <section className="ed-nas-grid">
            <Widget
              sprite="cpu"
              title="CPU 使用率"
              status={cpuUnavailable ? 'unavailable' : 'ok'}
              badge={cpuUnavailable ? '不可用' : `${latest.cpu.cores} 核`}
              footer={
                <>
                  <span>负载 {loadText}</span>
                  <span>{stale ? '数据已中断' : '1 分钟 / 5 分钟 / 15 分钟'}</span>
                </>
              }
            >
              <div className="ed-nas-value-row">
                <strong className="ed-nas-value">{kpiPercent(latest.cpu.usage_percent)}</strong>
                <DeltaChip delta={usageDelta(cpuValues, lookback)} />
              </div>
              <Progress value={latest.cpu.usage_percent} label="CPU 使用率" />
              <Sparkline
                values={cpuValues}
                height={52}
                max={100}
                color="var(--ed-primary)"
                label="CPU 使用率趋势"
                showValue={false}
              />
            </Widget>

            <Widget
              sprite="memory"
              title="内存使用率"
              status={memoryUnavailable ? 'unavailable' : 'ok'}
              badge={memoryUnavailable ? '不可用' : latest.memory ? formatBytes(latest.memory.total_kb * 1024) : '—'}
              footer={
                <>
                  <span>可用 {latest.memory ? formatBytes(latest.memory.available_kb * 1024) : '—'}</span>
                  <span>缓存 {latest.memory ? formatBytes(latest.memory.cached_kb * 1024) : '—'}</span>
                </>
              }
            >
              <div className="ed-nas-value-row">
                <strong className="ed-nas-value">{kpiPercent(latest.memory?.used_percent ?? null)}</strong>
                <DeltaChip delta={usageDelta(memoryUsed, lookback)} />
              </div>
              <Progress value={latest.memory?.used_percent ?? null} label="内存使用率" />
              <div className="ed-nas-stats">
                <StatRow label="已用" value={latest.memory ? formatBytes(latest.memory.used_kb * 1024) : '—'} />
                <StatRow
                  label="Swap"
                  value={
                    latest.memory
                      ? latest.memory.swap_total_kb > 0
                        ? `${formatBytes((latest.memory.swap_total_kb - latest.memory.swap_free_kb) * 1024)} / ${formatBytes(latest.memory.swap_total_kb * 1024)}`
                        : '未启用'
                      : '—'
                  }
                />
              </div>
            </Widget>

            <Widget
              sprite="storage"
              title="存储空间"
              status={disks.length === 0 ? 'unavailable' : criticalDisk ? 'warning' : 'ok'}
              badge={disks.length === 0 ? '不可用' : criticalDisk ? '空间紧张' : `${disks.length} 个挂载点`}
              footer={
                primaryDisk ? (
                  <>
                    <span>
                      {primaryDisk.mount} · {primaryDisk.filesystem}
                    </span>
                    <span>剩余 {formatBytes(Math.max(0, primaryDisk.total_bytes - primaryDisk.used_bytes))}</span>
                  </>
                ) : (
                  <span>{unavailable.has('disk') ? '未找到可统计的块设备挂载点' : '—'}</span>
                )
              }
            >
              <div className="ed-nas-value-row">
                <strong className="ed-nas-value">{kpiPercent(primaryDisk?.used_percent ?? null)}</strong>
                <DeltaChip delta={usageDelta(diskValues, lookback)} />
              </div>
              <Progress value={primaryDisk?.used_percent ?? null} tone={criticalDisk ? 'accent' : 'primary'} label="主挂载点使用率" />
              <div className="ed-nas-stats">
                <StatRow label="已用" value={primaryDisk ? formatBytes(primaryDisk.used_bytes) : '—'} />
                <StatRow label="总量" value={primaryDisk ? formatBytes(primaryDisk.total_bytes) : '—'} />
              </div>
            </Widget>

            <Widget
              sprite="network"
              title="网络吞吐"
              status={
                latest.network.length === 0 ? 'unavailable' : unavailable.has('network_rate_initial') ? 'warning' : 'ok'
              }
              badge={
                unavailable.has('network_rate_initial')
                  ? '采样中…'
                  : latest.network.length === 0
                    ? '不可用'
                    : physicalInterfaces.length === 0
                      ? '仅环回接口'
                      : `${physicalInterfaces.length} 个网卡`
              }
              footer={
                <>
                  <span>接收 ↓ {formatRate(rxTotal)}</span>
                  <span>发送 ↑ {formatRate(txTotal)}</span>
                </>
              }
            >
              <div className="ed-nas-value-row">
                <strong className="ed-nas-value">{formatRate(throughput)}</strong>
                <span className="ed-nas-unit">双向合计 · 不含环回</span>
              </div>
              <Sparkline
                values={networkValues}
                height={52}
                color="var(--ed-accent)"
                label="网络吞吐趋势"
                showValue={false}
              />
              <div className="ed-nas-stats">
                <StatRow
                  label="接口"
                  value={physicalInterfaces.length > 0 ? physicalInterfaces.map((item) => item.name).join('、') : '—'}
                />
                <StatRow
                  label="IP"
                  value={
                    physicalInterfaces.length > 0
                      ? physicalInterfaces
                          .map((item) => primaryAddress(item.addresses))
                          .filter((address): address is string => address !== null)
                          .join('、') || 'IP 未分配'
                      : '—'
                  }
                />
              </div>
            </Widget>
          </section>

          <section className="ed-nas-detail-grid">
            <Widget
              sprite="storage"
              title="存储挂载点"
              status={disks.length === 0 ? 'unavailable' : 'ok'}
              badge={disks.length > 0 ? `${disks.length} 个` : '不可用'}
            >
              {disks.length === 0 ? (
                <div className="ed-empty small">
                  <img src={asset('illustrations/monitor-unavailable.svg')} alt="" />
                  <p>{unavailable.has('disk') ? '磁盘统计不可用：未找到可统计的块设备挂载点。' : '正在采集磁盘容量…'}</p>
                </div>
              ) : (
                <ul className="ed-nas-mount-list">
                  {disks.map((disk) => {
                    const free = Math.max(0, disk.total_bytes - disk.used_bytes);
                    return (
                      <li className="ed-nas-mount" key={disk.mount}>
                        <div className="ed-nas-mount-head">
                          <code title={disk.mount}>{disk.mount}</code>
                          <span className="ed-badge subtle">{disk.filesystem}</span>
                          <b>{formatPercent(disk.used_percent)}</b>
                        </div>
                        <Progress value={disk.used_percent} label={`${disk.mount} 磁盘使用率`} />
                        <div className="ed-nas-mount-foot">
                          <span>已用 {formatBytes(disk.used_bytes)}</span>
                          <span>总量 {formatBytes(disk.total_bytes)}</span>
                          <span>可用 {formatBytes(disk.available_bytes || free)}</span>
                        </div>
                      </li>
                    );
                  })}
                </ul>
              )}
            </Widget>

            <Widget
              sprite="network"
              title="网卡状态"
              status={latest.network.length === 0 ? 'unavailable' : 'ok'}
              badge={latest.network.length > 0 ? `${latest.network.length} 个接口` : '不可用'}
            >
              {latest.network.length === 0 ? (
                <div className="ed-empty small">未发现可统计的网络接口。</div>
              ) : (
                <ul className="ed-nas-iface-list">
                  {latest.network.map((item) => {
                    const rx = visible.map(
                      (sample) => sample.network.find((entry) => entry.name === item.name)?.rx_bytes_per_second ?? null,
                    );
                    const tx = visible.map(
                      (sample) => sample.network.find((entry) => entry.name === item.name)?.tx_bytes_per_second ?? null,
                    );
                    return (
                      <li className="ed-nas-iface" key={item.name}>
                        <div className="ed-nas-iface-head">
                          <strong>{item.name}</strong>
                          <span className="ed-nas-iface-ip" title={item.addresses.join('、') || '未分配 IP'}>
                            {primaryAddress(item.addresses) ?? 'IP 未分配'}
                            {item.addresses.length > 1 && <em>+{item.addresses.length - 1}</em>}
                          </span>
                        </div>
                        <div className="ed-nas-iface-rates">
                          <span>接收 ↓ {formatRate(item.rx_bytes_per_second)}</span>
                          <span>发送 ↑ {formatRate(item.tx_bytes_per_second)}</span>
                        </div>
                        <Sparkline values={rx} height={34} color="var(--ed-primary)" label={`${item.name} 接收趋势`} showValue={false} />
                        <Sparkline values={tx} height={34} color="var(--ed-accent)" label={`${item.name} 发送趋势`} showValue={false} />
                      </li>
                    );
                  })}
                </ul>
              )}
            </Widget>

            <Widget sprite="system" title="系统信息" badge={latest.arch || '只读'}>
              <dl className="ed-nas-info-list">
                <div>
                  <dt>主机名</dt>
                  <dd>{latest.hostname || '—'}</dd>
                </div>
                <div>
                  <dt>系统架构</dt>
                  <dd>{latest.arch || '—'}</dd>
                </div>
                <div>
                  <dt>内核版本</dt>
                  <dd title={latest.kernel || undefined}>{latest.kernel || '—'}</dd>
                </div>
                <div>
                  <dt>运行时间</dt>
                  <dd>{formatUptime(latest.uptime_seconds)}</dd>
                </div>
                <div>
                  <dt>启动于</dt>
                  <dd>{formatDateTime(bootMoment(latest))}</dd>
                </div>
                <div>
                  <dt>采样间隔</dt>
                  <dd>{intervalMs} ms</dd>
                </div>
              </dl>
            </Widget>

            <ServiceStatusWidget system={system} systemError={systemError} latest={latest} intervalMs={intervalMs} now={now} />
          </section>

          <section className="ed-chart-grid">
            <article className="ed-panel ed-chart-panel">
              <header className="ed-panel-head">
                <h2>CPU 使用率趋势</h2>
                <span className="ed-panel-note">{terminals.length} 个终端会话</span>
              </header>
              <ul className="ed-legend">
                <li>
                  <span className="ed-dot primary" />
                  CPU 使用率
                </li>
              </ul>
              <MetricChart
                series={[{ label: 'CPU 使用率', color: 'var(--ed-primary)', values: cpuValues, fill: true }]}
                timestamps={timestamps}
                max={100}
                formatY={(value) => `${Math.round(value)}%`}
              />
            </article>

            <article className="ed-panel ed-chart-panel">
              <header className="ed-panel-head">
                <h2>内存使用趋势</h2>
                <span className="ed-panel-note">
                  已用 {latest.memory ? formatBytes(latest.memory.used_kb * 1024) : '—'} / 总量{' '}
                  {latest.memory ? formatBytes(latest.memory.total_kb * 1024) : '—'}
                </span>
              </header>
              <ul className="ed-legend">
                <li>
                  <span className="ed-dot accent" />
                  已使用
                </li>
                <li>
                  <span className="ed-dot muted" />
                  可用
                </li>
              </ul>
              <MetricChart
                stacked
                series={[
                  { label: '已使用', color: 'var(--ed-accent)', values: memoryUsed },
                  { label: '可用', color: 'var(--ed-chart-muted)', values: memoryFree },
                ]}
                timestamps={timestamps}
                max={100}
                formatY={(value) => `${Math.round(value)}%`}
              />
            </article>

            <article className="ed-panel ed-chart-panel">
              <header className="ed-panel-head">
                <h2>网络收发趋势（{networkScale.unit}）</h2>
                <span className="ed-panel-note">方向：接收 ↓ / 发送 ↑</span>
              </header>
              <ul className="ed-legend">
                <li>
                  <span className="ed-dot primary" />
                  接收 {formatRate(rxTotal)}
                </li>
                <li>
                  <span className="ed-dot accent" />
                  发送 {formatRate(txTotal)}
                </li>
              </ul>
              <MetricChart
                series={[
                  { label: '接收', color: 'var(--ed-primary)', values: rxValues, fill: true },
                  { label: '发送', color: 'var(--ed-accent)', values: txValues, fill: true },
                ]}
                timestamps={timestamps}
                formatY={(value) => (value / networkScale.divisor).toFixed(networkScale.divisor === 1 ? 0 : 1)}
              />
            </article>
          </section>

          <article className="ed-panel ed-nas-device-status">
            <header className="ed-panel-head">
              <h2>设备状态</h2>
              <img className="ed-device-art" src={asset('illustrations/edge-device.svg')} alt="" width="84" height="72" />
            </header>
            <div className="ed-device-banner">
              <span className={`ed-status-pill ${status.tone}`}>
                <span className="ed-status-dot" />
                {status.text}
              </span>
              <p>{status.detail}</p>
            </div>
            <p className="ed-panel-note">
              数据源：/api/system/summary 与 /ws/metrics。缺失字段显示“—”，不会用 0 或原型演示值替代。
            </p>
          </article>
        </>
      )}
    </div>
  );
}
