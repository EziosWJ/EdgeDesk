import { iconStyle } from '../../services/assets';
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
  const hasChildren = !(state?.status === 'loaded' && dirs.length === 0);

  return (
    <div className="ed-tree-node" role="treeitem" aria-expanded={isExpanded} aria-selected={isCurrent}>
      <div
        className={`ed-tree-row${isCurrent ? ' current' : ''}`}
        style={{ paddingLeft: 6 + depth * 14 }}
        title={path}
        tabIndex={0}
        onClick={() => {
          if (!isExpanded) onToggle(path);
          onSelect(path);
        }}
        onKeyDown={(event) => {
          if (event.key === 'Enter' || event.key === ' ') {
            event.preventDefault();
            if (!isExpanded) onToggle(path);
            onSelect(path);
          } else if (event.key === 'ArrowRight' && !isExpanded) {
            onToggle(path);
          } else if (event.key === 'ArrowLeft' && isExpanded) {
            onToggle(path);
          }
        }}
      >
        <button
          type="button"
          tabIndex={-1}
          className={`ed-tree-caret${hasChildren ? '' : ' placeholder'}`}
          aria-label={isExpanded ? '折叠目录' : '展开目录'}
          onClick={(event) => {
            event.stopPropagation();
            onToggle(path);
          }}
        >
          {isExpanded ? '▾' : '▸'}
        </button>
        <span
          className="ed-icon-mask ed-tree-icon"
          style={iconStyle('files')}
          aria-hidden="true"
        />
        <span className="ed-tree-label">{name}</span>
        {state?.status === 'loading' && <span className="ed-spinner" role="status" aria-label="加载中" />}
        {state?.status === 'loaded' && state.truncated && (
          <span className="ed-tree-status" title="子目录过多，仅显示前 1000 个">
            1000+
          </span>
        )}
      </div>
      {isExpanded && (
        <div role="group">
          {state?.status === 'error' && (
            <div className="ed-tree-error">
              <span>加载失败</span>
              <button type="button" className="ed-link" onClick={() => onRetry(path)}>
                重试
              </button>
            </div>
          )}
          {state?.status === 'loaded' && dirs.length === 0 && <div className="ed-tree-empty">无子目录</div>}
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
