import { useId } from 'react';
import { isFiniteNumber, maxOf, niceCeil, xTickLabels } from '../features/dashboard/chartUtils';

export type ChartSeries = {
  label: string;
  color: string;
  values: readonly (number | null)[];
  /** Gradient area under the line (ignored for stacked bands, which are always filled). */
  fill?: boolean;
};

type Props = {
  series: ChartSeries[];
  timestamps: readonly number[];
  /** Fixed upper bound (e.g. 100 for percent charts); auto-scaled when omitted. */
  max?: number | null;
  /** Draws cumulative bands (memory used/available) instead of overlay lines. */
  stacked?: boolean;
  formatY?: (value: number) => string;
};

const WIDTH = 600;
const HEIGHT = 220;
const TICKS = 5;

type XY = { x: number; y: number };

function buildLine(values: readonly (number | null)[], x: (index: number) => number, y: (value: number) => number): XY[][] {
  const segments: XY[][] = [];
  let current: XY[] = [];
  values.forEach((value, index) => {
    if (!isFiniteNumber(value)) {
      if (current.length > 0) segments.push(current);
      current = [];
      return;
    }
    current.push({ x: x(index), y: y(value) });
  });
  if (current.length > 0) segments.push(current);
  return segments;
}

/** Closed polygons between two value series (bottoms may be all zeros for a plain area fill). */
function buildBands(
  bottoms: readonly (number | null)[],
  tops: readonly (number | null)[],
  x: (index: number) => number,
  y: (value: number) => number,
): string[] {
  const polygons: string[] = [];
  let run: { index: number; top: number; bottom: number }[] = [];
  const flush = () => {
    if (run.length === 0) return;
    const upper = run.map((item) => `${x(item.index)},${y(item.top)}`).join(' ');
    const lower = [...run].reverse().map((item) => `${x(item.index)},${y(item.bottom)}`).join(' ');
    polygons.push(`${upper} ${lower}`);
    run = [];
  };
  tops.forEach((top, index) => {
    const bottom = bottoms[index];
    if (isFiniteNumber(top) && isFiniteNumber(bottom)) run.push({ index, top, bottom });
    else flush();
  });
  flush();
  return polygons;
}

/**
 * Dependency-free SVG line/area chart with axis labels. The plot area stretches
 * with CSS; labels live in HTML so they never distort.
 */
export function MetricChart({ series, timestamps, max = null, stacked = false, formatY = (value) => String(value) }: Props) {
  const uid = useId().replace(/[^a-zA-Z0-9]/g, '');
  const length = Math.max(1, ...series.map((item) => item.values.length));
  const totals: (number | null)[] = new Array(length).fill(null).map((_, index) => {
    let sum = 0;
    let known = false;
    for (const item of series) {
      const value = item.values[index];
      if (isFiniteNumber(value)) {
        sum += value;
        known = true;
      }
    }
    return known ? sum : null;
  });
  const upper = max ?? niceCeil(maxOf(stacked ? totals : series.flatMap((item) => [...item.values])) * 1.1);
  const x = (index: number) => (length > 1 ? (index / (length - 1)) * WIDTH : WIDTH / 2);
  const y = (value: number) => HEIGHT - (Math.min(Math.max(value, 0), upper) / upper) * HEIGHT;

  const yTicks = Array.from({ length: TICKS }, (_, index) => (upper / (TICKS - 1)) * (TICKS - 1 - index));

  const cumulative: (number | null)[] = new Array(length).fill(0);
  const bands = series.map((item) => {
    if (!stacked) return { item, bottoms: new Array(length).fill(0) as (number | null)[], tops: item.values };
    const bottoms = [...cumulative];
    for (let index = 0; index < length; index += 1) {
      const value = item.values[index];
      const previous = cumulative[index];
      cumulative[index] = isFiniteNumber(previous) && isFiniteNumber(value) ? previous + value : null;
    }
    return { item, bottoms, tops: item.values };
  });

  return (
    <div className="ed-chart">
      <div className="ed-chart-y" aria-hidden="true">
        {yTicks.map((value, index) => (
          <span key={index}>{formatY(value)}</span>
        ))}
      </div>
      <div className="ed-chart-plot">
        <svg viewBox={`0 0 ${WIDTH} ${HEIGHT}`} preserveAspectRatio="none" role="img" aria-label={series.map((item) => item.label).join('、')}>
          <defs>
            {bands.map(({ item }, index) => (
              <linearGradient key={index} id={`ed-chart-${uid}-${index}`} x1="0" y1="0" x2="0" y2="1">
                <stop offset="0%" stopColor={item.color} stopOpacity="0.32" />
                <stop offset="100%" stopColor={item.color} stopOpacity="0" />
              </linearGradient>
            ))}
          </defs>
          {yTicks.map((value, index) => (
            <line
              key={index}
              x1="0"
              x2={WIDTH}
              y1={y(value)}
              y2={y(value)}
              stroke="var(--ed-chart-grid)"
              strokeWidth="1"
              vectorEffect="non-scaling-stroke"
            />
          ))}
          {bands.map(({ item, bottoms, tops }, index) =>
            buildBands(bottoms, tops, x, y).map((points, polygonIndex) => (
              <polygon
                key={`${index}-${polygonIndex}`}
                points={points}
                fill={stacked ? item.color : `url(#ed-chart-${uid}-${index})`}
                opacity={stacked ? 0.32 : item.fill === false ? 0 : 1}
              />
            )),
          )}
          {bands.map(({ item, tops }, index) =>
            buildLine(tops, x, y).map((segment, segmentIndex) => (
              <polyline
                key={`${index}-${segmentIndex}`}
                fill="none"
                stroke={item.color}
                strokeWidth="1.8"
                strokeLinecap="round"
                strokeLinejoin="round"
                vectorEffect="non-scaling-stroke"
                points={segment.map((point) => `${point.x},${point.y}`).join(' ')}
              />
            )),
          )}
        </svg>
      </div>
      <div className="ed-chart-x" aria-hidden="true">
        {xTickLabels(timestamps, TICKS).map((label, index) => (
          <span key={index}>{label}</span>
        ))}
      </div>
    </div>
  );
}
