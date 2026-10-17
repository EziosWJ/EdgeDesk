type Props = {
  values: (number | null)[];
  width?: number;
  height?: number;
  color?: string;
  fill?: boolean;
  max?: number | null;
  label?: string;
  unit?: string;
  /** Overlay chip with the newest value; disable when the card already shows it. */
  showValue?: boolean;
};

/**
 * Dependency-free SVG sparkline. Null samples (missing data) are skipped, so
 * gaps are drawn as straight segments instead of fake zeros.
 */
export function Sparkline({
  values,
  width = 260,
  height = 64,
  color = 'var(--ed-primary)',
  fill = true,
  max,
  label,
  unit,
  showValue = true,
}: Props) {
  const points = values.filter((value): value is number => value !== null && Number.isFinite(value));
  const upper = max ?? (points.length > 0 ? Math.max(...points, 1) : 1);
  const lower = 0;
  const range = Math.max(upper - lower, 1e-9);
  const step = values.length > 1 ? width / (values.length - 1) : width;
  const toY = (value: number) => height - ((value - lower) / range) * (height - 6) - 3;

  const segments: { x: number; y: number }[][] = [];
  let current: { x: number; y: number }[] = [];
  values.forEach((value, index) => {
    if (value === null || !Number.isFinite(value)) {
      if (current.length > 1) segments.push(current);
      current = [];
      return;
    }
    current.push({ x: index * step, y: toY(value as number) });
  });
  if (current.length > 1) segments.push(current);

  const last = points.length > 0 ? points[points.length - 1] : null;
  const gradientId = `ed-spark-${label ?? color}`.replace(/[^a-zA-Z0-9-]/g, '');

  return (
    <div className="ed-spark">
      <svg viewBox={`0 0 ${width} ${height}`} preserveAspectRatio="none" width="100%" height={height} role="img" aria-label={label}>
        <defs>
          <linearGradient id={gradientId} x1="0" y1="0" x2="0" y2="1">
            <stop offset="0%" stopColor={color} stopOpacity="0.35" />
            <stop offset="100%" stopColor={color} stopOpacity="0" />
          </linearGradient>
        </defs>
        {fill &&
          segments.map((segment, index) => (
            <polygon
              key={`fill-${index}`}
              fill={`url(#${gradientId})`}
              points={`${segment[0].x},${height} ${segment.map((point) => `${point.x},${point.y}`).join(' ')} ${
                segment[segment.length - 1].x
              },${height}`}
            />
          ))}
        {segments.map((segment, index) => (
          <polyline
            key={`line-${index}`}
            fill="none"
            stroke={color}
            strokeWidth="1.8"
            strokeLinejoin="round"
            strokeLinecap="round"
            points={segment.map((point) => `${point.x},${point.y}`).join(' ')}
          />
        ))}
      </svg>
      {showValue && last !== null && (
        <span className="ed-spark-value">
          {last.toFixed(unit === '%' ? 1 : 1)}
          {unit}
        </span>
      )}
    </div>
  );
}
