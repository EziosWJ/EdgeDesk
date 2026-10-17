// @vitest-environment happy-dom
import { act } from 'react';
import { createRoot, type Root } from 'react-dom/client';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { DashboardView } from './DashboardView';
import { sessionStore } from '../../stores/sessionStore';
import type { MetricsSample } from '../../types';

const KIB = 1024;
const GIB = KIB * KIB * KIB;

const ROOT_DISK = {
  mount: '/',
  filesystem: 'ext4',
  total_bytes: 100 * GIB,
  used_bytes: 38 * GIB,
  available_bytes: 62 * GIB,
  used_percent: 38,
};

/** Shaped like the real /api/system payload; values are deterministic for assertions. */
function sample(index: number, ts: number, disks: MetricsSample['disks'] = [ROOT_DISK]): MetricsSample {
  return {
    ts,
    uptime_seconds: 93_600 + index,
    hostname: 'edge-001',
    kernel: '6.1.28',
    arch: 'aarch64',
    cpu: { cores: 4, usage_percent: 30 + index / 10, load1: 0.5, load5: 0.4, load15: 0.3 },
    memory: {
      total_kb: 1_048_576,
      available_kb: 524_288,
      used_kb: 524_288,
      free_kb: 100_000,
      buffers_kb: 1_000,
      cached_kb: 2_000,
      swap_total_kb: 0,
      swap_free_kb: 0,
      used_percent: 50,
    },
    disks,
    network: [
      {
        name: 'eth0',
        addresses: ['192.0.2.10'],
        rx_bytes_per_second: 1024,
        tx_bytes_per_second: 512,
        rx_bytes_total: 1,
        tx_bytes_total: 1,
      },
    ],
    unavailable: [],
  };
}

function prime(count: number, lastTs: number, disks?: MetricsSample['disks']): void {
  const samples = Array.from({ length: count }, (_, index) =>
    sample(index, lastTs - (count - 1 - index) * 1000, disks),
  );
  sessionStore.setMetricsHistory(samples, 1000);
}

describe('DashboardView', () => {
  let container: HTMLDivElement;
  let root: Root;

  beforeEach(() => {
    // No network in unit tests: history priming is covered by the empty-store case.
    vi.stubGlobal('fetch', () => Promise.reject(new Error('offline')));
    sessionStore.setMetricsHistory([], 1000);
    document.body.innerHTML = '';
    container = document.createElement('div');
    document.body.appendChild(container);
    root = createRoot(container);
  });

  afterEach(() => {
    vi.unstubAllGlobals();
  });

  const render = () => {
    act(() => {
      root.render(<DashboardView />);
    });
  };

  it('renders the six KPI cards and three chart panels from real samples', () => {
    prime(120, Date.now());
    render();
    expect(container.querySelectorAll('.ed-kpi')).toHaveLength(6);
    expect(container.querySelector('.ed-kpi h2')?.textContent).toBe('CPU 使用率');
    expect(container.querySelectorAll('.ed-chart')).toHaveLength(3);
    expect(container.textContent).toContain('设备概览');
    expect(container.textContent).toContain('网络收发（KiB/s）');
    expect(container.textContent).toContain('已使用');
    expect(container.textContent).toContain('下行（接收）');
    expect(container.textContent).toContain('192.0.2.10');
  });

  it('puts multi-value KPI details on separate rows instead of combining them', () => {
    prime(120, Date.now());
    render();
    const memoryCard = [...container.querySelectorAll('.ed-kpi')]
      .find((card) => card.querySelector('h2')?.textContent === '内存使用率');
    expect(memoryCard).toBeDefined();
    const labels = [...memoryCard!.querySelectorAll('.ed-kpi-rows span')].map((span) => span.textContent);
    expect(labels).toEqual(['已用', '可用', 'Swap 空闲']);
  });

  it('lists every mount with used/unused progress bars instead of a single donut', () => {
    const disks = [
      ROOT_DISK,
      {
        mount: '/data',
        filesystem: 'xfs',
        total_bytes: 200 * GIB,
        used_bytes: 50 * GIB,
        available_bytes: 150 * GIB,
        used_percent: 25,
      },
    ];
    prime(120, Date.now(), disks);
    render();
    const items = [...container.querySelectorAll('.ed-disk-item')];
    expect(items).toHaveLength(2);
    expect(items.map((item) => item.querySelector('.ed-disk-mount code')?.textContent)).toEqual(['/', '/data']);
    expect(items.map((item) => item.querySelector<HTMLElement>('.ed-disk-bar-used')?.style.width)).toEqual(['38%', '25%']);
    expect(items[0].textContent).toContain('已使用 38.0 GiB · 38.0%');
    expect(items[0].textContent).toContain('未使用 62.0 GiB · 62.0%');
    expect(items[1].textContent).toContain('已使用 50.0 GiB · 25.0%');
    expect(items[1].textContent).toContain('未使用 150.0 GiB · 75.0%');
    expect(items[0].querySelector('.ed-disk-bar')?.getAttribute('aria-valuenow')).toBe('38');
    expect(container.querySelector('.ed-donut')).toBeNull();
    expect(container.querySelectorAll('.ed-mount-chip')).toHaveLength(0);
  });

  it('uses one shared time-range selector at the top of the dashboard', () => {
    prime(120, Date.now());
    render();
    const selects = [...container.querySelectorAll<HTMLSelectElement>('select')];
    expect(selects).toHaveLength(1);
    expect(selects[0].getAttribute('aria-label')).toBe('全部曲线时间范围');
    expect(container.querySelector('.ed-dashboard-meta .ed-range-control select')).toBe(selects[0]);
    expect(container.querySelectorAll('.ed-chart-panel .ed-range-select')).toHaveLength(0);
  });

  it('marks the device online while samples are fresh and interrupts when stale', () => {
    prime(120, Date.now());
    render();
    expect(container.querySelector('.ed-status-pill.online')?.textContent).toContain('在线');

    prime(120, Date.now() - 3600_000);
    render();
    expect(container.querySelector('.ed-status-pill.warning')?.textContent).toContain('数据中断');
  });

  it('waits for the first sample without faking values', () => {
    render();
    expect(container.querySelectorAll('.ed-kpi')).toHaveLength(0);
    expect(container.textContent).toContain('等待指标数据');
  });
});
