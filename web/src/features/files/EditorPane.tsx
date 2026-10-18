import { useCallback, useEffect, useRef, useState } from 'react';
import { ConfirmDialog } from '../../components/ConfirmDialog';
import { api, formatBytes } from '../../services/api';
import { windowStore } from '../../stores/windowStore';
import type { FileEntry } from '../../types';

type Props = {
  path: string;
  windowId?: string;
};

/** Floating text editor: UTF-8 only, atomic save, external-change guard. */
export function EditorPane({ path, windowId }: Props) {
  const [content, setContent] = useState('');
  const [original, setOriginal] = useState('');
  const [entry, setEntry] = useState<FileEntry | null>(null);
  const [status, setStatus] = useState<'loading' | 'ready' | 'error' | 'saving' | 'saved'>('loading');
  const [error, setError] = useState<string | null>(null);
  const [conflict, setConflict] = useState(false);
  const [confirmClose, setConfirmClose] = useState(false);
  const dirty = content !== original;
  const dirtyRef = useRef(dirty);
  dirtyRef.current = dirty;

  const load = useCallback(async () => {
    setStatus('loading');
    setError(null);
    setConflict(false);
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

  const save = useCallback(
    async (force = false) => {
      setStatus('saving');
      setError(null);
      try {
        // Re-stat before writing: if the file changed on disk, never overwrite
        // silently. The user explicitly chooses to reload or force-save.
        const current = await api.statFile(path);
        if (!force && entry && (current.entry.mtime !== entry.mtime || current.entry.size !== entry.size)) {
          setConflict(true);
          setStatus('ready');
          return;
        }
        const data = await api.writeText(path, content);
        setEntry(data.entry);
        setOriginal(content);
        setConflict(false);
        setStatus('saved');
      } catch (caught) {
        setStatus('error');
        setError(caught instanceof Error ? caught.message : String(caught));
      }
    },
    [content, entry, path],
  );

  // Closing a dirty editor is a window-manager decision that the app can veto;
  // the manager keeps the component mounted until the user confirms.
  useEffect(() => {
    if (!windowId) return;
    return windowStore.registerCloseGuard(windowId, () => {
      if (!dirtyRef.current) return true;
      setConfirmClose(true);
      return false;
    });
  }, [windowId]);

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
          <button
            type="button"
            className="ed-button primary"
            onClick={() => void save()}
            disabled={!dirty || status === 'saving'}
          >
            {status === 'saving' ? '保存中…' : '保存 (Ctrl+S)'}
          </button>
        </div>
      </div>
      {dirty && <div className="ed-alert warning">有未保存的修改</div>}
      {conflict && (
        <div className="ed-alert warning" role="alert">
          <strong>文件已在磁盘上发生变化。</strong>
          <span>为避免静默覆盖，保存已暂停。请选择重新加载磁盘版本，或明确覆盖。</span>
          <div className="ed-toolbar">
            <button type="button" className="ed-button" onClick={() => void load()}>
              重新加载磁盘版本
            </button>
            <button type="button" className="ed-button danger" onClick={() => void save(true)}>
              仍然覆盖保存
            </button>
          </div>
        </div>
      )}
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
        onKeyDown={(event) => {
          if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === 's') {
            event.preventDefault();
            void save();
          }
        }}
        disabled={status === 'loading' || status === 'error'}
        aria-label={`编辑 ${path}`}
      />
      {confirmClose && windowId && (
        <ConfirmDialog
          title="关闭未保存的编辑器？"
          message="该文件有未保存的修改。关闭窗口会丢弃这些修改；V1.2 不会把文件正文写入浏览器存储。"
          details={[path]}
          confirmText="放弃修改并关闭"
          cancelText="继续编辑"
          danger
          onCancel={() => setConfirmClose(false)}
          onConfirm={() => {
            setConfirmClose(false);
            windowStore.forceClose(windowId);
          }}
        />
      )}
    </div>
  );
}
