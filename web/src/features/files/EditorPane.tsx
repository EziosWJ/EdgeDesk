import { useCallback, useEffect, useState } from 'react';
import { api, formatBytes } from '../../services/api';
import type { FileEntry } from '../../types';

type Props = {
  path: string;
};

/** Floating text editor window: UTF-8 only, atomic save, keeps permissions. */
export function EditorPane({ path }: Props) {
  const [content, setContent] = useState('');
  const [original, setOriginal] = useState('');
  const [entry, setEntry] = useState<FileEntry | null>(null);
  const [status, setStatus] = useState<'loading' | 'ready' | 'error' | 'saving' | 'saved'>('loading');
  const [error, setError] = useState<string | null>(null);

  const load = useCallback(async () => {
    setStatus('loading');
    setError(null);
    try {
      const data = await api.readText(path);
      setContent(data.content);
      setOriginal(data.content);
      setEntry(data.entry);
      setStatus('ready');
    } catch (caught) {
      setStatus('error');
      setError(caught instanceof Error ? caught.message : String(caught));
    }
  }, [path]);

  useEffect(() => {
    void load();
  }, [load]);

  const save = useCallback(async () => {
    setStatus('saving');
    setError(null);
    try {
      const data = await api.writeText(path, content);
      setEntry(data.entry);
      setOriginal(content);
      setStatus('saved');
    } catch (caught) {
      setStatus('error');
      setError(caught instanceof Error ? caught.message : String(caught));
    }
  }, [content, path]);

  useEffect(() => {
    const handler = (event: KeyboardEvent) => {
      if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === 's') {
        event.preventDefault();
        void save();
      }
    };
    window.addEventListener('keydown', handler);
    return () => window.removeEventListener('keydown', handler);
  }, [save]);

  const dirty = content !== original;

  return (
    <div className="ed-editor-pane">
      <div className="ed-editor-bar">
        <span className="ed-editor-path" title={path}>
          {path}
        </span>
        {entry && (
          <span className="ed-editor-meta">
            {formatBytes(entry.size)} · {entry.permissions} · utf-8
          </span>
        )}
        <div className="ed-toolbar">
          <button type="button" className="ed-button" onClick={() => void load()} disabled={status === 'loading'}>
            重新加载
          </button>
          <button type="button" className="ed-button primary" onClick={() => void save()} disabled={!dirty || status === 'saving'}>
            {status === 'saving' ? '保存中…' : '保存 (Ctrl+S)'}
          </button>
        </div>
      </div>
      {dirty && <div className="ed-alert warning">有未保存的修改</div>}
      {error && <div className="ed-alert danger">{error}</div>}
      {status === 'saved' && !dirty && <div className="ed-alert info">已原子保存（临时文件 + rename，保留原权限）</div>}
      <textarea
        className="ed-editor-textarea"
        value={content}
        spellCheck={false}
        onChange={(event) => {
          setContent(event.target.value);
          setStatus('ready');
        }}
        disabled={status === 'loading' || status === 'error'}
      />
    </div>
  );
}
