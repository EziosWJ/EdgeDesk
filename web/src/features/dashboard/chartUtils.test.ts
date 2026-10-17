import { describe, expect, it } from 'vitest';
import type { MetricsSample } from '../../types';
import {
  bootMoment,
  deltaTone,
  formatDelta,
  formatDateTime,
  kpiPercent,
  maxOf,
  niceCeil,
  previousValue,
  rateScale,
  sliceWindow,
  usageDelta,
  xTickLabels,
} from './chartUtils';

describe('sliceWindow', () => {
  it('keeps the newest samples that fit the range', () => {
    const values = Array.from({ length: 300 }, (_, index) => index);
    const visible = sliceWindow(values, 60, 1000);
    expect(visible).toHaveLength(60);
    expect(visible[0]).toBe(240);
    expect(visible[59]).toBe(299);
  });

  it('respects the sampling interval and returns a copy when the buffer is short', () => {
    expect(sliceWindow([1, 2, 3], 300, 1000)).toEqual([1, 2, 3]);
    const values = [1, 2, 3];
    expect(sliceWindow(values, 300, 1000)).not.toBe(values);
    expect(sliceWindow(Array.from({ length: 300 }, (_, index) => index), 60, 2000)).toHaveLength(30);
  });
});

describe('previousValue / usageDelta', () => {
  it('scans backwards across gaps instead of faking zeros', () => {
    expect(previousValue([10, null, null, null, 40], 4)).toBe(10);
    expect(previousValue([null, null, null], 3)).toBeNull();
    expect(previousValue([], 3)).toBeNull();
  });

  it('computes deltas in percentage points', () => {
    expect(usageDelta([10, 20, 30, 40], 2)).toBe(20);
    expect(usageDelta([40, 30, 20, 10], 3)).toBe(-30);
    expect(usageDelta([10, null, null, null, 40], 4)).toBe(30);
    // Short buffers clamp to the earliest known sample instead of dropping the chip.
    expect(usageDelta([10, 20, null], 2)).toBe(10);
    expect(usageDelta([], 2)).toBeNull();
    expect(usageDelta([null, null], 2)).toBeNull();
  });
});

describe('delta presentation', () => {
  it('marks falling usage as good and rising usage as bad', () => {
    expect(deltaTone(-2)).toBe('good');
    expect(deltaTone(2)).toBe('bad');
    expect(deltaTone(0.01)).toBe('flat');
    expect(deltaTone(null)).toBe('flat');
    expect(deltaTone(2, false)).toBe('good');
  });

  it('formats deltas with arrows and units', () => {
    expect(formatDelta(-3.25)).toBe('↓ 3.3%');
    expect(formatDelta(0)).toBe('· 0.0%');
    expect(formatDelta(null)).toBe('—');
  });
});

describe('axis helpers', () => {
  it('formats headline percentages without losing small values', () => {
    expect(kpiPercent(32.4)).toBe('32%');
    expect(kpiPercent(0.4)).toBe('0.4%');
    expect(kpiPercent(null)).toBe('—');
  });

  it('ignores nulls and negative noise when measuring', () => {
    expect(maxOf([null, 5, -2, 3])).toBe(5);
    expect(maxOf([null, null])).toBe(0);
  });

  it('rounds up to readable 1/2/5 steps', () => {
    expect(niceCeil(87)).toBe(100);
    expect(niceCeil(1.2)).toBe(2);
    expect(niceCeil(0)).toBe(1);
  });

  it('picks a byte-rate unit from the largest sample', () => {
    expect(rateScale([100, 2048])).toEqual({ divisor: 1024, unit: 'KiB/s' });
    expect(rateScale([3 * 1024 * 1024])).toEqual({ divisor: 1024 * 1024, unit: 'MiB/s' });
    expect(rateScale([null, 512])).toEqual({ divisor: 1, unit: 'B/s' });
  });
});

describe('time helpers', () => {
  const base = new Date(2026, 0, 1, 12, 0, 0).getTime() / 1000;

  it('labels a short window with seconds and a long window without', () => {
    expect(xTickLabels([base, base + 1, base + 2, base + 3, base + 4])).toEqual([
      '12:00:00',
      '12:00:01',
      '12:00:02',
      '12:00:03',
      '12:00:04',
    ]);
    const long = Array.from({ length: 5 }, (_, index) => base + index * 120);
    expect(xTickLabels(long)).toEqual(['12:00', '12:02', '12:04', '12:06', '12:08']);
  });

  it('derives the boot moment from uptime and formats it', () => {
    const sample = { ts: 1_000_000, uptime_seconds: 400 } as MetricsSample;
    expect(bootMoment(sample)).toBe(999_600);
    expect(formatDateTime(new Date(2026, 0, 1, 8, 5).getTime() / 1000)).toBe('2026-01-01 08:05');
  });
});
