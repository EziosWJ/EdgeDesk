import { useEffect, useRef, useState } from 'react';
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
import { windowStore } from '../stores/windowStore';

export default function App() {
  const [active, setActive] = useState<ModuleId>('dashboard');
  const [authState, setAuthState] = useState<'checking' | 'authenticated' | 'anonymous'>('checking');
  const [health, setHealth] = useState<HealthState>('checking');
  const metricsSocketRef = useRef<MetricsSocket | null>(null);

  useEffect(() => {
    api
      .session()
      .then((data) => setAuthState(data.authenticated ? 'authenticated' : 'anonymous'))
      .catch(() => setAuthState('anonymous'));
    api
      .health()
      .then(() => setHealth('online'))
      .catch(() => setHealth('offline'));
  }, []);

  useEffect(() => {
    if (authState !== 'authenticated') return;
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
  }, [authState]);

  if (authState === 'checking') {
    return <div className="ed-boot">正在校验会话…</div>;
  }
  if (authState === 'anonymous') {
    return (
      <LoginView
        onSuccess={() => {
          setAuthState('authenticated');
          api.health().then(() => setHealth('online')).catch(() => setHealth('offline'));
        }}
      />
    );
  }

  return (
    <div className="shell">
      <Sidebar active={active} health={health} onSelect={setActive} />

      <div className="content-shell">
        <header className="topbar">
          <div className="topbar-title">
            EdgeDesk <span>/ V1.0 单二进制运维面板</span>
          </div>
          <span className="version">文件 · 终端 · 监控 · 无数据库</span>
        </header>
        <main className="main-area">
          {active === 'dashboard' && <DashboardView />}
          {active === 'files' && <FilesView />}
          {active === 'terminal' && <TerminalView />}
          {active === 'settings' && (
            <SettingsView
              onLogout={() => {
                windowStore.closeAll();
                setAuthState('anonymous');
              }}
            />
          )}
        </main>
      </div>

      <WindowLayer />
    </div>
  );
}
