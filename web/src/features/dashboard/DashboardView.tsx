import { useEffect, useState, type ReactNode } from 'react';
import type { LucideIcon } from 'lucide-react';
import { Activity, CircuitBoard, Cpu, HardDrive, MemoryStick, Timer } from 'lucide-react';
import { Icon } from '../../components/Icon';
import { DonutChart } from '../../components/DonutChart';
import { MetricChart } from '../../components/MetricChart';
import { Sparkline } from '../../components/Sparkline';
import { api, formatBytes, formatPercent, formatRate, formatUptime } from '../../services/api';
import { sessionStore, useMetricsSamples } from '../../stores/sessionStore';
import { bootMoment, deltaTone, formatDelta, formatDateTime, kpiPercent, rateScale, sliceWindow, usageDelta } from './chartUtils';

const asset = (path: string) => `${import.meta.env.BASE_URL}assets/${path}`;

/** Wall clock for the page header (real time, not sample time). */
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

function KpiCard({ icon, title, badge, children }: { icon: LucideIcon; title: string; badge?: ReactNode; children: ReactNode }) {
  return (
    <article className="ed-panel ed-kpi">
      <header>
        <span className="ed-icon-tile">
          <Icon icon={icon} size={19} />
        </span>
        <h2>{title}</h2>
        {badge && <span className="ed-kpi-badge">{badge}</span>}
      </header>
      {children}
    </article>
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

function ProgressBar({ value, tone }: { value: number | null; tone: 'primary' | 'accent' }) {
  const width = value === null ? 0 : Math.min(100, Math.max(0, value));
  return (
    <div className="ed-progress">
      <span className={`tone-${tone}`} style={{ width: `${width}%` }} />
    </div>
  );
}

function RangeSelect({ value, onChange }: { value: number; onChange: (seconds: number) => void }) {
  return (
    <select className="ed-range-select" value={value} onChange={(event) => onChange(Number(event.target.value))} aria-label="曲线时间范围">
      <option value={60}>1 分钟</option>
      <option value={300}>5 分钟</option>
    </select>
  );
}

export function DashboardView() {
  const samples = useMetricsSamples();
  const intervalMs = sessionStore.intervalMs;
  const [rangeSeconds, setRangeSeconds] = useState(60);
  const [selectedMount, setSelectedMount] = useState<string | null>(null);
  const now = useClock();

  useEffect(() => {
    // Prime the charts with the server-side history so the dashboard is useful
    // immediately (the WebSocket then keeps it updated).
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

  const cpuValues = visible.map((sample) => sample.cpu.usage_percent);
  const memoryUsed = visible.map((sample) => (sample.memory ? sample.memory.used_percent : null));
  const memoryFree = memoryUsed.map((value) => (value === null ? null : Math.max(0, 100 - value)));
  const rxValues = visible.map((sample) =>
    sample.network.length > 0 ? sample.network.reduce((sum, item) => sum + item.rx_bytes_per_second, 0) : null,
  );
  const txValues = visible.map((sample) =>
    sample.network.length > 0 ? sample.network.reduce((sum, item) => sum + item.tx_bytes_per_second, 0) : null,
  );
  const throughput = rxValues.map((rx, index) => {
    const tx = txValues[index];
    return rx === null || tx === null ? null : rx + tx;
  });
  const networkScale = rateScale([...rxValues, ...txValues]);

  const sumRates = (pick: (item: { rx_bytes_per_second: number; tx_bytes_per_second: number }) => number): number | null =>
    latest && latest.network.length > 0 ? latest.network.reduce((sum, item) => sum + pick(item), 0) : null;
  const networkTotal = latest && latest.network.length > 0
    ? latest.network.reduce((sum, item) => sum + item.rx_bytes_per_second + item.tx_bytes_per_second, 0)
    : null;

  const disks = latest?.disks ?? [];
  const primaryDisk = disks.find((disk) => disk.mount === '/') ?? disks[0] ?? null;
  const activeDisk = (selectedMount ? disks.find((disk) => disk.mount === selectedMount) : null) ?? primaryDisk;
  const diskValues = primaryDisk
    ? visible.map((sample) => sample.disks.find((disk) => disk.mount === primaryDisk.mount)?.used_percent ?? null)
    : [];
  const loadText =
    latest && latest.cpu.load1 !== null
      ? `${latest.cpu.load1.toFixed(2)} / ${latest.cpu.load5?.toFixed(2) ?? '—'} / ${latest.cpu.load15?.toFixed(2) ?? '—'}`
      : '—';

  const hardUnavailable = [...unavailable].filter((code) => code !== 'network_rate_initial');
  const staleSeconds = latest ? Math.max(0, now.getTime() / 1000 - latest.ts) : null;
  const status = !latest
    ? { tone: 'warning', text: '等待数据', detail: '尚未收到指标采样，正在连接服务。' }
    : staleSeconds !== null && staleSeconds > Math.max(5, (intervalMs * 3) / 1000)
      ? { tone: 'warning', text: '数据中断', detail: '最近的采样已超过 3 个周期未更新，请检查 Metrics WebSocket 连接。' }
      : hardUnavailable.length > 0
        ? { tone: 'warning', text: '部分不可用', detail: `以下指标不可用：${hardUnavailable.join('、')}。` }
        : { tone: 'online', text: '在线', detail: '设备运行正常，指标持续采集中。' };

  return (
    <div className="ed-view ed-dashboard">
      <div className="ed-view-header">
        <div>
          <h1>设备概览</h1>
          <p>实时监控设备运行状态，快速掌握系统资源使用情况。</p>
        </div>
        <div className="ed-toolbar ed-dashboard-meta">
          <span className="ed-clock">{clockText(now)}</span>
          <span className="ed-live">
            <span className="ed-live-dot" />
            <span className="ed-live-label">
              实时 · {samples.length} 个样本 / 窗口 {Math.round((samples.length * intervalMs) / 1000)} 秒
            </span>
          </span>
          <span className="ed-badge subtle">采样 {intervalMs} ms</span>
        </div>
      </div>

      {!latest && (
        <div className="ed-panel ed-empty small">
          <img src={asset('illustrations/monitor-unavailable.svg')} alt="" />
          <p>等待指标数据…首次采样到达后自动刷新，不使用模拟数值。</p>
        </div>
      )}

      {latest && (
        <>
          <section className="ed-kpi-grid">
            <KpiCard
              icon={Cpu}
              title="CPU 使用率"
              badge={unavailable.has('cpu_usage') ? <span className="ed-badge warning">不可用</span> : undefined}
            >
              <div className="ed-kpi-value-row">
                <strong className="ed-kpi-value">{kpiPercent(latest.cpu.usage_percent)}</strong>
                <DeltaChip delta={usageDelta(cpuValues, lookback)} />
              </div>
              <ProgressBar value={latest.cpu.usage_percent} tone="primary" />
              <div className="ed-kpi-rows">
                <div>
                  <span>核心</span>
                  <b>{latest.cpu.cores} 核</b>
                </div>
                <div>
                  <span>负载 1/5/15</span>
                  <b>{loadText}</b>
                </div>
              </div>
            </KpiCard>

            <KpiCard
              icon={MemoryStick}
              title="内存使用率"
              badge={!latest.memory || unavailable.has('memory') ? <span className="ed-badge warning">不可用</span> : undefined}
            >
              <div className="ed-kpi-value-row">
                <strong className="ed-kpi-value">{kpiPercent(latest.memory?.used_percent ?? null)}</strong>
                <DeltaChip delta={usageDelta(memoryUsed, lookback)} />
              </div>
              <ProgressBar value={latest.memory?.used_percent ?? null} tone="primary" />
              <div className="ed-kpi-rows">
                <div>
                  <span>已用</span>
                  <b>{latest.memory ? formatBytes(latest.memory.used_kb * 1024) : '—'}</b>
                </div>
                <div>
                  <span>可用 / Swap 空闲</span>
                  <b>
                    {latest.memory
                      ? `${formatBytes(latest.memory.available_kb * 1024)} / ${formatBytes(latest.memory.swap_free_kb * 1024)}`
                      : '—'}
                  </b>
                </div>
              </div>
            </KpiCard>

            <KpiCard
              icon={HardDrive}
              title="磁盘使用率"
              badge={!primaryDisk ? <span className="ed-badge warning">不可用</span> : undefined}
            >
              <div className="ed-kpi-value-row">
                <strong className="ed-kpi-value">{kpiPercent(primaryDisk?.used_percent ?? null)}</strong>
                <DeltaChip delta={usageDelta(diskValues, lookback)} />
              </div>
              <ProgressBar value={primaryDisk?.used_percent ?? null} tone="accent" />
              <div className="ed-kpi-rows">
                <div>
                  <span>挂载点</span>
                  <b>{primaryDisk ? `${primaryDisk.mount}（${primaryDisk.filesystem}）` : '—'}</b>
                </div>
                <div>
                  <span>已用 / 总量</span>
                  <b>{primaryDisk ? `${formatBytes(primaryDisk.used_bytes)} / ${formatBytes(primaryDisk.total_bytes)}` : '—'}</b>
                </div>
              </div>
            </KpiCard>

            <KpiCard
              icon={Activity}
              title="网络吞吐"
              badge={unavailable.has('network_rate_initial') ? <span className="ed-badge">采样中…</span> : undefined}
            >
              <div className="ed-kpi-value-row">
                <strong className="ed-kpi-value">{formatRate(networkTotal)}</strong>
              </div>
              <Sparkline values={throughput} color="var(--ed-accent)" height={54} label="throughput" showValue={false} />
              <div className="ed-kpi-rows">
                <div>
                  <span>↑ 发送 / ↓ 接收</span>
                  <b>
                    {formatRate(sumRates((item) => item.tx_bytes_per_second))} / {formatRate(sumRates((item) => item.rx_bytes_per_second))}
                  </b>
                </div>
                <div>
                  <span>接口</span>
                  <b>{latest.network.length > 0 ? `${latest.network.length} 个` : '—'}</b>
                </div>
              </div>
            </KpiCard>

            <KpiCard icon={Timer} title="系统运行时间">
              <div className="ed-kpi-value-row">
                <strong className="ed-kpi-value ed-kpi-value-sm">{formatUptime(latest.uptime_seconds)}</strong>
              </div>
              <div className="ed-kpi-rows">
                <div>
                  <span>自</span>
                  <b>{formatDateTime(bootMoment(latest))}</b>
                </div>
                <div>
                  <span>主机名</span>
                  <b>{latest.hostname || '—'}</b>
                </div>
              </div>
            </KpiCard>

            <KpiCard icon={CircuitBoard} title="CPU 架构 / 内核">
              <div className="ed-kpi-value-row">
                <strong className="ed-kpi-value ed-kpi-value-sm">{latest.arch || '—'}</strong>
              </div>
              <div className="ed-kpi-rows">
                <div>
                  <span>内核版本</span>
                  <b>{latest.kernel || '—'}</b>
                </div>
                <div>
                  <span>采样间隔</span>
                  <b>{intervalMs} ms</b>
                </div>
              </div>
            </KpiCard>
          </section>

          <section className="ed-chart-grid">
            <article className="ed-panel ed-chart-panel">
              <header className="ed-panel-head">
                <h2>CPU 使用率</h2>
                <RangeSelect value={rangeSeconds} onChange={setRangeSeconds} />
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
                <h2>内存使用率</h2>
                <RangeSelect value={rangeSeconds} onChange={setRangeSeconds} />
              </header>
              <ul className="ed-legend">
                <li>
                  <span className="ed-dot accent" />
                  已使用 {latest.memory ? formatBytes(latest.memory.used_kb * 1024) : '—'}
                </li>
                <li>
                  <span className="ed-dot muted" />
                  可用 {latest.memory ? formatBytes(latest.memory.available_kb * 1024) : '—'}
                </li>
              </ul>
              <MetricChart
                stacked
                series={[
                  { label: '已使用', color: 'var(--ed-accent)', values: memoryUsed },
                  { label: '可用', color: '#5B7392', values: memoryFree },
                ]}
                timestamps={timestamps}
                max={100}
                formatY={(value) => `${Math.round(value)}%`}
              />
            </article>

            <article className="ed-panel ed-chart-panel">
              <header className="ed-panel-head">
                <h2>网络收发（{networkScale.unit}）</h2>
                <RangeSelect value={rangeSeconds} onChange={setRangeSeconds} />
              </header>
              <ul className="ed-legend">
                <li>
                  <span className="ed-dot primary" />
                  下行（接收）{formatRate(sumRates((item) => item.rx_bytes_per_second))}
                </li>
                <li>
                  <span className="ed-dot accent" />
                  上行（发送）{formatRate(sumRates((item) => item.tx_bytes_per_second))}
                </li>
              </ul>
              <MetricChart
                series={[
                  { label: '下行（接收）', color: 'var(--ed-primary)', values: rxValues, fill: true },
                  { label: '上行（发送）', color: 'var(--ed-accent)', values: txValues, fill: true },
                ]}
                timestamps={timestamps}
                formatY={(value) => (value / networkScale.divisor).toFixed(networkScale.divisor === 1 ? 0 : 1)}
              />
              <table className="ed-mini-table">
                <tbody>
                  {latest.network.map((item) => (
                    <tr key={item.name}>
                      <td>{item.name}</td>
                      <td>↓ {formatRate(item.rx_bytes_per_second)}</td>
                      <td>↑ {formatRate(item.tx_bytes_per_second)}</td>
                    </tr>
                  ))}
                </tbody>
              </table>
            </article>
          </section>

          <section className="ed-bottom-grid">
            <article className="ed-panel ed-chart-panel">
              <header className="ed-panel-head">
                <h2>磁盘使用情况</h2>
                <span className="ed-panel-note">{disks.length > 0 ? `${disks.length} 个挂载点` : '—'}</span>
              </header>
              {activeDisk ? (
                <>
                  <div className="ed-disk-body">
                    <DonutChart percent={activeDisk.used_percent} />
                    <div className="ed-disk-detail">
                      <div className="ed-disk-path">
                        <code>{activeDisk.mount}</code>
                        <span className="ed-badge subtle">{activeDisk.filesystem}</span>
                      </div>
                      <div className="ed-disk-amount">
                        {formatBytes(activeDisk.used_bytes)} / {formatBytes(activeDisk.total_bytes)}
                      </div>
                      <ul className="ed-disk-legend">
                        <li>
                          <span className="ed-dot primary" />
                          已使用
                          <b>
                            {formatBytes(activeDisk.used_bytes)} · {formatPercent(activeDisk.used_percent)}
                          </b>
                        </li>
                        <li>
                          <span className="ed-dot accent" />
                          可用
                          <b>
                            {formatBytes(activeDisk.available_bytes)} · {formatPercent(100 - activeDisk.used_percent)}
                          </b>
                        </li>
                        <li>
                          <span className="ed-dot muted" />
                          总容量
                          <b>{formatBytes(activeDisk.total_bytes)}</b>
                        </li>
                      </ul>
                    </div>
                  </div>
                  <div className="ed-mount-list" role="group" aria-label="切换挂载点">
                    {disks.map((disk) => (
                      <button
                        key={disk.mount}
                        type="button"
                        aria-pressed={disk.mount === activeDisk.mount}
                        className={`ed-mount-chip ${disk.mount === activeDisk.mount ? 'active' : ''}`}
                        onClick={() => setSelectedMount(disk.mount)}
                      >
                        <code>{disk.mount}</code>
                        <span>{formatPercent(disk.used_percent, 0)}</span>
                      </button>
                    ))}
                  </div>
                </>
              ) : (
                <div className="ed-empty small">
                  <img src={asset('illustrations/monitor-unavailable.svg')} alt="" />
                  <p>{unavailable.has('disk') ? '磁盘统计不可用：未找到可统计的块设备挂载点。' : '正在采集磁盘容量…'}</p>
                </div>
              )}
            </article>

            <article className="ed-panel ed-chart-panel">
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
              <dl className="ed-info-list">
                <div>
                  <dt>主机名</dt>
                  <dd>{latest.hostname || '—'}</dd>
                </div>
                <div>
                  <dt>内核版本</dt>
                  <dd>{latest.kernel || '—'}</dd>
                </div>
                <div>
                  <dt>系统架构</dt>
                  <dd>{latest.arch || '—'}</dd>
                </div>
                <div>
                  <dt>CPU 核心</dt>
                  <dd>{latest.cpu.cores} 核</dd>
                </div>
                <div>
                  <dt>运行时间</dt>
                  <dd>{formatUptime(latest.uptime_seconds)}</dd>
                </div>
                <div>
                  <dt>采样间隔</dt>
                  <dd>{intervalMs} ms</dd>
                </div>
              </dl>
            </article>
          </section>
        </>
      )}
    </div>
  );
}
