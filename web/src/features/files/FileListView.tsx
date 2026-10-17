import type { MouseEvent } from 'react';
import { iconStyle } from '../../services/assets';
import { formatBytes, formatTime } from '../../services/api';
import type { FileEntry } from '../../types';
import { FileEntryActions } from './FileEntryActions';
import { SelectionCheckbox, SelectionHeaderCheckbox } from './SelectionCheckbox';
import { iconName, type SortKey } from './fileUtils';

type Props = {
  path: string;
  entries: FileEntry[];
  selected: ReadonlySet<string>;
  sort: SortKey;
  desc: boolean;
  loading: boolean;
  onToggleSort: (key: SortKey) => void;
  onNavigateUp: () => void;
  onToggleAll: (checked: boolean) => void;
  onToggleEntry: (entry: FileEntry, checked: boolean) => void;
  onEntryClick: (entry: FileEntry, event: MouseEvent<HTMLElement>) => void;
  onOpen: (entry: FileEntry) => void;
  onPreview: (entry: FileEntry) => void;
  onEdit: (entry: FileEntry) => void;
  onRename: (entry: FileEntry) => void;
  onDelete: (entry: FileEntry) => void;
};

function sortMark(active: boolean, desc: boolean): string {
  if (!active) return '';
  return desc ? '▼' : '▲';
}

export function FileListView({
  path,
  entries,
  selected,
  sort,
  desc,
  loading,
  onToggleSort,
  onNavigateUp,
  onToggleAll,
  onToggleEntry,
  onEntryClick,
  onOpen,
  onPreview,
  onEdit,
  onRename,
  onDelete,
}: Props) {
  const allSelected = entries.length > 0 && entries.every((entry) => selected.has(entry.path));
  const someSelected = entries.some((entry) => selected.has(entry.path));

  return (
    <div className="ed-table-wrap ed-panel" aria-busy={loading}>
      <table className="ed-table ed-files-table">
        <thead>
          <tr>
            <th className="ed-col-check">
              <SelectionHeaderCheckbox
                checked={allSelected}
                indeterminate={!allSelected && someSelected}
                onChange={onToggleAll}
                label="全选当前页"
              />
            </th>
            <th
              onClick={() => onToggleSort('name')}
              className="sortable ed-col-name"
              aria-sort={sort === 'name' ? (desc ? 'descending' : 'ascending') : 'none'}
            >
              名称 {sortMark(sort === 'name', desc)}
            </th>
            <th
              onClick={() => onToggleSort('size')}
              className="sortable ed-col-size"
              aria-sort={sort === 'size' ? (desc ? 'descending' : 'ascending') : 'none'}
            >
              大小 {sortMark(sort === 'size', desc)}
            </th>
            <th
              onClick={() => onToggleSort('mtime')}
              className="sortable ed-col-time"
              aria-sort={sort === 'mtime' ? (desc ? 'descending' : 'ascending') : 'none'}
            >
              修改时间 {sortMark(sort === 'mtime', desc)}
            </th>
            <th className="ed-col-perm">权限</th>
            <th className="ed-col-owner">uid/gid</th>
            <th className="ed-col-actions">操作</th>
          </tr>
        </thead>
        <tbody>
          {path !== '/' && (
            <tr className="ed-parent-row">
              <td />
              <td colSpan={6}>
                <button type="button" className="ed-entry" onClick={onNavigateUp} title="上级目录" onDoubleClick={onNavigateUp}>
                  <span className="ed-icon-mask ed-entry-icon" style={iconStyle('files')} aria-hidden="true" />
                  <span className="ed-entry-name">..</span>
                  <span className="ed-badge subtle">上级</span>
                </button>
              </td>
            </tr>
          )}
          {entries.map((entry) => (
            <tr
              key={entry.path}
              className={selected.has(entry.path) ? 'selected' : ''}
              onDoubleClick={(event) => {
                if ((event.target as HTMLElement).closest('input, .ed-actions, a')) return;
                onOpen(entry);
              }}
            >
              <td>
                <SelectionCheckbox
                  checked={selected.has(entry.path)}
                  onChange={(checked) => onToggleEntry(entry, checked)}
                  label={`选择 ${entry.name}`}
                />
              </td>
              <td>
                <button
                  type="button"
                  className="ed-entry"
                  title={entry.path}
                  onClick={(event) => onEntryClick(entry, event)}
                >
                  <span className="ed-icon-mask ed-entry-icon" style={iconStyle(iconName(entry))} aria-hidden="true" />
                  <span className="ed-entry-name">{entry.name}</span>
                  {entry.type === 'symlink' && <span className="ed-badge">链接</span>}
                  {entry.hidden && <span className="ed-badge subtle">隐藏</span>}
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
                <FileEntryActions
                  entry={entry}
                  onOpen={onOpen}
                  onPreview={onPreview}
                  onEdit={onEdit}
                  onRename={onRename}
                  onDelete={onDelete}
                />
              </td>
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  );
}
