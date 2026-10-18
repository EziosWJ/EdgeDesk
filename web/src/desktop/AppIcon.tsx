import { asset } from '../services/assets';
import type { AppDefinition } from './appRegistry';

type Props = {
  app: Pick<AppDefinition, 'sprite' | 'tone'>;
  size?: number;
  className?: string;
  title?: string;
};

/**
 * Application glyph from the committed desktop SVG sprite. The sprite symbol
 * inherits color through currentColor; no runtime icon CDN is involved.
 */
export function AppSprite({ app, size = 22, className, title }: Props) {
  return (
    <svg
      className={className}
      width={size}
      height={size}
      viewBox="0 0 24 24"
      role={title ? 'img' : undefined}
      aria-label={title}
      aria-hidden={title ? undefined : true}
      focusable="false"
    >
      <use href={`${asset('desktop/app-icons.svg')}#${app.sprite}`} />
    </svg>
  );
}

export function AppTile({ app, size = 48, className = '' }: { app: Pick<AppDefinition, 'sprite' | 'tone' | 'name'>; size?: number; className?: string }) {
  return (
    <span
      className={`ed-app-tile tile-${app.tone} ${className}`.trim()}
      style={{ width: size, height: size, borderRadius: Math.max(10, Math.round(size * 0.27)) }}
    >
      <AppSprite app={app} size={Math.round(size * 0.5)} />
    </span>
  );
}
