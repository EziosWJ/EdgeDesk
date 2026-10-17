import {
  File,
  FileArchive,
  FileAudio,
  FileCode,
  FileImage,
  FileSpreadsheet,
  FileText,
  FileVideo,
  Folder,
  Link2,
  type LucideIcon,
} from 'lucide-react';
import { formatBytes, formatTime } from '../../services/api';
import type { FileEntry } from '../../types';

export type SortKey = 'name' | 'size' | 'mtime' | 'type';
export type ViewMode = 'list' | 'grid';

/** UI preferences only: never store credentials or session material here. */
export const VIEW_STORAGE_KEY = 'edgedesk.files.view';
export const TREE_WIDTH_STORAGE_KEY = 'edgedesk.files.treeWidth';
export const TREE_WIDTH_DEFAULT = 264;
export const TREE_WIDTH_MIN = 200;
export const TREE_WIDTH_MAX = 560;

/** Trailing slashes are dropped so path comparisons stay stable. */
export function normalizePath(path: string): string {
  if (!path) return '/';
  let next = path;
  while (next.length > 1 && next.endsWith('/')) next = next.slice(0, -1);
  return next.startsWith('/') ? next : `/${next}`;
}

export function joinPath(parent: string, name: string): string {
  const base = normalizePath(parent);
  return base === '/' ? `/${name}` : `${base}/${name}`;
}

export function parentPath(path: string): string {
  const normalized = normalizePath(path);
  if (normalized === '/') return '/';
  const index = normalized.lastIndexOf('/');
  return index <= 0 ? '/' : normalized.slice(0, index);
}

export function baseName(path: string): string {
  const normalized = normalizePath(path);
  if (normalized === '/') return '/';
  return normalized.slice(normalized.lastIndexOf('/') + 1);
}

export function breadcrumbsFor(path: string): { label: string; target: string }[] {
  const crumbs = [{ label: '/', target: '/' }];
  let current = '';
  for (const part of normalizePath(path).split('/').filter(Boolean)) {
    current += `/${part}`;
    crumbs.push({ label: part, target: current });
  }
  return crumbs;
}

/** `['/', '/opt', '/opt/app']` for `/opt/app`: every ancestor plus the path itself. */
export function ancestorChain(path: string): string[] {
  const chain = ['/'];
  let current = '';
  for (const part of normalizePath(path).split('/').filter(Boolean)) {
    current += `/${part}`;
    chain.push(current);
  }
  return chain;
}

/** Case-insensitive substring filter over the entries of the current directory. */
export function filterEntries(entries: FileEntry[], query: string): FileEntry[] {
  const needle = query.trim().toLowerCase();
  if (!needle) return entries;
  return entries.filter((entry) => entry.name.toLowerCase().includes(needle));
}

const CODE_EXTENSIONS = new Set([
  'c', 'h', 'cc', 'cpp', 'cxx', 'hpp', 'hh', 'rs', 'go', 'java', 'kt', 'py', 'rb', 'php', 'pl', 'lua',
  'swift', 'cs', 'sh', 'bash', 'zsh', 'fish', 'js', 'jsx', 'ts', 'tsx', 'mjs', 'cjs', 'vue', 'svelte',
  'css', 'scss', 'less', 'html', 'htm', 'xml', 'yaml', 'yml', 'toml', 'ini', 'conf', 'cfg', 'sql',
  'gradle', 'cmake', 'mk', 'proto', 'patch',
]);
const ARCHIVE_EXTENSIONS = new Set(['zip', 'tar', 'gz', 'tgz', 'bz2', 'xz', 'zst', '7z', 'rar', 'deb', 'rpm', 'apk', 'jar']);
const AUDIO_EXTENSIONS = new Set(['mp3', 'wav', 'flac', 'ogg', 'oga', 'm4a', 'aac', 'opus', 'wma', 'mid']);
const VIDEO_EXTENSIONS = new Set(['mp4', 'mkv', 'avi', 'mov', 'webm', 'wmv', 'flv', 'm4v', 'mpg', 'mpeg']);
const SHEET_EXTENSIONS = new Set(['csv', 'tsv', 'xls', 'xlsx', 'ods', 'numbers']);

function fileExtension(name: string): string {
  const dot = name.lastIndexOf('.');
  if (dot <= 0 || dot === name.length - 1) return '';
  return name.slice(dot + 1).toLowerCase();
}

/** Lucide icon for a listing entry; falls back to the generic file/ folder pair. */
export function entryIcon(entry: FileEntry): LucideIcon {
  if (entry.type === 'dir') return Folder;
  if (entry.type === 'symlink') return Link2;
  if (entry.mime.startsWith('image/')) return FileImage;
  const extension = fileExtension(entry.name);
  if (ARCHIVE_EXTENSIONS.has(extension)) return FileArchive;
  if (AUDIO_EXTENSIONS.has(extension)) return FileAudio;
  if (VIDEO_EXTENSIONS.has(extension)) return FileVideo;
  if (SHEET_EXTENSIONS.has(extension)) return FileSpreadsheet;
  if (!entry.text_editable) return File;
  if (CODE_EXTENSIONS.has(extension)) return FileCode;
  return FileText;
}

export function entryKindLabel(entry: FileEntry): string {
  if (entry.type === 'dir') return '目录';
  if (entry.type === 'symlink') return '符号链接';
  if (entry.mime.startsWith('image/')) return '图片';
  const extension = fileExtension(entry.name);
  if (ARCHIVE_EXTENSIONS.has(extension)) return '压缩包';
  if (AUDIO_EXTENSIONS.has(extension)) return '音频';
  if (VIDEO_EXTENSIONS.has(extension)) return '视频';
  if (SHEET_EXTENSIONS.has(extension)) return '表格';
  if (entry.text_editable) return CODE_EXTENSIONS.has(extension) ? '代码' : '文本';
  return '文件';
}

export function entryMeta(entry: FileEntry): string {
  const time = formatTime(entry.mtime);
  if (entry.type === 'dir') return time;
  if (entry.type === 'symlink') return `→ ${entry.link_target ?? '未知目标'}`;
  return `${formatBytes(entry.size)} · ${time}`;
}

/** Open behaviour: text opens the editor, everything else previews. */
export function opensInEditor(entry: FileEntry): boolean {
  return entry.type === 'file' && entry.text_editable;
}

/**
 * Client-side guard for browser-provided file names before they are joined to
 * the current path; the backend sandbox remains the authority.
 */
export function isSafeUploadName(name: string): boolean {
  if (!name || name === '.' || name === '..') return false;
  return !name.includes('/') && !name.includes('\\') && !name.includes('\0');
}

export function clampTreeWidth(value: number): number {
  if (!Number.isFinite(value)) return TREE_WIDTH_DEFAULT;
  return Math.min(TREE_WIDTH_MAX, Math.max(TREE_WIDTH_MIN, Math.round(value)));
}

function readStored(key: string): string | null {
  try {
    if (typeof window === 'undefined' || !window.localStorage) return null;
    return window.localStorage.getItem(key);
  } catch {
    return null;
  }
}

function writeStored(key: string, value: string): void {
  try {
    if (typeof window === 'undefined' || !window.localStorage) return;
    window.localStorage.setItem(key, value);
  } catch {
    /* preference persistence is best-effort */
  }
}

export function readViewPreference(): ViewMode {
  return readStored(VIEW_STORAGE_KEY) === 'grid' ? 'grid' : 'list';
}

export function writeViewPreference(view: ViewMode): void {
  writeStored(VIEW_STORAGE_KEY, view);
}

export function readTreeWidthPreference(): number {
  const stored = readStored(TREE_WIDTH_STORAGE_KEY);
  return stored ? clampTreeWidth(Number(stored)) : TREE_WIDTH_DEFAULT;
}

export function writeTreeWidthPreference(width: number): void {
  writeStored(TREE_WIDTH_STORAGE_KEY, String(clampTreeWidth(width)));
}
