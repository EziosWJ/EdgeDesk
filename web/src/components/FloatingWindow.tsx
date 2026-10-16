import { useCallback, useRef, type ReactNode } from 'react';
import type { WindowState } from '../types';
import { windowStore } from '../stores/windowStore';

type Props = {
  window: WindowState;
  children: ReactNode;
};

export function FloatingWindow({ window: state, children }: Props) {
  const dragRef = useRef<{ startX: number; startY: number; originX: number; originY: number } | null>(null);
  const resizeRef = useRef<{ startX: number; startY: number; width: number; height: number } | null>(null);

  const onTitlePointerDown = useCallback(
    (event: React.PointerEvent<HTMLDivElement>) => {
      if (state.state === 'maximized') return;
      if ((event.target as HTMLElement).closest('button')) return;
      dragRef.current = { startX: event.clientX, startY: event.clientY, originX: state.x, originY: state.y };
      (event.target as HTMLElement).setPointerCapture(event.pointerId);
      windowStore.focus(state.id);
    },
    [state.id, state.state, state.x, state.y],
  );

  const onTitlePointerMove = useCallback(
    (event: React.PointerEvent<HTMLDivElement>) => {
      const drag = dragRef.current;
      if (!drag) return;
      windowStore.move(state.id, drag.originX + (event.clientX - drag.startX), drag.originY + (event.clientY - drag.startY));
    },
    [state.id],
  );

  const onTitlePointerUp = useCallback(() => {
    dragRef.current = null;
  }, []);

  const onResizePointerDown = useCallback(
    (event: React.PointerEvent<HTMLDivElement>) => {
      event.stopPropagation();
      resizeRef.current = { startX: event.clientX, startY: event.clientY, width: state.width, height: state.height };
      (event.target as HTMLElement).setPointerCapture(event.pointerId);
      windowStore.focus(state.id);
    },
    [state.height, state.id, state.width],
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

  const onResizePointerUp = useCallback(() => {
    resizeRef.current = null;
  }, []);

  if (state.state === 'minimized') return null;
  const maximized = state.state === 'maximized';
  const style = maximized
    ? { left: 8, top: 8, width: 'calc(100vw - 16px)', height: 'calc(100vh - 16px)', zIndex: state.zIndex }
    : { left: state.x, top: state.y, width: state.width, height: state.height, zIndex: state.zIndex };

  return (
    <section
      className={`ed-floating ed-window ${maximized ? 'maximized' : ''}`}
      style={style}
      onPointerDown={() => windowStore.focus(state.id)}
      role="dialog"
      aria-label={state.title}
    >
      <header
        className="ed-window-title"
        onPointerDown={onTitlePointerDown}
        onPointerMove={onTitlePointerMove}
        onPointerUp={onTitlePointerUp}
        onDoubleClick={() => windowStore.toggleMaximize(state.id)}
      >
        <span className="ed-window-title-text" title={state.title}>
          {state.title}
        </span>
        <div className="ed-window-actions">
          <button type="button" title="最小化" onClick={() => windowStore.minimize(state.id)}>
            &#8211;
          </button>
          <button type="button" title={maximized ? '还原' : '最大化'} onClick={() => windowStore.toggleMaximize(state.id)}>
            {maximized ? '❐' : '□'}
          </button>
          <button type="button" className="danger" title="关闭" onClick={() => windowStore.close(state.id)}>
            ×
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
          title="拖动调整大小"
        />
      )}
    </section>
  );
}
