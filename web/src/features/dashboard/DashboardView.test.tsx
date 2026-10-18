// @vitest-environment happy-dom
import { act } from 'react';
import { createRoot, type Root } from 'react-dom/client';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { DashboardView } from './DashboardView';
import { sessionStore } from '../../stores/sessionStore';
import type { MetricsSample, NetworkSample } from '../../types';

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

const DATA_DISK = {
  mount: '/data',
  filesystem: 'xfs',
  total_bytes: 200 * GIB,
  used_bytes: 50 * GIB,
  available_bytes: 150 * GIB,
  used_percent: 25,
};

type SampleOverrides = {
  cpuUsage?: number | null;
  memory?: MetricsSample['memory'];
  disks?: MetricsSample['disks'];
  network?: NetworkSample[];
  unavailable?: string[];
};

function network(name: string, rx: number, tx: number): NetworkSample {
  return {
    name,
    addresses: name === 'eth0' ? ['192.0.2.10', 'fe80::1'] : ['192.0.2.11'],
    rx_bytes_per_second: rx,
    tx_bytes_per_second: tx,
    rx_bytes_total: 1,
    tx_bytes_total: 1,
  };
}

/** Shaped like the real /api/system payload; values are deterministic. */
function sample(index: number, ts: number, overrides: SampleOverrides = {}): MetricsSample {
  const cpuUsage = overrides.cpuUsage === undefined ? 30 + index / 10 : overrides.cpuUsage;
  return {
    ts,
    uptime_seconds: 93_600 + index,
    hostname: 'edge-001',
    kernel: '6.1.28',
    arch: 'aarch64',
    cpu: { cores: 4, usage_percent: cpuUsage, load1: 0.5, load5: 0.4, load15: 0.3 },
    memory:
      overrides.memory === undefined
        ? {
            total_kb: 1_048_576,
            available_kb: 524_288,
            used_kb: 524_288,
            free_kb: 100_000,
            buffers_kb: 1_000,
            cached_kb: 2_000,
            swap_total_kb: 0,
            swap_free_kb: 0,
            used_percent: 50,
          }
        : overrides.memory,
    disks: overrides.disks ?? [ROOT_DISK],
    network: overrides.network ?? [network('eth0', 1024, 512)],
    unavailable: overrides.unavailable ?? [],
  };
}

function prime(count: number, lastTs: number, overrides: SampleOverrides = {}): void {
  const samples = Array.from({ length: count }, (_, index) =>
    sample(index, lastTs - (count - 1 - index) * 1000, overrides),
  );
  sessionStore.setMetricsHistory(samples, 1000);
}

function jsonResponse(body: unknown, status = 200): Response {
  return new Response(JSON.stringify(body), { status, headers: { 'Content-Type': 'application/json' } });
}

function findWidget(container: HTMLElement, title: string): HTMLElement | undefined {
  return [...container.querySelectorAll<HTMLElement>('.ed-nas-widget')].find(
    (widget) => widget.querySelector('h2')?.textContent?.includes(title),
  );
}

describe('DashboardView', () => {
  let container: HTMLDivElement;
  let root: Root;

  beforeEach(() => {
    vi.stubGlobal('fetch', (url: string) => {
      if (url === '/api/system/info') {
        return Promise.resolve(
          jsonResponse({
            ok: true,
            version: '1.2.0',
            build: 'test',
            hostname: 'edge-001',
            arch: 'aarch64',
            kernel: '6.1.28',
            uptime_seconds: 93_600,
            listen: { host: '127.0.0.1', port: 8080 },
            file_root: '/srv',
            follow_symlinks: false,
            bash: '/bin/bash',
            audit_enabled: true,
            audit_log: '/var/log/edgedesk/audit.log',
            data_dir: '/var/lib/edgedesk',
            state_dir: '/run/edgedesk',
            max_upload_size: 1024,
            max_terminal_sessions: 8,
            terminal_idle_timeout: 1800,
            metrics_interval_ms: 1000,
            api_token_configured: false,
          }),
        );
      }
      return Promise.reject(new Error('offline'));
    });
    sessionStore.setMetricsHistory([], 1000);
    sessionStore.setSessions([]);
    document.body.innerHTML = '';
    container = document.createElement('div');
    document.body.appendChild(container);
    root = createRoot(container);
  });

  afterEach(() => {
    act(() => root.unmount());
    vi.unstubAllGlobals();
  });

  const render = async () => {
    act(() => {
      root.render(<DashboardView />);
    });
    for (let index = 0; index < 3; index += 1) {
      await act(async () => {
        await new Promise((resolve) => setTimeout(resolve, 0));
      });
    }
  };

  it('renders NAS resource widgets and trend charts from real samples', async () => {
    prime(120, Date.now());
    await render();
    expect(container.querySelectorAll('.ed-nas-widget')).toHaveLength(8);
    expect(container.querySelectorAll('.ed-chart')).toHaveLength(3);
    expect(container.textContent).toContain('CPU 使用率');
    expect(container.textContent).toContain('内存使用率');
    expect(container.textContent).toContain('存储空间');
    expect(container.textContent).toContain('网络吞吐');
    expect(container.textContent).toContain('服务状态');
    expect(container.textContent).toContain('系统信息');
    expect(container.textContent).toContain('192.0.2.10');
    expect(container.textContent).toContain('EdgeDesk API');
    expect(container.textContent).toContain('1.2.0');
  });

  it('shows every mount point separately with used and total values', async () => {
    prime(120, Date.now(), { disks: [ROOT_DISK, DATA_DISK] });
    await render();
    const mounts = [...container.querySelectorAll<HTMLElement>('.ed-nas-mount')];
    expect(mounts).toHaveLength(2);
    expect(mounts.map((item) => item.querySelector('code')?.textContent)).toEqual(['/', '/data']);
    expect(mounts[0].textContent).toContain('38.0 GiB');
    expect(mounts[0].textContent).toContain('100.0 GiB');
    expect(mounts[0].querySelector('.ed-nas-progress')?.getAttribute('aria-valuenow')).toBe('38');
    expect(mounts[1].textContent).toContain('xfs');
    expect(mounts[1].querySelector('.ed-nas-progress')?.getAttribute('aria-valuenow')).toBe('25');
  });

  it('shows every network interface and keeps RX/TX directions consistent', async () => {
    prime(120, Date.now(), { network: [network('eth0', 1024, 512), network('eth1', 2048, 256)] });
    await render();
    const interfaces = [...container.querySelectorAll<HTMLElement>('.ed-nas-iface')];
    expect(interfaces).toHaveLength(2);
    expect(interfaces.map((item) => item.querySelector('strong')?.textContent)).toEqual(['eth0', 'eth1']);
    expect(interfaces[0].textContent).toContain('接收 ↓ 1.0 KiB/s');
    expect(interfaces[0].textContent).toContain('发送 ↑ 512 B/s');
    expect(interfaces[0].textContent).toContain('192.0.2.10');
  });

  it('excludes loopback traffic from aggregate throughput but still lists the interface', async () => {
    prime(120, Date.now(), {
      network: [network('eth0', 1024, 512), { ...network('lo', 999_999, 999_999), addresses: ['127.0.0.1'] }],
    });
    await render();
    const net = findWidget(container, '网络吞吐');
    expect(net?.querySelector('.ed-nas-value')?.textContent).toBe('1.5 KiB/s');
    expect(net?.textContent).toContain('接收 ↓ 1.0 KiB/s');
    expect(net?.textContent).toContain('发送 ↑ 512 B/s');
    expect(container.querySelectorAll('.ed-nas-iface')).toHaveLength(2);
  });

  it('distinguishes unavailable fields from real zero values', async () => {
    prime(120, Date.now(), {
      cpuUsage: null,
      memory: null,
      disks: [],
      network: [],
      unavailable: ['cpu_usage', 'memory', 'disk'],
    });
    await render();
    const cpu = findWidget(container, 'CPU 使用率');
    const memory = findWidget(container, '内存使用率');
    const storage = findWidget(container, '存储空间');
    const net = findWidget(container, '网络吞吐');
    expect(cpu?.dataset.status).toBe('unavailable');
    expect(memory?.dataset.status).toBe('unavailable');
    expect(storage?.dataset.status).toBe('unavailable');
    expect(net?.dataset.status).toBe('unavailable');
    expect(cpu?.querySelector('.ed-nas-value')?.textContent).toBe('—');
    expect(memory?.querySelector('.ed-nas-value')?.textContent).toBe('—');
    expect(storage?.querySelector('.ed-nas-value')?.textContent).toBe('—');
    expect(net?.querySelector('.ed-nas-value')?.textContent).toBe('—');
    expect(container.textContent).toContain('未找到可统计的块设备挂载点');
  });

  it('renders real zeros as zero rather than as missing data', async () => {
    const zeroDisk = { ...ROOT_DISK, used_bytes: 0, used_percent: 0, available_bytes: 100 * GIB };
    prime(120, Date.now(), {
      cpuUsage: 0,
      memory: {
        total_kb: 1_048_576,
        available_kb: 1_048_576,
        used_kb: 0,
        free_kb: 1_048_576,
        buffers_kb: 0,
        cached_kb: 0,
        swap_total_kb: 0,
        swap_free_kb: 0,
        used_percent: 0,
      },
      disks: [zeroDisk],
      network: [network('eth0', 0, 0)],
    });
    await render();
    const cpu = findWidget(container, 'CPU 使用率');
    const net = findWidget(container, '网络吞吐');
    expect(cpu?.querySelector('.ed-nas-value')?.textContent).toBe('0.0%');
    expect(net?.querySelector('.ed-nas-value')?.textContent).toBe('0 B/s');
    expect(net?.textContent).toContain('接收 ↓ 0 B/s');
    expect(net?.textContent).toContain('发送 ↑ 0 B/s');
    expect(findWidget(container, '存储空间')?.querySelector('.ed-nas-value')?.textContent).toBe('0.0%');
  });

  it('marks stale sampling as an interrupted stream', async () => {
    prime(120, Date.now() - 3_600_000);
    await render();
    expect(container.querySelector('.ed-status-pill.warning')?.textContent).toContain('数据中断');
    expect(container.textContent).toContain('超过 3 个采样周期未更新');
  });

  it('waits for the first sample without inventing values', async () => {
    await render();
    expect(container.querySelectorAll('.ed-nas-widget[data-status="loading"]')).toHaveLength(4);
    expect(container.textContent).toContain('等待指标数据');
    expect(container.textContent).not.toContain('38%');
  });
});
