// @vitest-environment happy-dom
import { act } from 'react';
import { createRoot, type Root } from 'react-dom/client';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { createFileSystemMock, listingCalls, type MockNode } from '../../test/fileSystemMock';
import type { FileEntry } from '../../types';
import { DirectoryTree } from './DirectoryTree';
import { DirectoryTreeNode } from './DirectoryTreeNode';

const TREE: Record<string, MockNode[]> = {
  '/': [
    { name: 'docs', type: 'dir' },
    { name: 'opt', type: 'dir' },
    { name: '.cache', type: 'dir' },
    { name: 'a.txt', type: 'file' },
  ],
  '/docs': [
    { name: 'nested', type: 'dir' },
    { name: 'note.md', type: 'file' },
  ],
  '/docs/nested': [{ name: 'deep.txt', type: 'file' }],
  '/opt': [{ name: 'app', type: 'dir' }],
  '/opt/app': [{ name: 'src', type: 'dir' }],
  '/opt/app/src': [],
};

describe('DirectoryTree', () => {
  let container: HTMLDivElement;
  let root: Root;

  beforeEach(() => {
    document.body.innerHTML = '';
    container = document.createElement('div');
    document.body.appendChild(container);
    root = createRoot(container);
  });

  afterEach(() => {
    vi.unstubAllGlobals();
    act(() => root.unmount());
  });

  const install = (tree = TREE) => {
    const mock = createFileSystemMock(structuredClone(tree));
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

  const render = async (currentPath: string, showHidden = false, revision = 0) => {
    act(() => {
      root.render(
        <DirectoryTree currentPath={currentPath} showHidden={showHidden} revision={revision} onSelect={() => {}} />,
      );
    });
    await flush();
  };

  const labels = () => [...container.querySelectorAll('.ed-tree-label')].map((node) => node.textContent);
  const rowFor = (label: string) =>
    [...container.querySelectorAll<HTMLElement>('.ed-tree-row')].find(
      (row) => row.querySelector('.ed-tree-label')?.textContent === label,
    )!;

  it('loads direct subdirectories on demand and never walks deeper', async () => {
    const mock = install();
    await render('/');

    expect(labels()).toEqual(['/', 'docs', 'opt']);
    const treeRequests = listingCalls(mock).filter((call) => call.dirsOnly);
    expect(treeRequests.map((call) => call.path)).toEqual(['/']);
    expect(treeRequests.every((call) => call.hidden === false)).toBe(true);
  });

  it('expands a directory once and reuses the cached children', async () => {
    const mock = install();
    await render('/');

    act(() => rowFor('opt').click());
    await flush();
    expect(labels()).toEqual(['/', 'docs', 'opt', 'app']);
    expect(listingCalls(mock).filter((call) => call.dirsOnly && call.path === '/opt')).toHaveLength(1);

    // Collapse and expand again: no additional request.
    act(() => rowFor('opt').querySelector<HTMLElement>('.ed-tree-caret')!.click());
    await flush();
    expect(labels()).toEqual(['/', 'docs', 'opt']);
    act(() => rowFor('opt').querySelector<HTMLElement>('.ed-tree-caret')!.click());
    await flush();
    expect(labels()).toEqual(['/', 'docs', 'opt', 'app']);
    expect(listingCalls(mock).filter((call) => call.dirsOnly && call.path === '/opt')).toHaveLength(1);
  });

  it('expands the ancestors of the active path and marks it current', async () => {
    const mock = install();
    await render('/opt/app');
    expect(labels()).toEqual(['/', 'docs', 'opt', 'app', 'src']);
    expect(container.querySelector('.ed-tree-row.current .ed-tree-label')?.textContent).toBe('app');
    const paths = listingCalls(mock)
      .filter((call) => call.dirsOnly)
      .map((call) => call.path);
    expect(new Set(paths)).toEqual(new Set(['/', '/opt', '/opt/app']));
    expect(paths).toHaveLength(3); // exactly one request per level
  });

  it('reports load failures per node and retries on demand', async () => {
    const mock = install();
    mock.failListing('/docs', 'mock failure');
    await render('/');
    act(() => rowFor('docs').click());
    await flush();

    expect(container.querySelector('.ed-tree-error')?.textContent).toContain('加载失败');
    mock.clearListingFailure('/docs');
    act(() => container.querySelector<HTMLElement>('.ed-tree-error .ed-link')!.click());
    await flush();
    expect(container.querySelector('.ed-tree-error')).toBeNull();
    expect(labels()).toEqual(['/', 'docs', 'nested', 'opt']);
  });

  it('reloads the visible nodes through the refresh button and the revision prop', async () => {
    const mock = install();
    await render('/');
    act(() => rowFor('opt').click());
    await flush();
    const before = listingCalls(mock).filter((call) => call.dirsOnly).length;

    act(() => container.querySelector<HTMLElement>('[aria-label="刷新目录树"]')!.click());
    await flush();
    const afterRefresh = listingCalls(mock).filter((call) => call.dirsOnly).length;
    expect(afterRefresh - before).toBe(2); // '/' + expanded '/opt'

    act(() => {
      root.render(
        <DirectoryTree currentPath="/" showHidden={false} revision={1} onSelect={() => {}} />,
      );
    });
    await flush();
    const afterRevision = listingCalls(mock).filter((call) => call.dirsOnly).length;
    expect(afterRevision - afterRefresh).toBe(2);
    expect(labels()).toEqual(['/', 'docs', 'opt', 'app']);
  });

  it('ignores symlinked directories if a listing ever hands one to the tree', () => {
    const dir: FileEntry = {
      name: 'real',
      path: '/real',
      type: 'dir',
      size: 0,
      mtime: 0,
      mode: 0o755,
      permissions: 'rwxr-xr-x',
      uid: 0,
      gid: 0,
      hidden: false,
      mime: 'inode/directory',
      text_editable: false,
    };
    const link: FileEntry = {
      ...dir,
      name: 'linked',
      path: '/linked',
      type: 'symlink',
      link_target: '/real',
    };
    act(() => {
      root.render(
        <DirectoryTreeNode
          path="/"
          name="/"
          depth={0}
          currentPath="/"
          expanded={new Set(['/'])}
          nodes={{ '/': { status: 'loaded', dirs: [dir, link], truncated: false } }}
          onToggle={() => {}}
          onSelect={() => {}}
          onRetry={() => {}}
        />,
      );
    });
    expect([...container.querySelectorAll('.ed-tree-label')].map((node) => node.textContent)).toEqual(['/', 'real']);
  });
});
