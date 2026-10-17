import { iconStyle } from '../../services/assets';
import type { SortKey, ViewMode } from './fileUtils';

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
  selectionCount: number;
  downloadHref: string | null;
  onDeleteSelected: () => void;
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
  selectionCount,
  downloadHref,
  onDeleteSelected,
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
          ↑ 上级
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
            <span className="ed-icon-mask" style={iconStyle('list')} aria-hidden="true" />
          </button>
          <button
            type="button"
            aria-pressed={view === 'grid'}
            title="网格视图"
            aria-label="网格视图"
            onClick={() => onViewChange('grid')}
          >
            <span className="ed-icon-mask" style={iconStyle('grid')} aria-hidden="true" />
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
          <span className="ed-icon-mask" style={iconStyle('refresh')} aria-hidden="true" />
        </button>
      </div>

      <div className="ed-action-row">
        <button type="button" className="ed-button primary" onClick={onUpload}>
          <span className="ed-icon-mask" style={iconStyle('upload')} aria-hidden="true" />
          上传文件
        </button>
        <button type="button" className="ed-button" onClick={onNewFolder}>
          新建文件夹
        </button>
        {downloadHref ? (
          <a className="ed-button" href={downloadHref}>
            <span className="ed-icon-mask" style={iconStyle('download')} aria-hidden="true" />
            下载
          </a>
        ) : (
          <button type="button" className="ed-button" disabled title="请在列表中选择一个文件">
            下载
          </button>
        )}
        <button
          type="button"
          className="ed-button danger"
          disabled={selectionCount === 0}
          onClick={onDeleteSelected}
        >
          删除{selectionCount > 0 ? ` (${selectionCount})` : ''}
        </button>
        <button type="button" className="ed-button" onClick={onOpenTerminal} title="在浮动窗口中打开 Bash">
          <span className="ed-icon-mask" style={iconStyle('terminal')} aria-hidden="true" />
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
            <button type="button" className="ed-icon-button" onClick={onToggleDesc} title={desc ? '降序' : '升序'}>
              {desc ? '▼' : '▲'}
            </button>
          </span>
        )}
        <label className="ed-toggle" title="同时作用于目录树和文件列表">
          <input type="checkbox" checked={showHidden} onChange={onToggleHidden} />
          显示隐藏文件
        </label>
      </div>
    </div>
  );
}
