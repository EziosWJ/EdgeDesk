// @vitest-environment happy-dom
import { act } from 'react';
import { createRoot, type Root } from 'react-dom/client';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { preferencesStore } from '../../stores/preferencesStore';
import { SettingsView } from './SettingsView';

function jsonResponse(body: unknown, status = 200): Response {
  return new Response(JSON.stringify(body), {
    status,
    headers: { 'Content-Type': 'application/json' },
  });
}

const SETTINGS = {
  theme: 'dark',
  files_view: 'list',
  files_show_hidden: true,
  terminal_font_size: 13,
  dashboard_metrics_window: 300,
};

describe('SettingsView', () => {
  let container: HTMLDivElement;
  let root: Root;

  beforeEach(() => {
    preferencesStore.reset();
    document.body.innerHTML = '';
    container = document.createElement('div');
    document.body.appendChild(container);
    root = createRoot(container);
  });

  afterEach(() => {
    act(() => root.unmount());
    vi.unstubAllGlobals();
  });

  it('loads system information and saves a preference change', async () => {
    const calls: Array<{ url: string; init?: RequestInit }> = [];
    vi.stubGlobal('fetch', async (url: string, init?: RequestInit) => {
      calls.push({ url, init });
      if (url === '/api/system/info') {
        return jsonResponse({
          ok: true,
          version: '1.1.0',
          hostname: 'edge-001',
          arch: 'aarch64',
          kernel: '6.1.0',
          uptime_seconds: 120,
          listen: { host: '127.0.0.1', port: 8080 },
          file_root: '/srv',
          follow_symlinks: false,
          bash: '/bin/bash',
          audit_enabled: true,
          audit_log: '/var/log/audit.log',
          data_dir: '/var/lib/edgedesk',
          state_dir: '/run/edgedesk',
          max_upload_size: 1024,
          max_terminal_sessions: 8,
          terminal_idle_timeout: 1800,
          metrics_interval_ms: 1000,
          api_token_configured: true,
        });
      }
      if (url === '/api/auth/session') {
        return jsonResponse({ ok: true, authenticated: true, method: 'cookie', state: 'initialized', expires_at: 123 });
      }
      if (url === '/api/settings') {
        if (init?.method === 'PUT') {
          const patch = JSON.parse(String(init.body));
          return jsonResponse({ ok: true, settings: { ...SETTINGS, ...patch } });
        }
        return jsonResponse({ ok: true, settings: SETTINGS, api_token_configured: true });
      }
      return jsonResponse({ ok: false, error: 'not_found' }, 404);
    });

    act(() => root.render(<SettingsView onLogout={() => {}} />));
    await act(async () => {
      await new Promise((resolve) => setTimeout(resolve, 0));
    });

    expect(container.textContent).toContain('1.1.0');
    expect(container.textContent).toContain('已配置（不显示值）');
    const select = container.querySelector<HTMLSelectElement>('#ed-theme')!;
    act(() => {
      const descriptor = Object.getOwnPropertyDescriptor(HTMLSelectElement.prototype, 'value');
      if (descriptor?.set) descriptor.set.call(select, 'light');
      else select.value = 'light';
      select.dispatchEvent(new Event('change', { bubbles: true }));
    });
    await act(async () => {
      await new Promise((resolve) => setTimeout(resolve, 0));
    });
    expect(calls.some((call) => call.url === '/api/settings' && call.init?.method === 'PUT')).toBe(true);
  });
});
