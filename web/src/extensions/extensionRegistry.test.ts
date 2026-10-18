import { afterEach, describe, expect, it } from 'vitest';
import { appRegistry } from '../desktop/appRegistry';
import { windowStore } from '../stores/windowStore';
import { applyExtensionApps, clearExtensionRegistry, getInstalledExtension } from './extensionRegistry';
import type { ExtensionApp } from './extensionTypes';

function extensionApp(overrides: Partial<ExtensionApp> = {}): ExtensionApp {
  return {
    app_id: 'io.example.registry',
    display_name: 'Registry Test',
    description: 'Test extension',
    status: 'enabled',
    active_revision_id: 'abcdef0123456789abcdef0123456789',
    active_version: '1.2.3',
    requested_permissions: ['storage'],
    granted_permissions: ['storage'],
    data_retained: false,
    publisher_verified: false,
    installed_at: 1000,
    updated_at: 1000,
    network_origins: [],
    icon_url: '/apps/io.example.registry/1.2.3/rev/icon.png',
    window: { defaultWidth: 720, defaultHeight: 480, minWidth: 360, minHeight: 240, instances: 'multi' },
    ...overrides,
  };
}

afterEach(() => {
  clearExtensionRegistry();
  for (const window of windowStore.getSnapshot()) windowStore.forceClose(window.id);
});

describe('extension registry', () => {
  it('registers enabled apps and keeps pending apps unavailable', () => {
    applyExtensionApps([extensionApp()]);
    const definition = appRegistry.require('io.example.registry');
    expect(definition.name).toBe('Registry Test');
    expect(definition.instancePolicy).toBe('multi');
    expect(definition.defaultSize).toEqual({ width: 720, height: 480 });
    expect(definition.minSize).toEqual({ width: 360, height: 240 });
    expect(definition.launcherVisible).toBe(true);
    expect(definition.desktopShortcut).toBe(false);
    expect(definition.iconUrl).toContain('/apps/io.example.registry/');
    expect(getInstalledExtension('io.example.registry')?.active_version).toBe('1.2.3');

    applyExtensionApps([
      extensionApp({
        app_id: 'io.example.pending',
        display_name: 'Pending',
        status: 'pending_review',
      }),
    ]);
    expect(appRegistry.has('io.example.pending')).toBe(false);
  });

  it('closes running windows and unregisters disabled or removed apps', () => {
    applyExtensionApps([extensionApp()]);
    const windowId = windowStore.launch('io.example.registry');
    expect(windowStore.get(windowId)).toBeTruthy();
    applyExtensionApps([extensionApp({ status: 'disabled' })]);
    expect(appRegistry.has('io.example.registry')).toBe(false);
    expect(windowStore.get(windowId)).toBeUndefined();
    expect(getInstalledExtension('io.example.registry')).toBeUndefined();
  });

  it('updates metadata without duplicating native apps', () => {
    applyExtensionApps([extensionApp()]);
    const first = appRegistry.require('io.example.registry');
    applyExtensionApps([extensionApp({ display_name: 'Renamed', active_version: '1.3.0' })]);
    const second = appRegistry.require('io.example.registry');
    expect(second.name).toBe('Renamed');
    expect(second.id).toBe(first.id);
    expect(appRegistry.require('dashboard').name).toBe('设备概览');
  });

  it('ignores apps without an active revision even when marked enabled', () => {
    applyExtensionApps([extensionApp({ active_revision_id: '' })]);
    expect(appRegistry.has('io.example.registry')).toBe(false);
  });
});
