import { useEffect, useRef, useState } from 'react';
import { SetupView } from '../features/auth/SetupView';
import { CorruptStateView } from '../features/auth/CorruptStateView';
import { LoginView } from '../features/auth/LoginView';
import { DesktopShell, type HealthState } from '../desktop/DesktopShell';
import { restoreDesktop } from '../desktop/desktopPersistence';
import { desktopStore } from '../desktop/desktopStore';
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
  const [authView, setAuthView] = useState<AuthView>('checking');
  const [health, setHealth] = useState<HealthState>('checking');
  const metricsSocketRef = useRef<MetricsSocket | null>(null);
  const restoredRef = useRef(false);
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
    let cancelled = false;
    void preferencesStore.load().catch(() => undefined);
    const socket = new MetricsSocket();
    socket.onHistory = (samples, intervalMs) => sessionStore.setMetricsHistory(samples, intervalMs);
    socket.onSample = (sample) => sessionStore.pushSample(sample);
    socket.connect();
    metricsSocketRef.current = socket;

    const refreshTerminals = () =>
      api
        .terminalSessions()
        .then((data) => {
          if (cancelled) return null;
          sessionStore.setSessions(data.sessions);
          sessionStore.setShellInfo({
            shell: data.shell,
            shellAvailable: data.shell_available,
            maxSessions: data.max_sessions,
          });
          return data.sessions;
        })
        .catch(() => {
          if (!cancelled) sessionStore.setTerminalsLoaded(false);
          return null;
        });

    // Restore after authentication and after the first real session list so a
    // stale saved session is marked ended instead of being silently replaced.
    windowStore.setViewport({ width: window.innerWidth, height: window.innerHeight });
    void refreshTerminals().then((sessions) => {
      if (cancelled || restoredRef.current) return;
      restoredRef.current = true;
      restoreDesktop({
        viewport: { width: window.innerWidth, height: window.innerHeight },
        sessions,
      });
    });

    const interval = window.setInterval(refreshTerminals, 30000);

    return () => {
      cancelled = true;
      window.clearInterval(interval);
      socket.close();
      metricsSocketRef.current = null;
    };
  }, [authView]);

  const handleAuthenticated = () => {
    restoredRef.current = false;
    setAuthView('authenticated');
    api.health().then(() => setHealth('online')).catch(() => setHealth('offline'));
  };

  const handleLogout = () => {
    // Unsaved editor content can veto closing the workspace; the editor shows
    // its own confirmation and the server session stays valid until the user
    // resolves it and signs out again.
    if (!windowStore.requestCloseAll()) return;
    // Always invalidate the server-side session, even if the request fails
    // (for example when the session already expired or the device is offline).
    void api.logout().finally(() => {
      // Detach runtime windows but keep non-sensitive layout in localStorage;
      // the next login still re-validates terminal sessions and files.
      preferencesStore.reset();
      desktopStore.reset();
      restoredRef.current = false;
      setAuthView('anonymous');
    });
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

  return <DesktopShell onLogout={handleLogout} health={health} />;
}
