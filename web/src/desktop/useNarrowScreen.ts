import { useEffect, useState } from 'react';

export const NARROW_BREAKPOINT = 768;

const QUERY = `(max-width: ${NARROW_BREAKPOINT}px)`;

function readNarrow(): boolean {
  if (typeof window === 'undefined' || typeof window.matchMedia !== 'function') return false;
  return window.matchMedia(QUERY).matches;
}

/** Desktop-first layout; narrow viewports force maximized windows and disable free drag. */
export function useNarrowScreen(): boolean {
  const [narrow, setNarrow] = useState(readNarrow);

  useEffect(() => {
    if (typeof window.matchMedia !== 'function') return;
    const media = window.matchMedia(QUERY);
    const listener = () => setNarrow(media.matches);
    setNarrow(media.matches);
    media.addEventListener?.('change', listener);
    return () => media.removeEventListener?.('change', listener);
  }, []);

  return narrow;
}

/**
 * Global shortcuts must not steal keystrokes from text fields, editors,
 * selects or the terminal (xterm keeps a hidden textarea).
 */
export function isEditableTarget(target: EventTarget | null): boolean {
  if (typeof Element === 'undefined' || !(target instanceof Element)) return false;
  return Boolean(
    target.closest(
      'input, textarea, select, [contenteditable="true"], [contenteditable=""], .xterm, .ed-editor-textarea',
    ),
  );
}
