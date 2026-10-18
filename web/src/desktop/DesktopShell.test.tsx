// @vitest-environment happy-dom
import { act } from 'react';
import { createRoot, type Root } from 'react-dom/client';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { DesktopShell } from './DesktopShell';
import { desktopStore } from './desktopStore';
import { AppRegistryError } from './appRegistry';
import { windowStore } from '../stores/windowStore';
import { sessionStore } from '../stores/sessionStore';

const DEFAULT_MATCH_MEDIA = window.matchMedia;

function setValue(input: HTMLInputElement, value: string): void {
  const descriptor = Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value');
  if (descriptor?.set) descriptor.set.call(input, value);
  else input.value = value;
  input.dispatchEvent(new Event('input', { bubbles: true }));
}

function click(element: Element): void {
  element.dispatchEvent(new MouseEvent('click', { bubbles: true }));
}

describe('DesktopShell', () => {
  let container: HTMLDivElement;
  let root: Root;
  const onLogout = vi.fn();

  beforeEach(() => {
    windowStore.reset();
    desktopStore.reset();
    sessionStore.setSessions([]);
    windowStore.setViewport({ width: 1440, height: 900 });
    window.localStorage.removeItem('edgedesk.desktop.layout.v1');
    vi.stubGlobal('fetch', () => Promise.reject(new Error('offline')));
    document.body.innerHTML = '';
    container = document.createElement('div');
    document.body.appendChild(container);
    root = createRoot(container);
  });

  afterEach(() => {
    act(() => root.unmount());
    vi.unstubAllGlobals();
    window.matchMedia = DEFAULT_MATCH_MEDIA;
    onLogout.mockReset();
  });

  const render = () => {
    act(() => {
      root.render(<DesktopShell onLogout={onLogout} health="online" registryVersion={0} onExtensionsChanged={() => {}} />);
    });
  };

  it('shows dashboard, files, terminal and session manager icons; settings comes from the launcher', () => {
    render();
    const labels = [...container.querySelectorAll('.ed-desktop-icon')].map((item) => item.getAttribute('aria-label'));
    expect(labels).toHaveLength(4);
    expect(labels[0]).toContain('设备概览');
    expect(labels[1]).toContain('文件管理');
    expect(labels[2]).toContain('终端');
    expect(labels[3]).toContain('会话管理');
    expect(container.querySelector('.ed-desktop')).not.toBeNull();
    expect(container.querySelector('.ed-status-pill')).toBeNull();
  });

  it('selects on one click and launches only on double click', () => {
    render();
    const icon = container.querySelector<HTMLButtonElement>('[aria-label^="设备概览"]')!;
    act(() => click(icon));
    expect(icon.getAttribute('aria-pressed')).toBe('true');
    expect(windowStore.getSnapshot()).toHaveLength(0);

    act(() => icon.dispatchEvent(new MouseEvent('dblclick', { bubbles: true })));
    expect(windowStore.getSnapshot()).toHaveLength(1);
    expect(windowStore.getSnapshot()[0].appId).toBe('dashboard');
  });

  it('launches apps from the Dock, focuses a single window and offers a chooser for multiple windows', () => {
    render();
    const filesDock = () => container.querySelector<HTMLButtonElement>('.ed-dock-item[title="文件管理"]')!;
    act(() => click(filesDock()));
    expect(windowStore.getSnapshot().filter((item) => item.appId === 'files')).toHaveLength(1);

    act(() => click(filesDock()));
    expect(windowStore.getSnapshot().filter((item) => item.appId === 'files')).toHaveLength(1);

    act(() => {
      windowStore.launch('files', { payload: { initialPath: '/data' }, instanceKey: '/data' });
    });
    render();
    act(() => click(filesDock()));
    const menu = container.querySelector('[role="menu"]');
    expect(menu).not.toBeNull();
    expect(menu?.querySelectorAll('[role="menuitem"]')).toHaveLength(2);
  });

  it('minimizes to the Dock, keeps the app mounted and restores it', () => {
    render();
    act(() => windowStore.launch('dashboard'));
    render();
    const minimize = container.querySelector<HTMLButtonElement>('[aria-label="最小化 设备概览"]')!;
    act(() => click(minimize));
    const windowElement = container.querySelector<HTMLElement>('.ed-window')!;
    expect(windowElement.style.display).toBe('none');
    expect(container.querySelector('.ed-window-body')).not.toBeNull();

    const restore = container.querySelector<HTMLButtonElement>('[aria-label="恢复最小化窗口 设备概览"]')!;
    expect(restore).not.toBeNull();
    act(() => click(restore));
    expect(windowStore.getSnapshot()[0].state).toBe('normal');
    expect(container.querySelector<HTMLElement>('.ed-window')?.style.display).not.toBe('none');
  });

  it('turns an explicitly maximized window into true fullscreen', () => {
    render();
    act(() => windowStore.launch('dashboard'));
    render();
    act(() => click(container.querySelector('[aria-label="最大化 设备概览"]')!));
    render();
    expect(container.querySelector('.ed-desktop')?.classList.contains('has-fullscreen')).toBe(true);
    expect(container.querySelector<HTMLElement>('.ed-window')?.classList.contains('fullscreen')).toBe(true);

    act(() => click(container.querySelector('[aria-label="最小化 设备概览"]')!));
    render();
    expect(container.querySelector('.ed-desktop')?.classList.contains('has-fullscreen')).toBe(false);
  });

  it('opens the launcher, filters by name and shows an explicit empty state', () => {    render();
    const launcherButton = container.querySelector<HTMLButtonElement>('[aria-label="打开应用启动器"]')!;
    launcherButton.focus();
    act(() => click(launcherButton));
    const dialog = container.querySelector('[role="dialog"][aria-label="应用启动器"]');
    expect(dialog).not.toBeNull();

    const input = container.querySelector<HTMLInputElement>('input[aria-label="搜索应用"]')!;
    act(() => setValue(input, '终端'));
    expect(container.querySelectorAll('.ed-launcher-app')).toHaveLength(1);
    expect(container.querySelector('.ed-launcher-app strong')?.textContent).toBe('终端');

    act(() => setValue(input, 'zzz'));
    expect(container.querySelectorAll('.ed-launcher-app')).toHaveLength(0);
    expect(container.querySelector('.ed-launcher-empty')?.textContent).toContain('没有找到匹配');

    act(() => setValue(input, '设置'));
    expect(container.querySelectorAll('.ed-launcher-app')).toHaveLength(1);
    act(() => click(container.querySelector('.ed-launcher-app')!));
    expect(windowStore.getSnapshot().some((item) => item.appId === 'settings')).toBe(true);
    expect(container.querySelector('[role="dialog"][aria-label="应用启动器"]')).toBeNull();
    expect(document.activeElement).toBe(launcherButton);
  });

  it('closes the launcher with Escape and returns focus', async () => {
    render();
    const launcherButton = container.querySelector<HTMLButtonElement>('[aria-label="打开应用启动器"]')!;
    launcherButton.focus();
    act(() => click(launcherButton));
    const input = container.querySelector<HTMLInputElement>('input[aria-label="搜索应用"]')!;
    act(() => {
      input.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true }));
    });
    await act(async () => {
      await new Promise((resolve) => setTimeout(resolve, 0));
    });
    expect(container.querySelector('[role="dialog"][aria-label="应用启动器"]')).toBeNull();
    expect(document.activeElement).toBe(launcherButton);
  });

  it('handles window shortcuts through WindowManager and ignores editable targets', () => {
    render();
    act(() => windowStore.launch('dashboard'));
    render();

    act(() => {
      window.dispatchEvent(new KeyboardEvent('keydown', { key: 'm', code: 'KeyM', altKey: true, bubbles: true }));
    });
    expect(windowStore.getSnapshot()[0].state).toBe('maximized');

    act(() => {
      window.dispatchEvent(new KeyboardEvent('keydown', { key: 'h', code: 'KeyH', altKey: true, bubbles: true }));
    });
    expect(windowStore.getSnapshot()[0].state).toBe('minimized');

    const input = document.createElement('input');
    document.body.appendChild(input);
    input.focus();
    act(() => {
      input.dispatchEvent(new KeyboardEvent('keydown', { key: 'k', code: 'KeyK', altKey: true, bubbles: true }));
    });
    expect(container.querySelector('[role="dialog"][aria-label="应用启动器"]')).toBeNull();
    input.remove();
  });

  it('opens the launcher with Alt+K when the desktop has focus', () => {
    render();
    act(() => {
      window.dispatchEvent(new KeyboardEvent('keydown', { key: 'k', code: 'KeyK', altKey: true, bubbles: true }));
    });
    expect(container.querySelector('[role="dialog"][aria-label="应用启动器"]')).not.toBeNull();
  });

  it('forces narrow viewports to maximize windows and disables free maximization toggles', () => {
    window.matchMedia = ((query: string) => ({
      matches: query.includes('768'),
      media: query,
      addEventListener: () => {},
      removeEventListener: () => {},
    })) as unknown as typeof window.matchMedia;
    render();
    act(() => windowStore.launch('files', { payload: { initialPath: '/' } }));
    render();
    const windowElement = container.querySelector('.ed-window')!;
    expect(windowElement.classList.contains('maximized')).toBe(true);
    const maximize = container.querySelector<HTMLButtonElement>('[aria-label="还原 文件管理"]');
    expect(maximize?.disabled).toBe(true);
    expect(container.querySelector('.ed-desktop.narrow')).not.toBeNull();
  });

  it('logs out from the desktop top bar', () => {
    render();
    act(() => click(container.querySelector('[aria-label="退出登录"]')!));
    expect(onLogout).toHaveBeenCalledTimes(1);
  });
});

describe('app registry unknown ids', () => {
  it('is rejected rather than silently opening a blank app', () => {
    expect(() => windowStore.launch('nope' as never)).toThrowError(AppRegistryError);
  });
});
