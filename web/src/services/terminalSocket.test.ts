import { beforeEach, describe, expect, it } from 'vitest';
import { loadStoredTerminals, storeTerminals } from './terminalSocket';
import { sessionStore } from '../stores/sessionStore';
import type { MetricsSample, TerminalSessionInfo } from '../types';

const session: TerminalSessionInfo = {
  id: 'abc123',
  title: 'Bash 1',
  cwd: '/',
  created_ms: 1,
  last_activity_ms: 2,
  cols: 80,
  rows: 24,
  running: true,
  exited: false,
  exit_code: 0,
  subscribers: 1,
  buffered_bytes: 0,
  pending_confirmation: false,
};

function sample(ts: number, cpu: number | null): MetricsSample {
  return {
    ts,
    uptime_seconds: 10,
    hostname: 'dev',
    kernel: '6.1',
    arch: 'aarch64',
    cpu: { cores: 4, usage_percent: cpu, load1: 0.1, load5: 0.2, load15: 0.3 },
    memory: null,
    disks: [],
    network: [],
    unavailable: [],
  };
}

describe('terminal persistence', () => {
  beforeEach(() => {
    window.localStorage.removeItem('edgedesk.terminals.v1');
  });

  it('persists only session ids and titles', () => {
    storeTerminals([{ id: 'abc123', title: 'Bash 1' }]);
    const restored = loadStoredTerminals();
    expect(restored).toEqual([{ id: 'abc123', title: 'Bash 1' }]);
    expect(window.localStorage.getItem('edgedesk.terminals.v1')).not.toContain('token');
  });

  it('survives corrupted storage content', () => {
    window.localStorage.setItem('edgedesk.terminals.v1', '{not json');
    expect(loadStoredTerminals()).toEqual([]);
  });
});

describe('sessionStore', () => {
  it('mirrors terminal sessions and removes them', () => {
    sessionStore.setSessions([session]);
    expect(sessionStore.getSnapshot()).toHaveLength(1);
    sessionStore.upsert({ ...session, id: 'def456' });
    expect(sessionStore.getSnapshot()).toHaveLength(2);
    sessionStore.remove('abc123');
    expect(sessionStore.getSnapshot().map((item) => item.id)).toEqual(['def456']);
  });

  it('keeps a bounded metrics window with monotonic timestamps', () => {
    sessionStore.setMetricsHistory([], 1000);
    for (let index = 0; index < 400; index += 1) {
      sessionStore.pushSample(sample(index, index % 100));
    }
    const samples = sessionStore.metricsSnapshot();
    expect(samples.length).toBeLessThanOrEqual(300);
    const timestamps = samples.map((item) => item.ts);
    expect([...timestamps].sort((a, b) => a - b)).toEqual(timestamps);
  });
});
