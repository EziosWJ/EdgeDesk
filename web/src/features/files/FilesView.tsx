import { useEffect, useMemo, useRef, useState } from 'react';
import { PromptDialog } from '../../components/PromptDialog';
import { api, formatBytes, formatTime } from '../../services/api';
import { windowStore } from '../../stores/windowStore';
import type { FileEntry } from '../../types';

const asset = (path: string) => `${import.meta.env.BASE_URL}assets/${path}`;

type SortKey = 'name' | 'size' | 'mtime' | 'type';

function joinPath(parent: string, name: string): string {
  if (parent === '/') return `/${name}`;
  return `${parent}/${name}`;
}

function parentPath(path: string): string {
  if (path === '/') return '/';
  const index = path.lastIndexOf('/');
  return index <= 0 ? '/' : path.slice(0, index);
}

function iconFor(entry: FileEntry): string {
  if (entry.type === 'dir') return 'files';
  if (entry.type === 'symlink') return 'sessions';
  if (entry.mime.startsWith('image/')) return 'dashboard';
  if (entry.text_editable) return 'edit';
  return 'download';
}

export function FilesView({ initialPath = '/' }: { initialPath?: string }) {
  const [path, setPath] = useState(initialPath);
  const [entries, setEntries] = useState<FileEntry[]>([]);
  const [total, setTotal] = useState(0);
  const [page, setPage] = useState(1);
  const [pageSize] = useState(200);
  const [sort, setSort] = useState<SortKey>('name');
  const [desc, setDesc] = useState(false);
  const [showHidden, setShowHidden] = useState(true);
  const [selected, setSelected] = useState<Set<string>>(new Set());
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [notice, setNotice] = useState<string | null>(null);
  const [uploadPercent, setUploadPercent] = useState<number | null>(null);
  const [dialog, setDialog] = useState<{ type: 'mkdir' | 'rename'; target?: FileEntry } | null>(null);
  const uploadRef = useRef<HTMLInputElement | null>(null);
  const fileInputRef = useRef<HTMLInputElement | null>(null);

  const load = async (target = path, options: { page?: number; sort?: SortKey; desc?: boolean; hidden?: boolean } = {}) => {
    setLoading(true);
    setError(null);
    try {
      const data = await api.listFiles(target, {
        page: options.page ?? page,
        pageSize,
        sort: options.sort ?? sort,
        desc: options.desc ?? desc,
        hidden: options.hidden ?? showHidden,
      });
      setEntries(data.entries);
      setTotal(data.total);
      setPath(data.path);
      setPage(data.page);
    } catch (caught) {
      setError(caught instanceof Error ? caught.message : String(caught));
      setEntries([]);
    } finally {
      setLoading(false);
    }
  };

  useEffect(() => {
    void load(path);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  const breadcrumbs = useMemo(() => {
    const parts = path.split('/').filter(Boolean);
    const crumbs = [{ label: '/', target: '/' }];
    let current = '';
    parts.forEach((part) => {
      current += `/${part}`;
      crumbs.push({ label: part, target: current });
    });
    return crumbs;
  }, [path]);

  const toggleSort = (key: SortKey) => {
    const nextDesc = sort === key ? !desc : false;
    setSort(key);
    setDesc(nextDesc);
    void load(path, { sort: key, desc: nextDesc, page: 1 });
  };

  const openEntry = (entry: FileEntry) => {
    if (entry.type === 'dir') {
      setSelected(new Set());
      void load(entry.path, { page: 1 });
    } else if (entry.type === 'symlink') {
      setNotice(`符号链接 → ${entry.link_target ?? '未知目标'}（出于安全策略默认不跟随）`);
    }
  };

  const openTerminalHere = async () => {
    try {
      const data = await api.createTerminal({ cwd: path === '/' ? '/' : path, cols: 100, rows: 30 });
      windowStore.open({
        kind: 'terminal',
        title: `${data.session.title} · ${path}`,
        payload: { sessionId: data.session.id },
      });
    } catch (caught) {
      setError(caught instanceof Error ? caught.message : String(caught));
    }
  };

  const handleUpload = async (files: FileList | null) => {
    if (!files || files.length === 0) return;
    setUploadPercent(0);
    setError(null);
    try {
      for (const file of Array.from(files)) {
        const target = joinPath(path, file.name);
        await api.upload(target, file, (percent) => setUploadPercent(percent));
        setNotice(`已上传 ${file.name}（${formatBytes(file.size)}，原子替换完成）`);
      }
      void load(path);
    } catch (caught) {
      setError(caught instanceof Error ? caught.message : String(caught));
    } finally {
      setUploadPercent(null);
      if (uploadRef.current) uploadRef.current.value = '';
    }
  };

  const handleRename = async (entry: FileEntry, newName: string) => {
    const target = joinPath(parentPath(entry.path), newName);
    try {
      await api.rename(entry.path, target, false);
      setNotice(`已重命名为 ${newName}`);
      void load(path);
    } catch (caught) {
      setError(caught instanceof Error ? caught.message : String(caught));
    }
  };

  const handleMkdir = async (name: string) => {
    try {
      await api.mkdir(joinPath(path, name));
      setNotice(`已创建目录 ${name}`);
      void load(path);
    } catch (caught) {
      setError(caught instanceof Error ? caught.message : String(caught));
    }
  };

  const handleDelete = async (paths: string[]) => {
    if (paths.length === 0) return;
    if (!window.confirm(`确定删除 ${paths.length} 个条目？目录将递归删除，操作不可撤销。\n\n${paths.slice(0, 5).join('\n')}${paths.length > 5 ? '\n…' : ''}`)) {
      return;
    }
    try {
      const result = await api.remove(paths);
      setNotice(`已删除 ${result.removed_files} 个文件、${result.removed_dirs} 个目录`);
      setSelected(new Set());
      void load(path);
    } catch (caught) {
      setError(caught instanceof Error ? caught.message : String(caught));
    }
  };

  const selectedEntries = entries.filter((entry) => selected.has(entry.path));
  const currentPageCount = Math.max(1, Math.ceil(total / pageSize));

  return (
    <div className="ed-view ed-files-view">
      <div className="ed-view-header">
        <div>
          <h1>文件管理</h1>
          <p>浏览、上传、下载、编辑与删除都经过统一的路径沙箱；文本保存为原子替换并保留原权限。</p>
        </div>
        <div className="ed-toolbar">
          <button type="button" className="ed-button" onClick={() => void load(path)} disabled={loading}>
            刷新
          </button>
          <button type="button" className="ed-button" onClick={() => setDialog({ type: 'mkdir' })}>
            新建目录
          </button>
          <button type="button" className="ed-button" onClick={() => fileInputRef.current?.click()}>
            上传文件
          </button>
          <button type="button" className="ed-button" onClick={() => void openTerminalHere()}>
            在此打开终端
          </button>
        </div>
      </div>

      <div className="ed-pathbar ed-panel">
        <button
          type="button"
          className="ed-button small"
          onClick={() => void load(parentPath(path), { page: 1 })}
          disabled={path === '/'}
        >
          ↑ 上级
        </button>
        <nav className="ed-breadcrumbs" aria-label="路径">
          {breadcrumbs.map((crumb, index) => (
            <span key={crumb.target}>
              {index > 0 && <span className="ed-crumb-sep">/</span>}
              <button type="button" onClick={() => void load(crumb.target, { page: 1 })}>
                {crumb.label}
              </button>
            </span>
          ))}
        </nav>
        <label className="ed-toggle">
          <input
            type="checkbox"
            checked={!showHidden}
            onChange={(event) => {
              setShowHidden(!event.target.checked);
              void load(path, { hidden: !event.target.checked, page: 1 });
            }}
          />
          隐藏隐藏文件
        </label>
      </div>

      {error && <div className="ed-alert danger">{error}</div>}
      {notice && <div className="ed-alert info" onClick={() => setNotice(null)}>{notice}</div>}
      {uploadPercent !== null && (
        <div className="ed-alert info">
          正在上传… <progress max={100} value={uploadPercent} /> {uploadPercent}%
        </div>
      )}

      <div className="ed-table-wrap ed-panel">
        <table className="ed-table">
          <thead>
            <tr>
              <th className="ed-col-check">
                <input
                  type="checkbox"
                  checked={entries.length > 0 && selected.size === entries.length}
                  onChange={(event) => {
                    setSelected(event.target.checked ? new Set(entries.map((entry) => entry.path)) : new Set());
                  }}
                />
              </th>
              <th onClick={() => toggleSort('name')} className="sortable">
                名称 {sort === 'name' ? (desc ? '▼' : '▲') : ''}
              </th>
              <th onClick={() => toggleSort('size')} className="sortable ed-col-size">
                大小 {sort === 'size' ? (desc ? '▼' : '▲') : ''}
              </th>
              <th onClick={() => toggleSort('mtime')} className="sortable ed-col-time">
                修改时间 {sort === 'mtime' ? (desc ? '▼' : '▲') : ''}
              </th>
              <th className="ed-col-perm">权限</th>
              <th className="ed-col-owner">uid/gid</th>
              <th className="ed-col-actions">操作</th>
            </tr>
          </thead>
          <tbody>
            {entries.map((entry) => (
              <tr key={entry.path} className={selected.has(entry.path) ? 'selected' : ''}>
                <td>
                  <input
                    type="checkbox"
                    checked={selected.has(entry.path)}
                    onChange={(event) => {
                      const next = new Set(selected);
                      if (event.target.checked) next.add(entry.path);
                      else next.delete(entry.path);
                      setSelected(next);
                    }}
                  />
                </td>
                <td>
                  <button type="button" className="ed-entry" onDoubleClick={() => openEntry(entry)} onClick={() => openEntry(entry)}>
                    <img src={asset(`icons/${iconFor(entry)}.svg`)} alt="" width={16} height={16} />
                    <span className={entry.hidden ? 'hidden-name' : ''}>{entry.name}</span>
                    {entry.type === 'symlink' && <span className="ed-badge">链接</span>}
                    {entry.type === 'dir' && <span className="ed-badge subtle">目录</span>}
                  </button>
                </td>
                <td className="ed-col-size">{entry.type === 'dir' ? '—' : formatBytes(entry.size)}</td>
                <td className="ed-col-time">{formatTime(entry.mtime)}</td>
                <td className="ed-col-perm">
                  <code>{entry.permissions}</code>
                </td>
                <td className="ed-col-owner">
                  <code>
                    {entry.uid}:{entry.gid}
                  </code>
                </td>
                <td className="ed-col-actions">
                  {entry.type === 'file' && (
                    <>
                      <a className="ed-link" href={api.downloadUrl(entry.path)}>
                        下载
                      </a>
                      {entry.text_editable && (
                        <button
                          type="button"
                          className="ed-link"
                          onClick={() =>
                            windowStore.open({ kind: 'editor', title: `编辑 ${entry.name}`, payload: { path: entry.path } })
                          }
                        >
                          编辑
                        </button>
                      )}
                      <button
                        type="button"
                        className="ed-link"
                        onClick={() =>
                          windowStore.open({ kind: 'preview', title: `预览 ${entry.name}`, payload: { path: entry.path } })
                        }
                      >
                        预览
                      </button>
                    </>
                  )}
                  <button type="button" className="ed-link" onClick={() => setDialog({ type: 'rename', target: entry })}>
                    重命名
                  </button>
                  <button type="button" className="ed-link danger" onClick={() => void handleDelete([entry.path])}>
                    删除
                  </button>
                </td>
              </tr>
            ))}
            {entries.length === 0 && !loading && (
              <tr>
                <td colSpan={7} className="ed-table-empty">
                  <img src={asset('illustrations/empty-files.svg')} alt="" />
                  <span>当前目录没有内容</span>
                </td>
              </tr>
            )}
          </tbody>
        </table>
      </div>

      <div className="ed-table-footer">
        <span>
          共 {total} 项{selected.size > 0 ? `，已选 ${selected.size} 项` : ''}
        </span>
        <div className="ed-toolbar">
          <button
            type="button"
            className="ed-button danger"
            disabled={selected.size === 0}
            onClick={() => void handleDelete(selectedEntries.map((entry) => entry.path))}
          >
            批量删除
          </button>
          <button type="button" className="ed-button" disabled={page <= 1 || loading} onClick={() => void load(path, { page: page - 1 })}>
            上一页
          </button>
          <span>
            {page} / {currentPageCount}
          </span>
          <button
            type="button"
            className="ed-button"
            disabled={page >= currentPageCount || loading}
            onClick={() => void load(path, { page: page + 1 })}
          >
            下一页
          </button>
        </div>
      </div>

      <input
        ref={fileInputRef}
        type="file"
        multiple
        hidden
        onChange={(event) => void handleUpload(event.target.files)}
      />
      <input ref={uploadRef} type="file" hidden />

      {dialog?.type === 'mkdir' && (
        <PromptDialog
          title="新建目录"
          label={`在 ${path} 下创建`}
          initial="new-folder"
          confirmText="创建"
          onCancel={() => setDialog(null)}
          onConfirm={(value) => {
            setDialog(null);
            void handleMkdir(value);
          }}
        />
      )}
      {dialog?.type === 'rename' && dialog.target && (
        <PromptDialog
          title="重命名"
          label={dialog.target.path}
          initial={dialog.target.name}
          confirmText="重命名"
          onCancel={() => setDialog(null)}
          onConfirm={(value) => {
            const target = dialog.target;
            setDialog(null);
            if (target) void handleRename(target, value);
          }}
        />
      )}
    </div>
  );
}
