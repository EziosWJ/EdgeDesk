// @vitest-environment happy-dom
import { act } from 'react';
import { createRoot, type Root } from 'react-dom/client';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { ExtensionManagerView } from './ExtensionManagerView';
import type { ExtensionApp, ExtensionInstallResult, ExtensionRevision } from './extensionTypes';

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

function stubFetch(calls: FetchCall[], apps: ExtensionApp[]) {
  vi.stubGlobal('fetch', async (url: string, init?: RequestInit) => {
    calls.push({ url, init });
    if (url === '/api/extensions') return jsonResponse({ ok: true, apps });
    if (url === `/api/extensions/${APP.app_id}`) {
      return jsonResponse({ ok: true, app: APP, revisions: [REVISION] });
    }
    if (url === '/api/extensions/install' || url === `/api/extensions/${APP.app_id}/upgrade`) {
      return jsonResponse(INSTALL_RESULT);
    }
    return jsonResponse({ error: 'not_found' }, 404);
  });
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
    const dialog = container.querySelector('[role="alertdialog"]');
    expect(dialog?.getAttribute('aria-label')).toBe('升级 测试应用？');
    expect(container.textContent).toContain('app.zip');

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
    expect(container.querySelector('[role="alertdialog"]')).toBeNull();
  });

  it('installs as a new app when nothing is selected', async () => {
    const view = await renderView([]);
    const file = new File(['zip'], 'fresh.zip', { type: 'application/zip' });
    act(() => {
      view.dispatchEvent(dragEvent('drop', { types: ['Files'], files: [file] }));
    });
    const dialog = container.querySelector('[role="alertdialog"]');
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
    expect(container.querySelector('[role="alertdialog"]')).toBeNull();
    expect(container.querySelector('[role="alert"]')?.textContent).toContain('仅支持上传 ZIP 安装包');
    expect(calls.some((call) => call.url.includes('install') || call.url.includes('upgrade'))).toBe(false);
  });
});
