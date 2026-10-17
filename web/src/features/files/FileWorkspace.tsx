import {
  useCallback,
  useEffect,
  useMemo,
  useRef,
  useState,
  type CSSProperties,
  type DragEvent as ReactDragEvent,
  type PointerEvent as ReactPointerEvent,
} from 'react';
import { PanelLeftOpen, Upload } from 'lucide-react';
import { ConfirmDialog } from '../../components/ConfirmDialog';
import { Icon } from '../../components/Icon';
import { PromptDialog } from '../../components/PromptDialog';
import { api, formatBytes } from '../../services/api';
import { asset } from '../../services/assets';
import { preferencesStore, usePreferences } from '../../stores/preferencesStore';
import { windowStore } from '../../stores/windowStore';
import type { FileEntry } from '../../types';
import { DirectoryTree } from './DirectoryTree';
import { FileGridView } from './FileGridView';
import { FileListView } from './FileListView';
import { FileToolbar, type ToolbarSelection } from './FileToolbar';
import {
  breadcrumbsFor,
  clampTreeWidth,
  filterEntries,
  isSafeUploadName,
  joinPath,
  normalizePath,
  opensInEditor,
  parentPath,
  readTreeWidthPreference,
  readViewPreference,
  writeTreeWidthPreference,
  writeViewPreference,
  type SortKey,
  type ViewMode,
} from './fileUtils';
import { useDirectoryListing } from './useDirectoryListing';

type DialogState =
  | { type: 'mkdir' }
  | { type: 'rename'; entry: FileEntry }
  | { type: 'delete'; entries: FileEntry[] }
  | null;

/** Files collected from one drop, with the items that were refused. */
type DroppedFiles = {
  files: File[];
  skippedDirs: number;
  skippedInvalid: number;
};

/** Pending drag-and-drop upload shown in the confirmation dialog. */
type DropConfirm = DroppedFiles & { conflicts: string[] };

const EMPTY_ENTRIES: FileEntry[] = [];

function describeError(caught: unknown): string {
  return caught instanceof Error ? caught.message : String(caught);
}

function hasFilePayload(types: readonly string[] | undefined): boolean {
  return Array.from(types ?? []).includes('Files');
}

/**
 * Browser drag payloads only expose directory entries through
 * `webkitGetAsEntry`; when it is unavailable every file is treated as flat.
 */
function extractDroppedFiles(transfer: DataTransfer): DroppedFiles {
  const items = Array.from(transfer.items ?? []);
  const files: File[] = [];
  let skippedDirs = 0;
  let skippedInvalid = 0;
  if (items.length > 0 && typeof items[0].webkitGetAsEntry === 'function') {
    for (const item of items) {
      if (item.kind !== 'file') continue;
      const entry = item.webkitGetAsEntry();
      if (entry?.isDirectory) {
        skippedDirs += 1;
        continue;
      }
      const file = item.getAsFile();
      if (!file || !isSafeUploadName(file.name)) {
        skippedInvalid += 1;
        continue;
      }
      files.push(file);
    }
  } else {
    for (const file of Array.from(transfer.files ?? [])) {
      if (!isSafeUploadName(file.name)) {
        skippedInvalid += 1;
        continue;
      }
      files.push(file);
    }
  }
  return { files, skippedDirs, skippedInvalid };
}

function dropSummary(drop: DropConfirm): string {
  const parts = ['文件将流式写入当前目录，成功后原子替换。'];
  if (drop.conflicts.length > 0) parts.push(`${drop.conflicts.length} 个文件与当前页同名，将被覆盖。`);
  if (drop.skippedDirs > 0) parts.push(`已跳过 ${drop.skippedDirs} 个文件夹（暂不支持目录上传）。`);
  if (drop.skippedInvalid > 0) parts.push(`跳过 ${drop.skippedInvalid} 个名称不合法的文件。`);
  return parts.join(' ');
}

function dropDetails(drop: DropConfirm): string[] {
  const names = drop.files.map(
    (file) => `${file.name}（${formatBytes(file.size)}）${drop.conflicts.includes(file.name) ? ' · 将覆盖' : ''}`,
  );
  const details = names.slice(0, 5);
  if (names.length > 5) details.push(`…还有 ${names.length - 5} 个文件`);
  return details;
}

/**
 * File workspace: directory tree + breadcrumb + toolbar + list/grid views over
 * one shared listing and one shared selection. Selection is checkbox-only;
 * clicking an entry opens it. The toolbar swaps in selection actions in place
 * so the content below never shifts.
 */
export function FileWorkspace({ initialPath = '/' }: { initialPath?: string }) {
  const preferences = usePreferences();
  const [path, setPath] = useState(() => normalizePath(initialPath));
  const [showHidden, setShowHidden] = useState(preferences.files_show_hidden);
  const [view, setView] = useState<ViewMode>(() =>
    preferences.files_view === 'grid' || preferences.files_view === 'list'
      ? preferences.files_view
      : readViewPreference(),
  );
  const [sort, setSort] = useState<SortKey>('name');
  const [desc, setDesc] = useState(false);
  const [page, setPage] = useState(1);
  const [filter, setFilter] = useState('');
  const [selected, setSelected] = useState<Set<string>>(() => new Set());
  // Narrow layouts open without the drawer so it never covers the content;
  // desktop keeps the tree visible by default (its state is not persisted).
  const [treeVisible, setTreeVisible] = useState(
    () => typeof window.matchMedia !== 'function' || !window.matchMedia('(max-width: 980px)').matches,
  );
  const [treeWidth, setTreeWidth] = useState(readTreeWidthPreference);
  const [revision, setRevision] = useState(0);
  const [dialog, setDialog] = useState<DialogState>(null);
  const [dropConfirm, setDropConfirm] = useState<DropConfirm | null>(null);
  const [dropActive, setDropActive] = useState(false);
  const [notice, setNotice] = useState<string | null>(null);
  const [actionError, setActionError] = useState<string | null>(null);
  const [uploadPercent, setUploadPercent] = useState<number | null>(null);
  const [deleting, setDeleting] = useState(false);
  const fileInputRef = useRef<HTMLInputElement | null>(null);
  const resizeRef = useRef<{ startX: number; startWidth: number } | null>(null);
  const treeWidthRef = useRef(treeWidth);
  const dragDepthRef = useRef(0);

  useEffect(() => {
    setShowHidden(preferences.files_show_hidden);
  }, [preferences.files_show_hidden]);

  useEffect(() => {
    if (preferences.files_view === 'grid' || preferences.files_view === 'list') {
      setView(preferences.files_view);
    }
  }, [preferences.files_view]);

  const listing = useDirectoryListing(path, { showHidden, sort, desc, page });
  const { reload, loading: listingLoading } = listing;
  // Stale payloads belong to a previous directory; never mix them into the view.
  const data = listing.requestPath === path ? listing : null;
  const entries = data?.entries ?? EMPTY_ENTRIES;
  const loading = !data || listingLoading;
  const listingError = data?.error ?? null;

  const visibleEntries = useMemo(() => filterEntries(entries, filter), [entries, filter]);
  const breadcrumbs = useMemo(() => breadcrumbsFor(path), [path]);
  const selectedEntries = useMemo(
    () => entries.filter((entry) => selected.has(entry.path)),
    [entries, selected],
  );
  const pageCount = Math.max(1, Math.ceil((data?.total ?? 0) / (data?.pageSize ?? 200)));

  // Directory changes reset the selection; reloads prune entries that vanished.
  useEffect(() => {
    setSelected(new Set());
  }, [path]);
  useEffect(() => {
    setSelected((prev) => {
      if (prev.size === 0) return prev;
      const known = new Set(entries.map((entry) => entry.path));
      const next = new Set([...prev].filter((item) => known.has(item)));
      return next.size === prev.size ? prev : next;
    });
  }, [entries]);

  // Dropping files outside the workspace must not make the browser open them.
  useEffect(() => {
    const preventBrowserOpen = (event: DragEvent) => {
      if (hasFilePayload(event.dataTransfer?.types)) event.preventDefault();
    };
    window.addEventListener('dragover', preventBrowserOpen);
    window.addEventListener('drop', preventBrowserOpen);
    return () => {
      window.removeEventListener('dragover', preventBrowserOpen);
      window.removeEventListener('drop', preventBrowserOpen);
    };
  }, []);

  const navigate = useCallback(
    (target: string) => {
      const next = normalizePath(target);
      if (next === path) return; // clicking the active directory must not re-request it
      setPath(next);
      setPage(1);
      setFilter('');
      setActionError(null);
      setSelected(new Set());
    },
    [path],
  );

  const refresh = useCallback(() => {
    setActionError(null);
    reload();
    setRevision((value) => value + 1);
  }, [reload]);

  const openInEditor = (entry: FileEntry) => {
    windowStore.open({ kind: 'editor', title: `编辑 ${entry.name}`, payload: { path: entry.path } });
  };

  const openInPreview = (entry: FileEntry) => {
    windowStore.open({ kind: 'preview', title: `预览 ${entry.name}`, payload: { path: entry.path } });
  };

  const openEntry = (entry: FileEntry) => {
    if (entry.type === 'dir') {
      navigate(entry.path);
      return;
    }
    if (entry.type === 'symlink') {
      setNotice(`符号链接 → ${entry.link_target ?? '未知目标'}（出于安全策略默认不跟随）`);
      return;
    }
    if (opensInEditor(entry)) openInEditor(entry);
    else openInPreview(entry);
  };

  const openTerminalHere = async () => {
    setActionError(null);
    try {
      const data = await api.createTerminal({ cwd: path, cols: 100, rows: 30 });
      windowStore.open({ kind: 'terminal', title: `${path} · Bash`, payload: { sessionId: data.session.id } });
    } catch (caught) {
      setActionError(describeError(caught));
    }
  };

  const uploadFiles = async (files: File[]) => {
    if (files.length === 0) return;
    setUploadPercent(0);
    setActionError(null);
    setNotice(null);
    let uploaded = 0;
    let totalBytes = 0;
    try {
      for (const file of files) {
        await api.upload(joinPath(path, file.name), file, (percent) => setUploadPercent(percent));
        uploaded += 1;
        totalBytes += file.size;
      }
      setNotice(
        files.length === 1
          ? `已上传 ${files[0].name}（${formatBytes(files[0].size)}，原子替换完成）`
          : `已上传 ${files.length} 个文件（共 ${formatBytes(totalBytes)}，原子替换完成）`,
      );
      reload();
      setRevision((value) => value + 1);
    } catch (caught) {
      setNotice(null);
      const message = describeError(caught);
      setActionError(uploaded > 0 ? `${message}（前 ${uploaded} 个文件已上传成功）` : message);
    } finally {
      setUploadPercent(null);
      if (fileInputRef.current) fileInputRef.current.value = '';
    }
  };

  const handleUpload = (files: FileList | null) => {
    if (!files || files.length === 0) return;
    void uploadFiles(Array.from(files));
  };

  const onDragEnter = (event: ReactDragEvent<HTMLElement>) => {
    if (!hasFilePayload(event.dataTransfer?.types)) return;
    event.preventDefault();
    dragDepthRef.current += 1;
    setDropActive(true);
  };

  const onDragOver = (event: ReactDragEvent<HTMLElement>) => {
    if (!hasFilePayload(event.dataTransfer?.types)) return;
    event.preventDefault();
    if (event.dataTransfer) event.dataTransfer.dropEffect = 'copy';
  };

  const onDragLeave = (event: ReactDragEvent<HTMLElement>) => {
    if (!hasFilePayload(event.dataTransfer?.types)) return;
    dragDepthRef.current = Math.max(0, dragDepthRef.current - 1);
    if (dragDepthRef.current === 0) setDropActive(false);
  };

  const onDrop = (event: ReactDragEvent<HTMLElement>) => {
    const transfer = event.dataTransfer;
    if (!transfer || !hasFilePayload(transfer.types)) return;
    event.preventDefault();
    dragDepthRef.current = 0;
    setDropActive(false);
    if (uploadPercent !== null) {
      setNotice('正在上传，请等待当前任务完成后再拖入文件。');
      return;
    }
    const dropped = extractDroppedFiles(transfer);
    if (dropped.files.length === 0) {
      setNotice(
        dropped.skippedDirs > 0
          ? `未检测到可上传的文件（已跳过 ${dropped.skippedDirs} 个文件夹，暂不支持目录上传）。`
          : '未检测到可上传的文件。',
      );
      return;
    }
    const existing = new Set(entries.map((entry) => entry.name));
    const conflicts = dropped.files.map((file) => file.name).filter((name) => existing.has(name));
    setActionError(null);
    setNotice(null);
    setDropConfirm({ ...dropped, conflicts });
  };

  const handleMkdir = async (name: string) => {
    setActionError(null);
    try {
      await api.mkdir(joinPath(path, name));
      setNotice(`已创建目录 ${name}`);
      reload();
      setRevision((value) => value + 1);
    } catch (caught) {
      setActionError(describeError(caught));
    }
  };

  const handleRename = async (entry: FileEntry, newName: string) => {
    if (newName === entry.name) return;
    setActionError(null);
    try {
      await api.rename(entry.path, joinPath(parentPath(entry.path), newName), false);
      setNotice(`已重命名为 ${newName}`);
      reload();
      setRevision((value) => value + 1);
    } catch (caught) {
      setActionError(describeError(caught));
    }
  };

  const confirmDelete = async () => {
    if (!dialog || dialog.type !== 'delete') return;
    const targets = dialog.entries;
    setDeleting(true);
    setActionError(null);
    try {
      const result = await api.remove(targets.map((entry) => entry.path));
      setNotice(`已删除 ${result.removed_files} 个文件、${result.removed_dirs} 个目录`);
      setSelected(new Set());
      setDialog(null);
      reload();
      setRevision((value) => value + 1);
    } catch (caught) {
      setDialog(null);
      setActionError(describeError(caught));
    } finally {
      setDeleting(false);
    }
  };

  const toggleSort = useCallback(
    (key: SortKey) => {
      const nextDesc = sort === key ? !desc : false;
      setSort(key);
      setDesc(nextDesc);
      setPage(1);
    },
    [sort, desc],
  );

  const toggleEntry = (entry: FileEntry, checked: boolean) => {
    setSelected((prev) => {
      const next = new Set(prev);
      if (checked) next.add(entry.path);
      else next.delete(entry.path);
      return next;
    });
  };

  const toggleAll = (checked: boolean) => {
    setSelected(checked ? new Set(visibleEntries.map((entry) => entry.path)) : new Set());
  };

  const changeView = (next: ViewMode) => {
    setView(next);
    writeViewPreference(next);
    void preferencesStore.save({ files_view: next }).catch(() => undefined);
  };

  const onTreeResizeStart = (event: ReactPointerEvent<HTMLDivElement>) => {
    event.preventDefault();
    resizeRef.current = { startX: event.clientX, startWidth: treeWidth };
    event.currentTarget.setPointerCapture(event.pointerId);
  };
  const onTreeResizeMove = (event: ReactPointerEvent<HTMLDivElement>) => {
    const resize = resizeRef.current;
    if (!resize) return;
    const next = clampTreeWidth(resize.startWidth + (event.clientX - resize.startX));
    treeWidthRef.current = next;
    setTreeWidth(next);
  };
  const onTreeResizeEnd = () => {
    if (!resizeRef.current) return;
    resizeRef.current = null;
    writeTreeWidthPreference(treeWidthRef.current);
  };

  const selection: ToolbarSelection | null =
    selectedEntries.length > 0
      ? {
          count: selectedEntries.length,
          single: selectedEntries.length === 1 ? selectedEntries[0] : null,
          onRename: (entry) => setDialog({ type: 'rename', entry }),
          onDelete: () => setDialog({ type: 'delete', entries: selectedEntries }),
          onClear: () => setSelected(new Set()),
        }
      : null;

  const contentState: 'loading' | 'error' | 'empty' | 'filtered' | 'ready' = listingError
    ? 'error'
    : loading && entries.length === 0
      ? 'loading'
      : entries.length === 0
        ? 'empty'
        : visibleEntries.length === 0
          ? 'filtered'
          : 'ready';

  const listViewProps = {
    selected,
    sort,
    desc,
    loading,
    onToggleSort: toggleSort,
    onNavigateUp: () => navigate(parentPath(path)),
    onToggleAll: toggleAll,
    onToggleEntry: toggleEntry,
    onOpen: openEntry,
    onPreview: openInPreview,
    onEdit: openInEditor,
    onRename: (entry: FileEntry) => setDialog({ type: 'rename', entry }),
    onDelete: (entry: FileEntry) => setDialog({ type: 'delete', entries: [entry] }),
  };

  return (
    <div
      className={`ed-files-workspace${treeVisible ? '' : ' tree-collapsed'}`}
      style={{ '--ed-tree-width': `${treeWidth}px` } as CSSProperties}
    >
      {treeVisible && (
        <button
          type="button"
          className="ed-tree-backdrop"
          aria-label="关闭目录树"
          onClick={() => setTreeVisible(false)}
        />
      )}
      <aside className="ed-files-tree" aria-label="目录树面板">
        <DirectoryTree
          currentPath={path}
          showHidden={showHidden}
          revision={revision}
          onSelect={navigate}
          onCollapse={() => setTreeVisible(false)}
        />
        <div
          className="ed-tree-resizer"
          role="separator"
          aria-orientation="vertical"
          aria-label="调整目录树宽度"
          title="拖动调整目录树宽度"
          onPointerDown={onTreeResizeStart}
          onPointerMove={onTreeResizeMove}
          onPointerUp={onTreeResizeEnd}
        />
      </aside>
      {!treeVisible && (
        <button
          type="button"
          className="ed-files-tree-rail"
          onClick={() => setTreeVisible(true)}
          title="展开目录树"
          aria-label="展开目录树"
        >
          <Icon icon={PanelLeftOpen} size={16} />
        </button>
      )}

      <section
        className={`ed-files-main${dropActive ? ' drop-active' : ''}`}
        onDragEnter={onDragEnter}
        onDragOver={onDragOver}
        onDragLeave={onDragLeave}
        onDrop={onDrop}
      >
        {dropActive && (
          <div className="ed-drop-hint" aria-hidden="true">
            <Icon icon={Upload} size={28} />
            <strong>释放以上传到当前目录</strong>
            <span>{path}</span>
          </div>
        )}
        <FileToolbar
          path={path}
          breadcrumbs={breadcrumbs}
          onNavigate={navigate}
          onUp={() => navigate(parentPath(path))}
          filter={filter}
          onFilterChange={setFilter}
          view={view}
          onViewChange={changeView}
          sort={sort}
          desc={desc}
          onSortChange={toggleSort}
          onToggleDesc={() => setDesc((value) => !value)}
          showHidden={showHidden}
          onToggleHidden={() => {
            const next = !showHidden;
            setShowHidden(next);
            setPage(1);
            void preferencesStore.save({ files_show_hidden: next }).catch(() => undefined);
          }}
          loading={loading}
          onRefresh={refresh}
          selection={selection}
          onNewFolder={() => setDialog({ type: 'mkdir' })}
          onUpload={() => fileInputRef.current?.click()}
          onOpenTerminal={() => void openTerminalHere()}
        />

        {actionError && (
          <div className="ed-alert danger" role="alert">
            <span>{actionError}</span>
            <button type="button" className="ed-link" onClick={() => setActionError(null)}>
              关闭
            </button>
          </div>
        )}
        {contentState === 'error' && (
          <div className="ed-files-state" role="alert">
            <img src={asset('illustrations/monitor-unavailable.svg')} alt="" />
            <strong>目录加载失败</strong>
            <p>{listingError}</p>
            <div className="ed-toolbar">
              <button type="button" className="ed-button primary" onClick={refresh}>
                重试
              </button>
            </div>
          </div>
        )}
        {contentState === 'loading' && (
          <div className="ed-files-state" aria-busy="true" aria-live="polite">
            <span className="ed-spinner large" aria-hidden="true" />
            <span>正在加载 {path} …</span>
          </div>
        )}
        {contentState === 'empty' && (
          <div className="ed-files-state">
            <img src={asset('illustrations/empty-files.svg')} alt="" />
            <strong>当前目录为空</strong>
            <p>这里还没有文件或子目录，可以上传文件、拖入文件或新建文件夹。</p>
            <div className="ed-toolbar">
              <button type="button" className="ed-button primary" onClick={() => fileInputRef.current?.click()}>
                上传文件
              </button>
              <button type="button" className="ed-button" onClick={() => setDialog({ type: 'mkdir' })}>
                新建文件夹
              </button>
            </div>
          </div>
        )}
        {contentState === 'filtered' && (
          <div className="ed-files-state">
            <strong>没有匹配 “{filter.trim()}” 的条目</strong>
            <p>筛选只在当前目录的当前页内生效，不会跨目录搜索。</p>
            <button type="button" className="ed-button" onClick={() => setFilter('')}>
              清除筛选
            </button>
          </div>
        )}
        {contentState === 'ready' &&
          (view === 'list' ? (
            <FileListView {...listViewProps} path={path} entries={visibleEntries} />
          ) : (
            <FileGridView {...listViewProps} entries={visibleEntries} />
          ))}

        <div className="ed-table-footer">
          <div className="ed-files-status">
            {loading && entries.length > 0 && <span className="ed-spinner" aria-hidden="true" />}
            <span>
              {listingError ? '目录不可用' : `共 ${data?.total ?? 0} 项`}
              {filter.trim() ? ` · 已筛选 ${visibleEntries.length} / 当前页 ${entries.length} 项` : ''}
              {selected.size > 0 ? ` · 已选 ${selected.size} 项` : ''}
            </span>
            {notice && (
              <span className="ed-files-notice" role="status">
                {notice}
                <button type="button" className="ed-link" onClick={() => setNotice(null)}>
                  关闭
                </button>
              </span>
            )}
            {uploadPercent !== null && (
              <span className="ed-upload-progress" role="status">
                上传中
                <progress max={100} value={uploadPercent} />
                {uploadPercent}%
              </span>
            )}
          </div>
          <div className="ed-toolbar">
            <button
              type="button"
              className="ed-button"
              disabled={page <= 1 || loading}
              onClick={() => setPage((value) => Math.max(1, value - 1))}
            >
              上一页
            </button>
            <span>
              {page} / {pageCount}
            </span>
            <button
              type="button"
              className="ed-button"
              disabled={page >= pageCount || loading}
              onClick={() => setPage((value) => value + 1)}
            >
              下一页
            </button>
          </div>
        </div>
      </section>

      <input
        ref={fileInputRef}
        type="file"
        multiple
        hidden
        onChange={(event) => handleUpload(event.target.files)}
      />

      {dialog?.type === 'mkdir' && (
        <PromptDialog
          title="新建文件夹"
          label={`在 ${path} 下创建`}
          initial="新建文件夹"
          confirmText="创建"
          onCancel={() => setDialog(null)}
          onConfirm={(value) => {
            setDialog(null);
            void handleMkdir(value);
          }}
        />
      )}
      {dialog?.type === 'rename' && (
        <PromptDialog
          title="重命名"
          label={dialog.entry.path}
          initial={dialog.entry.name}
          confirmText="重命名"
          onCancel={() => setDialog(null)}
          onConfirm={(value) => {
            const entry = dialog.entry;
            setDialog(null);
            void handleRename(entry, value);
          }}
        />
      )}
      {dialog?.type === 'delete' && (
        <ConfirmDialog
          title={dialog.entries.length > 1 ? `删除 ${dialog.entries.length} 个条目` : `删除 ${dialog.entries[0]?.name}`}
          message="删除不可撤销；目录会连同其中的全部内容递归删除。"
          details={dialog.entries.slice(0, 5).map((entry) => entry.path).concat(dialog.entries.length > 5 ? ['…'] : [])}
          confirmText="删除"
          danger
          busy={deleting}
          onCancel={() => setDialog(null)}
          onConfirm={() => void confirmDelete()}
        />
      )}
      {dropConfirm && (
        <ConfirmDialog
          title={`上传 ${dropConfirm.files.length} 个文件到 ${path}`}
          message={dropSummary(dropConfirm)}
          details={dropDetails(dropConfirm)}
          confirmText="上传"
          onCancel={() => setDropConfirm(null)}
          onConfirm={() => {
            const files = dropConfirm.files;
            setDropConfirm(null);
            void uploadFiles(files);
          }}
        />
      )}
    </div>
  );
}
