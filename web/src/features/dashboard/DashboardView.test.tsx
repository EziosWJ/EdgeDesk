// @vitest-environment happy-dom
import { act } from 'react';
import { createRoot, type Root } from 'react-dom/client';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { DashboardView } from './DashboardView';
import { sessionStore } from '../../stores/sessionStore';
import type { MetricsSample } from '../../types';

const KIB = 1024;
const GIB = KIB * KIB * KIB;

/** Shaped like the real /api/system payload; values are deterministic for assertions. */
function sample(index: number, ts: number): MetricsSample {
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
    disks: [
      {
        mount: '/',
        filesystem: 'ext4',
        total_bytes: 100 * GIB,
        used_bytes: 38 * GIB,
        available_bytes: 62 * GIB,
        used_percent: 38,
      },
    ],
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

function prime(count: number, lastTs: number): void {
  const samples = Array.from({ length: count }, (_, index) => sample(index, lastTs - (count - 1 - index)));
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
    prime(120, Date.now() / 1000);
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

  it('shows the disk donut, legend and mount chips for the real mount points', () => {
    prime(120, Date.now() / 1000);
    render();
    expect(container.querySelector('.ed-donut-center strong')?.textContent).toBe('38%');
    const chips = [...container.querySelectorAll<HTMLButtonElement>('.ed-mount-chip')];
    expect(chips.map((chip) => chip.textContent)).toEqual(['/38%']);
    expect(chips[0].getAttribute('aria-pressed')).toBe('true');
  });

  it('marks the device online while samples are fresh and interrupts when stale', () => {
    prime(120, Date.now() / 1000);
    render();
    expect(container.querySelector('.ed-status-pill.online')?.textContent).toContain('在线');

    prime(120, Date.now() / 1000 - 3600);
    render();
    expect(container.querySelector('.ed-status-pill.warning')?.textContent).toContain('数据中断');
  });

  it('waits for the first sample without faking values', () => {
    render();
    expect(container.querySelectorAll('.ed-kpi')).toHaveLength(0);
    expect(container.textContent).toContain('等待指标数据');
  });
});
