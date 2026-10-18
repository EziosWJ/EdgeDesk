import type { IconSlot } from './desktopStore';
import { ICON_COLUMNS, ICON_ROWS } from './desktopStore';

export const ICON_CELL_WIDTH = 96;
export const ICON_CELL_HEIGHT = 104;

export function clampSlot(col: number, row: number): { col: number; row: number } {
  return {
    col: Math.min(ICON_COLUMNS - 1, Math.max(0, Math.round(col))),
    row: Math.min(ICON_ROWS - 1, Math.max(0, Math.round(row))),
  };
}

/** Convert a pointer position inside the desktop surface to the nearest icon slot. */
export function slotFromPoint(x: number, y: number): { col: number; row: number } {
  return clampSlot(Math.floor(x / ICON_CELL_WIDTH), Math.floor(y / ICON_CELL_HEIGHT));
}

/**
 * Move an icon to a slot. If another icon already occupies it the two swap, so
 * arrangements stay dense and no two icons can hide each other.
 */
export function moveIcon(icons: readonly IconSlot[], appId: string, col: number, row: number): IconSlot[] {
  const target = clampSlot(col, row);
  const moving = icons.find((icon) => icon.appId === appId);
  if (!moving) return [...icons];
  if (moving.col === target.col && moving.row === target.row) return [...icons];
  const occupant = icons.find((icon) => icon.col === target.col && icon.row === target.row && icon.appId !== appId);
  return icons.map((icon) => {
    if (icon.appId === appId) return { ...icon, ...target };
    if (occupant && icon.appId === occupant.appId) return { ...icon, col: moving.col, row: moving.row };
    return icon;
  });
}

/** Keep persisted/partial arrays deterministic even if a caller passes duplicates. */
export function normalizeIcons(icons: readonly IconSlot[]): IconSlot[] {
  const seen = new Set<string>();
  return icons.filter((icon) => {
    if (seen.has(icon.appId)) return false;
    seen.add(icon.appId);
    return true;
  });
}
