import { useEffect, useRef, useState } from 'react';
import { LoginView } from '../features/auth/LoginView';
import { DashboardView } from '../features/dashboard/DashboardView';
import { FilesView } from '../features/files/FilesView';
import { TerminalView } from '../features/terminal/TerminalView';
import { SettingsView } from '../features/settings/SettingsView';
import { WindowLayer } from '../components/WindowLayer';
import { api } from '../services/api';
import { MetricsSocket } from '../services/metricsSocket';
import { sessionStore } from '../stores/sessionStore';
import { windowStore } from '../stores/windowStore';

type ModuleId = 'dashboard' | 'files' | 'terminal' | 'settings';

const modules: { id: ModuleId; label: string; icon: string }[] = [
  { id: 'dashboard', label: '仪表盘', icon: 'dashboard' },
  { id: 'files', label: '文件管理', icon: 'files' },
  { id: 'terminal', label: '终端工作区', icon: 'terminal' },
  { id: 'settings', label: '设置', icon: 'settings' },
];

const asset = (path: string) => `${import.meta.env.BASE_URL}assets/${path}`;

export default function App() {
  const [active, setActive] = useState<ModuleId>('dashboard');
  const [authState, setAuthState] = useState<'checking' | 'authenticated' | 'anonymous'>('checking');
  const [health, setHealth] = useState<'checking' | 'online' | 'offline'>('checking');
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
      <aside className="sidebar">
        <div className="brand">
          <img src={asset('brand/edgedesk-mark.svg')} width="44" height="44" alt="" />
          <div>
            <strong>
              Edge<span>Desk</span>
            </strong>
            <small>嵌入式 Linux 运维工作台</small>
          </div>
        </div>
        <nav aria-label="主导航" className="main-nav">
          {modules.map((item) => (
            <button
              key={item.id}
              type="button"
              className={`nav-item ${active === item.id ? 'active' : ''}`}
              aria-current={active === item.id ? 'page' : undefined}
              onClick={() => setActive(item.id)}
            >
              <img className="nav-icon" src={asset(`icons/${item.icon}.svg`)} alt="" />
              {item.label}
            </button>
          ))}
        </nav>
        <div className="device-status ed-panel">
          <img src={asset('illustrations/edge-device.svg')} alt="" width="52" height="44" />
          <div>
            <strong>本机服务</strong>
            <span className={`status ${health}`}>
              {health === 'online' ? '运行中 · 已认证' : health === 'offline' ? '未连接' : '检测中'}
            </span>
          </div>
        </div>
      </aside>

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
