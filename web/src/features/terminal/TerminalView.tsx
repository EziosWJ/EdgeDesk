import { useEffect, useRef, useState, type KeyboardEvent } from 'react';
import { Plus, RefreshCw, SquareArrowOutUpRight, X } from 'lucide-react';
import { TerminalPane } from './TerminalPane';
import { Icon } from '../../components/Icon';
import { api, formatTime } from '../../services/api';
import { sessionStore, useTerminals } from '../../stores/sessionStore';
import { loadStoredTerminals, storeTerminals } from '../../services/terminalSocket';
import { windowStore } from '../../stores/windowStore';

const asset = (path: string) => `${import.meta.env.BASE_URL}assets/${path}`;

export function TerminalView({
  initialSessionId = null,
  restoredSessionMissing = false,
  windowId,
}: {
  initialSessionId?: string | null;
  restoredSessionMissing?: boolean;
  windowId?: string;
}) {
  const sessions = useTerminals();
  const [activeId, setActiveId] = useState<string | null>(initialSessionId);
  const [error, setError] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);
  const tabRefs = useRef(new Map<string, HTMLDivElement>());

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

  useEffect(() => {
    if (sessions.length === 0) {
      if (activeId !== null) setActiveId(null);
      return;
    }
    if (!activeId || !sessions.some((session) => session.id === activeId)) {
      setActiveId(initialSessionId && sessions.some((session) => session.id === initialSessionId) ? initialSessionId : sessions[0].id);
    }
  }, [sessions, activeId, initialSessionId]);

  // Persist the active session on the owning window so a refresh restores the
  // same tab. The session id is not a credential; the server still authorizes it.
  useEffect(() => {
    if (windowId && activeId) windowStore.updatePayload(windowId, { sessionId: activeId });
  }, [activeId, windowId]);

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
    // Explicitly ending a session also detaches any UI window showing it.
    windowStore
      .getSnapshot()
      .filter((item) => item.appId === 'terminal' && item.payload.sessionId === sessionId)
      .forEach((item) => windowStore.forceClose(item.id));
    setActiveId((current) => (current === sessionId ? (sessions.find((s) => s.id !== sessionId)?.id ?? null) : current));
  };

  const activateTab = (sessionId: string) => {
    setActiveId(sessionId);
    tabRefs.current.get(sessionId)?.focus();
  };

  const onTabKeyDown = (event: KeyboardEvent<HTMLDivElement>, index: number) => {
    if (event.target !== event.currentTarget) return;
    const last = sessions.length - 1;
    let nextIndex: number | null = null;
    if (event.key === 'ArrowRight') nextIndex = (index + 1) % sessions.length;
    else if (event.key === 'ArrowLeft') nextIndex = (index - 1 + sessions.length) % sessions.length;
    else if (event.key === 'Home') nextIndex = 0;
    else if (event.key === 'End') nextIndex = last;
    else if (event.key === 'Enter' || event.key === ' ') {
      event.preventDefault();
      setActiveId(sessions[index].id);
      return;
    } else {
      return;
    }
    event.preventDefault();
    const next = sessions[nextIndex];
    if (next) activateTab(next.id);
  };

  const active = sessions.find((session) => session.id === activeId) ?? null;
  // A restored window keeps its ended state even when the first session list
  // arrives later (for example after a transient API failure).
  const missingRestoredSession =
    restoredSessionMissing ||
    (initialSessionId !== null &&
      sessionStore.sessionsLoaded &&
      !sessions.some((session) => session.id === initialSessionId));

  return (
    <div className="ed-view ed-terminal-view">
      <div className="ed-view-header ed-view-header-compact">
        <div>
          <h1>终端工作区</h1>
          <p>在真实 Bash 会话中工作；切换页面或刷新不会中断。</p>
        </div>
        <div className="ed-toolbar">
          <button type="button" className="ed-button" onClick={() => void refresh()} disabled={busy}>
            <Icon icon={RefreshCw} size={14} />
            刷新
          </button>
        </div>
      </div>

      {error && <div className="ed-alert danger">{error}</div>}
      {missingRestoredSession && (
        <div className="ed-alert warning" role="status">
          恢复的终端会话已结束或不可恢复；未创建替代 Bash 会话。其他真实会话仍可在下方打开。
        </div>
      )}
      {!sessionStore.shellInfo.available && (
        <div className="ed-alert warning">
          未找到可用的登录 Shell（{sessionStore.shellInfo.shell}）。EdgeDesk 不会回退到 /bin/sh，请先在设备上安装 Bash。
        </div>
      )}

      {sessions.length === 0 ? (
        <div className="ed-empty ed-terminal-empty">
          <img src={asset('illustrations/empty-terminal.svg')} alt="" />
          <h2>还没有终端会话</h2>
          <p>启动一个真实的 Bash 登录 Shell，会话上限 {sessionStore.shellInfo.maxSessions} 个。</p>
          <div className="ed-toolbar">
            <button
              type="button"
              className="ed-button primary"
              onClick={() => void createSession()}
              disabled={busy || !sessionStore.shellInfo.available}
            >
              <Icon icon={Plus} size={15} />
              新建终端
            </button>
          </div>
        </div>
      ) : (
        <>
          <div className="ed-tabbar" role="tablist" aria-label="终端会话">
            {sessions.map((session, index) => (
              <div
                key={session.id}
                ref={(node) => {
                  if (node) tabRefs.current.set(session.id, node);
                  else tabRefs.current.delete(session.id);
                }}
                role="tab"
                id={`ed-terminal-tab-${session.id}`}
                aria-selected={session.id === activeId}
                aria-controls="ed-terminal-panel"
                aria-label={`${session.title}，${session.running ? '运行中' : '已退出'}`}
                tabIndex={session.id === activeId ? 0 : -1}
                className={`ed-tab ${session.id === activeId ? 'active' : ''} ${session.running ? '' : 'exited'}`}
                title={`创建于 ${formatTime(session.created_ms / 1000)}`}
                onClick={() => setActiveId(session.id)}
                onKeyDown={(event) => onTabKeyDown(event, index)}
              >
                <span className={`ed-tab-dot ${session.running ? 'running' : 'exited'}`} aria-hidden="true" />
                <span className="ed-tab-title">{session.title}</span>
                <span className="ed-tab-actions">
                  <button
                    type="button"
                    title="在浮动窗口中打开"
                    aria-label={`在浮动窗口中打开 ${session.title}`}
                    onClick={(event) => {
                      event.stopPropagation();
                      windowStore.launch('terminal', {
                        title: session.title,
                        payload: { sessionId: session.id },
                        instanceKey: `session:${session.id}`,
                      });
                    }}
                  >
                    <Icon icon={SquareArrowOutUpRight} size={14} />
                  </button>
                  <button
                    type="button"
                    title="关闭并销毁会话"
                    aria-label={`关闭并销毁 ${session.title}`}
                    onClick={(event) => {
                      event.stopPropagation();
                      void closeSession(session.id);
                    }}
                  >
                    <Icon icon={X} size={14} />
                  </button>
                </span>
              </div>
            ))}
            <button
              type="button"
              className="ed-tab-add"
              onClick={() => void createSession()}
              title="新建终端"
              aria-label="新建终端"
              disabled={busy || !sessionStore.shellInfo.available}
            >
              <Icon icon={Plus} size={16} />
            </button>
          </div>
          <div
            className="ed-terminal-surface ed-panel"
            id="ed-terminal-panel"
            role="tabpanel"
            aria-labelledby={active ? `ed-terminal-tab-${active.id}` : undefined}
          >
            {active ? (
              <TerminalPane
                key={active.id}
                sessionId={active.id}
                title={active.title}
                cwd={active.cwd}
                subscribers={active.subscribers}
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
