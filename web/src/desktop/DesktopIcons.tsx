import { useRef, useState, type KeyboardEvent, type PointerEvent } from 'react';
import { appRegistry } from './appRegistry';
import { AppTile } from './AppIcon';
import { slotFromPoint, moveIcon, ICON_CELL_WIDTH, ICON_CELL_HEIGHT } from './iconLayout';
import { desktopStore, ICON_COLUMNS, type IconSlot } from './desktopStore';

type Props = {
  icons: readonly IconSlot[];
  onLaunch: (appId: IconSlot['appId']) => void;
};

type DragState = {
  appId: IconSlot['appId'];
  pointerId: number;
  startX: number;
  startY: number;
  moved: boolean;
};

/**
 * Desktop icon grid: single click selects, double click (or Enter) launches,
 * dragging rearranges slots. Positions are persisted by DesktopPersistence.
 */
export function DesktopIcons({ icons, onLaunch }: Props) {
  const [selected, setSelected] = useState<IconSlot['appId'] | null>(null);
  const [dragging, setDragging] = useState<IconSlot['appId'] | null>(null);
  const dragRef = useRef<DragState | null>(null);
  const suppressLaunchRef = useRef(false);
  const containerRef = useRef<HTMLDivElement | null>(null);
  const buttonRefs = useRef(new Map<string, HTMLButtonElement>());

  const onPointerDown = (event: PointerEvent<HTMLButtonElement>, appId: IconSlot['appId']) => {
    if (event.button !== 0) return;
    dragRef.current = {
      appId,
      pointerId: event.pointerId,
      startX: event.clientX,
      startY: event.clientY,
      moved: false,
    };
    event.currentTarget.setPointerCapture?.(event.pointerId);
    setSelected(appId);
  };

  const onPointerMove = (event: PointerEvent<HTMLButtonElement>) => {
    const drag = dragRef.current;
    if (!drag || drag.pointerId !== event.pointerId) return;
    if (!drag.moved && Math.abs(event.clientX - drag.startX) + Math.abs(event.clientY - drag.startY) > 6) {
      drag.moved = true;
      setDragging(drag.appId);
    }
  };

  const finishDrag = (event: PointerEvent<HTMLButtonElement>, cancelled: boolean) => {
    const drag = dragRef.current;
    if (!drag || drag.pointerId !== event.pointerId) return;
    dragRef.current = null;
    event.currentTarget.releasePointerCapture?.(event.pointerId);
    setDragging(null);
    if (!drag.moved || cancelled) return;
    suppressLaunchRef.current = true;
    window.setTimeout(() => {
      suppressLaunchRef.current = false;
    }, 0);
    const container = containerRef.current;
    if (!container) return;
    const rect = container.getBoundingClientRect();
    const slot = slotFromPoint(event.clientX - rect.left, event.clientY - rect.top);
    const next = moveIcon(icons, drag.appId, slot.col, slot.row);
    desktopStore.setIcons(next);
  };

  const onKeyDown = (event: KeyboardEvent<HTMLButtonElement>, appId: IconSlot['appId'], index: number) => {
    if (event.key === 'Enter') {
      event.preventDefault();
      onLaunch(appId);
      return;
    }
    if (event.key === ' ' || event.key === 'Spacebar') {
      event.preventDefault();
      setSelected(appId);
      return;
    }
    const order = icons.map((icon) => icon.appId);
    const columns = ICON_COLUMNS;
    let delta = 0;
    if (event.key === 'ArrowDown') delta = columns;
    else if (event.key === 'ArrowUp') delta = -columns;
    else if (event.key === 'ArrowRight') delta = 1;
    else if (event.key === 'ArrowLeft') delta = -1;
    else return;
    event.preventDefault();
    const nextIndex = Math.min(order.length - 1, Math.max(0, index + delta));
    const nextApp = order[nextIndex];
    setSelected(nextApp);
    buttonRefs.current.get(nextApp)?.focus();
  };

  return (
    <div className="ed-desktop-icons" ref={containerRef} role="group" aria-label="桌面应用">
      {icons.map((icon, index) => {
        const app = appRegistry.get(icon.appId);
        if (!app) return null;
        return (
          <button
            key={icon.appId}
            ref={(node) => {
              if (node) buttonRefs.current.set(icon.appId, node);
              else buttonRefs.current.delete(icon.appId);
            }}
            type="button"
            className={`ed-desktop-icon${selected === icon.appId ? ' selected' : ''}${dragging === icon.appId ? ' dragging' : ''}`}
            style={{ left: icon.col * ICON_CELL_WIDTH, top: icon.row * ICON_CELL_HEIGHT }}
            aria-pressed={selected === icon.appId}
            aria-label={`${app.name}，双击或按 Enter 打开`}
            title={`${app.name} — ${app.description}`}
            onPointerDown={(event) => onPointerDown(event, icon.appId)}
            onPointerMove={onPointerMove}
            onPointerUp={(event) => finishDrag(event, false)}
            onPointerCancel={(event) => finishDrag(event, true)}
            onClick={() => setSelected(icon.appId)}
            onDoubleClick={() => {
              if (suppressLaunchRef.current) return;
              onLaunch(icon.appId);
            }}
            onKeyDown={(event) => onKeyDown(event, icon.appId, index)}
          >
            <AppTile app={app} size={48} />
            <span className="ed-desktop-icon-label">{app.name}</span>
          </button>
        );
      })}
    </div>
  );
}
