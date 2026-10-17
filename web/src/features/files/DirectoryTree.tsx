import { useCallback, useEffect, useRef, useState } from 'react';
import { PanelLeftClose, RefreshCw } from 'lucide-react';
import { Icon } from '../../components/Icon';
import { api } from '../../services/api';
import { DirectoryTreeNode, type TreeChildrenState } from './DirectoryTreeNode';
import { ancestorChain } from './fileUtils';

// One request per expanded directory, capped at the API's maximum page size.
const TREE_PAGE_SIZE = 1000;

type Props = {
  currentPath: string;
  showHidden: boolean;
  /** Incremented by the workspace after mkdir / rename / delete / upload. */
  revision: number;
  onSelect: (path: string) => void;
  onCollapse?: () => void;
};

/**
 * On-demand directory tree. Only directories that the user expanded (plus the
 * ancestors of the active path) are requested; nothing walks the file system
 * recursively and no request follows a symlink.
 */
export function DirectoryTree({ currentPath, showHidden, revision, onSelect, onCollapse }: Props) {
  const [nodes, setNodes] = useState<Record<string, TreeChildrenState>>({});
  const [expanded, setExpanded] = useState<ReadonlySet<string>>(() => new Set(['/']));
  const nodesRef = useRef(nodes);
  const expandedRef = useRef(expanded);
  const pendingRef = useRef<Set<string>>(new Set());
  const requestSeqRef = useRef<Map<string, number>>(new Map());

  useEffect(() => {
    nodesRef.current = nodes;
  }, [nodes]);
  useEffect(() => {
    expandedRef.current = expanded;
  }, [expanded]);

  const ensureLoaded = useCallback(async (dirPath: string, hidden: boolean, force = false) => {
    if (!force) {
      if (nodesRef.current[dirPath]?.status === 'loaded') return;
      if (pendingRef.current.has(dirPath)) return;
    }
    pendingRef.current.add(dirPath);
    const sequence = (requestSeqRef.current.get(dirPath) ?? 0) + 1;
    requestSeqRef.current.set(dirPath, sequence);
    setNodes((prev) => ({
      ...prev,
      [dirPath]: { status: 'loading', dirs: prev[dirPath]?.dirs ?? [] },
    }));
    try {
      const data = await api.listFiles(dirPath, {
        dirsOnly: true,
        page: 1,
        pageSize: TREE_PAGE_SIZE,
        sort: 'name',
        hidden,
      });
      if (requestSeqRef.current.get(dirPath) !== sequence) return;
      const dirs = data.entries.filter((entry) => entry.type === 'dir');
      setNodes((prev) => ({
        ...prev,
        [dirPath]: { status: 'loaded', dirs, truncated: data.total > dirs.length },
      }));
    } catch (caught) {
      if (requestSeqRef.current.get(dirPath) !== sequence) return;
      setNodes((prev) => ({
        ...prev,
        [dirPath]: {
          status: 'error',
          dirs: [],
          error: caught instanceof Error ? caught.message : String(caught),
        },
      }));
    } finally {
      pendingRef.current.delete(dirPath);
    }
  }, []);

  /** Drops the cache and reloads only '/' plus the nodes that are expanded. */
  const reloadVisible = useCallback(
    (hidden: boolean) => {
      requestSeqRef.current = new Map();
      pendingRef.current.clear();
      nodesRef.current = {};
      setNodes({});
      const paths = Array.from(new Set(['/', ...expandedRef.current]));
      for (const path of paths) void ensureLoaded(path, hidden, true);
    },
    [ensureLoaded],
  );

  // The hidden-file setting and workspace mutations invalidate the visible tree.
  useEffect(() => {
    reloadVisible(showHidden);
  }, [showHidden, revision, reloadVisible]);

  // Ancestors (and the directory itself) of the active path stay expanded.
  useEffect(() => {
    const chain = ancestorChain(currentPath);
    setExpanded((prev) => {
      if (chain.every((path) => prev.has(path))) return prev;
      const next = new Set(prev);
      chain.forEach((path) => next.add(path));
      return next;
    });
    chain.forEach((path) => void ensureLoaded(path, showHidden));
  }, [currentPath, showHidden, ensureLoaded]);

  const toggle = useCallback(
    (target: string) => {
      const willExpand = !expandedRef.current.has(target);
      setExpanded((prev) => {
        const next = new Set(prev);
        if (next.has(target)) next.delete(target);
        else next.add(target);
        return next;
      });
      if (willExpand) void ensureLoaded(target, showHidden);
    },
    [ensureLoaded, showHidden],
  );

  const retry = useCallback(
    (target: string) => {
      void ensureLoaded(target, showHidden, true);
    },
    [ensureLoaded, showHidden],
  );

  return (
    <div className="ed-tree">
      <header className="ed-tree-head">
        <span>文件目录</span>
        <div className="ed-tree-head-actions">
          <button
            type="button"
            className="ed-icon-button"
            title="刷新目录树"
            aria-label="刷新目录树"
            onClick={() => reloadVisible(showHidden)}
          >
            <Icon icon={RefreshCw} size={16} />
          </button>
          {onCollapse && (
            <button
              type="button"
              className="ed-icon-button"
              title="收起目录树"
              aria-label="收起目录树"
              onClick={onCollapse}
            >
              <Icon icon={PanelLeftClose} size={16} />
            </button>
          )}
        </div>
      </header>
      <div className="ed-tree-body" role="tree" aria-label="目录树">
        <DirectoryTreeNode
          path="/"
          name="/"
          depth={0}
          currentPath={currentPath}
          expanded={expanded}
          nodes={nodes}
          onToggle={toggle}
          onSelect={onSelect}
          onRetry={retry}
        />
      </div>
    </div>
  );
}
