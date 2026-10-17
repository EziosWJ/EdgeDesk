import { useEffect, useRef, useState } from 'react';
import { SetupView } from '../features/auth/SetupView';
import { CorruptStateView } from '../features/auth/CorruptStateView';
import { LoginView } from '../features/auth/LoginView';
import { DashboardView } from '../features/dashboard/DashboardView';
import { FilesView } from '../features/files/FilesView';
import { TerminalView } from '../features/terminal/TerminalView';
import { SettingsView } from '../features/settings/SettingsView';
import { Sidebar, type HealthState, type ModuleId } from '../components/Sidebar';
import { WindowLayer } from '../components/WindowLayer';
import { api } from '../services/api';
import { MetricsSocket } from '../services/metricsSocket';
import { sessionStore } from '../stores/sessionStore';
import { preferencesStore, usePreferences } from '../stores/preferencesStore';
import { windowStore } from '../stores/windowStore';

type AuthView = 'checking' | 'setup' | 'anonymous' | 'authenticated' | 'corrupt';

function applyTheme(theme: 'light' | 'dark' | 'system'): void {
  const root = document.documentElement;
  const media = typeof window.matchMedia === 'function' ? window.matchMedia('(prefers-color-scheme: dark)') : null;
  const effective = theme === 'system' ? (media?.matches ? 'dark' : 'light') : theme;
  root.dataset.theme = effective;
  root.style.colorScheme = effective;
}

export default function App() {
  const [active, setActive] = useState<ModuleId>('dashboard');
  const [authView, setAuthView] = useState<AuthView>('checking');
  const [health, setHealth] = useState<HealthState>('checking');
  const metricsSocketRef = useRef<MetricsSocket | null>(null);
  const preferences = usePreferences();

  useEffect(() => {
    applyTheme(preferences.theme);
    if (preferences.theme !== 'system' || typeof window.matchMedia !== 'function') return;
    const media = window.matchMedia('(prefers-color-scheme: dark)');
    const listener = () => applyTheme('system');
    media.addEventListener('change', listener);
    return () => media.removeEventListener('change', listener);
  }, [preferences.theme]);

  useEffect(() => {
    let cancelled = false;
    api
      .setupStatus()
      .then((status) => {
        if (cancelled) return;
        if (status.state === 'corrupt') {
          setAuthView('corrupt');
          return;
        }
        if (!status.initialized) {
          setAuthView('setup');
          return;
        }
        return api
          .session()
          .then((session) => {
            if (cancelled) return;
            setAuthView(session.authenticated ? 'authenticated' : 'anonymous');
          })
          .catch(() => {
            if (!cancelled) setAuthView('anonymous');
          });
      })
      .catch(() => {
        if (!cancelled) setAuthView('anonymous');
      });
    api
      .health()
      .then(() => setHealth('online'))
      .catch(() => setHealth('offline'));
    return () => {
      cancelled = true;
    };
  }, []);

  useEffect(() => {
    if (authView !== 'authenticated') return;
    void preferencesStore.load().catch(() => undefined);
    const socket = new MetricsSocket();
    socket.onHistory = (samples, intervalMs) => sessionStore.setMetricsHistory(samples, intervalMs);
    socket.onSample = (sample) => sessionStore.pushSample(sample);
    socket.connect();
    metricsSocketRef.current = socket;

    const refreshTerminals = () => {
      api
        .terminalSessions()
        .then((data) => {
          sessionStore.setSessions(data.sessions);
          sessionStore.setShellInfo({
            shell: data.shell,
            shellAvailable: data.shell_available,
            maxSessions: data.max_sessions,
          });
        })
        .catch(() => undefined);
    };
    refreshTerminals();
    const interval = window.setInterval(refreshTerminals, 30000);

    return () => {
      window.clearInterval(interval);
      socket.close();
      metricsSocketRef.current = null;
    };
  }, [authView]);

  const handleAuthenticated = () => {
    setAuthView('authenticated');
    api.health().then(() => setHealth('online')).catch(() => setHealth('offline'));
  };

  const handleLogout = () => {
    windowStore.closeAll();
    preferencesStore.reset();
    setAuthView('anonymous');
  };

  if (authView === 'checking') {
    return <div className="ed-boot">正在校验认证状态…</div>;
  }
  if (authView === 'corrupt') {
    return <CorruptStateView />;
  }
  if (authView === 'setup') {
    return <SetupView onSuccess={() => setAuthView('anonymous')} />;
  }
  if (authView === 'anonymous') {
    return <LoginView onSuccess={handleAuthenticated} />;
  }

  return (
    <div className="shell">
      <Sidebar active={active} health={health} onSelect={setActive} />

      <div className="content-shell">
        <main className="main-area">
          {active === 'dashboard' && <DashboardView />}
          {active === 'files' && <FilesView />}
          {active === 'terminal' && <TerminalView />}
          {active === 'settings' && <SettingsView onLogout={handleLogout} />}
        </main>
      </div>

      <WindowLayer />
    </div>
  );
}
