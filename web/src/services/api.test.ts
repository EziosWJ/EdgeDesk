import { describe, expect, it } from 'vitest';
import { formatBytes, formatPercent, formatRate, formatUptime } from './api';

describe('formatting helpers', () => {
  it('formats byte sizes', () => {
    expect(formatBytes(0)).toBe('0 B');
    expect(formatBytes(2048)).toBe('2.0 KiB');
    expect(formatBytes(5 * 1024 * 1024)).toBe('5.0 MiB');
    expect(formatBytes(Number.NaN)).toBe('—');
  });

  it('formats rates and percentages', () => {
    expect(formatRate(1536)).toBe('1.5 KiB/s');
    expect(formatRate(null)).toBe('—');
    expect(formatPercent(12.345)).toBe('12.3%');
    expect(formatPercent(null)).toBe('—');
  });

  it('formats uptime', () => {
    expect(formatUptime(0)).toBe('—');
    expect(formatUptime(90)).toBe('1 分 30 秒');
    expect(formatUptime(3600 * 5)).toBe('5 小时 0 分');
    expect(formatUptime(86400 * 2 + 3600)).toBe('2 天 1 小时');
  });
});
