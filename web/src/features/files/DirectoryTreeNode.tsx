import { ChevronDown, ChevronRight, Folder, FolderOpen } from 'lucide-react';
import { Icon } from '../../components/Icon';
import type { FileEntry } from '../../types';

/** Per-directory children cache of the tree: on-demand, never recursive. */
export type TreeChildrenState =
  | { status: 'loading'; dirs: FileEntry[] }
  | { status: 'loaded'; dirs: FileEntry[]; truncated: boolean }
  | { status: 'error'; dirs: FileEntry[]; error: string };

type Props = {
  path: string;
  name: string;
  depth: number;
  currentPath: string;
  expanded: ReadonlySet<string>;
  nodes: Record<string, TreeChildrenState>;
  onToggle: (path: string) => void;
  onSelect: (path: string) => void;
  onRetry: (path: string) => void;
};

export function DirectoryTreeNode({
  path,
  name,
  depth,
  currentPath,
  expanded,
  nodes,
  onToggle,
  onSelect,
  onRetry,
}: Props) {
  const state = nodes[path];
  const isExpanded = expanded.has(path);
  const isCurrent = currentPath === path;
  // Only real subdirectories make it here: the backend `dirs_only` listing and
  // this filter both exclude symlinks, so the tree never follows links.
  const dirs = (state?.dirs ?? []).filter((entry) => entry.type === 'dir');
  // Confirmed-empty directories are leaves: no caret, no "无子目录" row, and
  // clicking them only navigates. Unloaded directories keep the caret — hiding
  // it before the first listing would make them impossible to expand.
  const isLeaf = state?.status === 'loaded' && dirs.length === 0;
  const expandIfPossible = () => {
    if (isLeaf || isExpanded) return;
    onToggle(path);
  };

  return (
    <div
      className="ed-tree-node"
      role="treeitem"
      aria-expanded={isLeaf ? undefined : isExpanded}
      aria-selected={isCurrent}
    >
      <div
        className={`ed-tree-row${isCurrent ? ' current' : ''}`}
        style={{ paddingLeft: 6 + depth * 14 }}
        title={path}
        tabIndex={0}
        onClick={() => {
          expandIfPossible();
          onSelect(path);
        }}
        onKeyDown={(event) => {
          if (event.key === 'Enter' || event.key === ' ') {
            event.preventDefault();
            expandIfPossible();
            onSelect(path);
          } else if (event.key === 'ArrowRight') {
            expandIfPossible();
          } else if (event.key === 'ArrowLeft' && isExpanded && !isLeaf) {
            onToggle(path);
          }
        }}
      >
        {isLeaf ? (
          <span className="ed-tree-caret placeholder" aria-hidden="true" />
        ) : (
          <button
            type="button"
            tabIndex={-1}
            className="ed-tree-caret"
            aria-label={isExpanded ? '折叠目录' : '展开目录'}
            onClick={(event) => {
              event.stopPropagation();
              onToggle(path);
            }}
          >
            <Icon icon={isExpanded ? ChevronDown : ChevronRight} size={14} />
          </button>
        )}
        <Icon icon={isExpanded ? FolderOpen : Folder} size={16} className="ed-tree-icon" />
        <span className="ed-tree-label">{name}</span>
        {state?.status === 'loading' && <span className="ed-spinner" role="status" aria-label="加载中" />}
        {state?.status === 'loaded' && state.truncated && (
          <span className="ed-tree-status" title="子目录过多，仅显示前 1000 个">
            1000+
          </span>
        )}
      </div>
      {isExpanded && !isLeaf && (
        <div role="group">
          {state?.status === 'error' && (
            <div className="ed-tree-error">
              <span>加载失败</span>
              <button type="button" className="ed-link" onClick={() => onRetry(path)}>
                重试
              </button>
            </div>
          )}
          {dirs.map((child) => (
            <DirectoryTreeNode
              key={child.path}
              path={child.path}
              name={child.name}
              depth={depth + 1}
              currentPath={currentPath}
              expanded={expanded}
              nodes={nodes}
              onToggle={onToggle}
              onSelect={onSelect}
              onRetry={onRetry}
            />
          ))}
        </div>
      )}
    </div>
  );
}
