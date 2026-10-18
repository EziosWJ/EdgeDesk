import { useEffect, useRef, useState, type KeyboardEvent } from 'react';
import { Search, X } from 'lucide-react';
import { Icon } from '../components/Icon';
import { appRegistry } from './appRegistry';
import { AppTile } from './AppIcon';
import type { AppId } from '../types';

type Props = {
  open: boolean;
  onClose: () => void;
  onLaunch: (appId: AppId) => void;
};

/**
 * Application launcher. The app list always comes from AppRegistry; the search
 * filters name, description and keywords, with an explicit empty state.
 */
export function Launcher({ open, onClose, onLaunch }: Props) {
  const [query, setQuery] = useState('');
  const dialogRef = useRef<HTMLDivElement | null>(null);
  const inputRef = useRef<HTMLInputElement | null>(null);
  const returnFocusRef = useRef<HTMLElement | null>(null);

  useEffect(() => {
    if (!open) return;
    setQuery('');
    returnFocusRef.current = document.activeElement instanceof HTMLElement ? document.activeElement : null;
    const frame = window.requestAnimationFrame(() => inputRef.current?.focus());
    return () => {
      window.cancelAnimationFrame(frame);
      returnFocusRef.current?.focus?.();
    };
  }, [open]);

  if (!open) return null;

  const apps = appRegistry.search(query);
  const trimmed = query.trim();

  const onKeyDown = (event: KeyboardEvent<HTMLDivElement>) => {
    if (event.key === 'Escape') {
      event.preventDefault();
      onClose();
      return;
    }
    if (event.key !== 'Tab') return;
    const focusable = dialogRef.current?.querySelectorAll<HTMLElement>(
      'button:not(:disabled), input:not(:disabled), [tabindex="0"]',
    );
    if (!focusable || focusable.length === 0) return;
    const first = focusable[0];
    const last = focusable[focusable.length - 1];
    if (event.shiftKey && document.activeElement === first) {
      event.preventDefault();
      last.focus();
    } else if (!event.shiftKey && document.activeElement === last) {
      event.preventDefault();
      first.focus();
    }
  };

  return (
    <div className="ed-launcher-backdrop" onPointerDown={onClose}>
      <div
        className="ed-launcher"
        role="dialog"
        aria-modal="true"
        aria-label="应用启动器"
        ref={dialogRef}
        onPointerDown={(event) => event.stopPropagation()}
        onKeyDown={onKeyDown}
      >
        <div className="ed-launcher-head">
          <div>
            <p className="ed-launcher-eyebrow">APPLICATION LAUNCHER</p>
            <h2>应用启动器</h2>
          </div>
          <button type="button" className="ed-icon-button" aria-label="关闭启动器" title="关闭启动器 (Esc)" onClick={onClose}>
            <Icon icon={X} size={16} />
          </button>
        </div>

        <div className="ed-launcher-search">
          <Icon icon={Search} size={16} />
          <input
            ref={inputRef}
            type="search"
            value={query}
            placeholder="搜索应用，例如：终端、文件、监控…"
            aria-label="搜索应用"
            onChange={(event) => setQuery(event.target.value)}
          />
          {trimmed && (
            <button type="button" className="ed-icon-button" aria-label="清除搜索" onClick={() => setQuery('')}>
              <Icon icon={X} size={14} />
            </button>
          )}
        </div>

        {apps.length === 0 ? (
          <div className="ed-launcher-empty" role="status">
            <strong>没有找到匹配“{trimmed}”的应用</strong>
            <p>尝试搜索名称或用途，例如“终端”“文件”“设置”。</p>
          </div>
        ) : (
          <div className="ed-launcher-grid">
            {apps.map((app) => (
              <button
                key={app.id}
                type="button"
                className="ed-launcher-app"
                onClick={() => {
                  onLaunch(app.id);
                  onClose();
                }}
              >
                <AppTile app={app} size={48} />
                <strong>{app.name}</strong>
                <span>{app.description}</span>
              </button>
            ))}
          </div>
        )}
        <p className="ed-launcher-hint">
          按 Esc 关闭 · Alt+K 打开启动器 · 应用入口来自统一注册表
        </p>
      </div>
    </div>
  );
}
