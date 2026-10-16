import { useEffect, useState } from 'react';
import { api, formatBytes, formatTime } from '../../services/api';
import type { FileEntry } from '../../types';

type Props = {
  path: string;
};

/** Floating read-only preview: text content, images, or a download hint. */
export function PreviewPane({ path }: Props) {
  const [content, setContent] = useState<string | null>(null);
  const [entry, setEntry] = useState<FileEntry | null>(null);
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {
    let cancelled = false;
    setContent(null);
    setEntry(null);
    setError(null);
    api
      .readText(path)
      .then((data) => {
        if (cancelled) return;
        setContent(data.content);
        setEntry(data.entry);
      })
      .catch((caught: unknown) => {
        if (cancelled) return;
        setError(caught instanceof Error ? caught.message : String(caught));
        api.statFile(path).then((data) => !cancelled && setEntry(data.entry)).catch(() => undefined);
      });
    return () => {
      cancelled = true;
    };
  }, [path]);

  const isImage = entry?.mime.startsWith('image/') ?? false;

  return (
    <div className="ed-preview-pane">
      <div className="ed-editor-bar">
        <span className="ed-editor-path" title={path}>
          {path}
        </span>
        <div className="ed-toolbar">
          <a className="ed-button" href={api.downloadUrl(path)}>
            下载
          </a>
        </div>
      </div>
      {entry && (
        <div className="ed-preview-meta">
          <span>类型 {entry.mime}</span>
          <span>大小 {formatBytes(entry.size)}</span>
          <span>权限 {entry.permissions}</span>
          <span>修改 {formatTime(entry.mtime)}</span>
        </div>
      )}
      {error && !isImage && <div className="ed-alert warning">{error}（可下载后本地查看）</div>}
      {isImage ? (
        <div className="ed-preview-image">
          <img src={api.downloadUrl(path)} alt={path} />
        </div>
      ) : (
        content !== null && <pre className="ed-preview-text">{content}</pre>
      )}
    </div>
  );
}
