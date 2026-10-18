import { useSyncExternalStore } from 'react';
import type { AppId, TerminalSessionInfo, ViewportSize, WindowPayload, WindowState, WindowStatus } from '../types';
import { appRegistry, type AppDefinition } from '../desktop/appRegistry';

type Listener = () => void;

export type OpenWindowRequest = {
  appId?: AppId;
  /** Backwards-compatible alias for appId used by document windows. */
  kind?: AppId;
  title?: string;
  payload?: WindowPayload;
  width?: number;
  height?: number;
  /** Stable key for multi-instance windows (session id, path, caller-chosen id). */
  instanceKey?: string;
};

export type RestoredWindow = {
  id?: string;
  appId: AppId;
  title?: string;
  x: number;
  y: number;
  width: number;
  height: number;
  state: WindowStatus;
  payload?: WindowPayload;
};

export type RestoreOptions = {
  viewport?: ViewportSize;
  /** null means the server list could not be fetched; validate later. */
  sessions?: readonly TerminalSessionInfo[] | null;
};

const BASE_Z = 10;
const CASCADE_STEP = 32;
const WINDOW_MARGIN = 12;
const TOPBAR_HEIGHT = 48;
const DOCK_CLEARANCE = 92;
/** Minimum title-bar width that must stay inside the viewport. */
const MIN_VISIBLE_TITLE = 160;
const TITLEBAR_HEIGHT = 40;

function defaultViewport(): ViewportSize {
  if (typeof window === 'undefined') return { width: 1440, height: 900 };
  return {
    width: Math.max(320, window.innerWidth || 1440),
    height: Math.max(240, window.innerHeight || 900),
  };
}

export type Geometry = { x: number; y: number; width: number; height: number };

/** Fit a window into the usable desktop area while keeping its title bar reachable. */
export function constrainGeometry(
  geometry: Geometry,
  viewport: ViewportSize,
  minSize: { width: number; height: number },
): Geometry {
  const maxWidth = Math.max(minSize.width, viewport.width - WINDOW_MARGIN * 2);
  const width = Math.min(Math.max(geometry.width, minSize.width), maxWidth);
  // Keep the title bar above the Dock so windows never hide dock targets.
  const maxHeight = Math.max(minSize.height, viewport.height - TOPBAR_HEIGHT - DOCK_CLEARANCE);
  const height = Math.min(Math.max(geometry.height, minSize.height), maxHeight);
  const minX = -width + MIN_VISIBLE_TITLE;
  const maxX = Math.max(minX, viewport.width - MIN_VISIBLE_TITLE);
  const x = Math.min(Math.max(geometry.x, minX), maxX);
  const minY = TOPBAR_HEIGHT;
  const maxY = Math.max(minY, viewport.height - TITLEBAR_HEIGHT - WINDOW_MARGIN);
  const y = Math.min(Math.max(geometry.y, minY), maxY);
  return { x, y, width, height };
}

function sanitizeTitle(value: string | undefined, fallback: string): string {
  const title = (value ?? '').replace(/[\u0000-\u001f\u007f]/g, ' ').trim();
  return title.slice(0, 120) || fallback;
}

function stableKey(raw: string): string {
  const cleaned = raw.replace(/[^a-zA-Z0-9_.-]+/g, '_').slice(0, 160);
  return cleaned || 'window';
}

export class WindowStore {
  private windows: WindowState[] = [];
  private focusedId: string | null = null;
  private listeners = new Set<Listener>();
  private viewport: ViewportSize = defaultViewport();
  private sequence = 0;
  private closeGuards = new Map<string, () => boolean>();

  subscribe = (listener: Listener): (() => void) => {
    this.listeners.add(listener);
    return () => this.listeners.delete(listener);
  };

  getSnapshot = (): WindowState[] => this.windows;
  getFocusedId = (): string | null => this.focusedId;
  getViewport = (): ViewportSize => this.viewport;

  get(id: string): WindowState | undefined {
    return this.windows.find((item) => item.id === id);
  }

  private notify(): void {
    this.listeners.forEach((listener) => listener());
  }

  private emit(): void {
    this.windows = [...this.windows];
    this.notify();
  }

  /** Normalize z-index from array order so it can never grow without bound. */
  private applyZOrder(): void {
    this.windows = this.windows.map((item, index) => ({ ...item, zIndex: BASE_Z + index }));
  }

  private bringToFront(id: string): void {
    const index = this.windows.findIndex((item) => item.id === id);
    if (index < 0) return;
    const [target] = this.windows.splice(index, 1);
    this.windows.push(target);
    this.applyZOrder();
  }

  /** State to use when a minimized window is brought back. */
  private restoredState(target: WindowState): WindowStatus {
    return target.maxRestore ? 'maximized' : 'normal';
  }

  private topVisible(): WindowState | null {
    for (let index = this.windows.length - 1; index >= 0; index -= 1) {
      if (this.windows[index].state !== 'minimized') return this.windows[index];
    }
    return null;
  }

  setViewport(size: ViewportSize): void {
    const width = Math.max(320, Math.round(size.width));
    const height = Math.max(240, Math.round(size.height));
    if (width === this.viewport.width && height === this.viewport.height) return;
    this.viewport = { width, height };
    this.windows = this.windows.map((item) => {
      const app = appRegistry.require(item.appId);
      const geometry = constrainGeometry(item, this.viewport, app.minSize);
      return { ...item, ...geometry };
    });
    this.emit();
  }

  /** Open a window. Single-instance apps focus the existing window instead. */
  open(request: OpenWindowRequest): string {
    const appId = request.appId ?? request.kind;
    if (!appId) throw new Error('window request requires appId');
    const app = appRegistry.require(appId);
    return this.launchDefinition(app, request);
  }

  launch(appId: AppId, options: Omit<OpenWindowRequest, 'appId' | 'kind'> = {}): string {
    return this.open({ ...options, appId });
  }

  private launchDefinition(app: AppDefinition, request: OpenWindowRequest): string {
    const existing = app.instancePolicy === 'single' ? this.windows.find((item) => item.appId === app.id) : undefined;
    if (existing) {
      this.update(existing.id, {
        state: existing.state === 'minimized' ? this.restoredState(existing) : existing.state,
        payload: { ...existing.payload, ...(request.payload ?? {}) },
        title: sanitizeTitle(request.title, existing.title),
      });
      this.focus(existing.id);
      return existing.id;
    }

    const payload: WindowPayload = { ...(request.payload ?? {}) };
    // One standalone terminal per backend session: opening a session that is
    // already displayed should focus that window instead of attaching twice.
    if (app.id === 'terminal' && payload.sessionId) {
      const bound = this.windows.find(
        (item) => item.appId === 'terminal' && item.payload.sessionId === payload.sessionId,
      );
      if (bound) {
        this.update(bound.id, { title: sanitizeTitle(request.title, bound.title) });
        this.focus(bound.id);
        return bound.id;
      }
    }
    // Document windows and session-bound terminals are naturally keyed by their
    // resource so reopening focuses the same window. App launchers (files) get a
    // fresh instance each time unless the caller supplies an explicit key.
    const resourceKey =
      app.id === 'editor' || app.id === 'preview'
        ? payload.path
        : app.id === 'terminal'
          ? payload.sessionId
          : undefined;
    const explicitKey = request.instanceKey ?? resourceKey ?? `w${++this.sequence}`;
    let id = `${app.id}:${stableKey(explicitKey)}`;
    if (app.instancePolicy === 'single') id = `${app.id}:solo`;
    const collision = this.windows.find((item) => item.id === id);
    if (collision) {
      if (app.instancePolicy === 'multi' && (payload.sessionId || payload.path || payload.initialPath)) {
        this.focus(collision.id);
        return collision.id;
      }
      id = `${app.id}:${stableKey(explicitKey)}_${++this.sequence}`;
    }

    const size = {
      width: request.width ?? app.defaultSize.width,
      height: request.height ?? app.defaultSize.height,
    };
    const offset = (this.windows.length % 6) * CASCADE_STEP;
    const geometry = constrainGeometry(
      {
        x: WINDOW_MARGIN + 40 + offset,
        y: TOPBAR_HEIGHT + 24 + offset,
        ...size,
      },
      this.viewport,
      app.minSize,
    );
    // A newly opened window should be fully visible when it fits. constrainGeometry
    // only guarantees a reachable title bar, which leaves the close button clipped
    // on medium viewports once the default size is clamped to the viewport width.
    const maxFittingX = this.viewport.width - geometry.width - WINDOW_MARGIN;
    if (maxFittingX >= WINDOW_MARGIN) {
      geometry.x = Math.min(geometry.x, maxFittingX);
    }
    const window: WindowState = {
      id,
      appId: app.id,
      title: sanitizeTitle(request.title, app.name),
      ...geometry,
      zIndex: BASE_Z,
      state: 'normal',
      payload,
    };
    this.windows.push(window);
    this.bringToFront(id);
    this.focusedId = id;
    this.emit();
    return id;
  }

  private update(id: string, patch: Partial<WindowState>): void {
    let changed = false;
    const next = this.windows.map((item) => {
      if (item.id !== id) return item;
      changed = true;
      return { ...item, ...patch };
    });
    if (!changed) return;
    this.windows = next;
    this.notify();
  }

  focus(id: string): void {
    const target = this.get(id);
    if (!target) return;
    if (target.state === 'minimized') {
      this.update(id, { state: this.restoredState(target) });
    }
    this.bringToFront(id);
    this.focusedId = id;
    this.emit();
  }

  /** Focus the next/previous non-minimized window in z order. */
  cycleFocus(direction: 1 | -1): void {
    const visible = this.windows.filter((item) => item.state !== 'minimized');
    if (visible.length === 0) return;
    const current = visible.findIndex((item) => item.id === this.focusedId);
    const nextIndex = ((current < 0 ? 0 : current + direction) + visible.length) % visible.length;
    this.focus(visible[nextIndex].id);
  }

  move(id: string, x: number, y: number): void {
    const target = this.get(id);
    if (!target || target.state === 'maximized') return;
    const app = appRegistry.require(target.appId);
    const geometry = constrainGeometry({ ...target, x, y }, this.viewport, app.minSize);
    this.update(id, { x: geometry.x, y: geometry.y });
  }

  resize(id: string, width: number, height: number): void {
    const target = this.get(id);
    if (!target || target.state === 'maximized') return;
    const app = appRegistry.require(target.appId);
    const geometry = constrainGeometry({ ...target, width, height }, this.viewport, app.minSize);
    this.update(id, { width: geometry.width, height: geometry.height });
  }

  toggleMaximize(id: string): void {
    const target = this.get(id);
    if (!target) return;
    if (target.state === 'maximized') {
      const restore = target.maxRestore;
      if (restore) {
        const app = appRegistry.require(target.appId);
        const geometry = constrainGeometry(restore, this.viewport, app.minSize);
        this.update(id, { ...geometry, state: 'normal', maxRestore: undefined });
      } else {
        this.update(id, { state: 'normal', maxRestore: undefined });
      }
    } else {
      this.update(id, {
        state: 'maximized',
        maxRestore: { x: target.x, y: target.y, width: target.width, height: target.height },
      });
    }
    this.focus(id);
  }

  minimize(id: string): void {
    const target = this.get(id);
    if (!target || target.state === 'minimized') return;
    this.update(id, { state: 'minimized' });
    const top = this.topVisible();
    this.focusedId = top?.id ?? null;
    this.emit();
  }

  restoreMinimized(id: string): void {
    const target = this.get(id);
    if (!target || target.state !== 'minimized') return;
    this.focus(id);
  }

  close(id: string): void {
    this.forceClose(id);
  }

  /** Runs an app-supplied guard (for example unsaved editor content). */
  requestClose(id: string): boolean {
    const guard = this.closeGuards.get(id);
    if (guard && guard() === false) return false;
    this.forceClose(id);
    return true;
  }

  forceClose(id: string): void {    const before = this.windows.length;
    this.windows = this.windows.filter((item) => item.id !== id);
    if (this.windows.length === before) return;
    this.closeGuards.delete(id);
    this.applyZOrder();
    if (this.focusedId === id) this.focusedId = this.topVisible()?.id ?? null;
    this.emit();
  }

  /** Force-close every window of an app (disable, uninstall, isolation). */
  closeAllForApp(appId: AppId): number {
    const before = this.windows.length;
    const closedFocus = this.focusedId ? this.get(this.focusedId)?.appId === appId : false;
    this.windows = this.windows.filter((item) => item.appId !== appId);
    const closed = before - this.windows.length;
    if (closed === 0) return 0;
    this.applyZOrder();
    if (closedFocus) this.focusedId = this.topVisible()?.id ?? null;
    this.emit();
    return closed;
  }

  registerCloseGuard(id: string, guard: () => boolean): () => void {    this.closeGuards.set(id, guard);
    return () => {
      if (this.closeGuards.get(id) === guard) this.closeGuards.delete(id);
    };
  }

  updatePayload(id: string, patch: Partial<WindowPayload>): void {
    const target = this.get(id);
    if (!target) return;
    this.update(id, { payload: { ...target.payload, ...patch } });
  }

  setRestoreError(id: string, error: WindowState['restoreError']): void {
    this.update(id, { restoreError: error });
  }

  setTitle(id: string, title: string): void {
    const target = this.get(id);
    if (!target) return;
    this.update(id, { title: sanitizeTitle(title, target.title) });
  }

  /** Replace the desktop window set from a validated layout. One bad record never aborts the rest. */
  restoreWindows(records: readonly RestoredWindow[], options: RestoreOptions = {}): void {
    if (options.viewport) this.viewport = options.viewport;
    const sessions = options.sessions;
    const sessionsById = sessions ? new Map(sessions.map((session) => [session.id, session])) : null;
    const singleSeen = new Set<AppId>();
    const sessionSeen = new Set<string>();
    const next: WindowState[] = [];
    const seenIds = new Set<string>();

    for (const record of records) {
      let app = appRegistry.get(record.appId);
      if (!app) continue;
      const payload: WindowPayload = { ...(record.payload ?? {}) };
      // Before the terminal/session-manager split, the multi-tab workspace was
      // registered as `terminal` without a bound session. Keep those windows
      // usable by restoring them as the session manager.
      if (app.id === 'terminal' && !payload.sessionId) app = appRegistry.require('sessions');
      if (app.instancePolicy === 'single') {
        if (singleSeen.has(app.id)) continue;
        singleSeen.add(app.id);
      }
      if (app.id === 'terminal' && payload.sessionId) {
        if (sessionSeen.has(payload.sessionId)) continue;
        sessionSeen.add(payload.sessionId);
      }
      if (app.id === 'editor' || app.id === 'preview') {
        if (!payload.path) continue;
      }
      const id = record.id ?? `${record.appId}:r${next.length + 1}`;
      if (seenIds.has(id)) continue;
      seenIds.add(id);
      const geometry = constrainGeometry(
        { x: record.x, y: record.y, width: record.width, height: record.height },
        this.viewport,
        app.minSize,
      );
      const state: WindowStatus = record.state === 'minimized' || record.state === 'maximized' ? record.state : 'normal';
      let restoreError: WindowState['restoreError'] = null;
      if (app.id === 'terminal' && payload.sessionId && sessionsById && !sessionsById.has(payload.sessionId)) {
        restoreError = 'session_ended';
      }
      next.push({
        id,
        appId: app.id,
        title: sanitizeTitle(record.title, app.name),
        ...geometry,
        zIndex: BASE_Z + next.length,
        state,
        payload,
        restoreError,
        ...(state === 'maximized' ? { maxRestore: { ...geometry } } : {}),
      });
    }

    this.windows = next;
    this.focusedId = this.topVisible()?.id ?? null;
    this.applyZOrder();
    this.emit();
  }

  /** Serialization-ready copies; the persistence layer decides what is safe to store. */
  exportWindows(): WindowState[] {
    return this.windows.map((item) => ({ ...item, payload: { ...item.payload } }));
  }

  /**
   * Attempts to close every window through the registered guards, for example
   * when signing out. If any app vetoes (unsaved editor content), nothing is
   * closed and the caller must abort the sign-out. Guards may open their own
   * confirmation UI as a side effect.
   */
  requestCloseAll(): boolean {
    for (const item of this.windows) {
      const guard = this.closeGuards.get(item.id);
      if (guard && guard() === false) return false;
    }
    this.closeAll();
    return true;
  }

  closeAll(): void {
    this.windows = [];
    this.focusedId = null;
    this.closeGuards.clear();
    this.applyZOrder();
    this.emit();
  }

  /** Full reset used by tests and logout. */
  reset(): void {
    this.closeAll();
    this.sequence = 0;
    this.viewport = defaultViewport();
  }
}

export const windowStore = new WindowStore();

export function useWindows(): WindowState[] {
  return useSyncExternalStore(windowStore.subscribe, windowStore.getSnapshot);
}

export function useFocusedWindowId(): string | null {
  return useSyncExternalStore(windowStore.subscribe, windowStore.getFocusedId);
}

export function useDesktopViewport(): ViewportSize {
  return useSyncExternalStore(windowStore.subscribe, windowStore.getViewport);
}

export { BASE_Z, DOCK_CLEARANCE, TOPBAR_HEIGHT, WINDOW_MARGIN };
