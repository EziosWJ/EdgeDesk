import { formatBytes, formatTime } from '../../services/api';
import type { FileEntry } from '../../types';

export type SortKey = 'name' | 'size' | 'mtime' | 'type';
export type ViewMode = 'list' | 'grid';

/** Kinds resolved from file name/mime; components live in `FileTypeIcon.tsx`. */
export type FileIconKey =
  | 'go' | 'c' | 'cpp' | 'rust' | 'python' | 'java' | 'kotlin' | 'scala' | 'groovy'
  | 'javascript' | 'typescript' | 'reactjs' | 'reactts' | 'vue' | 'svelte'
  | 'ruby' | 'php' | 'swift' | 'dartlang' | 'lua' | 'perl' | 'r' | 'julia' | 'haskell'
  | 'elixir' | 'erlang' | 'clojure' | 'fsharp' | 'nim' | 'zig' | 'fortran' | 'assembly' | 'vb'
  | 'sql' | 'sqlite' | 'graphql' | 'protobuf' | 'solidity' | 'jupyter'
  | 'shell' | 'powershell' | 'bat' | 'markdown' | 'html' | 'css' | 'scss' | 'less' | 'sass'
  | 'xml' | 'json' | 'yaml' | 'toml' | 'ini' | 'config' | 'dotenv'
  | 'text' | 'log' | 'pdf' | 'word' | 'excel' | 'csv' | 'powerpoint'
  | 'image' | 'audio' | 'video' | 'zip' | 'font' | 'binary'
  | 'docker' | 'makefile' | 'cmake' | 'gradle' | 'maven' | 'license' | 'git'
  | 'npm' | 'yarn' | 'pnpm' | 'eslint' | 'prettier' | 'editorconfig' | 'tsconfig'
  | 'vite' | 'vitest' | 'terraform'
  | 'default';

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

function fileExtension(name: string): string {
  const dot = name.lastIndexOf('.');
  if (dot <= 0 || dot === name.length - 1) return '';
  return name.slice(dot + 1).toLowerCase();
}

const EXTENSION_ICONS: Record<string, FileIconKey> = {
  go: 'go',
  c: 'c',
  h: 'c',
  cc: 'cpp', cpp: 'cpp', cxx: 'cpp', hpp: 'cpp', hh: 'cpp', hxx: 'cpp', ino: 'cpp',
  rs: 'rust',
  py: 'python', pyw: 'python', pyi: 'python',
  java: 'java',
  kt: 'kotlin', kts: 'kotlin',
  scala: 'scala', sc: 'scala',
  groovy: 'groovy', gvy: 'groovy',
  js: 'javascript', mjs: 'javascript', cjs: 'javascript',
  ts: 'typescript', mts: 'typescript', cts: 'typescript',
  jsx: 'reactjs',
  tsx: 'reactts',
  vue: 'vue',
  svelte: 'svelte',
  rb: 'ruby', erb: 'ruby',
  php: 'php',
  swift: 'swift',
  dart: 'dartlang',
  lua: 'lua',
  pl: 'perl', pm: 'perl',
  r: 'r',
  jl: 'julia',
  hs: 'haskell',
  ex: 'elixir', exs: 'elixir',
  erl: 'erlang', hrl: 'erlang',
  clj: 'clojure', cljs: 'clojure', cljc: 'clojure', edn: 'clojure',
  fs: 'fsharp', fsx: 'fsharp',
  nim: 'nim', nims: 'nim',
  zig: 'zig',
  f: 'fortran', f90: 'fortran', f95: 'fortran', for: 'fortran',
  asm: 'assembly', s: 'assembly',
  vb: 'vb', vbs: 'vb',
  sql: 'sql',
  db: 'sqlite', sqlite: 'sqlite', sqlite3: 'sqlite',
  graphql: 'graphql', gql: 'graphql',
  proto: 'protobuf',
  sol: 'solidity',
  ipynb: 'jupyter',
  tf: 'terraform', tfvars: 'terraform',
  sh: 'shell', bash: 'shell', zsh: 'shell', fish: 'shell', ksh: 'shell',
  ps1: 'powershell', psm1: 'powershell', psd1: 'powershell',
  bat: 'bat', cmd: 'bat',
  md: 'markdown', markdown: 'markdown', mdx: 'markdown',
  html: 'html', htm: 'html',
  css: 'css',
  scss: 'scss',
  less: 'less',
  sass: 'sass',
  xml: 'xml', xsl: 'xml', xslt: 'xml', plist: 'xml',
  json: 'json', jsonc: 'json', json5: 'json',
  yaml: 'yaml', yml: 'yaml',
  toml: 'toml',
  ini: 'ini', properties: 'ini',
  conf: 'config', cfg: 'config',
  env: 'dotenv',
  txt: 'text',
  log: 'log',
  pdf: 'pdf',
  doc: 'word', docx: 'word', odt: 'word', rtf: 'word',
  xls: 'excel', xlsx: 'excel', ods: 'excel',
  csv: 'csv', tsv: 'csv',
  ppt: 'powerpoint', pptx: 'powerpoint', odp: 'powerpoint',
  png: 'image', jpg: 'image', jpeg: 'image', gif: 'image', svg: 'image', webp: 'image',
  bmp: 'image', ico: 'image', avif: 'image', tif: 'image', tiff: 'image',
  mp3: 'audio', wav: 'audio', flac: 'audio', ogg: 'audio', oga: 'audio', m4a: 'audio',
  aac: 'audio', opus: 'audio', wma: 'audio', mid: 'audio', midi: 'audio',
  mp4: 'video', mkv: 'video', avi: 'video', mov: 'video', webm: 'video', wmv: 'video',
  flv: 'video', m4v: 'video', mpg: 'video', mpeg: 'video',
  zip: 'zip', tar: 'zip', gz: 'zip', tgz: 'zip', bz2: 'zip', xz: 'zip', zst: 'zip',
  '7z': 'zip', rar: 'zip', deb: 'zip', rpm: 'zip', apk: 'zip', jar: 'zip', war: 'zip',
  ttf: 'font', otf: 'font', woff: 'font', woff2: 'font', eot: 'font',
  bin: 'binary', exe: 'binary', dll: 'binary', so: 'binary', o: 'binary', a: 'binary',
  iso: 'binary', img: 'binary', dmg: 'binary', class: 'binary', pyc: 'binary',
};

/** Exact file names (lower-cased) that beat the extension mapping. */
const FILENAME_ICONS: Record<string, FileIconKey> = {
  dockerfile: 'docker',
  makefile: 'makefile',
  gnumakefile: 'makefile',
  kbuild: 'makefile',
  'cmakelists.txt': 'cmake',
  license: 'license',
  'license.md': 'license',
  'license.txt': 'license',
  licence: 'license',
  copying: 'license',
  notice: 'license',
  'package.json': 'npm',
  'package-lock.json': 'npm',
  'npm-shrinkwrap.json': 'npm',
  '.npmrc': 'npm',
  'yarn.lock': 'yarn',
  '.yarnrc': 'yarn',
  '.yarnrc.yml': 'yarn',
  'pnpm-lock.yaml': 'pnpm',
  'pnpm-workspace.yaml': 'pnpm',
  '.editorconfig': 'editorconfig',
  'tsconfig.json': 'tsconfig',
  'go.mod': 'go',
  'go.sum': 'go',
  'cargo.toml': 'rust',
  'cargo.lock': 'rust',
  'pom.xml': 'maven',
  'build.gradle': 'gradle',
  'build.gradle.kts': 'gradle',
  'settings.gradle': 'gradle',
  'settings.gradle.kts': 'gradle',
  'gradle.properties': 'gradle',
  'requirements.txt': 'python',
  'pyproject.toml': 'python',
  pipfile: 'python',
  'pipfile.lock': 'python',
  '.gitignore': 'git',
  '.gitattributes': 'git',
  '.gitmodules': 'git',
  '.gitconfig': 'git',
  '.gitkeep': 'git',
  'docker-compose.yml': 'docker',
  'docker-compose.yaml': 'docker',
};

function specialFilenameIcon(name: string): FileIconKey | null {
  const lower = name.toLowerCase();
  const exact = FILENAME_ICONS[lower];
  if (exact) return exact;
  if (lower.startsWith('dockerfile')) return 'docker';
  if (lower.startsWith('.env')) return 'dotenv';
  if (lower.startsWith('tsconfig.')) return 'tsconfig';
  if (lower.startsWith('vite.config.') || lower.startsWith('vite.workspace.')) return 'vite';
  if (lower.startsWith('vitest.')) return 'vitest';
  if (lower.startsWith('.eslintrc') || lower.startsWith('eslint.config.')) return 'eslint';
  if (lower.startsWith('.prettierrc') || lower.startsWith('prettier.config.')) return 'prettier';
  return null;
}

/**
 * Icon kind for a regular file entry, resolved from special names, mime and
 * extension. Unknown editable text falls back to `text`, everything else to
 * the neutral `default` file icon.
 */
export function fileIconKey(entry: FileEntry): FileIconKey {
  const special = specialFilenameIcon(entry.name);
  if (special) return special;
  if (entry.mime.startsWith('image/')) return 'image';
  if (entry.mime.startsWith('audio/')) return 'audio';
  if (entry.mime.startsWith('video/')) return 'video';
  if (entry.mime === 'application/pdf') return 'pdf';
  const mapped = EXTENSION_ICONS[fileExtension(entry.name)];
  if (mapped) return mapped;
  return entry.text_editable ? 'text' : 'default';
}

const CODE_ICON_KEYS = new Set<FileIconKey>([
  'go', 'c', 'cpp', 'rust', 'python', 'java', 'kotlin', 'scala', 'groovy',
  'javascript', 'typescript', 'reactjs', 'reactts', 'vue', 'svelte', 'ruby', 'php', 'swift',
  'dartlang', 'lua', 'perl', 'r', 'julia', 'haskell', 'elixir', 'erlang', 'clojure', 'fsharp',
  'nim', 'zig', 'fortran', 'assembly', 'vb', 'sql', 'sqlite', 'graphql', 'protobuf', 'solidity',
  'jupyter',
]);
const CONFIG_ICON_KEYS = new Set<FileIconKey>([
  'json', 'yaml', 'toml', 'xml', 'ini', 'config', 'dotenv', 'git', 'npm', 'yarn', 'pnpm',
  'eslint', 'prettier', 'editorconfig', 'tsconfig', 'vite', 'vitest', 'terraform',
]);
const BUILD_ICON_KEYS = new Set<FileIconKey>(['docker', 'makefile', 'cmake', 'gradle', 'maven']);
const SCRIPT_ICON_KEYS = new Set<FileIconKey>(['shell', 'powershell', 'bat']);
const MEDIA_KIND_LABELS: Partial<Record<FileIconKey, string>> = {
  image: '图片',
  audio: '音频',
  video: '视频',
  pdf: 'PDF',
  word: '文档',
  excel: '表格',
  csv: '表格',
  powerpoint: '演示文稿',
  zip: '压缩包',
  font: '字体',
  binary: '二进制',
};

export function entryKindLabel(entry: FileEntry): string {
  if (entry.type === 'dir') return '目录';
  if (entry.type === 'symlink') return '符号链接';
  const key = fileIconKey(entry);
  if (CODE_ICON_KEYS.has(key)) return '代码';
  if (SCRIPT_ICON_KEYS.has(key)) return '脚本';
  if (BUILD_ICON_KEYS.has(key)) return '构建文件';
  if (CONFIG_ICON_KEYS.has(key)) return '配置';
  if (key === 'markdown' || key === 'text' || key === 'license') return '文本';
  if (key === 'log') return '日志';
  return MEDIA_KIND_LABELS[key] ?? '文件';
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
