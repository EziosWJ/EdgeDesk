import type { LucideIcon } from 'lucide-react';

type Props = {
  icon: LucideIcon;
  /** Rendered size in px; defaults to the 18px inline size used across the app. */
  size?: number;
  strokeWidth?: number;
  className?: string;
  /** Accessible label. Decorative icons stay hidden from assistive tech. */
  label?: string;
};

/**
 * Thin wrapper around `lucide-react` so every icon shares sizing, stroke and
 * accessibility defaults. Icons are bundled by Vite and colored through
 * `currentColor`; no runtime CDN or `web/public` icon files are involved.
 */
export function Icon({ icon: Glyph, size = 18, strokeWidth = 1.8, className, label }: Props) {
  return (
    <Glyph
      className={className}
      size={size}
      strokeWidth={strokeWidth}
      aria-hidden={label ? undefined : true}
      role={label ? 'img' : undefined}
      aria-label={label}
    />
  );
}
