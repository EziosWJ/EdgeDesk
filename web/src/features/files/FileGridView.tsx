import type { MouseEvent } from 'react';
import { iconStyle } from '../../services/assets';
import type { FileEntry } from '../../types';
import { FileEntryActions } from './FileEntryActions';
import { SelectionHeaderCheckbox } from './SelectionCheckbox';
import { entryKindLabel, entryMeta, iconName } from './fileUtils';

type Props = {
  entries: FileEntry[];
  selected: ReadonlySet<string>;
  loading: boolean;
  onToggleAll: (checked: boolean) => void;
  onToggleEntry: (entry: FileEntry, checked: boolean) => void;
  onEntryClick: (entry: FileEntry, event: MouseEvent<HTMLElement>) => void;
  onOpen: (entry: FileEntry) => void;
  onPreview: (entry: FileEntry) => void;
  onEdit: (entry: FileEntry) => void;
  onRename: (entry: FileEntry) => void;
  onDelete: (entry: FileEntry) => void;
};

/** Card grid: same data and selection as the table view, never a scaled table. */
export function FileGridView({
  entries,
  selected,
  loading,
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
    <section className="ed-file-grid-wrap ed-panel" aria-busy={loading}>
      <header className="ed-file-grid-head">
        <SelectionHeaderCheckbox
          checked={allSelected}
          indeterminate={!allSelected && someSelected}
          onChange={onToggleAll}
          label="全选当前页"
        />
        <span>网格视图 · 双击打开</span>
      </header>
      <div className="ed-file-grid">
        {entries.map((entry) => (
          <article
            key={entry.path}
            className={`ed-file-card is-${entry.type}${selected.has(entry.path) ? ' selected' : ''}`}
            tabIndex={0}
            onDoubleClick={(event) => {
              if ((event.target as HTMLElement).closest('input, .ed-actions, a')) return;
              onOpen(entry);
            }}
            onClick={(event) => onEntryClick(entry, event)}
            onKeyDown={(event) => {
              if (event.key === 'Enter') {
                event.preventDefault();
                onOpen(entry);
              } else if (event.key === ' ') {
                event.preventDefault();
                onToggleEntry(entry, !selected.has(entry.path));
              }
            }}
          >
            <div className="ed-file-card-top">
              <input
                type="checkbox"
                checked={selected.has(entry.path)}
                aria-label={`选择 ${entry.name}`}
                onClick={(event) => event.stopPropagation()}
                onChange={(event) => onToggleEntry(entry, event.target.checked)}
              />
              <FileEntryActions
                entry={entry}
                onOpen={onOpen}
                onPreview={onPreview}
                onEdit={onEdit}
                onRename={onRename}
                onDelete={onDelete}
              />
            </div>
            <div className="ed-file-card-icon">
              <span className="ed-icon-mask" style={iconStyle(iconName(entry))} aria-hidden="true" />
            </div>
            <div className="ed-file-card-name" title={entry.path}>
              {entry.name}
            </div>
            <div className="ed-file-card-meta">
              <span className="ed-file-card-kind">{entryKindLabel(entry)}</span>
              <span title={entryMeta(entry)}>{entryMeta(entry)}</span>
            </div>
          </article>
        ))}
      </div>
    </section>
  );
}
