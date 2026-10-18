import { describe, expect, it } from 'vitest';
import { clampSlot, moveIcon, normalizeIcons, slotFromPoint } from './iconLayout';

describe('desktop icon layout', () => {
  it('maps pointer positions to clamped grid slots', () => {
    expect(slotFromPoint(0, 0)).toEqual({ col: 0, row: 0 });
    expect(slotFromPoint(190, 210)).toEqual({ col: 1, row: 2 });
    expect(slotFromPoint(-500, -500)).toEqual({ col: 0, row: 0 });
    expect(clampSlot(99, 99)).toEqual({ col: 3, row: 7 });
  });

  it('moves an icon and swaps with an occupied slot', () => {
    const icons = [
      { appId: 'dashboard' as const, col: 0, row: 0 },
      { appId: 'files' as const, col: 0, row: 1 },
    ];
    const moved = moveIcon(icons, 'dashboard', 0, 1);
    expect(moved).toEqual([
      { appId: 'dashboard', col: 0, row: 1 },
      { appId: 'files', col: 0, row: 0 },
    ]);
  });

  it('is a no-op for the same slot or an unknown icon', () => {
    const icons = [{ appId: 'dashboard' as const, col: 0, row: 0 }];
    expect(moveIcon(icons, 'dashboard', 0, 0)).toEqual(icons);
    expect(moveIcon(icons, 'files', 1, 1)).toEqual(icons);
  });

  it('removes duplicate icons defensively', () => {
    const icons = [
      { appId: 'dashboard' as const, col: 0, row: 0 },
      { appId: 'dashboard' as const, col: 1, row: 0 },
      { appId: 'files' as const, col: 0, row: 1 },
    ];
    expect(normalizeIcons(icons)).toHaveLength(2);
  });
});
