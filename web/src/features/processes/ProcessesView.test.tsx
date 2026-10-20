// @vitest-environment happy-dom
import { act } from 'react';
import { createRoot, type Root } from 'react-dom/client';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { ProcessesView } from './ProcessesView';
import type { SupervisedService } from '../../types';

function jsonResponse(body: unknown, status = 200): Response {
  return new Response(JSON.stringify(body), {
    status,
    headers: { 'Content-Type': 'application/json' },
  });
}

function setValue(element: HTMLInputElement | HTMLTextAreaElement, value: string): void {
  const proto = element instanceof HTMLTextAreaElement ? HTMLTextAreaElement.prototype : HTMLInputElement.prototype;
  const descriptor = Object.getOwnPropertyDescriptor(proto, 'value');
  if (descriptor?.set) descriptor.set.call(element, value);
  else element.value = value;
  element.dispatchEvent(new Event('input', { bubbles: true }));
}

function buttonByText(container: ParentNode, text: string): HTMLButtonElement | undefined {
  return [...container.querySelectorAll('button')].find((button) => button.textContent?.trim() === text);
}

const SERVICE: SupervisedService = {
  id: 'svc-1',
  name: 'mihomo',
  executable: '/usr/local/bin/mihomo',
  args: ['-c', '/etc/mihomo/config.yaml', 'value with spaces', ''],
  cwd: '/var/lib/mihomo',
  env: { LOG_LEVEL: 'info', SECRET_TOKEN: 'do-not-render' },
  run_user: 'root',
  run_group: 'root',
  allow_root: true,
  autostart: true,
  restart_policy: 'on-failure',
  max_restarts: 5,
  restart_window_sec: 60,
  restart_delay_ms: 1000,
  max_restart_delay_ms: 60000,
  stable_after_sec: 300,
  stop_timeout_ms: 10000,
  log_max_bytes: 10 * 1024 * 1024,
  created_at: 1700000000,
  updated_at: 1700000100,
  state: 'RUNNING',
  desired_state: 'running',
  pid: 4242,
  pgid: 4242,
  boot_id: 'boot-abc',
  started_at: 1700000200000,
  last_exit_code: 0,
  last_signal: null,
  last_exit_at: 1700000000000,
  last_error: '',
  restart_count: 2,
  failed_locked: false,
  uptime_seconds: 3661,
  cpu_percent: 12.5,
  rss_bytes: 52 * 1024 * 1024,
  metrics_available: true,
};

const HEALTH = {
  ok: true,
  status: 'ok',
  service: 'edgedesk-supervisor',
  version: '1.4.0',
  boot_id: 'boot-abc',
  uptime_seconds: 100,
  services: 1,
  running: 1,
  persistence_errors: 0,
  log_errors: 0,
  degraded: false,
};

function installApi(overrides: { service?: Partial<SupervisedService>; unavailable?: boolean } = {}) {
  const calls: Array<{ url: string; method: string; body: unknown }> = [];
  const service = { ...SERVICE, ...(overrides.service ?? {}) };
  vi.stubGlobal('fetch', async (url: string, init?: RequestInit) => {
    calls.push({
      url,
      method: init?.method ?? 'GET',
      body: init?.body && typeof init.body === 'string' ? JSON.parse(init.body) : undefined,
    });
    if (overrides.unavailable) {
      return jsonResponse(
        { ok: false, error: 'supervisor_unavailable', message: 'supervisor socket is unavailable' },
        503,
      );
    }
    if (url === '/api/supervisor/health') return jsonResponse({ ...HEALTH, ok: true });
    if (url.startsWith('/api/supervisor/services?') || url === '/api/supervisor/services') {
      return jsonResponse({ ok: true, services: [service], total: 1, count: 1, offset: 0, limit: 500 });
    }
    if (url.includes('/logs?')) {
      return jsonResponse({
        ok: true,
        service_id: service.id,
        content: 'started mihomo\nlistening on 127.0.0.1:9090\n',
        cursor: 10,
        next_cursor: 48,
        size: 48,
        reset: false,
        truncated: false,
      });
    }
    if (url === '/api/supervisor/services' && init?.method === 'POST') {
      return jsonResponse({ ...service, id: 'svc-new', name: 'created-service', state: 'STOPPED', ok: true });
    }
    if (url === `/api/supervisor/services/${service.id}/stop` && init?.method === 'POST') {
      return jsonResponse({ ...service, state: 'STOPPED', pid: null, pgid: null, uptime_seconds: null, ok: true });
    }
    if (url === `/api/supervisor/services/${service.id}/start` && init?.method === 'POST') {
      return jsonResponse({ ...service, state: 'RUNNING', ok: true });
    }
    if (url === `/api/supervisor/services/${service.id}/restart` && init?.method === 'POST') {
      return jsonResponse({ ...service, state: 'RUNNING', restart_count: 3, ok: true });
    }
    return jsonResponse({ ok: false, error: 'not_found', message: 'not found' }, 404);
  });
  return calls;
}

describe('ProcessesView', () => {
  let container: HTMLDivElement;
  let root: Root;

  beforeEach(() => {
    document.body.innerHTML = '';
    container = document.createElement('div');
    document.body.appendChild(container);
    root = createRoot(container);
  });

  afterEach(() => {
    act(() => root.unmount());
    vi.unstubAllGlobals();
    vi.useRealTimers();
  });

  const renderAndLoad = async () => {
    act(() => root.render(<ProcessesView />));
    await act(async () => {
      await new Promise((resolve) => setTimeout(resolve, 0));
    });
    await act(async () => {
      await new Promise((resolve) => setTimeout(resolve, 0));
    });
  };

  it('renders real service state, process metrics and logs without exposing env values', async () => {
    const calls = installApi();
    await renderAndLoad();

    expect(container.querySelectorAll('.ed-proc-item')).toHaveLength(1);
    expect(container.textContent).toContain('mihomo');
    expect(container.textContent).toContain('运行中');
    expect(container.textContent).toContain('4242');
    expect(container.textContent).toContain('12.5%');
    expect(container.textContent).toContain('52.0 MiB');
    expect(container.textContent).toContain('root 身份');
    expect(container.textContent).toContain('LOG_LEVEL');
    expect(container.textContent).toContain('SECRET_TOKEN');
    expect(container.textContent).not.toContain('do-not-render');
    expect(container.querySelector('.ed-proc-log')?.textContent).toContain('listening on 127.0.0.1:9090');
    expect(calls.some((call) => call.url.includes('/logs?') && call.url.includes('tail_bytes=16384'))).toBe(true);
  });

  it('confirms stop and calls the supervisor stop endpoint', async () => {
    const calls = installApi();
    await renderAndLoad();

    act(() => buttonByText(container, '停止')!.click());
    const dialog = document.querySelector('[role="alertdialog"]');
    expect(dialog?.textContent).toContain('SIGTERM');
    await act(async () => {
      buttonByText(dialog!, '停止服务')!.click();
      await new Promise((resolve) => setTimeout(resolve, 0));
    });

    expect(calls.some((call) => call.url === '/api/supervisor/services/svc-1/stop' && call.method === 'POST')).toBe(true);
    expect(container.textContent).toContain('已停止 mihomo。');
  });

  it('asks for explicit root confirmation before creating a root service', async () => {
    const calls = installApi();
    await renderAndLoad();

    act(() => buttonByText(container, '新建服务')!.click());
    const dialog = document.querySelector('[role="dialog"][aria-label="创建服务"]')!;
    const inputs = dialog.querySelectorAll<HTMLInputElement>('input');
    act(() => {
      setValue(inputs[0], 'created-service');
      setValue(inputs[1], '/bin/sleep');
      setValue(inputs[2], '/');
      setValue(inputs[3], 'root');
      setValue(inputs[4], 'root');
    });
    const textareas = dialog.querySelectorAll<HTMLTextAreaElement>('textarea');
    act(() => {
      setValue(textareas[0], '30');
      setValue(textareas[1], 'X=1\nEMPTY=');
    });

    await act(async () => {
      buttonByText(dialog, '创建服务')!.click();
      await new Promise((resolve) => setTimeout(resolve, 0));
    });
    expect(dialog.textContent).toContain('必须勾选明确的确认项');
    expect(calls.some((call) => call.method === 'POST')).toBe(false);

    const checkbox = dialog.querySelector<HTMLInputElement>('.ed-proc-root-warning input[type="checkbox"]')!;
    act(() => checkbox.click());
    await act(async () => {
      buttonByText(dialog, '创建服务')!.click();
      await new Promise((resolve) => setTimeout(resolve, 0));
    });

    const post = calls.find((call) => call.method === 'POST');
    expect(post).toBeDefined();
    expect(post!.body).toMatchObject({
      name: 'created-service',
      executable: '/bin/sleep',
      args: ['30'],
      run_user: 'root',
      run_group: 'root',
      allow_root: true,
      env: { X: '1', EMPTY: '' },
      restart_policy: 'on-failure',
      log_max_bytes: 10 * 1024 * 1024,
    });
  });

  it('keeps the rest of the window usable when the supervisor is unavailable', async () => {
    installApi({ unavailable: true });
    await renderAndLoad();

    expect(container.textContent).toContain('Supervisor 不可用');
    expect(container.textContent).toContain('其他 EdgeDesk 功能不受影响');
    expect(container.querySelectorAll('.ed-proc-item')).toHaveLength(0);
    expect(container.querySelector('.ed-proc-main')?.textContent).toContain('Supervisor 暂不可用');
    expect(container.textContent).not.toContain('mihomo');
  });

  it('shows FAILED lock and allows deleting a stopped or failed service', async () => {
    installApi({ service: { state: 'FAILED', failed_locked: true, pid: null, pgid: null, uptime_seconds: null } });
    await renderAndLoad();

    expect(container.textContent).toContain('失败锁定');
    expect(container.textContent).toContain('自动恢复已锁定');
    const deleteButton = buttonByText(container, '删除')!;
    expect(deleteButton.disabled).toBe(false);
    const startButton = buttonByText(container, '启动')!;
    expect(startButton.disabled).toBe(false);
  });
});
