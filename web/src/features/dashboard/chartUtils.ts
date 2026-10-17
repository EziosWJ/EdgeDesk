import type { MetricsSample } from '../../types';

/**
 * Pure helpers for the dashboard charts. They operate on real samples only;
 * missing values stay `null` and are never replaced with zeros.
 */

export const isFiniteNumber = (value: number | null | undefined): value is number =>
  value !== null && value !== undefined && Number.isFinite(value);

/** Tail slice that covers `rangeSeconds` at the given sampling interval. */
export function sliceWindow<T>(values: readonly T[], rangeSeconds: number, intervalMs: number): T[] {
  const count = Math.max(2, Math.round((rangeSeconds * 1000) / Math.max(intervalMs, 1)));
  return values.length > count ? values.slice(-count) : values.slice();
}

/** Nearest known value `lookback` samples before the end, scanning across gaps. */
export function previousValue(values: readonly (number | null)[], lookback: number): number | null {
  const start = values.length - 1 - lookback;
  for (let index = start; index >= 0; index -= 1) {
    const value = values[index];
    if (isFiniteNumber(value)) return value;
  }
  return null;
}

/** Change in percentage points between the newest value and the one `lookback` samples earlier. */
export function usageDelta(values: readonly (number | null)[], lookback: number): number | null {
  let end = -1;
  for (let index = values.length - 1; index >= 0; index -= 1) {
    if (isFiniteNumber(values[index])) {
      end = index;
      break;
    }
  }
  if (end <= 0) return null;
  const target = Math.max(0, end - lookback);
  for (let index = target; index >= 0; index -= 1) {
    const value = values[index];
    if (isFiniteNumber(value)) return (values[end] as number) - value;
  }
  return null;
}

export type DeltaTone = 'good' | 'bad' | 'flat';

/** Lower usage is "good" for CPU/memory/disk; throughput uses `lowerIsBetter: false`. */
export function deltaTone(delta: number | null, lowerIsBetter = true, epsilon = 0.05): DeltaTone {
  if (delta === null || Math.abs(delta) < epsilon) return 'flat';
  const good = lowerIsBetter ? delta < 0 : delta > 0;
  return good ? 'good' : 'bad';
}

export function formatDelta(delta: number | null, unit = '%'): string {
  if (delta === null) return '—';
  const arrow = delta > 0 ? '↑' : delta < 0 ? '↓' : '·';
  return `${arrow} ${Math.abs(delta).toFixed(1)}${unit}`;
}

/** Headline percentage: integer for typical values, one decimal below 10% so small load stays visible. */
export function kpiPercent(value: number | null | undefined): string {
  if (value === null || value === undefined || !Number.isFinite(value)) return '—';
  return `${value.toFixed(Math.abs(value) >= 10 ? 0 : 1)}%`;
}

export function maxOf(values: readonly (number | null)[]): number {
  let max = 0;
  for (const value of values) {
    if (isFiniteNumber(value) && value > max) max = value;
  }
  return max;
}

/** Rounds up to a 1/2/5 x 10^n step so axis labels stay readable. */
export function niceCeil(value: number): number {
  if (!Number.isFinite(value) || value <= 0) return 1;
  const power = 10 ** Math.floor(Math.log10(value));
  const scaled = value / power;
  const step = scaled <= 1 ? 1 : scaled <= 2 ? 2 : scaled <= 5 ? 5 : 10;
  return step * power;
}

/** Picks a byte/second unit so chart axes and legends stay readable. */
export function rateScale(values: readonly (number | null)[]): { divisor: number; unit: string } {
  const max = maxOf(values);
  if (max >= 1024 * 1024) return { divisor: 1024 * 1024, unit: 'MiB/s' };
  if (max >= 1024) return { divisor: 1024, unit: 'KiB/s' };
  return { divisor: 1, unit: 'B/s' };
}

const two = (value: number): string => String(value).padStart(2, '0');

export function formatClock(ts: number, withSeconds = true): string {
  const date = new Date(ts * 1000);
  const base = `${two(date.getHours())}:${two(date.getMinutes())}`;
  return withSeconds ? `${base}:${two(date.getSeconds())}` : base;
}

export function formatDateTime(ts: number): string {
  const date = new Date(ts * 1000);
  return `${date.getFullYear()}-${two(date.getMonth() + 1)}-${two(date.getDate())} ${two(date.getHours())}:${two(date.getMinutes())}`;
}

/** Evenly spaced x-axis labels; seconds only for short windows. */
export function xTickLabels(timestamps: readonly number[], count = 5): string[] {
  if (timestamps.length === 0) return [];
  const span = timestamps[timestamps.length - 1] - timestamps[0];
  const withSeconds = span <= 180;
  const step = (timestamps.length - 1) / Math.max(1, count - 1);
  const labels: string[] = [];
  for (let index = 0; index < count; index += 1) {
    const source = Math.min(timestamps.length - 1, Math.round(index * step));
    labels.push(formatClock(timestamps[source], withSeconds));
  }
  return labels;
}

/** Wall-clock moment the device booted, derived from uptime and the sample time. */
export function bootMoment(sample: MetricsSample): number {
  return sample.ts - sample.uptime_seconds;
}
