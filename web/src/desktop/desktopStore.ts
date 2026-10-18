import { useSyncExternalStore } from 'react';
import type { AppId } from '../types';
import { appRegistry } from './appRegistry';

export type IconSlot = { appId: AppId; col: number; row: number };

export type DesktopStoreState = {
  icons: IconSlot[];
  dock: AppId[];
  warning: string | null;
  restored: boolean;
};

type Listener = () => void;

export const ICON_COLUMNS = 4;
export const ICON_ROWS = 8;

function defaultIcons(): IconSlot[] {
  return appRegistry.desktopApps().map((app, index) => ({ appId: app.id, col: 0, row: index }));
}

function defaultDock(): AppId[] {
  return appRegistry.dockApps().map((app) => app.id);
}

class DesktopStore {
  private state: DesktopStoreState = {
    icons: defaultIcons(),
    dock: defaultDock(),
    warning: null,
    restored: false,
  };
  private listeners = new Set<Listener>();

  subscribe = (listener: Listener): (() => void) => {
    this.listeners.add(listener);
    return () => this.listeners.delete(listener);
  };

  getSnapshot = (): DesktopStoreState => this.state;

  private emit(next: Partial<DesktopStoreState>): void {
    this.state = { ...this.state, ...next };
    this.listeners.forEach((listener) => listener());
  }

  setIcons(icons: IconSlot[]): void {
    this.emit({ icons });
  }

  setDock(dock: AppId[]): void {
    this.emit({ dock });
  }

  setWarning(warning: string | null): void {
    if (this.state.warning === warning) return;
    this.emit({ warning });
  }

  markRestored(): void {
    this.emit({ restored: true });
  }

  isRestored(): boolean {
    return this.state.restored;
  }

  reset(): void {
    this.state = { icons: defaultIcons(), dock: defaultDock(), warning: null, restored: false };
    this.listeners.forEach((listener) => listener());
  }
}

export const desktopStore = new DesktopStore();

export function useDesktopState(): DesktopStoreState {
  return useSyncExternalStore(desktopStore.subscribe, desktopStore.getSnapshot);
}
