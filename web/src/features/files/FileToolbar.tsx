import {
  ArrowDown,
  ArrowUp,
  Download,
  LayoutGrid,
  List,
  PenLine,
  RefreshCw,
  SquareTerminal,
  Trash2,
  Upload,
  X,
} from 'lucide-react';
import { Icon } from '../../components/Icon';
import { api } from '../../services/api';
import type { FileEntry } from '../../types';
import type { SortKey, ViewMode } from './fileUtils';

/** Selection mode: the action row is replaced in place, never a new panel. */
export type ToolbarSelection = {
  count: number;
  /** Set when exactly one entry is selected (rename always, download for files). */
  single: FileEntry | null;
  onRename: (entry: FileEntry) => void;
  onDelete: () => void;
  onClear: () => void;
};

type Props = {
  path: string;
  breadcrumbs: { label: string; target: string }[];
  onNavigate: (path: string) => void;
  onUp: () => void;
  filter: string;
  onFilterChange: (value: string) => void;
  view: ViewMode;
  onViewChange: (view: ViewMode) => void;
  sort: SortKey;
  desc: boolean;
  onSortChange: (key: SortKey) => void;
  onToggleDesc: () => void;
  showHidden: boolean;
  onToggleHidden: () => void;
  loading: boolean;
  onRefresh: () => void;
  selection: ToolbarSelection | null;
  onNewFolder: () => void;
  onUpload: () => void;
  onOpenTerminal: () => void;
};

export function FileToolbar({
  path,
  breadcrumbs,
  onNavigate,
  onUp,
  filter,
  onFilterChange,
  view,
  onViewChange,
  sort,
  desc,
  onSortChange,
  onToggleDesc,
  showHidden,
  onToggleHidden,
  loading,
  onRefresh,
  selection,
  onNewFolder,
  onUpload,
  onOpenTerminal,
}: Props) {
  return (
    <div className="ed-files-toolbar ed-panel">
      <div className="ed-breadcrumb-row">
        <button
          type="button"
          className="ed-button small"
          onClick={onUp}
          disabled={path === '/'}
          title="上级目录"
        >
          <Icon icon={ArrowUp} size={14} />
          上级
        </button>
        <nav className="ed-breadcrumbs" aria-label="当前路径">
          {breadcrumbs.map((crumb, index) => (
            <span key={crumb.target}>
              {index > 0 && <span className="ed-crumb-sep">/</span>}
              <button
                type="button"
                onClick={() => onNavigate(crumb.target)}
                aria-current={crumb.target === path ? 'page' : undefined}
              >
                {crumb.label}
              </button>
            </span>
          ))}
        </nav>
        <label className="ed-filter">
          <span className="ed-visually-hidden">在当前目录筛选文件名</span>
          <input
            type="search"
            value={filter}
            placeholder="在当前目录筛选文件名…"
            aria-label="在当前目录筛选文件名"
            onChange={(event) => onFilterChange(event.target.value)}
          />
        </label>
        <div className="ed-viewswitch" role="group" aria-label="视图类型">
          <button
            type="button"
            aria-pressed={view === 'list'}
            title="列表视图"
            aria-label="列表视图"
            onClick={() => onViewChange('list')}
          >
            <Icon icon={List} size={16} />
          </button>
          <button
            type="button"
            aria-pressed={view === 'grid'}
            title="网格视图"
            aria-label="网格视图"
            onClick={() => onViewChange('grid')}
          >
            <Icon icon={LayoutGrid} size={16} />
          </button>
        </div>
        <button
          type="button"
          className="ed-icon-button"
          title="刷新"
          aria-label="刷新"
          onClick={onRefresh}
          disabled={loading}
        >
          <Icon icon={RefreshCw} size={16} />
        </button>
      </div>

      <div className="ed-action-row">
        {selection ? (
          <>
            <span className="ed-selection-count" role="status">
              已选 {selection.count} 项
            </span>
            {selection.single?.type === 'file' && (
              <a className="ed-button" href={api.downloadUrl(selection.single.path)}>
                <Icon icon={Download} size={15} />
                下载
              </a>
            )}
            {selection.single && (
              <button type="button" className="ed-button" onClick={() => selection.onRename(selection.single!)}>
                <Icon icon={PenLine} size={15} />
                重命名
              </button>
            )}
            <button type="button" className="ed-button danger" onClick={selection.onDelete}>
              <Icon icon={Trash2} size={15} />
              删除
            </button>
            <button type="button" className="ed-button" onClick={selection.onClear}>
              <Icon icon={X} size={15} />
              取消选择
            </button>
          </>
        ) : (
          <>
            <button type="button" className="ed-button primary" onClick={onUpload}>
              <Icon icon={Upload} size={15} />
              上传文件
            </button>
            <button type="button" className="ed-button" onClick={onNewFolder}>
              新建文件夹
            </button>
            <button
              type="button"
              className="ed-button"
              onClick={onOpenTerminal}
              title="在浮动窗口中打开 Bash"
            >
              <Icon icon={SquareTerminal} size={15} />
              在当前目录打开终端
            </button>
            {view === 'grid' && (
              <span className="ed-sort-control">
                <label htmlFor="ed-files-sort">排序</label>
                <select
                  id="ed-files-sort"
                  value={sort}
                  onChange={(event) => onSortChange(event.target.value as SortKey)}
                >
                  <option value="name">名称</option>
                  <option value="size">大小</option>
                  <option value="mtime">修改时间</option>
                </select>
                <button
                  type="button"
                  className="ed-icon-button"
                  onClick={onToggleDesc}
                  title={desc ? '降序' : '升序'}
                >
                  <Icon icon={desc ? ArrowDown : ArrowUp} size={14} />
                </button>
              </span>
            )}
          </>
        )}
        <label className="ed-toggle" title="同时作用于目录树和文件列表">
          <input type="checkbox" checked={showHidden} onChange={onToggleHidden} />
          显示隐藏文件
        </label>
      </div>
    </div>
  );
}
