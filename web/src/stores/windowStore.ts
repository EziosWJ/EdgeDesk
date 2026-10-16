import { useSyncExternalStore } from 'react';
import type { WindowKind, WindowState } from '../types';

type Listener = () => void;

type WindowRequest = {
  kind: WindowKind;
  title: string;
  payload: { sessionId?: string; path?: string };
  width?: number;
  height?: number;
};

const DEFAULT_SIZES: Record<WindowKind, { width: number; height: number }> = {
  terminal: { width: 720, height: 440 },
  editor: { width: 760, height: 560 },
  preview: { width: 640, height: 520 },
};

const CASCADE_STEP = 32;
const MIN_WIDTH = 380;
const MIN_HEIGHT = 240;

class WindowStore {
  private windows: WindowState[] = [];
  private nextZ = 10;
  private listeners = new Set<Listener>();

  subscribe = (listener: Listener): (() => void) => {
    this.listeners.add(listener);
    return () => this.listeners.delete(listener);
  };

  getSnapshot = (): WindowState[] => this.windows;

  private emit(): void {
    this.windows = [...this.windows];
    this.listeners.forEach((listener) => listener());
  }

  /** Immutable item update: snapshots stay stable for useSyncExternalStore. */
  private update(id: string, patch: Partial<WindowState>): void {
    let changed = false;
    const next = this.windows.map((item) => {
      if (item.id !== id) return item;
      changed = true;
      return { ...item, ...patch };
    });
    if (!changed) return;
    this.windows = next;
    this.listeners.forEach((listener) => listener());
  }

  open(request: WindowRequest): string {
    const key = request.payload.sessionId ?? request.payload.path ?? request.title;
    const id = `${request.kind}:${key}`;
    const existing = this.windows.find((item) => item.id === id);
    if (existing) {
      this.update(id, { state: 'normal', zIndex: ++this.nextZ });
      return id;
    }
    const size = {
      width: request.width ?? DEFAULT_SIZES[request.kind].width,
      height: request.height ?? DEFAULT_SIZES[request.kind].height,
    };
    const offset = (this.windows.length % 6) * CASCADE_STEP;
    const maxX = Math.max(12, window.innerWidth - size.width - 24);
    const maxY = Math.max(12, window.innerHeight - size.height - 24);
    this.windows = [
      ...this.windows,
      {
        id,
        kind: request.kind,
        title: request.title,
        x: Math.min(80 + offset, maxX),
        y: Math.min(72 + offset, maxY),
        width: size.width,
        height: size.height,
        zIndex: ++this.nextZ,
        state: 'normal',
        payload: request.payload,
      },
    ];
    this.emit();
    return id;
  }

  close(id: string): void {
    this.windows = this.windows.filter((item) => item.id !== id);
    this.emit();
  }

  focus(id: string): void {
    const target = this.windows.find((item) => item.id === id);
    if (!target) return;
    this.update(id, { zIndex: ++this.nextZ, ...(target.state === 'minimized' ? { state: 'normal' as const } : {}) });
  }

  move(id: string, x: number, y: number): void {
    const target = this.windows.find((item) => item.id === id);
    if (!target) return;
    this.update(id, {
      x: Math.max(-target.width + 120, Math.min(x, window.innerWidth - 80)),
      y: Math.max(0, Math.min(y, window.innerHeight - 48)),
    });
  }

  resize(id: string, width: number, height: number): void {
    this.update(id, {
      width: Math.max(MIN_WIDTH, Math.min(width, window.innerWidth - 16)),
      height: Math.max(MIN_HEIGHT, Math.min(height, window.innerHeight - 16)),
    });
  }

  toggleMaximize(id: string): void {
    const target = this.windows.find((item) => item.id === id);
    if (!target) return;
    this.update(id, {
      state: target.state === 'maximized' ? 'normal' : 'maximized',
      zIndex: ++this.nextZ,
    });
  }

  minimize(id: string): void {
    this.update(id, { state: 'minimized' });
  }

  closeAll(): void {
    this.windows = [];
    this.emit();
  }
}

export const windowStore = new WindowStore();

export function useWindows(): WindowState[] {
  return useSyncExternalStore(windowStore.subscribe, windowStore.getSnapshot);
}
