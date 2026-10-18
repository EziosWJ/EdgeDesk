import { useCallback, useEffect, useRef, useState } from 'react';
import { LogOut, ShieldCheck } from 'lucide-react';
import { Icon } from '../components/Icon';
import { WindowLayer } from '../components/WindowLayer';
import { asset } from '../services/assets';
import { useMetricsSamples } from '../stores/sessionStore';
import { windowStore, useFocusedWindowId, useWindows } from '../stores/windowStore';
import { desktopStore, useDesktopState } from './desktopStore';
import { DesktopIcons } from './DesktopIcons';
import { Dock } from './Dock';
import { Launcher } from './Launcher';
import { startDesktopPersistence } from './desktopPersistence';
import { isEditableTarget, useNarrowScreen } from './useNarrowScreen';
import type { AppId } from '../types';

export type HealthState = 'checking' | 'online' | 'offline';

type Props = {
  onLogout: () => void;
  health: HealthState;
};

function useClock(): Date {
  const [now, setNow] = useState(() => new Date());
  useEffect(() => {
    const timer = window.setInterval(() => setNow(new Date()), 1000);
    return () => window.clearInterval(timer);
  }, []);
  return now;
}

function clockText(now: Date): string {
  const two = (value: number) => String(value).padStart(2, '0');
  return `${two(now.getHours())}:${two(now.getMinutes())}:${two(now.getSeconds())}`;
}

/** Desktop shell: wallpaper, icons, window layer, Dock, launcher and shortcuts. */
export function DesktopShell({ onLogout, health }: Props) {
  const desktop = useDesktopState();
  const windows = useWindows();
  const focusedId = useFocusedWindowId();
  const narrow = useNarrowScreen();
  const samples = useMetricsSamples();
  const now = useClock();
  const [launcherOpen, setLauncherOpen] = useState(false);
  const persistenceStarted = useRef(false);

  const latest = samples.length > 0 ? samples[samples.length - 1] : null;

  const launch = useCallback((appId: AppId) => {
    if (appId === 'files') {
      windowStore.launch('files', { payload: { initialPath: '/' } });
      return;
    }
    windowStore.launch(appId);
  }, []);

  useEffect(() => {
    const syncViewport = () => {
      windowStore.setViewport({ width: window.innerWidth, height: window.innerHeight });
    };
    syncViewport();
    window.addEventListener('resize', syncViewport);
    return () => window.removeEventListener('resize', syncViewport);
  }, []);

  useEffect(() => {
    if (persistenceStarted.current) return;
    persistenceStarted.current = true;
    return startDesktopPersistence();
  }, []);

  useEffect(() => {
    const onKeyDown = (event: KeyboardEvent) => {
      if (event.defaultPrevented) return;
      if (event.ctrlKey || event.metaKey) return;
      if (isEditableTarget(event.target)) return;
      if (!event.altKey) return;

      const code = event.code;
      if (code === 'KeyK') {
        event.preventDefault();
        setLauncherOpen((value) => !value);
        return;
      }
      if (code === 'KeyM') {
        if (focusedId && !narrow) {
          event.preventDefault();
          windowStore.toggleMaximize(focusedId);
        }
        return;
      }
      if (code === 'KeyH') {
        if (focusedId) {
          event.preventDefault();
          windowStore.minimize(focusedId);
        }
        return;
      }
      if (code === 'Period') {
        event.preventDefault();
        windowStore.cycleFocus(1);
        return;
      }
      if (code === 'Comma') {
        event.preventDefault();
        windowStore.cycleFocus(-1);
      }
    };
    window.addEventListener('keydown', onKeyDown);
    return () => window.removeEventListener('keydown', onKeyDown);
  }, [focusedId, narrow]);

  const healthText = health === 'online' ? '服务在线' : health === 'offline' ? '服务离线' : '检测中';

  return (
    <div className={`ed-desktop${narrow ? ' narrow' : ''}`} data-testid="desktop-shell">
      <div
        className="ed-desktop-wallpaper"
        aria-hidden="true"
        style={{ backgroundImage: `url(${asset('desktop/wallpaper-mountain-lake.svg')})` }}
      />

      <header className="ed-desktop-topbar">
        <div className="ed-desktop-brand">
          <img src={asset('brand/edgedesk-mark.svg')} width="26" height="26" alt="" />
          <strong>
            Edge<span>Desk</span>
          </strong>
          <span className="ed-desktop-version">V1.2</span>
        </div>
        <div className="ed-desktop-topbar-meta">
          <span className="ed-desktop-hostname" title={latest?.hostname ?? undefined}>
            {latest?.hostname ?? '设备桌面'}
          </span>
          <span className={`ed-desktop-health ${health}`} title={`EdgeDesk ${healthText}`}>
            <span className="ed-desktop-health-dot" aria-hidden="true" />
            {healthText}
          </span>
          <span className="ed-desktop-clock" aria-label={`当前时间 ${clockText(now)}`}>
            {clockText(now)}
          </span>
          <button
            type="button"
            className="ed-desktop-logout"
            title="退出登录"
            aria-label="退出登录"
            onClick={onLogout}
          >
            <Icon icon={LogOut} size={15} />
          </button>
        </div>
      </header>

      <DesktopIcons icons={desktop.icons} onLaunch={launch} />

      {desktop.warning && (
        <div className="ed-desktop-notice" role="status">
          <span>{desktop.warning}</span>
          <button type="button" className="ed-link" onClick={() => desktopStore.setWarning(null)}>
            关闭
          </button>
        </div>
      )}

      <WindowLayer onLogout={onLogout} />

      <div className="ed-desktop-shortcut-hint" aria-hidden="true">
        <Icon icon={ShieldCheck} size={12} />
        Alt+K 启动器 · Alt+M 最大化 · Alt+H 最小化 · Alt+,/Alt+. 切换窗口
      </div>

      <Dock
        dockApps={desktop.dock}
        windows={windows}
        focusedId={focusedId}
        onLaunch={launch}
        onOpenLauncher={() => setLauncherOpen(true)}
      />

      <Launcher open={launcherOpen} onClose={() => setLauncherOpen(false)} onLaunch={launch} />
    </div>
  );
}
