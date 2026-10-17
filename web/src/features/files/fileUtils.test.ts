import { beforeEach, describe, expect, it } from 'vitest';
import type { FileEntry } from '../../types';
import {
  ancestorChain,
  baseName,
  breadcrumbsFor,
  clampTreeWidth,
  entryKindLabel,
  entryMeta,
  fileIconKey,
  filterEntries,
  isSafeUploadName,
  joinPath,
  normalizePath,
  opensInEditor,
  parentPath,
  readTreeWidthPreference,
  readViewPreference,
  TREE_WIDTH_DEFAULT,
  TREE_WIDTH_MAX,
  TREE_WIDTH_MIN,
  writeTreeWidthPreference,
  writeViewPreference,
} from './fileUtils';

function entry(partial: Partial<FileEntry> & { name: string }): FileEntry {
  return {
    path: `/${partial.name}`,
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

describe('fileUtils paths', () => {
  it('normalizes and joins sandbox paths', () => {
    expect(normalizePath('/opt/app/')).toBe('/opt/app');
    expect(normalizePath('')).toBe('/');
    expect(normalizePath('/')).toBe('/');
    expect(joinPath('/', 'docs')).toBe('/docs');
    expect(joinPath('/opt/app', 'config')).toBe('/opt/app/config');
    expect(parentPath('/opt/app')).toBe('/opt');
    expect(parentPath('/opt')).toBe('/');
    expect(parentPath('/')).toBe('/');
    expect(baseName('/opt/app')).toBe('app');
    expect(baseName('/')).toBe('/');
  });

  it('builds breadcrumbs and the ancestor chain for the active path', () => {
    expect(breadcrumbsFor('/opt/app/config')).toEqual([
      { label: '/', target: '/' },
      { label: 'opt', target: '/opt' },
      { label: 'app', target: '/opt/app' },
      { label: 'config', target: '/opt/app/config' },
    ]);
    expect(ancestorChain('/opt/app/config')).toEqual(['/', '/opt', '/opt/app', '/opt/app/config']);
    expect(ancestorChain('/')).toEqual(['/']);
  });
});

describe('fileUtils entries', () => {
  it('filters by case-insensitive name substring without dropping matches', () => {
    const entries = [entry({ name: 'README.md' }), entry({ name: 'app.log' }), entry({ name: 'nginx.conf' })];
    expect(filterEntries(entries, '')).toHaveLength(3);
    expect(filterEntries(entries, 'LOG').map((item) => item.name)).toEqual(['app.log']);
    expect(filterEntries(entries, 'no-match')).toHaveLength(0);
  });

  it('maps file names to per-format icon keys and labels', () => {
    expect(fileIconKey(entry({ name: 'main.go' }))).toBe('go');
    expect(fileIconKey(entry({ name: 'main.cpp' }))).toBe('cpp');
    expect(fileIconKey(entry({ name: 'header.hpp' }))).toBe('cpp');
    expect(fileIconKey(entry({ name: 'README.md' }))).toBe('markdown');
    expect(fileIconKey(entry({ name: 'app.ts' }))).toBe('typescript');
    expect(fileIconKey(entry({ name: 'App.tsx' }))).toBe('reactts');
    expect(fileIconKey(entry({ name: 'data.json' }))).toBe('json');
    expect(fileIconKey(entry({ name: 'config.yaml' }))).toBe('yaml');
    expect(fileIconKey(entry({ name: 'script.sh' }))).toBe('shell');
    expect(fileIconKey(entry({ name: 'Dockerfile' }))).toBe('docker');
    expect(fileIconKey(entry({ name: 'Makefile' }))).toBe('makefile');
    expect(fileIconKey(entry({ name: 'CMakeLists.txt' }))).toBe('cmake');
    expect(fileIconKey(entry({ name: 'LICENSE' }))).toBe('license');
    expect(fileIconKey(entry({ name: 'photo.png', mime: 'image/png', text_editable: false }))).toBe('image');
    expect(fileIconKey(entry({ name: 'report.xlsx', text_editable: false }))).toBe('excel');
    expect(fileIconKey(entry({ name: 'archive.tar.gz', text_editable: false }))).toBe('zip');
    expect(fileIconKey(entry({ name: 'blob.bin', text_editable: false, mime: 'application/octet-stream' }))).toBe(
      'binary',
    );
    expect(fileIconKey(entry({ name: 'blob.unknownext', text_editable: false, mime: 'application/octet-stream' }))).toBe(
      'default',
    );
    expect(fileIconKey(entry({ name: 'unknown.xyz' }))).toBe('text');
    expect(entryKindLabel(entry({ name: 'docs', type: 'dir' }))).toBe('目录');
    expect(entryKindLabel(entry({ name: 'link', type: 'symlink' }))).toBe('符号链接');
    expect(entryKindLabel(entry({ name: 'main.go' }))).toBe('代码');
    expect(entryKindLabel(entry({ name: 'README.md' }))).toBe('文本');
    expect(entryKindLabel(entry({ name: 'config.yaml' }))).toBe('配置');
    expect(entryKindLabel(entry({ name: 'script.sh' }))).toBe('脚本');
    expect(entryKindLabel(entry({ name: 'backup.tar.gz', text_editable: false }))).toBe('压缩包');
    expect(opensInEditor(entry({ name: 'note.md' }))).toBe(true);
    expect(opensInEditor(entry({ name: 'photo.png', text_editable: false }))).toBe(false);
    expect(opensInEditor(entry({ name: 'docs', type: 'dir' }))).toBe(false);
    expect(entryMeta(entry({ name: 'photo.png', size: 2048, text_editable: false }))).toContain('2.0 KiB');
  });

  it('refuses upload names that could escape the current directory', () => {
    expect(isSafeUploadName('notes.txt')).toBe(true);
    expect(isSafeUploadName('.env')).toBe(true);
    expect(isSafeUploadName('')).toBe(false);
    expect(isSafeUploadName('.')).toBe(false);
    expect(isSafeUploadName('..')).toBe(false);
    expect(isSafeUploadName('../etc/passwd')).toBe(false);
    expect(isSafeUploadName('a/b.txt')).toBe(false);
    expect(isSafeUploadName('a\\b.txt')).toBe(false);
    expect(isSafeUploadName('a\0b')).toBe(false);
  });
});

describe('fileUtils preferences', () => {
  beforeEach(() => {
    window.localStorage.removeItem('edgedesk.files.view');
    window.localStorage.removeItem('edgedesk.files.treeWidth');
  });

  it('defaults to the list view and persists an explicit choice', () => {
    expect(readViewPreference()).toBe('list');
    writeViewPreference('grid');
    expect(readViewPreference()).toBe('grid');
    writeViewPreference('list');
    expect(readViewPreference()).toBe('list');
  });

  it('clamps the stored tree width', () => {
    expect(readTreeWidthPreference()).toBe(TREE_WIDTH_DEFAULT);
    writeTreeWidthPreference(10);
    expect(readTreeWidthPreference()).toBe(TREE_WIDTH_MIN);
    writeTreeWidthPreference(9999);
    expect(readTreeWidthPreference()).toBe(TREE_WIDTH_MAX);
    writeTreeWidthPreference(300);
    expect(readTreeWidthPreference()).toBe(300);
    expect(clampTreeWidth(Number.NaN)).toBe(TREE_WIDTH_DEFAULT);
  });
});
