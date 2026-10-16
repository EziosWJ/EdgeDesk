import { useEffect } from 'react';
import { Sparkline } from '../../components/Sparkline';
import { api, formatBytes, formatPercent, formatRate, formatUptime } from '../../services/api';
import { sessionStore, useMetricsSamples } from '../../stores/sessionStore';

const asset = (path: string) => `${import.meta.env.BASE_URL}assets/${path}`;

export function DashboardView() {
  const samples = useMetricsSamples();
  const latest = samples.length > 0 ? samples[samples.length - 1] : null;

  useEffect(() => {
    // Prime the chart with the server-side history so the dashboard is useful
    // immediately (the WebSocket then keeps it updated).
    if (sessionStore.metricsSnapshot().length > 0) return;
    api
      .systemSummary(120)
      .then((data) => {
        sessionStore.setMetricsHistory(data.history ?? [data.sample], data.interval_ms);
      })
      .catch(() => undefined);
  }, []);

  const unavailable = new Set(latest?.unavailable ?? []);
  const cpuValues = samples.map((sample) => sample.cpu.usage_percent);
  const memoryValues = samples.map((sample) => (sample.memory ? sample.memory.used_percent : null));
  const rxValues = samples.map((sample) =>
    sample.network.length > 0 ? sample.network.reduce((sum, item) => sum + item.rx_bytes_per_second, 0) : null,
  );
  const txValues = samples.map((sample) =>
    sample.network.length > 0 ? sample.network.reduce((sum, item) => sum + item.tx_bytes_per_second, 0) : null,
  );

  return (
    <div className="ed-view ed-dashboard">
      <div className="ed-view-header">
        <div>
          <h1>设备仪表盘</h1>
          <p>
            {latest
              ? `${latest.hostname || '本机'} · ${latest.arch || '未知架构'} · 内核 ${latest.kernel || '未知'}`
              : '正在采集设备指标…'}
          </p>
        </div>
        <div className="ed-toolbar">
          <span className="ed-live-dot" />
          <span className="ed-live-label">
            实时 · {samples.length} 个样本 / 窗口 {Math.round((samples.length * sessionStore.intervalMs) / 1000)} 秒
          </span>
        </div>
      </div>

      <section className="ed-card-grid">
        <article className="ed-panel ed-metric-card">
          <header>
            <img src={asset('icons/cpu.svg')} alt="" />
            <div>
              <h2>CPU</h2>
              <span>{latest ? `${latest.cpu.cores} 核` : '—'}</span>
            </div>
          </header>
          {unavailable.has('cpu_usage') && <span className="ed-badge warning">不可用</span>}
          <Sparkline values={cpuValues} color="var(--ed-primary)" max={100} unit="%" label="cpu" />
          <dl className="ed-metric-list">
            <div>
              <dt>使用率</dt>
              <dd>{formatPercent(latest?.cpu.usage_percent ?? null)}</dd>
            </div>
            <div>
              <dt>load 1/5/15</dt>
              <dd>
                {latest?.cpu.load1 === null || latest === null
                  ? '—'
                  : `${latest.cpu.load1.toFixed(2)} / ${latest.cpu.load5?.toFixed(2)} / ${latest.cpu.load15?.toFixed(2)}`}
              </dd>
            </div>
          </dl>
        </article>

        <article className="ed-panel ed-metric-card">
          <header>
            <img src={asset('icons/memory.svg')} alt="" />
            <div>
              <h2>内存</h2>
              <span>{latest?.memory ? formatBytes(latest.memory.total_kb * 1024) : '—'}</span>
            </div>
          </header>
          {(!latest?.memory || unavailable.has('memory')) && <span className="ed-badge warning">不可用</span>}
          <Sparkline values={memoryValues} color="var(--ed-accent)" max={100} unit="%" label="memory" />
          <dl className="ed-metric-list">
            <div>
              <dt>已用</dt>
              <dd>
                {latest?.memory ? `${formatBytes(latest.memory.used_kb * 1024)}（${formatPercent(latest.memory.used_percent)}）` : '—'}
              </dd>
            </div>
            <div>
              <dt>可用 / Swap</dt>
              <dd>
                {latest?.memory
                  ? `${formatBytes(latest.memory.available_kb * 1024)} / ${formatBytes(latest.memory.swap_free_kb * 1024)}`
                  : '—'}
              </dd>
            </div>
          </dl>
        </article>

        <article className="ed-panel ed-metric-card">
          <header>
            <img src={asset('icons/network.svg')} alt="" />
            <div>
              <h2>网络</h2>
              <span>{latest?.network.length ? `${latest.network.length} 个接口` : '—'}</span>
            </div>
          </header>
          {unavailable.has('network_rate_initial') && <span className="ed-badge">采样中…</span>}
          <Sparkline values={rxValues} color="var(--ed-accent)" unit="" label="rx" />
          <Sparkline values={txValues} color="var(--ed-warning)" unit="" label="tx" />
          <dl className="ed-metric-list">
            <div>
              <dt>接收 / 发送</dt>
              <dd>
                {latest?.network.length
                  ? `${formatRate(latest.network.reduce((sum, item) => sum + item.rx_bytes_per_second, 0))} / ${formatRate(
                      latest.network.reduce((sum, item) => sum + item.tx_bytes_per_second, 0),
                    )}`
                  : '—'}
              </dd>
            </div>
          </dl>
          <table className="ed-mini-table">
            <tbody>
              {latest?.network.map((item) => (
                <tr key={item.name}>
                  <td>{item.name}</td>
                  <td>↓ {formatRate(item.rx_bytes_per_second)}</td>
                  <td>↑ {formatRate(item.tx_bytes_per_second)}</td>
                </tr>
              ))}
            </tbody>
          </table>
        </article>

        <article className="ed-panel ed-metric-card">
          <header>
            <img src={asset('icons/uptime.svg')} alt="" />
            <div>
              <h2>运行时间</h2>
              <span>{latest ? formatUptime(latest.uptime_seconds) : '—'}</span>
            </div>
          </header>
          <dl className="ed-metric-list">
            <div>
              <dt>主机名</dt>
              <dd>{latest?.hostname || '—'}</dd>
            </div>
            <div>
              <dt>内核 / 架构</dt>
              <dd>{latest ? `${latest.kernel || '—'} / ${latest.arch || '—'}` : '—'}</dd>
            </div>
            <div>
              <dt>采样间隔</dt>
              <dd>{sessionStore.intervalMs} ms</dd>
            </div>
          </dl>
        </article>
      </section>

      <section className="ed-panel ed-disk-panel">
        <header className="ed-disk-header">
          <div>
            <h2>磁盘容量</h2>
            <span>真实块设备挂载点，低频刷新；缺失接口显示为不可用而不是 0</span>
          </div>
          {unavailable.has('disk') && <span className="ed-badge warning">不可用</span>}
        </header>
        <table className="ed-table compact">
          <thead>
            <tr>
              <th>挂载点</th>
              <th>文件系统</th>
              <th>总容量</th>
              <th>已用</th>
              <th>可用</th>
              <th>使用率</th>
            </tr>
          </thead>
          <tbody>
            {latest?.disks.map((disk) => (
              <tr key={disk.mount}>
                <td>
                  <code>{disk.mount}</code>
                </td>
                <td>{disk.filesystem}</td>
                <td>{formatBytes(disk.total_bytes)}</td>
                <td>{formatBytes(disk.used_bytes)}</td>
                <td>{formatBytes(disk.available_bytes)}</td>
                <td className="ed-usage-cell">
                  <div className="ed-usage-bar">
                    <span style={{ width: `${Math.min(100, disk.used_percent)}%` }} />
                  </div>
                  {formatPercent(disk.used_percent)}
                </td>
              </tr>
            ))}
            {(!latest || latest.disks.length === 0) && (
              <tr>
                <td colSpan={6} className="ed-table-empty">
                  <img src={asset('illustrations/monitor-unavailable.svg')} alt="" />
                  <span>未检测到可统计的块设备挂载点</span>
                </td>
              </tr>
            )}
          </tbody>
        </table>
      </section>
    </div>
  );
}
