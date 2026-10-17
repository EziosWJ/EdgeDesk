import { useSyncExternalStore } from 'react';
import { api } from '../services/api';
import type { AppSettings } from '../types';

const DEFAULT_SETTINGS: AppSettings = {
  theme: 'dark',
  files_view: 'list',
  files_show_hidden: true,
  terminal_font_size: 13,
  dashboard_metrics_window: 300,
};

type Listener = () => void;

/**
 * Server-backed application preferences. Layout state that only makes sense in
 * one browser (tree width, floating-window geometry, selected tab) stays in
 * localStorage; these five settings are shared across browsers.
 */
class PreferencesStore {
  private settings: AppSettings = { ...DEFAULT_SETTINGS };
  private tokenConfigured = false;
  private listeners = new Set<Listener>();

  subscribe = (listener: Listener): (() => void) => {
    this.listeners.add(listener);
    return () => this.listeners.delete(listener);
  };

  getSnapshot = (): AppSettings => this.settings;
  getTokenConfigured = (): boolean => this.tokenConfigured;

  private emit(): void {
    this.listeners.forEach((listener) => listener());
  }

  async load(): Promise<void> {
    const data = await api.settings();
    this.settings = { ...data.settings };
    this.tokenConfigured = data.api_token_configured;
    this.emit();
  }

  async save(patch: Partial<AppSettings>): Promise<void> {
    this.settings = { ...this.settings, ...patch };
    this.emit();
    // Keep the optimistic value on failure: the caller surfaces the error and
    // the next explicit save retries. Reverting here would fight local UI state
    // (for example a view toggle) while the user is still interacting.
    const data = await api.updateSettings(patch);
    this.settings = { ...data.settings };
    this.emit();
  }

  reset(): void {
    this.settings = { ...DEFAULT_SETTINGS };
    this.tokenConfigured = false;
    this.emit();
  }
}

export const preferencesStore = new PreferencesStore();

export function usePreferences(): AppSettings {
  return useSyncExternalStore(preferencesStore.subscribe, preferencesStore.getSnapshot);
}

export { DEFAULT_SETTINGS };
