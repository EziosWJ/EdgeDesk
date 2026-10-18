import type { AppDefinition, AppTone } from '../desktop/appRegistry';
import { appRegistry } from '../desktop/appRegistry';
import { windowStore } from '../stores/windowStore';
import type { AppId } from '../types';
import { extensionApi } from './extensionApi';
import type { ExtensionApp } from './extensionTypes';

const TONES: AppTone[] = ['purple', 'blue', 'green', 'yellow', 'gray'];

/** Full metadata for installed extension apps, keyed by stable appId. */
const installed = new Map<string, ExtensionApp>();

function toneFor(id: string): AppTone {
  let hash = 0;
  for (let index = 0; index < id.length; index += 1) hash = (hash * 31 + id.charCodeAt(index)) >>> 0;
  return TONES[hash % TONES.length];
}

function definitionFor(app: ExtensionApp): AppDefinition {
  const window = app.window ?? {};
  const defaultWidth = Math.max(320, window.defaultWidth ?? 960);
  const defaultHeight = Math.max(240, window.defaultHeight ?? 640);
  const minWidth = Math.min(defaultWidth, Math.max(240, window.minWidth ?? 320));
  const minHeight = Math.min(defaultHeight, Math.max(180, window.minHeight ?? 240));
  const description = app.description?.trim() || `扩展应用 · ${app.active_version}`;
  return {
    id: app.app_id as AppId,
    name: app.display_name,
    description,
    keywords: [app.app_id, 'extension', '扩展', '应用', app.active_version, ...app.requested_permissions],
    sprite: 'logs',
    iconUrl: app.icon_url,
    tone: toneFor(app.app_id),
    instancePolicy: window.instances === 'multi' ? 'multi' : 'single',
    defaultSize: { width: defaultWidth, height: defaultHeight },
    minSize: { width: minWidth, height: minHeight },
    desktopShortcut: false,
    dockPinned: false,
    launcherVisible: true,
  };
}

/** Apply the backend list, registering enabled apps and removing the rest. */
export function applyExtensionApps(apps: readonly ExtensionApp[]): void {
  const enabled = new Map<string, ExtensionApp>();
  for (const app of apps) {
    if (app.status === 'enabled' && app.active_revision_id) enabled.set(app.app_id, app);
  }
  for (const app of enabled.values()) {
    appRegistry.upsert(definitionFor(app));
    installed.set(app.app_id, app);
  }
  for (const appId of [...installed.keys()]) {
    if (enabled.has(appId)) continue;
    installed.delete(appId);
    appRegistry.unregister(appId);
    // Disable/uninstall/isolation must take effect immediately.
    windowStore.closeAllForApp(appId as AppId);
  }
}

export async function syncExtensionRegistry(): Promise<readonly ExtensionApp[]> {
  const response = await extensionApi.list();
  applyExtensionApps(response.apps);
  return response.apps;
}

export function clearExtensionRegistry(): void {
  for (const appId of installed.keys()) {
    appRegistry.unregister(appId);
    windowStore.closeAllForApp(appId as AppId);
  }
  installed.clear();
}

export function getInstalledExtension(appId: string): ExtensionApp | undefined {
  return installed.get(appId);
}

export function listInstalledExtensions(): readonly ExtensionApp[] {
  return [...installed.values()];
}
