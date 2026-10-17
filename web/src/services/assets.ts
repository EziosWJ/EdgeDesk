import type { CSSProperties } from 'react';

/** `web/public/assets` URL for the configured Vite base path. */
export const asset = (path: string) => `${import.meta.env.BASE_URL}assets/${path}`;

/** Inline icon URL for `.ed-icon-mask` consumers. */
export function iconStyle(icon: string): CSSProperties {
  return { '--ed-icon-url': `url(${asset(`icons/${icon}.svg`)})` } as CSSProperties;
}
