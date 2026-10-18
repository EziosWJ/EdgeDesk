import { useCallback, useRef, type ReactNode } from 'react';
import { Maximize2, Minimize2, Minus, X } from 'lucide-react';
import { Icon } from './Icon';
import { AppSprite } from '../desktop/AppIcon';
import { appRegistry } from '../desktop/appRegistry';
import type { WindowState } from '../types';
import { windowStore } from '../stores/windowStore';

type Props = {
  window: WindowState;
  children: ReactNode;
  /** Narrow viewports force the maximized presentation and disable dragging. */
  narrow?: boolean;
  focused?: boolean;
};

const INTERACTIVE = 'button, input, textarea, select, a, [contenteditable="true"], [data-no-drag]';

export function FloatingWindow({ window: state, children, narrow = false, focused = false }: Props) {
  const dragRef = useRef<{ startX: number; startY: number; originX: number; originY: number } | null>(null);
  const resizeRef = useRef<{ startX: number; startY: number; width: number; height: number } | null>(null);
  const app = appRegistry.get(state.appId);
  const minimized = state.state === 'minimized';
  const maximized = state.state === 'maximized' || narrow;
  const dragEnabled = !maximized && !minimized;

  const onTitlePointerDown = useCallback(
    (event: React.PointerEvent<HTMLElement>) => {
      if (!dragEnabled || event.button !== 0) return;
      if ((event.target as HTMLElement).closest(INTERACTIVE)) return;
      event.preventDefault();
      dragRef.current = { startX: event.clientX, startY: event.clientY, originX: state.x, originY: state.y };
      event.currentTarget.setPointerCapture?.(event.pointerId);
      windowStore.focus(state.id);
    },
    [dragEnabled, state.id, state.x, state.y],
  );

  const onTitlePointerMove = useCallback(
    (event: React.PointerEvent<HTMLElement>) => {
      const drag = dragRef.current;
      if (!drag) return;
      windowStore.move(state.id, drag.originX + (event.clientX - drag.startX), drag.originY + (event.clientY - drag.startY));
    },
    [state.id],
  );

  const endDrag = useCallback((event: React.PointerEvent<HTMLElement>) => {
    if (!dragRef.current) return;
    dragRef.current = null;
    event.currentTarget.releasePointerCapture?.(event.pointerId);
  }, []);

  const onResizePointerDown = useCallback(
    (event: React.PointerEvent<HTMLDivElement>) => {
      if (!dragEnabled) return;
      event.stopPropagation();
      event.preventDefault();
      resizeRef.current = { startX: event.clientX, startY: event.clientY, width: state.width, height: state.height };
      event.currentTarget.setPointerCapture?.(event.pointerId);
      windowStore.focus(state.id);
    },
    [dragEnabled, state.height, state.id, state.width],
  );

  const onResizePointerMove = useCallback(
    (event: React.PointerEvent<HTMLDivElement>) => {
      const resize = resizeRef.current;
      if (!resize) return;
      windowStore.resize(
        state.id,
        resize.width + (event.clientX - resize.startX),
        resize.height + (event.clientY - resize.startY),
      );
    },
    [state.id],
  );

  const onResizePointerUp = useCallback((event: React.PointerEvent<HTMLDivElement>) => {
    resizeRef.current = null;
    event.currentTarget.releasePointerCapture?.(event.pointerId);
  }, []);

  const style = maximized
    ? { zIndex: state.zIndex }
    : { left: state.x, top: state.y, width: state.width, height: state.height, zIndex: state.zIndex };

  return (
    <section
      className={[
        'ed-floating',
        'ed-window',
        maximized ? 'maximized' : '',
        minimized ? 'ed-window--minimized' : '',
        focused ? 'focused' : '',
      ]
        .filter(Boolean)
        .join(' ')}
      style={{ ...style, display: minimized ? 'none' : undefined }}
      onPointerDown={() => windowStore.focus(state.id)}
      role="dialog"
      aria-label={state.title}
      aria-hidden={minimized || undefined}
      data-window-id={state.id}
      data-window-app={state.appId}
    >
      <header
        className="ed-window-title"
        onPointerDown={onTitlePointerDown}
        onPointerMove={onTitlePointerMove}
        onPointerUp={endDrag}
        onPointerCancel={endDrag}
        onDoubleClick={(event) => {
          if ((event.target as HTMLElement).closest(INTERACTIVE)) return;
          if (!narrow && !minimized) windowStore.toggleMaximize(state.id);
        }}
        title={maximized ? state.title : `${state.title} — 拖动标题栏移动窗口`}
      >
        {app && (
          <span className={`ed-window-app-icon tile-${app.tone}`} aria-hidden="true">
            <AppSprite app={app} size={14} />
          </span>
        )}
        <span className="ed-window-title-text">{state.title}</span>
        <div className="ed-window-actions">
          <button type="button" title="最小化" aria-label={`最小化 ${state.title}`} onClick={() => windowStore.minimize(state.id)}>
            <Icon icon={Minus} size={14} />
          </button>
          <button
            type="button"
            title={maximized ? '还原' : '最大化'}
            aria-label={`${maximized ? '还原' : '最大化'} ${state.title}`}
            disabled={narrow}
            onClick={() => windowStore.toggleMaximize(state.id)}
          >
            <Icon icon={maximized ? Minimize2 : Maximize2} size={14} />
          </button>
          <button
            type="button"
            className="danger"
            title="关闭窗口（不会终止后端会话）"
            aria-label={`关闭 ${state.title}`}
            onClick={() => windowStore.requestClose(state.id)}
          >
            <Icon icon={X} size={14} />
          </button>
        </div>
      </header>
      <div className="ed-window-body">{children}</div>
      {!maximized && (
        <div
          className="ed-window-resize"
          onPointerDown={onResizePointerDown}
          onPointerMove={onResizePointerMove}
          onPointerUp={onResizePointerUp}
          onPointerCancel={onResizePointerUp}
          title="拖动调整大小"
          aria-hidden="true"
        />
      )}
    </section>
  );
}
