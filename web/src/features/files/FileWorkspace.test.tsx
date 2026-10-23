// @vitest-environment happy-dom
import { act } from 'react';
import { createRoot, type Root } from 'react-dom/client';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { preferencesStore } from '../../stores/preferencesStore';
import { windowStore } from '../../stores/windowStore';
import { createFileSystemMock, listingCalls, type FileSystemMock, type MockNode } from '../../test/fileSystemMock';
import { FileWorkspace } from './FileWorkspace';

const TREE: Record<string, MockNode[]> = {
  '/': [
    { name: 'opt', type: 'dir' },
    { name: 'var', type: 'dir' },
    { name: '.cache', type: 'dir' },
    { name: 'README.md', type: 'file', size: 3200 },
    { name: 'archive.bin', type: 'file', size: 2048, text: false },
    { name: '.env', type: 'file', size: 64 },
  ],
  '/opt': [{ name: 'app', type: 'dir' }],
  '/opt/app': [
    { name: 'config.yaml', type: 'file', size: 2457 },
    { name: 'logs', type: 'dir' },
  ],
  '/opt/app/logs': [{ name: 'app.log', type: 'file', size: 512 }],
  '/var': [],
};

describe('FileWorkspace', () => {
  let container: HTMLDivElement;
  let root: Root;

  beforeEach(() => {
    windowStore.closeAll();
    preferencesStore.reset();
    window.localStorage.removeItem('edgedesk.files.view');
    window.localStorage.removeItem('edgedesk.files.treeWidth');
    document.body.innerHTML = '';
    container = document.createElement('div');
    document.body.appendChild(container);
    root = createRoot(container);
    // happy-dom does not implement pointer capture; the tree resizer calls it.
    const element = Element.prototype as unknown as {
      setPointerCapture?: (id: number) => void;
      releasePointerCapture?: (id: number) => void;
    };
    if (!element.setPointerCapture) {
      element.setPointerCapture = () => {};
      element.releasePointerCapture = () => {};
    }
  });

  afterEach(() => {
    vi.unstubAllGlobals();
    act(() => root.unmount());
  });

  const install = (): FileSystemMock => {
    const mock = createFileSystemMock(structuredClone(TREE));
    vi.stubGlobal('fetch', mock.fetchMock);
    return mock;
  };

  const flush = async () => {
    for (let index = 0; index < 4; index += 1) {
      await act(async () => {
        await new Promise((resolve) => setTimeout(resolve, 0));
      });
    }
  };

  const render = async (initialPath = '/') => {
    act(() => {
      root.render(<FileWorkspace initialPath={initialPath} />);
    });
    await flush();
  };

  const tableRows = () =>
    [...container.querySelectorAll('.ed-files-table .ed-entry-name')].map((node) => node.textContent ?? '');
  const gridNames = () => [...container.querySelectorAll('.ed-file-card-name')].map((node) => node.textContent ?? '');
  const treeLabels = () => [...container.querySelectorAll('.ed-tree-label')].map((node) => node.textContent ?? '');
  const breadcrumbs = () => [...container.querySelectorAll('.ed-breadcrumbs button')].map((node) => node.textContent ?? '');
  const rowFor = (name: string) => {
    const row = [...container.querySelectorAll<HTMLTableRowElement>('.ed-files-table tbody tr')].find(
      (item) => item.querySelector('.ed-entry-name')?.textContent === name,
    );
    if (!row) throw new Error(`row not found: ${name}`);
    return row;
  };
  const buttonByText = (text: string) => {
    const button = [...container.querySelectorAll<HTMLButtonElement>('button')].find((item) =>
      (item.textContent ?? '').includes(text),
    );
    if (!button) throw new Error(`button not found: ${text}`);
    return button;
  };
  const checkRow = (name: string) => {
    const box = rowFor(name).querySelector<HTMLInputElement>('input[type="checkbox"]')!;
    act(() => box.click());
  };
  const dropEvent = (type: string, transfer: unknown): Event => {
    const event = new Event(type, { bubbles: true, cancelable: true });
    Object.defineProperty(event, 'dataTransfer', { value: transfer });
    return event;
  };
  const typeInto = (input: HTMLInputElement, value: string) => {
    act(() => {
      // React tracks input values; write through the prototype setter so the
      // dispatched `input` event is seen as a real change.
      const descriptor = Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value');
      if (descriptor?.set) descriptor.set.call(input, value);
      else input.value = value;
      input.dispatchEvent(new Event('input', { bubbles: true }));
    });
  };
  const pointerEvent = (type: string, init: { clientX: number; clientY: number; pointerId: number }): Event => {
    const ctor = (globalThis as unknown as { PointerEvent?: typeof Event }).PointerEvent;
    if (ctor) return new ctor(type, { ...init, bubbles: true });
    const event = new Event(type, { bubbles: true });
    Object.assign(event, init);
    return event;
  };

  it('renders the tree, breadcrumbs and listing of the initial directory', async () => {
    const mock = install();
    await render('/opt/app');

    expect(breadcrumbs()).toEqual(['/', 'opt', 'app']);
    expect(treeLabels()).toEqual(['/', '.cache', 'opt', 'app', 'logs', 'var']);
    expect(tableRows()).toEqual(['..', 'logs', 'config.yaml']);
    expect(listingCalls(mock).filter((call) => call.dirsOnly).map((call) => call.path)).toEqual([
      '/',
      '/opt',
      '/opt/app',
    ]);
    const listings = listingCalls(mock).filter((call) => !call.dirsOnly);
    expect(listings).toHaveLength(1);
    expect(listings[0].path).toBe('/opt/app');
  });

  it('opens on a single click and selects only through checkboxes', async () => {
    const mock = install();
    await render('/');

    const before = listingCalls(mock).length;
    act(() => rowFor('README.md').querySelector<HTMLElement>('.ed-entry')!.click());
    await flush();
    // The name opens the editor; it never selects and never refetches.
    expect(windowStore.getSnapshot().map((item) => item.appId)).toEqual(['editor']);
    expect(rowFor('README.md').className).not.toContain('selected');
    expect(rowFor('README.md').querySelector<HTMLInputElement>('input[type="checkbox"]')?.checked).toBe(false);
    expect(container.querySelector('.ed-selection-count')).toBeNull();
    expect(listingCalls(mock)).toHaveLength(before);

    checkRow('README.md');
    await flush();
    expect(rowFor('README.md').className).toContain('selected');
    expect(container.querySelector('.ed-selection-count')?.textContent).toContain('已选 1 项');

    // A single click on a directory navigates into it.
    act(() => rowFor('opt').querySelector<HTMLElement>('.ed-entry')!.click());
    await flush();
    expect(breadcrumbs()).toEqual(['/', 'opt']);
    expect(listingCalls(mock).filter((call) => !call.dirsOnly && call.path === '/opt')).toHaveLength(1);
    expect(container.querySelector('.ed-tree-row.current .ed-tree-label')?.textContent).toBe('opt');
  });

  it('keeps selection actions in the toolbar without inserting a panel', async () => {
    install();
    await render('/');
    const toolbar = container.querySelector<HTMLElement>('.ed-files-toolbar')!;

    checkRow('README.md');
    await flush();
    expect(container.querySelector('.ed-selection-bar')).toBeNull();
    expect(toolbar.querySelector('.ed-selection-count')?.textContent).toContain('已选 1 项');
    expect(toolbar.textContent).toContain('重命名');
    expect(toolbar.textContent).toContain('取消选择');
    expect(toolbar.querySelector('a[href^="/api/files/download"]')).not.toBeNull();
    // The regular actions swap back when the selection is cleared.
    expect(toolbar.textContent).not.toContain('上传文件');

    act(() => buttonByText('取消选择').click());
    await flush();
    expect(toolbar.querySelector('.ed-selection-count')).toBeNull();
    expect(toolbar.textContent).toContain('上传文件');
  });

  it('keeps one selection across the list and grid views', async () => {
    const mock = install();
    await render('/');
    checkRow('README.md');
    await flush();
    const before = listingCalls(mock).length;

    expect(container.querySelector('.ed-sort-control')).not.toBeNull();
    act(() => container.querySelector<HTMLElement>('[aria-label="网格视图"]')!.click());
    await flush();
    expect(gridNames()).toEqual(['.cache', 'opt', 'var', '.env', 'README.md', 'archive.bin']);
    expect(container.querySelector('.ed-file-card.selected .ed-file-card-name')?.textContent).toBe('README.md');
    expect(listingCalls(mock)).toHaveLength(before); // switching views never refetches
    expect(window.localStorage.getItem('edgedesk.files.view')).toBe('grid');
    expect(container.querySelector('.ed-sort-control')).not.toBeNull();

    act(() => container.querySelector<HTMLElement>('[aria-label="列表视图"]')!.click());
    await flush();
    expect(rowFor('README.md').className).toContain('selected');
    expect(rowFor('README.md').querySelector<HTMLInputElement>('input[type="checkbox"]')?.checked).toBe(true);
    expect(window.localStorage.getItem('edgedesk.files.view')).toBe('list');
    expect(container.querySelector('.ed-sort-control')).not.toBeNull();
  });

  it('filters the current page locally and explains the scope', async () => {
    const mock = install();
    await render('/');
    const before = listingCalls(mock).length;
    const input = container.querySelector<HTMLInputElement>('.ed-filter input')!;

    typeInto(input, 'read');
    await flush();
    expect(tableRows()).toEqual(['README.md']);
    expect(listingCalls(mock)).toHaveLength(before);
    expect(container.querySelector('.ed-files-status')?.textContent).toContain('已筛选 1 / 当前页 6 项');

    typeInto(input, 'nothing-matches');
    await flush();
    expect(container.textContent).toContain('没有匹配');
    expect(container.textContent).toContain('不会跨目录搜索');
  });

  it('applies the hidden-file toggle to the tree and the list', async () => {
    const mock = install();
    await render('/');
    expect(tableRows()).toContain('.env');
    expect(treeLabels()).toContain('.cache');

    act(() => container.querySelector<HTMLElement>('[aria-label="视图选项"]')!.click());
    const toggle = container.querySelector<HTMLInputElement>('.ed-view-menu .ed-toggle input')!;
    act(() => toggle.click());
    await flush();

    expect(tableRows()).not.toContain('.env');
    expect(treeLabels()).not.toContain('.cache');
    const treeRequest = listingCalls(mock).filter((call) => call.dirsOnly).at(-1)!;
    const listRequest = listingCalls(mock).filter((call) => !call.dirsOnly).at(-1)!;
    expect(treeRequest).toMatchObject({ path: '/', hidden: false });
    expect(listRequest).toMatchObject({ path: '/', hidden: false });
  });

  it('shows a real failure state and reloads through retry', async () => {
    const mock = install();
    mock.failListing('/', 'mock failure');
    await render('/');
    expect(container.querySelector('.ed-files-state')?.textContent).toContain('目录加载失败');
    expect(container.textContent).toContain('mock failure');
    expect(listingCalls(mock).filter((call) => !call.dirsOnly)).toHaveLength(1);

    mock.clearListingFailure('/');
    act(() => container.querySelector<HTMLElement>('.ed-files-state .ed-button')!.click());
    await flush();
    expect(container.querySelector('.ed-files-state')).toBeNull();
    expect(tableRows()).toContain('README.md');
    expect(listingCalls(mock).filter((call) => !call.dirsOnly)).toHaveLength(2);
  });

  it('confirms batch delete, then reports the real counters', async () => {
    const mock = install();
    await render('/');
    checkRow('README.md');
    checkRow('.env');
    await flush();
    expect(container.querySelector('.ed-selection-count')?.textContent).toContain('已选 2 项');

    act(() => buttonByText('删除').click());
    const dialog = document.querySelector('[role="alertdialog"]')!;
    expect(dialog.textContent).toContain('/README.md');
    expect(dialog.textContent).toContain('/.env');

    act(() => dialog.querySelector<HTMLElement>('.ed-modal-actions .danger')!.click());
    await flush();
    expect([...mock.deleted].sort()).toEqual(['/.env', '/README.md']);
    expect(container.textContent).toContain('已删除 2 个文件、0 个目录');
    expect(container.querySelector('.ed-selection-count')).toBeNull();
    expect(tableRows()).not.toContain('README.md');
  });

  it('never reports a failed delete as success', async () => {
    const mock = install();
    await render('/');
    checkRow('README.md');
    await flush();
    mock.failNextDelete('mock delete failure');

    act(() => buttonByText('删除').click());
    act(() => document.querySelector<HTMLElement>('[role="alertdialog"] .ed-modal-actions .danger')!.click());
    await flush();

    expect(container.textContent).toContain('mock delete failure');
    expect(container.textContent).not.toContain('已删除');
    expect(tableRows()).toContain('README.md');
  });

  it('confirms before uploading dropped files, skips folders and reports the result', async () => {
    const mock = install();
    const uploads: { url: string; name: string }[] = [];
    class FakeXHR {
      upload: { onprogress: ((event: { lengthComputable: boolean; loaded: number; total: number }) => void) | null } = {
        onprogress: null,
      };
      onload: (() => void) | null = null;
      onerror: (() => void) | null = null;
      status = 201;
      responseText = JSON.stringify({ ok: true, size: 5 });
      withCredentials = false;
      private url = '';
      open(_method: string, url: string) {
        this.url = url;
      }
      send(body: unknown) {
        uploads.push({ url: this.url, name: (body as File).name });
        this.upload.onprogress?.({ lengthComputable: true, loaded: 5, total: 5 });
        queueMicrotask(() => this.onload?.());
      }
    }
    vi.stubGlobal('XMLHttpRequest', FakeXHR as unknown as typeof XMLHttpRequest);

    await render('/');
    const section = container.querySelector<HTMLElement>('.ed-files-main')!;
    const notes = new File(['hello'], 'notes.txt', { type: 'text/plain' });
    const readme = new File(['# readme'], 'README.md', { type: 'text/markdown' });
    const transfer = {
      types: ['Files'],
      dropEffect: '',
      files: [notes, readme],
      items: [
        { kind: 'file', webkitGetAsEntry: () => ({ isDirectory: false, isFile: true }), getAsFile: () => notes },
        { kind: 'file', webkitGetAsEntry: () => ({ isDirectory: true, isFile: false }), getAsFile: () => null },
        { kind: 'file', webkitGetAsEntry: () => ({ isDirectory: false, isFile: true }), getAsFile: () => readme },
      ],
    };

    act(() => {
      section.dispatchEvent(dropEvent('dragenter', transfer));
      section.dispatchEvent(dropEvent('dragover', transfer));
      section.dispatchEvent(dropEvent('drop', transfer));
    });
    await flush();

    let dialog = document.querySelector<HTMLElement>('[role="alertdialog"]')!;
    expect(dialog.textContent).toContain('上传 2 个文件到 /');
    expect(dialog.textContent).toContain('notes.txt');
    expect(dialog.textContent).toContain('README.md');
    expect(dialog.textContent).toContain('将覆盖');
    expect(dialog.textContent).toContain('已跳过 1 个文件夹');

    // Cancel keeps the directory untouched.
    act(() => dialog.querySelectorAll<HTMLButtonElement>('.ed-modal-actions button')[0].click());
    await flush();
    expect(uploads).toHaveLength(0);
    expect(document.querySelector('[role="alertdialog"]')).toBeNull();

    // Dropping again and confirming uploads every file to the current directory.
    act(() => section.dispatchEvent(dropEvent('drop', transfer)));
    await flush();
    dialog = document.querySelector<HTMLElement>('[role="alertdialog"]')!;
    act(() => dialog.querySelector<HTMLElement>('.ed-modal-actions .primary')!.click());
    await flush();
    expect(uploads.map((item) => item.name)).toEqual(['notes.txt', 'README.md']);
    expect(uploads[0].url).toBe('/api/files/content?path=%2Fnotes.txt');
    expect(container.querySelector('.ed-table-footer')?.textContent).toContain('已上传 2 个文件');
    expect(mock.calls.some((call) => call.method === 'GET' && call.url.startsWith('/api/files?'))).toBe(true);
  });

  it('opens editor, preview and terminal floating windows and keeps them across navigation', async () => {
    const mock = install();
    await render('/');

    act(() => rowFor('README.md').querySelector<HTMLElement>('.ed-entry')!.click());
    act(() => rowFor('archive.bin').querySelector<HTMLElement>('.ed-entry')!.click());
    await flush();
    expect(windowStore.getSnapshot().map((item) => item.appId).sort()).toEqual(['editor', 'preview']);

    act(() => buttonByText('在当前目录打开终端').click());
    await flush();
    const terminal = windowStore.getSnapshot().find((item) => item.appId === 'terminal');
    expect(terminal?.title).toBe('/ · Bash');
    expect(mock.calls.some((call) => call.url === '/api/terminal/sessions' && call.method === 'POST')).toBe(true);

    act(() => rowFor('opt').querySelector<HTMLElement>('.ed-entry')!.click());
    await flush();
    expect(breadcrumbs()).toEqual(['/', 'opt']);
    expect(windowStore.getSnapshot()).toHaveLength(3);
  });

  it('collapses the tree into a rail and reopens it without refetching', async () => {
    const mock = install();
    await render('/');
    const before = listingCalls(mock).length;

    act(() => container.querySelector<HTMLElement>('[aria-label="收起目录树"]')!.click());
    expect(container.querySelector('.ed-files-workspace')?.className).toContain('tree-collapsed');
    const rail = container.querySelector<HTMLElement>('.ed-files-tree-rail')!;
    expect(rail.getAttribute('aria-label')).toBe('展开目录树');

    act(() => rail.click());
    expect(container.querySelector('.ed-files-workspace')?.className).not.toContain('tree-collapsed');
    // The tree stays mounted while collapsed: expansion state survives and no refetch happens.
    expect(container.querySelector('.ed-tree-label')?.textContent).toBe('/');
    expect(listingCalls(mock)).toHaveLength(before);
  });

  it('resizes the tree panel and persists the width', async () => {
    await render('/');
    const resizer = container.querySelector<HTMLElement>('.ed-tree-resizer')!;
    act(() => {
      resizer.dispatchEvent(pointerEvent('pointerdown', { clientX: 300, clientY: 120, pointerId: 7 }));
      resizer.dispatchEvent(pointerEvent('pointermove', { clientX: 360, clientY: 120, pointerId: 7 }));
      resizer.dispatchEvent(pointerEvent('pointerup', { clientX: 360, clientY: 120, pointerId: 7 }));
    });
    const workspace = container.querySelector<HTMLElement>('.ed-files-workspace')!;
    expect(workspace.style.getPropertyValue('--ed-tree-width')).toBe('324px');
    expect(window.localStorage.getItem('edgedesk.files.treeWidth')).toBe('324');
  });

  it('creates a folder and refreshes the tree after the mutation', async () => {
    const mock = install();
    await render('/');
    const treeRequestsBefore = listingCalls(mock).filter((call) => call.dirsOnly).length;

    act(() => buttonByText('新建文件夹').click());
    const input = document.querySelector<HTMLInputElement>('.ed-modal input')!;
    typeInto(input, 'newdir');
    const form = document.querySelector<HTMLFormElement>('.ed-modal')!;
    act(() => form.dispatchEvent(new Event('submit', { bubbles: true, cancelable: true })));
    await flush();

    expect(mock.createdDirs).toEqual(['/newdir']);
    expect(container.textContent).toContain('已创建目录 newdir');
    expect(listingCalls(mock).filter((call) => call.dirsOnly).length).toBeGreaterThan(treeRequestsBefore);
    expect(tableRows()).toContain('newdir');
  });

  it('creates an empty file and opens it in the editor', async () => {
    const mock = install();
    await render('/');

    act(() => buttonByText('新建文件').click());
    const input = document.querySelector<HTMLInputElement>('.ed-modal input')!;
    typeInto(input, 'notes.md');
    const form = document.querySelector<HTMLFormElement>('.ed-modal')!;
    act(() => form.dispatchEvent(new Event('submit', { bubbles: true, cancelable: true })));
    await flush();

    expect(mock.createdFiles).toEqual(['/notes.md']);
    expect(container.textContent).toContain('已创建文件 notes.md');
    expect(tableRows()).toContain('notes.md');
    const editor = windowStore.getSnapshot().find((item) => item.appId === 'editor');
    expect(editor?.payload).toEqual({ path: '/notes.md' });
  });

  it('renders the root slash once in the breadcrumb', async () => {
    await render('/opt/app');
    expect(breadcrumbs()).toEqual(['/', 'opt', 'app']);
    const separators = [...container.querySelectorAll('.ed-crumb-sep')].map((node) => node.textContent ?? '');
    expect(separators).toEqual(['/']);
  });

  it('renders a paginated, sorted listing through the toolbar controls', async () => {
    const many: MockNode[] = Array.from({ length: 205 }, (_, index) => ({
      name: `file-${String(index).padStart(3, '0')}.txt`,
      type: 'file',
      size: index,
    }));
    many.push({ name: 'zzz-last', type: 'file', size: 999_999 });
    const paged = createFileSystemMock({ '/': many });
    vi.stubGlobal('fetch', paged.fetchMock);
    await render('/');

    expect(tableRows()).toHaveLength(200);
    expect(container.querySelector('.ed-table-footer')?.textContent).toContain('共 206 项');
    act(() => buttonByText('下一页').click());
    await flush();
    expect(tableRows()).toHaveLength(6);
    expect(listingCalls(paged).filter((call) => !call.dirsOnly)).toHaveLength(2);

    act(() => container.querySelector<HTMLElement>('.ed-col-size')!.click());
    await flush();
    const firstSizeSort = listingCalls(paged).filter((call) => !call.dirsOnly).at(-1)!;
    expect(new URL(firstSizeSort.url, 'http://localhost').searchParams.get('sort')).toBe('size');
    expect(tableRows()[0]).toBe('file-000.txt');
  });
});
