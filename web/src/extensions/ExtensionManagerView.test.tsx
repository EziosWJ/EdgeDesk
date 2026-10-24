// @vitest-environment happy-dom
import { act } from 'react';
import { createRoot, type Root } from 'react-dom/client';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { ApiError } from '../services/api';
import { ExtensionManagerView } from './ExtensionManagerView';
import { extensionApi } from './extensionApi';
import type {
  BackendInfo,
  ExtensionApp,
  ExtensionInstallResult,
  ExtensionRevision,
} from './extensionTypes';

function jsonResponse(body: unknown, status = 200): Response {
  return new Response(JSON.stringify(body), {
    status,
    headers: { 'Content-Type': 'application/json' },
  });
}

const REVISION: ExtensionRevision = {
  revision_id: 'be01377effdc1234',
  version: '1.0.0',
  status: 'active',
  active: true,
  pinned: false,
  integrity: 'ok',
  created_at: 1760000000000,
  file_count: 5,
  total_bytes: 32768,
};

const APP: ExtensionApp = {
  app_id: 'io.test.app',
  display_name: '测试应用',
  status: 'enabled',
  active_revision_id: REVISION.revision_id,
  active_version: '1.0.0',
  requested_permissions: [],
  granted_permissions: [],
  data_retained: false,
  publisher_verified: false,
  installed_at: 1760000000000,
  updated_at: 1760000000000,
  network_origins: [],
};

const INSTALL_RESULT: ExtensionInstallResult = {
  ok: true,
  app: APP,
  revision: REVISION,
  warnings: [],
  approval_required: false,
  retained_data_restored: false,
};

type FetchCall = { url: string; init?: RequestInit };

function stubFetch(calls: FetchCall[], apps: ExtensionApp[], backend?: BackendInfo) {
  vi.stubGlobal('fetch', async (url: string, init?: RequestInit) => {
    calls.push({ url, init });
    if (url === '/api/extensions') return jsonResponse({ ok: true, apps });
    if (url === `/api/extensions/${APP.app_id}`) {
      return jsonResponse({ ok: true, app: APP, revisions: [REVISION], backend });
    }
    if (url === '/api/extensions/install' || url === `/api/extensions/${APP.app_id}/upgrade`) {
      return jsonResponse(INSTALL_RESULT);
    }
    return jsonResponse({ error: 'not_found' }, 404);
  });
}

function setValue(element: HTMLInputElement, value: string): void {
  const descriptor = Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value');
  if (descriptor?.set) descriptor.set.call(element, value);
  else element.value = value;
  element.dispatchEvent(new Event('input', { bubbles: true }));
}

function buttonByText(container: ParentNode, text: string): HTMLButtonElement | undefined {
  return [...container.querySelectorAll('button')].find((button) => button.textContent?.trim() === text);
}

function dragEvent(type: string, dataTransfer: unknown): DragEvent {
  const event = new Event(type, { bubbles: true, cancelable: true }) as DragEvent;
  Object.defineProperty(event, 'dataTransfer', { value: dataTransfer });
  return event;
}

async function flush() {
  await act(async () => {
    await Promise.resolve();
    await Promise.resolve();
  });
}

describe('ExtensionManagerView drag-drop upload', () => {
  let container: HTMLDivElement;
  let root: Root;
  let calls: FetchCall[];

  beforeEach(() => {
    document.body.innerHTML = '';
    calls = [];
    container = document.createElement('div');
    document.body.appendChild(container);
    root = createRoot(container);
  });

  afterEach(() => {
    act(() => root.unmount());
    vi.unstubAllGlobals();
  });

  async function renderView(apps: ExtensionApp[] = [APP]) {
    stubFetch(calls, apps);
    await act(async () => {
      root.render(<ExtensionManagerView onExtensionsChanged={() => {}} />);
    });
    await flush();
    return container.querySelector<HTMLElement>('.ed-ext-view')!;
  }

  it('shows the drop overlay while a file is dragged over the view', async () => {
    const view = await renderView();
    act(() => {
      view.dispatchEvent(dragEvent('dragenter', { types: ['Files'] }));
    });
    expect(container.querySelector('.ed-ext-drop-overlay')).not.toBeNull();
    act(() => {
      view.dispatchEvent(dragEvent('dragleave', { types: ['Files'] }));
    });
    expect(container.querySelector('.ed-ext-drop-overlay')).toBeNull();
  });

  it('asks for confirmation and upgrades the selected app on drop', async () => {
    const view = await renderView();
    const file = new File(['zip'], 'app.zip', { type: 'application/zip' });
    act(() => {
      view.dispatchEvent(dragEvent('drop', { types: ['Files'], files: [file] }));
    });
    const dialog = document.querySelector('[role="alertdialog"]');
    expect(dialog?.getAttribute('aria-label')).toBe('升级 测试应用？');
    expect(dialog!.textContent).toContain('app.zip');

    const confirm = [...dialog!.querySelectorAll('button')].find(
      (button) => button.textContent === '上传升级包',
    )!;
    await act(async () => {
      confirm.click();
      await Promise.resolve();
      await Promise.resolve();
    });
    const upgrade = calls.find((call) => call.url === `/api/extensions/${APP.app_id}/upgrade`);
    expect(upgrade?.init?.method).toBe('PUT');
    expect(document.querySelector('[role="alertdialog"]')).toBeNull();
  });

  it('installs as a new app when nothing is selected', async () => {
    const view = await renderView([]);
    const file = new File(['zip'], 'fresh.zip', { type: 'application/zip' });
    act(() => {
      view.dispatchEvent(dragEvent('drop', { types: ['Files'], files: [file] }));
    });
    const dialog = document.querySelector('[role="alertdialog"]');
    expect(dialog?.getAttribute('aria-label')).toBe('安装扩展应用？');

    const confirm = [...dialog!.querySelectorAll('button')].find(
      (button) => button.textContent === '上传并安装',
    )!;
    await act(async () => {
      confirm.click();
      await Promise.resolve();
      await Promise.resolve();
    });
    expect(calls.find((call) => call.url === '/api/extensions/install')?.init?.method).toBe('PUT');
  });

  it('rejects non-zip drops with an error banner and no dialog', async () => {
    const view = await renderView();
    const file = new File(['text'], 'notes.txt', { type: 'text/plain' });
    act(() => {
      view.dispatchEvent(dragEvent('drop', { types: ['Files'], files: [file] }));
    });
    expect(document.querySelector('[role="alertdialog"]')).toBeNull();
    expect(container.querySelector('[role="alert"]')?.textContent).toContain('仅支持上传 ZIP 安装包');
    expect(calls.some((call) => call.url.includes('install') || call.url.includes('upgrade'))).toBe(false);
  });
});

const BACKEND: BackendInfo = {
  present: true,
  service_id: 'app-0123456789abcdef',
  arch: 'x86_64',
  host_arch: 'aarch64',
  executable: '/srv/apps/native/io.test.app/backend',
  program_dir: '/srv/apps/native/io.test.app',
  data_dir: '/srv/apps/data/io.test.app',
  socket_path: '/srv/apps/run/io.test.app/backend.sock',
  run_user: 'appuser',
  run_group: 'appgroup',
  allow_root: false,
  autostart: false,
  state: 'upgrade_failed',
  error: '后端覆盖失败：磁盘写入错误',
  supervisor: { state: 'RUNNING', pid: 4242, last_exit_code: 1, last_exit_at: 1760000300000 },
};

async function settle() {
  await act(async () => {
    for (let index = 0; index < 10; index += 1) await Promise.resolve();
  });
}

describe('ExtensionManagerView native backend', () => {
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
    vi.restoreAllMocks();
  });

  async function renderBackend(backend?: BackendInfo) {
    stubFetch([], [APP], backend);
    await act(async () => {
      root.render(<ExtensionManagerView onExtensionsChanged={() => {}} />);
    });
    await settle();
  }

  it('shows no backend controls for a pure Web application', async () => {
    await renderBackend(undefined);
    expect(container.querySelector('.ed-ext-backend')).toBeNull();
    expect(container.textContent).not.toContain('原生后端');
    expect(buttonByText(container, '启动')).toBeUndefined();
    // Data cleanup remains available and is separate from backend controls.
    expect(buttonByText(container, '彻底删除应用数据')).toBeDefined();
  });

  it('renders architecture, persisted failure state, Supervisor state and paths', async () => {
    await renderBackend(BACKEND);
    const section = container.querySelector('.ed-ext-backend')!;
    expect(section).not.toBeNull();
    const text = section.textContent ?? '';
    expect(text).toContain('原生后端');
    expect(text).toContain('升级失败');
    expect(text).toContain('x86_64');
    expect(text).toContain('aarch64');
    expect(text).toContain('不匹配');
    expect(text).toContain('4242');
    expect(text).toContain('退出码 1');
    expect(text).toContain(BACKEND.executable!);
    expect(text).toContain(BACKEND.data_dir!);
    expect(text).toContain('后端覆盖失败：磁盘写入错误');
    expect(text).toContain('未签名包，发布者身份未验证');
  });

  it('saves the run identity and shows a root privilege warning', async () => {
    const save = vi
      .spyOn(extensionApi, 'setBackendIdentity')
      .mockResolvedValue({ ok: true, backend: BACKEND });
    await renderBackend(BACKEND);

    const inputs = container.querySelectorAll<HTMLInputElement>('.ed-ext-backend-fields input');
    act(() => {
      setValue(inputs[0], 'newuser');
      setValue(inputs[1], 'newgroup');
    });
    const rootCheckbox = container.querySelector<HTMLInputElement>(
      '.ed-ext-backend-identity input[type="checkbox"]',
    )!;
    act(() => rootCheckbox.click());
    expect(container.textContent).toContain('root 后端拥有主机级权限');

    act(() => buttonByText(container, '保存运行身份')!.click());
    await settle();
    expect(save).toHaveBeenCalledWith(APP.app_id, {
      run_user: 'newuser',
      run_group: 'newgroup',
      allow_root: true,
    });
  });

  it('starts, stops and restarts through the backend API', async () => {
    const backend = { ...BACKEND, supervisor: { state: 'UNKNOWN' } };
    const start = vi.spyOn(extensionApi, 'startBackend').mockResolvedValue({ ok: true, backend });
    const stop = vi.spyOn(extensionApi, 'stopBackend').mockResolvedValue({ ok: true, backend });
    const restart = vi.spyOn(extensionApi, 'restartBackend').mockResolvedValue({ ok: true, backend });
    await renderBackend(backend);

    act(() => buttonByText(container, '启动')!.click());
    await settle();
    act(() => buttonByText(container, '停止')!.click());
    await settle();
    act(() => buttonByText(container, '重启')!.click());
    await settle();

    expect(start).toHaveBeenCalledWith(APP.app_id);
    expect(stop).toHaveBeenCalledWith(APP.app_id);
    expect(restart).toHaveBeenCalledWith(APP.app_id);
  });

  it('toggles autostart independently of start/stop', async () => {
    const autostart = vi
      .spyOn(extensionApi, 'setBackendAutostart')
      .mockResolvedValue({ ok: true, backend: { ...BACKEND, autostart: true } });
    await renderBackend(BACKEND);

    const checkbox = container.querySelector<HTMLInputElement>('.ed-ext-autostart input')!;
    expect(checkbox.checked).toBe(false);
    act(() => checkbox.click());
    await settle();
    expect(autostart).toHaveBeenCalledWith(APP.app_id, true);
  });

  it('requires typing the exact app id before purging application data', async () => {
    const purge = vi.spyOn(extensionApi, 'purgeAppData').mockResolvedValue({ ok: true, purged: true });
    await renderBackend(BACKEND);

    act(() => buttonByText(container, '彻底删除应用数据')!.click());
    const dialog = document.querySelector<HTMLElement>('[aria-label="彻底删除应用数据"]')!;
    expect(dialog).not.toBeNull();
    expect(dialog.textContent).toContain(BACKEND.data_dir!);

    const confirm = buttonByText(dialog, '彻底删除数据') as HTMLButtonElement;
    expect(confirm.disabled).toBe(true);
    const input = dialog.querySelector<HTMLInputElement>('input[aria-label="确认应用 ID"]')!;
    act(() => setValue(input, 'io.test.wrong'));
    expect(confirm.disabled).toBe(true);
    act(() => setValue(input, APP.app_id));
    expect(confirm.disabled).toBe(false);

    act(() => confirm.click());
    await settle();
    expect(purge).toHaveBeenCalledWith(APP.app_id);
    expect(document.querySelector('[aria-label="彻底删除应用数据"]')).toBeNull();
  });

  it('surfaces backend API errors', async () => {
    vi.spyOn(extensionApi, 'startBackend').mockRejectedValue(
      new ApiError(409, 'backend_identity_required', '请先配置运行用户与用户组。'),
    );
    await renderBackend({ ...BACKEND, supervisor: { state: 'STOPPED' } });

    act(() => buttonByText(container, '启动')!.click());
    await settle();
    expect(container.querySelector('[role="alert"]')?.textContent).toContain(
      '请先配置运行用户与用户组。',
    );
  });
});
