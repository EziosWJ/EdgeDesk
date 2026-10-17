import { useEffect, useState } from 'react';
import { Plus, RefreshCw, SquareArrowOutUpRight, X } from 'lucide-react';
import { TerminalPane } from './TerminalPane';
import { Icon } from '../../components/Icon';
import { api, formatTime } from '../../services/api';
import { sessionStore, useTerminals } from '../../stores/sessionStore';
import { loadStoredTerminals, storeTerminals } from '../../services/terminalSocket';
import { windowStore } from '../../stores/windowStore';

const asset = (path: string) => `${import.meta.env.BASE_URL}assets/${path}`;

export function TerminalView() {
  const sessions = useTerminals();
  const [activeId, setActiveId] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);

  const refresh = async () => {
    try {
      const data = await api.terminalSessions();
      sessionStore.setSessions(data.sessions);
      sessionStore.setShellInfo({ shell: data.shell, shellAvailable: data.shell_available, maxSessions: data.max_sessions });
      const stored = loadStoredTerminals();
      const merged = new Map(stored.map((item) => [item.id, item.title]));
      data.sessions.forEach((session) => merged.set(session.id, session.title));
      storeTerminals([...merged.entries()].map(([id, title]) => ({ id, title })));
      setActiveId((current) => {
        if (current && data.sessions.some((session) => session.id === current)) return current;
        return data.sessions[0]?.id ?? null;
      });
    } catch (caught) {
      setError(caught instanceof Error ? caught.message : String(caught));
    }
  };

  useEffect(() => {
    void refresh();
  }, []);

  const createSession = async () => {
    setBusy(true);
    setError(null);
    try {
      const data = await api.createTerminal({ cols: 100, rows: 30 });
      sessionStore.upsert(data.session);
      setActiveId(data.session.id);
      const stored = loadStoredTerminals().filter((item) => item.id !== data.session.id);
      storeTerminals([...stored, { id: data.session.id, title: data.session.title }]);
    } catch (caught) {
      setError(caught instanceof Error ? caught.message : String(caught));
    } finally {
      setBusy(false);
    }
  };

  const closeSession = async (sessionId: string) => {
    await api.closeTerminal(sessionId);
    sessionStore.remove(sessionId);
    storeTerminals(loadStoredTerminals().filter((item) => item.id !== sessionId));
    windowStore.close(`terminal:${sessionId}`);
    setActiveId((current) => (current === sessionId ? (sessions.find((s) => s.id !== sessionId)?.id ?? null) : current));
  };

  const active = sessions.find((session) => session.id === activeId) ?? null;

  return (
    <div className="ed-view ed-terminal-view">
      <div className="ed-view-header">
        <div>
          <h1>终端工作区</h1>
          <p>
            真实 Bash 登录 Shell（{sessionStore.shellInfo.shell}），会话独立于页面生命周期；切换 Tab、导航或刷新后仍可重连。
          </p>
        </div>
        <div className="ed-toolbar">
          <button type="button" className="ed-button" onClick={() => void refresh()} disabled={busy}>
            <Icon icon={RefreshCw} size={14} />
            刷新
          </button>
          <button type="button" className="ed-button primary" onClick={() => void createSession()} disabled={busy || !sessionStore.shellInfo.available}>
            <Icon icon={Plus} size={14} />
            新建终端
          </button>
        </div>
      </div>

      {error && <div className="ed-alert danger">{error}</div>}
      {!sessionStore.shellInfo.available && (
        <div className="ed-alert warning">
          未找到可用的登录 Shell（{sessionStore.shellInfo.shell}）。EdgeDesk 不会回退到 /bin/sh，请先在设备上安装 Bash。
        </div>
      )}

      {sessions.length === 0 ? (
        <div className="ed-empty">
          <img src={asset('illustrations/empty-terminal.svg')} alt="" />
          <h2>还没有终端会话</h2>
          <p>点击“新建终端”启动一个真实的 Bash 登录 Shell，会话上限 {sessionStore.shellInfo.maxSessions} 个。</p>
        </div>
      ) : (
        <>
          <div className="ed-tabbar" role="tablist">
            {sessions.map((session) => (
              <div
                key={session.id}
                role="tab"
                aria-selected={session.id === activeId}
                className={`ed-tab ${session.id === activeId ? 'active' : ''} ${session.running ? '' : 'exited'}`}
                onClick={() => setActiveId(session.id)}
              >
                <span className="ed-tab-title">{session.title}</span>
                <span className="ed-tab-meta" title={`创建于 ${formatTime(session.created_ms / 1000)}`}>
                  {session.running ? (session.subscribers > 1 ? `${session.subscribers} 个视图` : '运行中') : '已退出'}
                </span>
                <button
                  type="button"
                  title="在浮动窗口中打开"
                  onClick={(event) => {
                    event.stopPropagation();
                    windowStore.open({
                      kind: 'terminal',
                      title: session.title,
                      payload: { sessionId: session.id },
                    });
                  }}
                >
                  <Icon icon={SquareArrowOutUpRight} size={14} />
                </button>
                <button
                  type="button"
                  title="关闭并销毁会话"
                  onClick={(event) => {
                    event.stopPropagation();
                    void closeSession(session.id);
                  }}
                >
                  <Icon icon={X} size={14} />
                </button>
              </div>
            ))}
            <button type="button" className="ed-tab-add" onClick={() => void createSession()} title="新建终端">
              <Icon icon={Plus} size={16} />
            </button>
          </div>
          <div className="ed-terminal-surface ed-panel">
            {active ? (
              <TerminalPane
                key={active.id}
                sessionId={active.id}
                title={active.title}
                onSessionCreated={(session) => sessionStore.upsert(session)}
                onExited={() => void refresh()}
              />
            ) : (
              <div className="ed-empty small">选择一个终端会话</div>
            )}
          </div>
        </>
      )}
    </div>
  );
}
