// @vitest-environment happy-dom
import { act } from 'react';
import { createRoot, type Root } from 'react-dom/client';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { WindowLayer } from '../../components/WindowLayer';
import { api } from '../../services/api';
import { windowStore } from '../../stores/windowStore';
import type { FileEntry } from '../../types';

const FILE: FileEntry = {
  name: 'a.txt',
  path: '/data/a.txt',
  type: 'file',
  size: 5,
  mtime: 100,
  mode: 0o644,
  permissions: '-rw-r--r--',
  uid: 1000,
  gid: 1000,
  hidden: false,
  mime: 'text/plain',
  text_editable: true,
};

function setTextarea(textarea: HTMLTextAreaElement, value: string): void {
  const descriptor = Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype, 'value');
  if (descriptor?.set) descriptor.set.call(textarea, value);
  else textarea.value = value;
  textarea.dispatchEvent(new Event('input', { bubbles: true }));
}

async function flush(): Promise<void> {
  for (let index = 0; index < 3; index += 1) {
    await act(async () => {
      await new Promise((resolve) => setTimeout(resolve, 0));
    });
  }
}

describe('editor window lifecycle', () => {
  let container: HTMLDivElement;
  let root: Root;

  beforeEach(() => {
    windowStore.reset();
    windowStore.setViewport({ width: 1440, height: 900 });
    vi.spyOn(api, 'readText').mockResolvedValue({ ok: true, content: 'hello', entry: FILE });
    vi.spyOn(api, 'statFile').mockResolvedValue({ ok: true, entry: FILE });
    vi.spyOn(api, 'writeText').mockResolvedValue({ ok: true, entry: { ...FILE, size: 11, mtime: 101 } });
    document.body.innerHTML = '';
    container = document.createElement('div');
    document.body.appendChild(container);
    root = createRoot(container);
  });

  afterEach(() => {
    act(() => root.unmount());
    vi.restoreAllMocks();
    windowStore.reset();
  });

  const openEditor = async () => {
    const id = windowStore.launch('editor', {
      title: '编辑 a.txt',
      payload: { path: FILE.path },
      instanceKey: FILE.path,
    });
    act(() => {
      root.render(<WindowLayer onLogout={() => {}} onExtensionsChanged={() => {}} />);
    });
    await flush();
    return id;
  };

  it('keeps unsaved content while minimized and confirms before closing', async () => {
    const id = await openEditor();
    const textarea = container.querySelector<HTMLTextAreaElement>('.ed-editor-textarea')!;
    expect(textarea.value).toBe('hello');
    act(() => setTextarea(textarea, 'hello world'));
    expect(container.textContent).toContain('有未保存的修改');

    act(() => windowStore.minimize(id));
    const hiddenWindow = container.querySelector<HTMLElement>('.ed-window')!;
    expect(hiddenWindow.style.display).toBe('none');
    // The textarea is still mounted with the in-memory buffer intact.
    expect(container.querySelector<HTMLTextAreaElement>('.ed-editor-textarea')?.value).toBe('hello world');

    act(() => windowStore.focus(id));
    let closed = false;
    act(() => {
      closed = windowStore.requestClose(id);
    });
    expect(closed).toBe(false);
    expect(windowStore.getSnapshot()).toHaveLength(1);
    expect(document.body.textContent).toContain('关闭未保存的编辑器？');

    act(() => {
      const cancel = [...document.querySelectorAll<HTMLButtonElement>('.ed-modal button')].find(
        (button) => button.textContent === '继续编辑',
      )!;
      cancel.click();
    });
    expect(document.body.textContent).not.toContain('关闭未保存的编辑器？');

    act(() => {
      windowStore.requestClose(id);
    });
    act(() => {
      const discard = [...document.querySelectorAll<HTMLButtonElement>('.ed-modal button')].find(
        (button) => button.textContent === '放弃修改并关闭',
      )!;
      discard.click();
    });
    expect(windowStore.getSnapshot()).toHaveLength(0);
  });

  it('does not overwrite external changes silently', async () => {
    await openEditor();
    const textarea = container.querySelector<HTMLTextAreaElement>('.ed-editor-textarea')!;
    act(() => setTextarea(textarea, 'local edit'));
    vi.spyOn(api, 'statFile').mockResolvedValueOnce({
      ok: true,
      entry: { ...FILE, mtime: 999, size: 42 },
    });

    act(() => {
      const save = [...container.querySelectorAll<HTMLButtonElement>('button')].find((button) =>
        button.textContent?.includes('保存'),
      )!;
      save.click();
    });
    await flush();
    expect(container.textContent).toContain('文件已在磁盘上发生变化');
    expect(api.writeText).not.toHaveBeenCalled();

    act(() => {
      const overwrite = [...container.querySelectorAll<HTMLButtonElement>('button')].find(
        (button) => button.textContent === '仍然覆盖保存',
      )!;
      overwrite.click();
    });
    await flush();
    expect(api.writeText).toHaveBeenCalledWith(FILE.path, 'local edit');
  });

  it('refuses to save when the file disappeared', async () => {
    await openEditor();
    const textarea = container.querySelector<HTMLTextAreaElement>('.ed-editor-textarea')!;
    act(() => setTextarea(textarea, 'local edit'));
    vi.spyOn(api, 'statFile').mockRejectedValueOnce(new Error('file_not_found'));

    act(() => {
      const save = [...container.querySelectorAll<HTMLButtonElement>('button')].find((button) =>
        button.textContent?.includes('保存'),
      )!;
      save.click();
    });
    await flush();
    expect(container.textContent).toContain('file_not_found');
    expect(api.writeText).not.toHaveBeenCalled();
  });
});
