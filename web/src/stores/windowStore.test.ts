import { beforeEach, describe, expect, it } from 'vitest';
import { windowStore, constrainGeometry } from './windowStore';
import type { TerminalSessionInfo } from '../types';

const SESSION: TerminalSessionInfo = {
  id: 's1',
  title: 'Bash 1',
  cwd: '/',
  created_ms: 1,
  last_activity_ms: 1,
  cols: 80,
  rows: 24,
  running: true,
  exited: false,
  exit_code: 0,
  subscribers: 1,
  buffered_bytes: 0,
  pending_confirmation: false,
};

describe('windowStore', () => {
  beforeEach(() => {
    windowStore.reset();
    windowStore.setViewport({ width: 1440, height: 900 });
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

  it('enforces single-instance apps and allows multi-instance apps', () => {
    const dashboard = windowStore.launch('dashboard');
    const dashboardAgain = windowStore.launch('dashboard');
    expect(dashboardAgain).toBe(dashboard);
    expect(windowStore.getSnapshot()).toHaveLength(1);

    const filesOne = windowStore.launch('files', { payload: { initialPath: '/' } });
    const filesTwo = windowStore.launch('files', { payload: { initialPath: '/data' } });
    expect(filesTwo).not.toBe(filesOne);
    expect(windowStore.getSnapshot().filter((item) => item.appId === 'files')).toHaveLength(2);
    expect(new Set(windowStore.getSnapshot().map((item) => item.id)).size).toBe(3);
  });

  it('keeps minimized windows in the snapshot and restores their previous state', () => {
    const id = windowStore.launch('terminal');
    windowStore.toggleMaximize(id);
    windowStore.minimize(id);
    const minimized = windowStore.getSnapshot().find((item) => item.id === id)!;
    expect(minimized.state).toBe('minimized');
    // The record (and therefore the mounted component) stays alive.
    expect(windowStore.getSnapshot()).toHaveLength(1);

    windowStore.focus(id);
    expect(windowStore.getSnapshot().find((item) => item.id === id)?.state).toBe('maximized');
  });

  it('raises focus, keeps z-index bounded and skips minimized windows', () => {
    const ids = Array.from({ length: 8 }, (_, index) => windowStore.launch('files', { instanceKey: `f${index}` }));
    for (let round = 0; round < 20; round += 1) {
      for (const id of ids) windowStore.focus(id);
    }
    const z = windowStore.getSnapshot().map((item) => item.zIndex);
    expect(new Set(z).size).toBe(ids.length);
    expect(Math.max(...z)).toBeLessThanOrEqual(10 + ids.length - 1);
    expect(windowStore.getFocusedId()).toBe(ids[ids.length - 1]);

    windowStore.minimize(ids[ids.length - 1]);
    windowStore.cycleFocus(1);
    expect(windowStore.getSnapshot().find((item) => item.id === windowStore.getFocusedId())?.state).not.toBe('minimized');
  });

  it('clamps move and resize to the viewport, Dock clearance and minimum sizes', () => {
    const id = windowStore.open({ kind: 'editor', title: '编辑 /x', payload: { path: '/x' } });
    windowStore.move(id, -5000, -5000);
    let item = windowStore.getSnapshot().find((entry) => entry.id === id)!;
    expect(item.x).toBeLessThan(0);
    expect(item.x).toBeGreaterThanOrEqual(-item.width + 160);
    expect(item.y).toBe(48);

    windowStore.move(id, 5000, 5000);
    item = windowStore.getSnapshot().find((entry) => entry.id === id)!;
    expect(item.x).toBeLessThanOrEqual(1440 - 160);
    expect(item.y).toBeLessThanOrEqual(900 - 40 - 12);

    windowStore.resize(id, 10, 10);
    item = windowStore.getSnapshot().find((entry) => entry.id === id)!;
    expect(item.width).toBeGreaterThanOrEqual(380);
    expect(item.height).toBeGreaterThanOrEqual(240);

    windowStore.resize(id, 999999, 999999);
    item = windowStore.getSnapshot().find((entry) => entry.id === id)!;
    expect(item.width).toBeLessThanOrEqual(1440 - 24);
    expect(item.height).toBeLessThanOrEqual(900 - 48 - 92);
  });

  it('re-constrains windows when the viewport shrinks', () => {
    const id = windowStore.launch('dashboard');
    windowStore.setViewport({ width: 600, height: 420 });
    const item = windowStore.getSnapshot().find((entry) => entry.id === id)!;
    expect(item.width).toBeLessThanOrEqual(600 - 24);
    // The app minimum height (360) wins over the remaining work area (280).
    expect(item.height).toBe(360);
    expect(item.y).toBeLessThanOrEqual(420 - 40 - 12);
  });

  it('toggles maximize, restores geometry and closes windows', () => {
    const id = windowStore.open({ kind: 'preview', title: '预览 /x', payload: { path: '/x' } });
    const before = windowStore.getSnapshot().find((item) => item.id === id)!;
    windowStore.toggleMaximize(id);
    expect(windowStore.getSnapshot().find((item) => item.id === id)?.state).toBe('maximized');
    windowStore.toggleMaximize(id);
    const restored = windowStore.getSnapshot().find((item) => item.id === id)!;
    expect(restored.state).toBe('normal');
    expect({ x: restored.x, y: restored.y, width: restored.width, height: restored.height }).toEqual({
      x: before.x,
      y: before.y,
      width: before.width,
      height: before.height,
    });
    windowStore.close(id);
    expect(windowStore.getSnapshot()).toHaveLength(0);
  });

  it('lets an app veto closing (unsaved editor content)', () => {
    const id = windowStore.launch('editor', { title: '编辑 /a.txt', payload: { path: '/a.txt' }, instanceKey: '/a.txt' });
    let dirty = true;
    const unregister = windowStore.registerCloseGuard(id, () => !dirty);
    expect(windowStore.requestClose(id)).toBe(false);
    expect(windowStore.getSnapshot()).toHaveLength(1);
    dirty = false;
    expect(windowStore.requestClose(id)).toBe(true);
    expect(windowStore.getSnapshot()).toHaveLength(0);
    unregister();
  });

  it('restores a layout record-by-record, dropping invalid records', () => {
    windowStore.restoreWindows(
      [
        {
          id: 'files:bad-geometry',
          appId: 'files',
          x: -99999,
          y: 99999,
          width: 99999,
          height: 99999,
          state: 'normal',
          payload: { initialPath: '/data' },
        },
        { id: 'dashboard:one', appId: 'dashboard', x: 100, y: 100, width: 800, height: 600, state: 'normal' },
        { id: 'dashboard:two', appId: 'dashboard', x: 120, y: 120, width: 800, height: 600, state: 'minimized' },
        { id: 'editor:nopath', appId: 'editor', x: 0, y: 0, width: 500, height: 400, state: 'normal', payload: {} },
        {
          id: 'terminal:ended',
          appId: 'terminal',
          x: 200,
          y: 100,
          width: 900,
          height: 600,
          state: 'normal',
          payload: { sessionId: 'gone' },
        },
      ],
      { sessions: [SESSION] },
    );
    const windows = windowStore.getSnapshot();
    expect(windows.map((item) => item.id)).toEqual(['files:bad-geometry', 'dashboard:one', 'terminal:ended']);
    expect(windows[0].width).toBeLessThanOrEqual(1440 - 24);
    expect(windows[0].payload.initialPath).toBe('/data');
    expect(windows[1].state).toBe('normal');
    expect(windows[2].restoreError).toBe('session_ended');
  });

  it('keeps a terminal restore valid when the session still exists', () => {
    windowStore.restoreWindows(
      [
        {
          id: 'terminal:live',
          appId: 'terminal',
          x: 200,
          y: 100,
          width: 900,
          height: 600,
          state: 'normal',
          payload: { sessionId: 's1' },
        },
      ],
      { sessions: [SESSION] },
    );
    expect(windowStore.getSnapshot()[0].restoreError).toBeNull();
  });

  it('computes constrained geometry as a pure helper', () => {
    const geometry = constrainGeometry(
      { x: 9999, y: 9999, width: 9999, height: 9999 },
      { width: 800, height: 600 },
      { width: 400, height: 300 },
    );
    expect(geometry.width).toBeLessThanOrEqual(800 - 24);
    expect(geometry.height).toBeLessThanOrEqual(600 - 48 - 92);
    expect(geometry.x).toBeLessThanOrEqual(800 - 160);
    expect(geometry.y).toBeLessThanOrEqual(600 - 40 - 12);
  });
});
