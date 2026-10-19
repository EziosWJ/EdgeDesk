import { beforeEach, describe, expect, it } from 'vitest';
import {
  LAYOUT_KEY,
  buildLayout,
  loadLayout,
  parseLayout,
  restoreDesktop,
  sanitizePayload,
  saveLayout,
  startDesktopPersistence,
  type DesktopLayoutV1,
  type StorageLike,
} from './desktopPersistence';
import { desktopStore } from './desktopStore';
import { windowStore } from '../stores/windowStore';

class MemoryStorage implements StorageLike {
  data = new Map<string, string>();
  getItem(key: string): string | null {
    return this.data.get(key) ?? null;
  }
  setItem(key: string, value: string): void {
    this.data.set(key, value);
  }
  removeItem(key: string): void {
    this.data.delete(key);
  }
}

function layout(overrides: Partial<DesktopLayoutV1> = {}): DesktopLayoutV1 {
  return {
    schemaVersion: 1,
    icons: [
      { appId: 'dashboard', col: 0, row: 0 },
      { appId: 'files', col: 1, row: 0 },
    ],
    dock: ['dashboard', 'files', 'terminal', 'sessions', 'processes', 'settings'],
    windows: [
      {
        id: 'dashboard:solo',
        appId: 'dashboard',
        title: '设备概览',
        x: 80,
        y: 100,
        width: 900,
        height: 600,
        state: 'normal',
        payload: {},
      },
    ],
    ...overrides,
  };
}

describe('DesktopPersistence', () => {
  beforeEach(() => {
    windowStore.reset();
    desktopStore.reset();
    windowStore.setViewport({ width: 1440, height: 900 });
  });

  it('serializes and reloads icon slots, Dock and window state', () => {
    desktopStore.setIcons([
      { appId: 'terminal', col: 2, row: 1 },
      { appId: 'files', col: 0, row: 3 },
    ]);
    const id = windowStore.launch('files', {
      title: '文件 /data',
      payload: { initialPath: '/data' },
      instanceKey: '/data',
    });
    windowStore.toggleMaximize(id);
    const storage = new MemoryStorage();
    const document = buildLayout();
    expect(saveLayout(document, storage)).toBe(true);

    const loaded = loadLayout(storage);
    expect(loaded.warning).toBeNull();
    expect(loaded.layout?.icons).toEqual([
      { appId: 'terminal', col: 2, row: 1 },
      { appId: 'files', col: 0, row: 3 },
    ]);
    expect(loaded.layout?.dock).toEqual(['dashboard', 'files', 'terminal', 'sessions', 'processes', 'settings']);
    expect(loaded.layout?.windows[0]).toMatchObject({
      appId: 'files',
      state: 'maximized',
      payload: { initialPath: '/data' },
    });
  });

  it('drops credentials and file bodies instead of persisting them', () => {
    const sanitized = sanitizePayload({
      sessionId: 's1',
      initialPath: '/data',
      password: 'secret',
      token: 'abc',
      cookie: 'edgedesk_session=xyz',
      content: 'private file body',
      nested: { path: '/should-not-survive' },
    });
    expect(sanitized).toEqual({ sessionId: 's1', initialPath: '/data' });
    expect(JSON.stringify(sanitized)).not.toContain('secret');
    expect(JSON.stringify(sanitized)).not.toContain('private file body');
  });

  it('falls back safely on corrupt JSON and incompatible versions', () => {
    expect(parseLayout('{not-json').layout).toBeNull();
    expect(parseLayout('{not-json').warning).toContain('损坏');
    expect(parseLayout(JSON.stringify({ schemaVersion: 99, icons: [], dock: [], windows: [] })).warning).toContain(
      '更新版本',
    );
    expect(parseLayout(JSON.stringify({ schemaVersion: 0 })).warning).toContain('不兼容');
  });

  it('drops unknown apps, duplicate single-instance windows and invalid geometry', () => {
    const raw = JSON.stringify({
      schemaVersion: 1,
      icons: [
        { appId: 'unknown', col: 0, row: 0 },
        { appId: 'files', col: 9, row: 0 },
        { appId: 'terminal', col: 1, row: 1 },
      ],
      dock: ['unknown', 'dashboard', 'dashboard'],
      windows: [
        layout().windows[0],
        { ...layout().windows[0], id: 'dashboard:duplicate' },
        { id: 'files:bad', appId: 'files', x: 0, y: 0, width: -5, height: 100, state: 'normal' },
        { id: 'files:ok', appId: 'files', x: 20, y: 80, width: 700, height: 500, state: 'normal', payload: {} },
        { id: 'editor:no-path', appId: 'editor', x: 20, y: 80, width: 700, height: 500, state: 'normal' },
      ],
    });
    const parsed = parseLayout(raw).layout!;
    expect(parsed.icons).toEqual([{ appId: 'terminal', col: 1, row: 1 }]);
    expect(parsed.dock).toEqual(['dashboard']);
    expect(parsed.windows.map((item) => item.id)).toEqual(['dashboard:solo', 'files:ok']);
  });

  it('handles unavailable storage without throwing or blocking login', () => {
    const result = loadLayout(null);
    expect(result.layout).toBeNull();
    expect(result.warning).toContain('本地存储不可用');
    expect(saveLayout(layout(), null)).toBe(false);

    const throwing: StorageLike = {
      getItem: () => {
        throw new Error('denied');
      },
      setItem: () => {
        throw new Error('quota');
      },
      removeItem: () => {},
    };
    expect(loadLayout(throwing).warning).toContain('无法读取');
    expect(saveLayout(layout(), throwing)).toBe(false);
  });

  it('rebuilds the workspace and marks ended terminal sessions without creating replacements', () => {
    const storage = new MemoryStorage();
    saveLayout(
      layout({
        windows: [
          layout().windows[0],
          {
            id: 'terminal:gone',
            appId: 'terminal',
            title: '已结束会话',
            x: 200,
            y: 120,
            width: 800,
            height: 500,
            state: 'normal',
            payload: { sessionId: 'removed-session' },
          },
        ],
      }),
      storage,
    );
    desktopStore.setIcons([]);
    desktopStore.setDock([]);
    const warning = restoreDesktop({ storage, sessions: [], viewport: { width: 1200, height: 800 } });
    expect(warning).toBeNull();
    expect(desktopStore.getSnapshot().icons).toHaveLength(2);
    expect(desktopStore.getSnapshot().dock).toEqual(['dashboard', 'files', 'terminal', 'sessions', 'processes', 'settings']);
    const windows = windowStore.getSnapshot();
    expect(windows).toHaveLength(2);
    expect(windows[1].restoreError).toBe('session_ended');
    expect(windows[1].payload.sessionId).toBe('removed-session');
    // Geometry was re-constrained to the current viewport.
    expect(windows[0].width).toBeLessThanOrEqual(1200 - 24);
  });

  it('migrates a legacy multi-tab terminal window to the session manager', () => {
    const storage = new MemoryStorage();
    saveLayout(
      layout({
        windows: [
          {
            id: 'terminal:workspace',
            appId: 'terminal',
            title: '终端工作区',
            x: 200,
            y: 120,
            width: 800,
            height: 500,
            state: 'normal',
            payload: {},
          },
        ],
      }),
      storage,
    );
    restoreDesktop({ storage, sessions: [], viewport: { width: 1200, height: 800 } });
    const windows = windowStore.getSnapshot();
    expect(windows).toHaveLength(1);
    expect(windows[0].appId).toBe('sessions');
    expect(windows[0].restoreError).toBeNull();
  });

  it('starts from defaults when no layout exists', () => {
    const storage = new MemoryStorage();
    expect(restoreDesktop({ storage, sessions: [] })).toBeNull();
    expect(desktopStore.getSnapshot().icons.map((icon) => icon.appId)).toEqual(['dashboard', 'files', 'terminal', 'sessions', 'processes']);
    expect(windowStore.getSnapshot()).toHaveLength(0);
    expect(desktopStore.isRestored()).toBe(true);
  });

  it('persists changes after restore through the debounced autosave', async () => {
    const storage = new MemoryStorage();
    // startDesktopPersistence uses the real localStorage through defaultStorage.
    window.localStorage.removeItem(LAYOUT_KEY);
    restoreDesktop({ storage, sessions: [] });
    const stop = startDesktopPersistence();
    windowStore.launch('dashboard');
    await new Promise((resolve) => setTimeout(resolve, 320));
    const saved = window.localStorage.getItem(LAYOUT_KEY);
    stop();
    expect(saved).not.toBeNull();
    expect(JSON.parse(saved!).windows).toHaveLength(1);
    window.localStorage.removeItem(LAYOUT_KEY);
  });
});
