import { useEffect, useRef, useState } from 'react';
import { LayoutGrid, Minus } from 'lucide-react';
import { Icon } from '../components/Icon';
import type { AppId, WindowState } from '../types';
import { appRegistry } from './appRegistry';
import { AppSprite } from './AppIcon';
import { windowStore } from '../stores/windowStore';

type Props = {
  dockApps: readonly AppId[];
  windows: readonly WindowState[];
  focusedId: string | null;
  onLaunch: (appId: AppId) => void;
  onOpenLauncher: () => void;
};

function windowStatusLabel(item: WindowState): string {
  if (item.state === 'minimized') return '已最小化';
  if (item.state === 'maximized') return '已最大化';
  return '已打开';
}

/**
 * Bottom-centered floating Dock. It sits above the window layer, exposes
 * running/minimized state and asks the user to choose when an app has more
 * than one window instead of focusing an arbitrary instance.
 */
export function Dock({ dockApps, windows, focusedId, onLaunch, onOpenLauncher }: Props) {
  const [chooser, setChooser] = useState<AppId | null>(null);
  const chooserRef = useRef<HTMLDivElement | null>(null);

  useEffect(() => {
    if (!chooser) return;
    const onPointerDown = (event: PointerEvent) => {
      // Dock buttons manage their own toggling; only outside clicks dismiss.
      if ((event.target as Element | null)?.closest?.('.ed-dock-slot')) return;
      if (chooserRef.current && !chooserRef.current.contains(event.target as Node)) setChooser(null);
    };
    const onKeyDown = (event: KeyboardEvent) => {
      if (event.key === 'Escape') setChooser(null);
    };
    document.addEventListener('pointerdown', onPointerDown);
    document.addEventListener('keydown', onKeyDown);
    return () => {
      document.removeEventListener('pointerdown', onPointerDown);
      document.removeEventListener('keydown', onKeyDown);
    };
  }, [chooser]);

  const focused = windows.find((item) => item.id === focusedId) ?? null;
  const minimized = windows.filter((item) => item.state === 'minimized');

  const activate = (appId: AppId) => {
    const instances = windows.filter((item) => item.appId === appId);
    if (instances.length === 0) {
      onLaunch(appId);
      return;
    }
    if (instances.length === 1) {
      setChooser(null);
      windowStore.focus(instances[0].id);
      return;
    }
    setChooser((current) => (current === appId ? null : appId));
  };

  return (
    <nav className="ed-dock" aria-label="Dock">
      {minimized.length > 0 && (
        <div className="ed-dock-minimized" role="group" aria-label="最小化的窗口">
          {minimized.map((item) => {
            const app = appRegistry.get(item.appId);
            if (!app) return null;
            return (
              <button
                key={item.id}
                type="button"
                className="ed-dock-minimized-item"
                title={`恢复 ${item.title}`}
                aria-label={`恢复最小化窗口 ${item.title}`}
                onClick={() => windowStore.focus(item.id)}
              >
                <Icon icon={Minus} size={12} />
                <span>{item.title}</span>
              </button>
            );
          })}
          <span className="ed-dock-sep" aria-hidden="true" />
        </div>
      )}

      <div className="ed-dock-apps">
        {dockApps.map((appId) => {
          const app = appRegistry.get(appId);
          if (!app) return null;
          const instances = windows.filter((item) => item.appId === appId);
          const running = instances.length > 0;
          const active = focused?.appId === appId;
          const open = chooser === appId;
          return (
            <div className="ed-dock-slot" key={appId}>
              <button
                type="button"
                className={`ed-dock-item${active ? ' active' : ''}${running ? ' running' : ''}`}
                title={app.name}
                aria-label={
                  instances.length > 1
                    ? `${app.name}，${instances.length} 个窗口，选择窗口`
                    : instances.length === 1
                      ? `${app.name}，${windowStatusLabel(instances[0])}`
                      : `打开 ${app.name}`
                }
                aria-haspopup={instances.length > 1 ? 'menu' : undefined}
                aria-expanded={instances.length > 1 ? open : undefined}
                onClick={() => activate(appId)}
              >
                <AppSprite app={app} size={23} />
                {running && <span className="ed-dock-running" aria-hidden="true" />}
              </button>
              {open && instances.length > 1 && (
                <div className="ed-dock-chooser" role="menu" aria-label={`${app.name} 窗口`} ref={chooserRef}>
                  <span className="ed-dock-chooser-title">{app.name} · {instances.length} 个窗口</span>
                  {instances.map((item) => (
                    <button
                      key={item.id}
                      type="button"
                      role="menuitem"
                      className="ed-dock-chooser-item"
                      onClick={() => {
                        setChooser(null);
                        windowStore.focus(item.id);
                      }}
                    >
                      <span className="ed-dock-chooser-name">{item.title}</span>
                      <span className="ed-dock-chooser-state">{windowStatusLabel(item)}</span>
                    </button>
                  ))}
                </div>
              )}
            </div>
          );
        })}
      </div>

      <span className="ed-dock-sep" aria-hidden="true" />
      <button
        type="button"
        className="ed-dock-item ed-dock-launcher"
        title="应用启动器 (Alt+K)"
        aria-label="打开应用启动器"
        onClick={onOpenLauncher}
      >
        <Icon icon={LayoutGrid} size={22} />
      </button>
    </nav>
  );
}
