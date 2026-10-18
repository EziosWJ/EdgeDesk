// @vitest-environment happy-dom
import { act } from 'react';
import { createRoot, type Root } from 'react-dom/client';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import App from './App';
import { desktopStore } from '../desktop/desktopStore';
import { preferencesStore } from '../stores/preferencesStore';
import { windowStore } from '../stores/windowStore';

vi.mock('../services/metricsSocket', () => ({
  MetricsSocket: class {
    onHistory = () => {};
    onSample = () => {};
    connect() {}
    close() {}
  },
}));

type Call = { url: string; init?: RequestInit };

function jsonResponse(body: unknown, status = 200): Response {
  return new Response(JSON.stringify(body), {
    status,
    headers: { 'Content-Type': 'application/json' },
  });
}

function click(element: Element): void {
  element.dispatchEvent(new MouseEvent('click', { bubbles: true }));
}

async function flush(): Promise<void> {
  await new Promise((resolve) => setTimeout(resolve, 0));
}

describe('App authentication flow', () => {
  let container: HTMLDivElement;
  let root: Root;
  let calls: Call[];

  beforeEach(() => {
    windowStore.reset();
    desktopStore.reset();
    preferencesStore.reset();
    window.localStorage.removeItem('edgedesk.desktop.layout.v1');
    window.localStorage.removeItem('edgedesk.terminals.v1');
    calls = [];
    vi.stubGlobal('fetch', async (input: string | URL | Request, init?: RequestInit) => {
      const url = String(input);
      calls.push({ url, init });
      switch (url) {
        case '/api/auth/setup-status':
          return jsonResponse({ ok: true, initialized: true, state: 'initialized' });
        case '/api/auth/session':
          return jsonResponse({ ok: true, authenticated: true, method: 'cookie', state: 'initialized' });
        case '/api/health':
          return jsonResponse({ status: 'ok', service: 'edgedesk', phase: 1, version: '1.2.0' });
        case '/api/terminal/sessions':
          return jsonResponse({
            ok: true,
            sessions: [],
            shell: '/bin/bash',
            shell_available: true,
            max_sessions: 8,
            idle_timeout_seconds: 1800,
            running: 0,
          });
        case '/api/auth/logout':
          return jsonResponse({ ok: true });
        default:
          return jsonResponse({ ok: false, error: 'not_found' }, 404);
      }
    });
    document.body.innerHTML = '';
    container = document.createElement('div');
    document.body.appendChild(container);
    root = createRoot(container);
  });

  afterEach(() => {
    act(() => root.unmount());
    vi.unstubAllGlobals();
  });

  async function renderDesktop(): Promise<void> {
    act(() => root.render(<App />));
    for (let attempt = 0; attempt < 20 && !desktopStore.isRestored(); attempt += 1) {
      await act(async () => {
        await flush();
      });
    }
    expect(desktopStore.isRestored()).toBe(true);
    expect(container.querySelector('.ed-desktop')).not.toBeNull();
  }

  it('invalidates the server session when logging out from the desktop top bar', async () => {
    await renderDesktop();

    await act(async () => {
      click(container.querySelector('[aria-label="退出登录"]')!);
      await flush();
    });

    expect(calls.filter((call) => call.url === '/api/auth/logout')).toHaveLength(1);
    expect(calls.find((call) => call.url === '/api/auth/logout')?.init?.method).toBe('POST');
    expect(container.querySelector('.ed-desktop')).toBeNull();
    expect(container.querySelector('input[type="password"]')).not.toBeNull();
  });

  it('does not log out or close windows when an app vetoes closing (unsaved editor)', async () => {
    await renderDesktop();

    let id = '';
    act(() => {
      id = windowStore.launch('dashboard');
    });
    let dirty = true;
    const unregister = windowStore.registerCloseGuard(id, () => !dirty);

    await act(async () => {
      click(container.querySelector('[aria-label="退出登录"]')!);
      await flush();
    });

    expect(calls.some((call) => call.url === '/api/auth/logout')).toBe(false);
    expect(windowStore.get(id)).toBeDefined();
    expect(container.querySelector('.ed-desktop')).not.toBeNull();

    dirty = false;
    await act(async () => {
      click(container.querySelector('[aria-label="退出登录"]')!);
      await flush();
    });

    expect(calls.filter((call) => call.url === '/api/auth/logout')).toHaveLength(1);
    expect(windowStore.getSnapshot()).toHaveLength(0);
    expect(container.querySelector('.ed-desktop')).toBeNull();
    unregister();
  });
});
