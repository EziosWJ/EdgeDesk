/**
 * Minimal deterministic stand-in for the `/api/files` endpoints used by the
 * file-management DOM tests. It only implements what the workspace calls.
 */
import { baseName, joinPath, parentPath } from '../features/files/fileUtils';
import type { FileEntry, FileListing } from '../types';

export type MockNode = {
  name: string;
  type: 'dir' | 'file' | 'symlink';
  size?: number;
  mtime?: number;
  target?: string;
  /** `text_editable` flag for regular files. */
  text?: boolean;
};

export type MockCall = { method: string; url: string };

const DEFAULT_MTIME = 1_700_000_000;

function toEntry(dir: string, node: MockNode): FileEntry {
  const isDir = node.type === 'dir';
  const isLink = node.type === 'symlink';
  return {
    name: node.name,
    path: joinPath(dir, node.name),
    type: node.type,
    size: node.type === 'file' ? node.size ?? 16 : 0,
    mtime: node.mtime ?? DEFAULT_MTIME,
    mode: isDir ? 0o755 : 0o644,
    permissions: isDir ? 'rwxr-xr-x' : 'rw-r--r--',
    uid: 0,
    gid: 0,
    hidden: node.name.startsWith('.'),
    mime: isDir ? 'inode/directory' : isLink ? 'inode/symlink' : 'text/plain',
    text_editable: node.type === 'file' && (node.text ?? true),
    ...(isLink ? { link_target: node.target ?? '/docs' } : {}),
  };
}

export type FileSystemMock = {
  calls: MockCall[];
  deleted: string[];
  renames: { from: string; to: string }[];
  createdDirs: string[];
  createdFiles: string[];
  /** Fail every listing of this path until cleared. */
  failListing(path: string, message: string): void;
  clearListingFailure(path: string): void;
  /** Fail the next delete request. */
  failNextDelete(message: string): void;
  fetchMock: (input: RequestInfo | URL, init?: RequestInit) => Promise<Response>;
};

export function createFileSystemMock(tree: Record<string, MockNode[]>): FileSystemMock {
  const calls: MockCall[] = [];
  const deleted: string[] = [];
  const renames: { from: string; to: string }[] = [];
  const createdDirs: string[] = [];
  const createdFiles: string[] = [];
  const listingFailures = new Map<string, string>();
  let deleteFailure: string | null = null;

  const handle = (url: string, method: string, body?: string): { status: number; payload: unknown } => {
    calls.push({ method, url });
    const parsed = new URL(url, 'http://localhost');
    if (parsed.pathname === '/api/files' && method === 'GET') {
      const path = parsed.searchParams.get('path') ?? '/';
      const failure = listingFailures.get(path);
      if (failure) {
        return { status: 500, payload: { ok: false, error: 'io_error', message: failure } };
      }
      const showHidden = parsed.searchParams.get('hidden') !== '0';
      const dirsOnly = parsed.searchParams.get('dirs_only') === '1';
      const sort = parsed.searchParams.get('sort') ?? 'name';
      const desc = parsed.searchParams.get('order') === 'desc';
      const page = Number(parsed.searchParams.get('page') ?? '1');
      const pageSize = Number(parsed.searchParams.get('page_size') ?? '200');
      const nodes = (tree[path] ?? []).filter((node) => {
        if (!showHidden && node.name.startsWith('.')) return false;
        if (dirsOnly && node.type !== 'dir') return false;
        return true;
      });
      const entries = nodes.map((node) => toEntry(path, node)).sort((a, b) => {
        if ((a.type === 'dir') !== (b.type === 'dir')) return a.type === 'dir' ? -1 : 1;
        let order = 0;
        if (sort === 'size') order = a.size - b.size;
        else if (sort === 'mtime') order = a.mtime - b.mtime;
        // Byte order, like the C++ backend (`std::string::compare`).
        if (order === 0) order = a.name < b.name ? -1 : a.name > b.name ? 1 : 0;
        return desc ? -order : order;
      });
      const start = Math.max(0, (page - 1) * pageSize);
      const listing: FileListing = {
        ok: true,
        path,
        page,
        page_size: pageSize,
        total: entries.length,
        truncated: false,
        entries: entries.slice(start, start + pageSize),
      };
      return { status: 200, payload: listing };
    }
    if (parsed.pathname === '/api/files/mkdir' && method === 'POST') {
      const target = String((JSON.parse(body ?? '{}') as { path?: string }).path ?? '');
      tree[parentPath(target)] = [...(tree[parentPath(target)] ?? []), { name: baseName(target), type: 'dir' }];
      tree[target] = tree[target] ?? [];
      createdDirs.push(target);
      return { status: 201, payload: { ok: true, path: target } };
    }
    if (parsed.pathname === '/api/files/create' && method === 'POST') {
      const target = String((JSON.parse(body ?? '{}') as { path?: string }).path ?? '');
      const dir = parentPath(target);
      const name = baseName(target);
      const siblings = tree[dir] ?? [];
      if (siblings.some((node) => node.name === name)) {
        return {
          status: 409,
          payload: { ok: false, error: 'already_exists', message: `file already exists: ${target}` },
        };
      }
      const node: MockNode = { name, type: 'file', size: 0 };
      tree[dir] = [...siblings, node];
      createdFiles.push(target);
      return { status: 201, payload: { ok: true, path: target, entry: toEntry(dir, node) } };
    }
    if (parsed.pathname === '/api/files/rename' && method === 'POST') {
      const { from, to } = JSON.parse(body ?? '{}') as { from: string; to: string };
      const source = tree[parentPath(from)] ?? [];
      const index = source.findIndex((node) => node.name === baseName(from));
      if (index >= 0) {
        const [node] = source.splice(index, 1);
        node.name = baseName(to);
        tree[parentPath(to)] = [...(tree[parentPath(to)] ?? []), node];
        if (node.type === 'dir' && tree[from]) {
          tree[to] = tree[from];
          delete tree[from];
        }
      }
      renames.push({ from, to });
      return { status: 200, payload: { ok: true } };
    }
    if (parsed.pathname === '/api/files/delete' && method === 'POST') {
      if (deleteFailure) {
        const message = deleteFailure;
        deleteFailure = null;
        return { status: 500, payload: { ok: false, error: 'io_error', message } };
      }
      const targets = (JSON.parse(body ?? '{}') as { paths?: string[] }).paths ?? [];
      let removedDirs = 0;
      for (const target of targets) {
        const parent = tree[parentPath(target)] ?? [];
        const index = parent.findIndex((node) => node.name === baseName(target));
        if (index >= 0 && parent[index].type === 'dir') removedDirs += 1;
        if (index >= 0) parent.splice(index, 1);
        for (const key of Object.keys(tree)) {
          if (key === target || key.startsWith(`${target}/`)) delete tree[key];
        }
        deleted.push(target);
      }
      return {
        status: 200,
        payload: { ok: true, removed_files: targets.length - removedDirs, removed_dirs: removedDirs },
      };
    }
    if (parsed.pathname === '/api/terminal/sessions' && method === 'POST') {
      return { status: 201, payload: { ok: true, session: { id: 'sess-1', title: 'Bash', cwd: '/' } } };
    }
    return { status: 404, payload: { ok: false, error: 'not_found', message: `unmocked ${method} ${url}` } };
  };

  const fetchMock = (input: RequestInfo | URL, init?: RequestInit): Promise<Response> => {
    const url = typeof input === 'string' ? input : String(input);
    const method = (init?.method ?? 'GET').toUpperCase();
    const body = typeof init?.body === 'string' ? init.body : undefined;
    const { status, payload } = handle(url, method, body);
    return Promise.resolve({
      ok: status >= 200 && status < 300,
      status,
      statusText: status >= 400 ? 'Error' : 'OK',
      text: () => Promise.resolve(JSON.stringify(payload)),
    } as unknown as Response);
  };

  return {
    calls,
    deleted,
    renames,
    createdDirs,
    createdFiles,
    failListing: (path, message) => listingFailures.set(path, message),
    clearListingFailure: (path) => listingFailures.delete(path),
    failNextDelete: (message) => {
      deleteFailure = message;
    },
    fetchMock,
  };
}

/** Listing calls (GET /api/files) recorded by the mock, in order. */
export function listingCalls(
  mock: FileSystemMock,
): { url: string; path: string; dirsOnly: boolean; hidden: boolean }[] {
  return mock.calls
    .filter((call) => call.method === 'GET' && call.url.startsWith('/api/files?'))
    .map((call) => {
      const parsed = new URL(call.url, 'http://localhost');
      return {
        url: call.url,
        path: parsed.searchParams.get('path') ?? '/',
        dirsOnly: parsed.searchParams.get('dirs_only') === '1',
        hidden: parsed.searchParams.get('hidden') !== '0',
      };
    });
}
