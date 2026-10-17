// @vitest-environment happy-dom
import { act, type ReactNode } from 'react';
import { createRoot, type Root } from 'react-dom/client';
import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import type { FileEntry } from '../../types';
import { EntryIcon } from './FileTypeIcon';

function entry(name: string, partial: Partial<FileEntry> = {}): FileEntry {
  return {
    name,
    path: `/${name}`,
    type: 'file',
    size: 1024,
    mtime: 1_700_000_000,
    mode: 0o644,
    permissions: 'rw-r--r--',
    uid: 0,
    gid: 0,
    hidden: false,
    mime: 'text/plain',
    text_editable: true,
    ...partial,
  };
}

describe('EntryIcon', () => {
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
  });

  const render = (node: ReactNode) => {
    act(() => root.render(node));
  };
  const markup = () => container.querySelector('svg')?.innerHTML ?? '';

  it('renders distinct artwork for go, cpp and markdown files', () => {
    render(<EntryIcon entry={entry('main.go')} />);
    const go = markup();
    render(<EntryIcon entry={entry('main.cpp')} />);
    const cpp = markup();
    render(<EntryIcon entry={entry('README.md')} />);
    const markdown = markup();

    expect(go).not.toBe('');
    expect(cpp).not.toBe('');
    expect(markdown).not.toBe('');
    expect(new Set([go, cpp, markdown]).size).toBe(3);
  });

  it('uses Lucide for directories and applies the requested size', () => {
    render(<EntryIcon entry={entry('src', { type: 'dir', mime: 'inode/directory' })} size={22} />);
    const svg = container.querySelector('svg')!;
    expect(svg.getAttribute('width')).toBe('22');
    expect(svg.getAttribute('class')).toContain('lucide-folder');
  });
});
