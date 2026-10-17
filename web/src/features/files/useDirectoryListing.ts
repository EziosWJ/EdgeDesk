import { useCallback, useEffect, useRef, useState } from 'react';
import { api } from '../../services/api';
import type { FileEntry } from '../../types';
import type { SortKey } from './fileUtils';

export type DirectoryListingState = {
  /** Path the payload belongs to; callers must verify it matches the active path. */
  requestPath: string;
  entries: FileEntry[];
  total: number;
  page: number;
  pageSize: number;
  loading: boolean;
  error: string | null;
};

export type DirectoryListingQuery = {
  showHidden: boolean;
  sort: SortKey;
  desc: boolean;
  page: number;
  pageSize?: number;
};

/**
 * Single source of truth for the active directory listing. Every key change
 * triggers exactly one request; responses from superseded requests are dropped
 * so sorting/paging/navigation cannot race each other.
 */
export function useDirectoryListing(path: string, query: DirectoryListingQuery) {
  const { showHidden, sort, desc, page, pageSize = 200 } = query;
  const [reloadToken, setReloadToken] = useState(0);
  const [state, setState] = useState<DirectoryListingState>({
    requestPath: path,
    entries: [],
    total: 0,
    page,
    pageSize,
    loading: true,
    error: null,
  });
  const sequenceRef = useRef(0);

  useEffect(() => {
    const sequence = sequenceRef.current + 1;
    sequenceRef.current = sequence;
    setState((prev) =>
      prev.requestPath === path
        ? { ...prev, loading: true, error: null }
        : { requestPath: path, entries: [], total: 0, page, pageSize, loading: true, error: null },
    );
    api
      .listFiles(path, { page, pageSize, sort, desc, hidden: showHidden })
      .then((data) => {
        if (sequenceRef.current !== sequence) return;
        setState({
          requestPath: data.path,
          entries: data.entries,
          total: data.total,
          page: data.page,
          pageSize,
          loading: false,
          error: null,
        });
      })
      .catch((caught: unknown) => {
        if (sequenceRef.current !== sequence) return;
        setState({
          requestPath: path,
          entries: [],
          total: 0,
          page,
          pageSize,
          loading: false,
          error: caught instanceof Error ? caught.message : String(caught),
        });
      });
  }, [path, showHidden, sort, desc, page, pageSize, reloadToken]);

  const reload = useCallback(() => setReloadToken((value) => value + 1), []);

  return { ...state, reload };
}
