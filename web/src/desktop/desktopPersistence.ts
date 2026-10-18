import type { AppId, TerminalSessionInfo, ViewportSize, WindowPayload, WindowStatus } from '../types';
import { appRegistry } from './appRegistry';
import { desktopStore, ICON_COLUMNS, ICON_ROWS, type IconSlot } from './desktopStore';
import { windowStore, type RestoredWindow } from '../stores/windowStore';

export const LAYOUT_KEY = 'edgedesk.desktop.layout.v1';
export const SCHEMA_VERSION = 1;
const MAX_WINDOWS = 24;
const MAX_TITLE = 120;

export type PersistedWindow = {
  id: string;
  appId: AppId;
  title: string;
  x: number;
  y: number;
  width: number;
  height: number;
  state: WindowStatus;
  payload: WindowPayload;
};

export type DesktopLayoutV1 = {
  schemaVersion: 1;
  icons: IconSlot[];
  dock: AppId[];
  windows: PersistedWindow[];
};

export type StorageLike = {
  getItem: (key: string) => string | null;
  setItem: (key: string, value: string) => void;
  removeItem: (key: string) => void;
};

export type LoadResult = {
  layout: DesktopLayoutV1 | null;
  warning: string | null;
};

const STATUSES: readonly WindowStatus[] = ['normal', 'minimized', 'maximized'];

export function defaultStorage(): StorageLike | null {
  try {
    if (typeof window === 'undefined' || !window.localStorage) return null;
    return window.localStorage;
  } catch {
    return null;
  }
}

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === 'object' && value !== null && !Array.isArray(value);
}

function finite(value: unknown): value is number {
  return typeof value === 'number' && Number.isFinite(value) && Math.abs(value) < 1_000_000;
}

function cleanString(value: unknown, max = MAX_TITLE): string {
  return typeof value === 'string' ? value.replace(/[\u0000-\u001f\u007f]/g, ' ').trim().slice(0, max) : '';
}

/** Only reconnect-relevant identifiers survive; tokens, cookies and file bodies never enter layout state. */
export function sanitizePayload(raw: unknown): WindowPayload {
  if (!isRecord(raw)) return {};
  const payload: WindowPayload = {};
  const sessionId = cleanString(raw.sessionId, 160);
  if (sessionId) payload.sessionId = sessionId;
  const path = cleanString(raw.path, 4096);
  if (path) payload.path = path;
  const initialPath = cleanString(raw.initialPath, 4096);
  if (initialPath) payload.initialPath = initialPath;
  return payload;
}

function sanitizeIcon(raw: unknown): IconSlot | null {
  if (!isRecord(raw)) return null;
  const appId = typeof raw.appId === 'string' ? raw.appId : '';
  const app = appRegistry.get(appId);
  if (!app || !app.desktopShortcut) return null;
  if (!Number.isInteger(raw.col) || !Number.isInteger(raw.row)) return null;
  const col = raw.col as number;
  const row = raw.row as number;
  if (col < 0 || col >= ICON_COLUMNS || row < 0 || row >= ICON_ROWS) return null;
  return { appId: app.id, col, row };
}

function sanitizeDock(raw: unknown): AppId[] {
  if (!Array.isArray(raw)) return [];
  const seen = new Set<AppId>();
  const dock: AppId[] = [];
  for (const value of raw) {
    if (typeof value !== 'string') continue;
    const app = appRegistry.get(value);
    if (!app || !app.dockPinned || seen.has(app.id)) continue;
    seen.add(app.id);
    dock.push(app.id);
  }
  return dock;
}

function sanitizeWindow(raw: unknown): PersistedWindow | null {
  if (!isRecord(raw)) return null;
  const appId = typeof raw.appId === 'string' ? raw.appId : '';
  const app = appRegistry.get(appId);
  if (!app) return null;
  const id = cleanString(raw.id, 200);
  if (!id) return null;
  if (!finite(raw.x) || !finite(raw.y) || !finite(raw.width) || !finite(raw.height)) return null;
  if (raw.width <= 0 || raw.height <= 0) return null;
  const payload = sanitizePayload(raw.payload);
  if ((app.id === 'editor' || app.id === 'preview') && !payload.path) return null;
  const state = STATUSES.includes(raw.state as WindowStatus) ? (raw.state as WindowStatus) : 'normal';
  return {
    id,
    appId: app.id,
    title: cleanString(raw.title) || app.name,
    x: raw.x,
    y: raw.y,
    width: raw.width,
    height: raw.height,
    state,
    payload,
  };
}

function sanitizeWindows(raw: unknown): PersistedWindow[] {
  if (!Array.isArray(raw)) return [];
  const windows: PersistedWindow[] = [];
  const ids = new Set<string>();
  const singles = new Set<AppId>();
  for (const value of raw.slice(0, MAX_WINDOWS)) {
    const window = sanitizeWindow(value);
    if (!window || ids.has(window.id)) continue;
    const app = appRegistry.require(window.appId);
    if (app.instancePolicy === 'single') {
      if (singles.has(app.id)) continue;
      singles.add(app.id);
    }
    ids.add(window.id);
    windows.push(window);
  }
  return windows;
}

/** Parse, validate and migrate a raw layout document. Never throws. */
export function parseLayout(raw: string | null): LoadResult {
  if (!raw) return { layout: null, warning: null };
  let parsed: unknown;
  try {
    parsed = JSON.parse(raw);
  } catch {
    return { layout: null, warning: '桌面布局数据损坏，已使用默认桌面。' };
  }
  if (!isRecord(parsed)) {
    return { layout: null, warning: '桌面布局格式无效，已使用默认桌面。' };
  }
  const version = parsed.schemaVersion;
  if (version !== SCHEMA_VERSION) {
    return {
      layout: null,
      warning:
        typeof version === 'number' && version > SCHEMA_VERSION
          ? '桌面布局来自更新版本，已使用默认桌面。'
          : '桌面布局版本不兼容，已使用默认桌面。',
    };
  }
  const validIcons = Array.isArray(parsed.icons) ? parsed.icons.map(sanitizeIcon).filter((item): item is IconSlot => item !== null) : [];
  const icons = validIcons.filter((icon, index, all) => all.findIndex((other) => other.appId === icon.appId) === index);
  const dock = sanitizeDock(parsed.dock);
  const windows = sanitizeWindows(parsed.windows);
  return {
    layout: {
      schemaVersion: SCHEMA_VERSION,
      icons,
      dock,
      windows,
    },
    warning: null,
  };
}

export function loadLayout(storage: StorageLike | null = defaultStorage()): LoadResult {
  if (!storage) {
    return { layout: null, warning: '浏览器本地存储不可用，桌面布局不会被保存。' };
  }
  try {
    return parseLayout(storage.getItem(LAYOUT_KEY));
  } catch {
    return { layout: null, warning: '无法读取桌面布局，已使用默认桌面。' };
  }
}

export function buildLayout(): DesktopLayoutV1 {
  const desktop = desktopStore.getSnapshot();
  const windows: PersistedWindow[] = windowStore
    .exportWindows()
    .slice(0, MAX_WINDOWS)
    .map((item) => ({
      id: item.id,
      appId: item.appId,
      title: item.title,
      x: item.x,
      y: item.y,
      width: item.width,
      height: item.height,
      state: item.state,
      payload: sanitizePayload(item.payload),
    }));
  return {
    schemaVersion: SCHEMA_VERSION,
    icons: desktop.icons.filter((icon) => appRegistry.get(icon.appId)?.desktopShortcut),
    dock: desktop.dock.filter((appId) => appRegistry.get(appId)?.dockPinned),
    windows,
  };
}

/** Returns false instead of throwing on quota/private-mode failures. */
export function saveLayout(layout: DesktopLayoutV1 = buildLayout(), storage: StorageLike | null = defaultStorage()): boolean {
  if (!storage) return false;
  try {
    storage.setItem(LAYOUT_KEY, JSON.stringify(layout));
    return true;
  } catch {
    return false;
  }
}

export type RestoreDesktopOptions = {
  viewport?: ViewportSize;
  sessions?: readonly TerminalSessionInfo[] | null;
  storage?: StorageLike | null;
};

/** Rebuild the desktop from local layout. A bad record is dropped, never fatal. */
export function restoreDesktop(options: RestoreDesktopOptions = {}): string | null {
  const loaded = loadLayout(options.storage ?? defaultStorage());
  const layout = loaded.layout;
  if (!layout) {
    desktopStore.setIcons(defaultDesktopIcons());
    desktopStore.setDock(defaultDockApps());
    windowStore.restoreWindows([], {
      viewport: options.viewport ?? windowStore.getViewport(),
      sessions: options.sessions ?? null,
    });
    desktopStore.setWarning(loaded.warning);
    desktopStore.markRestored();
    return loaded.warning;
  }

  desktopStore.setIcons(layout.icons.length > 0 ? layout.icons : defaultDesktopIcons());
  desktopStore.setDock(layout.dock.length > 0 ? layout.dock : defaultDockApps());
  const restored: RestoredWindow[] = layout.windows.map((item) => ({
    id: item.id,
    appId: item.appId,
    title: item.title,
    x: item.x,
    y: item.y,
    width: item.width,
    height: item.height,
    state: item.state,
    payload: item.payload,
  }));
  windowStore.restoreWindows(restored, {
    viewport: options.viewport ?? windowStore.getViewport(),
    sessions: options.sessions ?? null,
  });
  desktopStore.setWarning(loaded.warning);
  desktopStore.markRestored();
  return loaded.warning;
}

function defaultDesktopIcons(): IconSlot[] {
  return appRegistry.desktopApps().map((app, index) => ({ appId: app.id, col: 0, row: index }));
}

function defaultDockApps(): AppId[] {
  return appRegistry.dockApps().map((app) => app.id);
}

const SAVE_DELAY_MS = 250;

/**
 * Persist layout changes after they settle. Failures only surface as a
 * non-blocking notice; dragging and window interaction always stay responsive.
 */
export function startDesktopPersistence(): () => void {
  let timer: number | null = null;
  let disposed = false;

  const flush = () => {
    timer = null;
    if (disposed || !desktopStore.isRestored()) return;
    if (!saveLayout()) {
      desktopStore.setWarning('无法保存桌面布局，浏览器存储可能已满或被禁用。');
    }
  };

  const schedule = () => {
    if (disposed || !desktopStore.isRestored()) return;
    if (timer !== null) return;
    timer = window.setTimeout(flush, SAVE_DELAY_MS);
  };

  const unsubscribeWindows = windowStore.subscribe(schedule);
  const unsubscribeDesktop = desktopStore.subscribe(schedule);

  return () => {
    disposed = true;
    if (timer !== null) window.clearTimeout(timer);
    unsubscribeWindows();
    unsubscribeDesktop();
  };
}
