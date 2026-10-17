import { formatPercent } from '../services/api';

type Props = {
  percent: number | null;
  size?: number;
  label?: string;
};

/** Donut for a single percentage (disk usage). `null` renders an explicit unavailable state. */
export function DonutChart({ percent, size = 148, label = '已使用' }: Props) {
  const stroke = 13;
  const radius = (size - stroke) / 2;
  const circumference = 2 * Math.PI * radius;
  const clamped = percent === null ? 0 : Math.min(100, Math.max(0, percent));
  const arc = (clamped / 100) * circumference;

  return (
    <div className="ed-donut" style={{ width: size, height: size }}>
      <svg
        viewBox={`0 0 ${size} ${size}`}
        width={size}
        height={size}
        role="img"
        aria-label={percent === null ? `磁盘使用率不可用` : `磁盘${label} ${formatPercent(percent, 0)}`}
      >
        <circle cx={size / 2} cy={size / 2} r={radius} fill="none" stroke="var(--ed-donut-track)" strokeWidth={stroke} />
        {percent !== null && (
          <circle
            cx={size / 2}
            cy={size / 2}
            r={radius}
            fill="none"
            stroke="var(--ed-primary)"
            strokeWidth={stroke}
            strokeLinecap="round"
            strokeDasharray={`${arc} ${circumference - arc}`}
            transform={`rotate(-90 ${size / 2} ${size / 2})`}
          />
        )}
      </svg>
      <div className="ed-donut-center">
        <strong>{percent === null ? '—' : formatPercent(percent, 0)}</strong>
        <span>{label}</span>
      </div>
    </div>
  );
}
