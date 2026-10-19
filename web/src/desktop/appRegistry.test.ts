import { describe, expect, it } from 'vitest';
import { AppRegistry, AppRegistryError, appRegistry, createDefaultRegistry, type AppDefinition } from './appRegistry';

function app(overrides: Partial<AppDefinition> = {}): AppDefinition {
  return {
    id: 'dashboard',
    name: '设备',
    description: '说明',
    keywords: ['monitor'],
    sprite: 'dashboard',
    tone: 'blue',
    instancePolicy: 'single',
    defaultSize: { width: 800, height: 600 },
    minSize: { width: 400, height: 300 },
    desktopShortcut: true,
    dockPinned: true,
    launcherVisible: true,
    ...overrides,
  };
}

describe('AppRegistry', () => {
  it('registers the default applications and internal window types', () => {
    const registry = createDefaultRegistry();
    expect(registry.launcherApps().map((item) => item.id)).toEqual(['dashboard', 'files', 'terminal', 'sessions', 'processes', 'settings', 'extensions']);
    expect(registry.desktopApps().map((item) => item.id)).toEqual(['dashboard', 'files', 'terminal', 'sessions', 'processes']);
    expect(registry.dockApps().map((item) => item.id)).toEqual(['dashboard', 'files', 'terminal', 'sessions', 'processes', 'settings']);
    expect(registry.get('editor')?.launcherVisible).toBe(false);
    expect(registry.require('files').instancePolicy).toBe('multi');
    expect(registry.require('dashboard').instancePolicy).toBe('single');
    expect(registry.require('terminal').instancePolicy).toBe('multi');
    expect(registry.require('sessions').instancePolicy).toBe('single');
    expect(registry.require('processes').instancePolicy).toBe('single');
  });

  it('rejects unknown app ids instead of fabricating metadata', () => {
    const registry = createDefaultRegistry();
    expect(registry.get('does-not-exist')).toBeUndefined();
    expect(registry.has('does-not-exist')).toBe(false);
    expect(() => registry.require('does-not-exist')).toThrowError(AppRegistryError);
    try {
      registry.require('does-not-exist');
    } catch (error) {
      expect((error as AppRegistryError).code).toBe('unknown_app');
    }
  });

  it('rejects duplicate ids and incomplete definitions', () => {
    const registry = new AppRegistry();
    registry.register(app());
    expect(() => registry.register(app())).toThrowError(/already registered/);
    expect(() =>
      registry.register(app({ id: 'files', name: '' })),
    ).toThrowError(/missing app metadata/);
    expect(() =>
      registry.register(app({ id: 'terminal', defaultSize: { width: 0, height: 100 } })),
    ).toThrowError(/positive finite/);
  });

  it('searches name, description and keywords without maintaining a second list', () => {
    const registry = createDefaultRegistry();
    expect(registry.search('终端').map((item) => item.id)).toEqual(['terminal']);
    expect(registry.search('扩展').map((item) => item.id)).toEqual(['extensions']);
    expect(registry.search('会话').map((item) => item.id)).toEqual(['sessions', 'settings']);
    expect(registry.search('bash').map((item) => item.id)).toEqual(['terminal', 'sessions']);
    expect(registry.search('下载').map((item) => item.id)).toEqual(['files']);
    expect(registry.search('进程').map((item) => item.id)).toEqual(['processes']);
    expect(registry.search('service').map((item) => item.id)).toEqual(['processes']);
    expect(registry.search('不存在')).toEqual([]);
    expect(registry.search(' ').map((item) => item.id)).toEqual(['dashboard', 'files', 'terminal', 'sessions', 'processes', 'settings', 'extensions']);
  });

  it('exposes the same singleton metadata used by the desktop shell', () => {
    expect(appRegistry.list().map((item) => item.id)).toEqual([
      'dashboard',
      'files',
      'terminal',
      'sessions',
      'processes',
      'settings',
      'extensions',
      'editor',
      'preview',
    ]);
    expect(appRegistry.require('settings').desktopShortcut).toBe(false);
  });
});
