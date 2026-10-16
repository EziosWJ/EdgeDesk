import { beforeEach, describe, expect, it } from 'vitest';
import { windowStore } from './windowStore';

describe('windowStore', () => {
  beforeEach(() => {
    windowStore.closeAll();
  });

  it('opens a window and reuses it for the same payload key', () => {
    const first = windowStore.open({ kind: 'editor', title: '编辑 /a.txt', payload: { path: '/a.txt' } });
    const second = windowStore.open({ kind: 'editor', title: '编辑 /a.txt', payload: { path: '/a.txt' } });
    expect(second).toBe(first);
    expect(windowStore.getSnapshot()).toHaveLength(1);
  });

  it('tracks distinct terminal windows independently', () => {
    windowStore.open({ kind: 'terminal', title: 'Bash 1', payload: { sessionId: 's1' } });
    windowStore.open({ kind: 'terminal', title: 'Bash 2', payload: { sessionId: 's2' } });
    expect(windowStore.getSnapshot()).toHaveLength(2);
  });

  it('raises the z-index on focus and restores minimized windows', () => {
    const first = windowStore.open({ kind: 'terminal', title: 'Bash 1', payload: { sessionId: 's1' } });
    const second = windowStore.open({ kind: 'terminal', title: 'Bash 2', payload: { sessionId: 's2' } });
    windowStore.minimize(second);
    expect(windowStore.getSnapshot().find((item) => item.id === second)?.state).toBe('minimized');
    windowStore.focus(second);
    const secondWindow = windowStore.getSnapshot().find((item) => item.id === second)!;
    const firstWindow = windowStore.getSnapshot().find((item) => item.id === first)!;
    expect(secondWindow.state).toBe('normal');
    expect(secondWindow.zIndex).toBeGreaterThan(firstWindow.zIndex);
  });

  it('clamps move and resize to the viewport and minimum sizes', () => {
    const id = windowStore.open({ kind: 'editor', title: '编辑 /x', payload: { path: '/x' } });
    windowStore.move(id, -5000, -5000);
    let item = windowStore.getSnapshot().find((entry) => entry.id === id)!;
    expect(item.x).toBeLessThan(0);
    expect(item.x).toBeGreaterThanOrEqual(-item.width + 120);
    expect(item.y).toBe(0);

    windowStore.resize(id, 10, 10);
    item = windowStore.getSnapshot().find((entry) => entry.id === id)!;
    expect(item.width).toBeGreaterThanOrEqual(380);
    expect(item.height).toBeGreaterThanOrEqual(240);

    windowStore.resize(id, 999999, 999999);
    item = windowStore.getSnapshot().find((entry) => entry.id === id)!;
    expect(item.width).toBeLessThanOrEqual(1440);
  });

  it('toggles maximize and closes windows', () => {
    const id = windowStore.open({ kind: 'preview', title: '预览 /x', payload: { path: '/x' } });
    windowStore.toggleMaximize(id);
    expect(windowStore.getSnapshot().find((item) => item.id === id)?.state).toBe('maximized');
    windowStore.toggleMaximize(id);
    expect(windowStore.getSnapshot().find((item) => item.id === id)?.state).toBe('normal');
    windowStore.close(id);
    expect(windowStore.getSnapshot()).toHaveLength(0);
  });
});
