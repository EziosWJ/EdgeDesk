import { useCallback, useEffect, useRef, useState } from 'react';
import { Plus, RefreshCw, Terminal as TerminalIcon } from 'lucide-react';
import { TerminalPane } from './TerminalPane';
import { Icon } from '../../components/Icon';
import { api } from '../../services/api';
import { sessionStore } from '../../stores/sessionStore';
import { windowStore } from '../../stores/windowStore';
import type { TerminalSessionInfo } from '../../types';

type Props = {
  /** Existing backend session to attach to (restored window or file action). */
  initialSessionId?: string | null;
  /** Working directory for a session created by this view. */
  initialCwd?: string;
  /** Restore validation already knows the saved session is gone. */
  restoredSessionMissing?: boolean;
  windowId?: string;
};

type Phase = 'booting' | 'ready' | 'ended' | 'error';

/**
 * Standalone single-page terminal.
 *
 * One window owns one Bash session and shows no tab strip. Opening the app
 * creates a session immediately; closing the window only detaches the UI, so
 * the PTY keeps running and can be found again in the session manager.
 */
export function TerminalView({
  initialSessionId = null,
  initialCwd,
  restoredSessionMissing = false,
  windowId,
}: Props) {
  const [sessionId, setSessionId] = useState<string | null>(restoredSessionMissing ? null : initialSessionId);
  const [session, setSession] = useState<TerminalSessionInfo | null>(null);
  const [phase, setPhase] = useState<Phase>(restoredSessionMissing ? 'ended' : initialSessionId ? 'ready' : 'booting');
  const [blocked, setBlocked] = useState(restoredSessionMissing);
  const [error, setError] = useState<string | null>(null);
  const [exitCode, setExitCode] = useState<number | null>(null);
  const [attempt, setAttempt] = useState(0);
  const bootRef = useRef<Promise<TerminalSessionInfo> | null>(null);

  useEffect(() => {
    if (sessionId || blocked) return;
    let cancelled = false;
    setPhase('booting');
    setError(null);
    if (!bootRef.current) {
      bootRef.current = api
        .createTerminal({ cwd: initialCwd, cols: 100, rows: 30 })
        .then((data) => {
          sessionStore.upsert(data.session);
          return data.session;
        });
    }
    bootRef.current
      .then((session) => {
        if (cancelled) return;
        setSession(session);
        setSessionId(session.id);
        setPhase('ready');
        if (windowId) {
          windowStore.updatePayload(windowId, { sessionId: session.id });
          windowStore.setTitle(windowId, session.title);
        }
      })
      .catch((caught) => {
        if (cancelled) return;
        bootRef.current = null;
        setError(caught instanceof Error ? caught.message : String(caught));
        setPhase('error');
      });
    return () => {
      cancelled = true;
    };
  }, [attempt, blocked, initialCwd, sessionId, windowId]);

  useEffect(() => {
    if (!windowId || !sessionId) return;
    windowStore.updatePayload(windowId, { sessionId });
  }, [sessionId, windowId]);

  const startNewSession = useCallback(() => {
    setSessionId(null);
    setBlocked(false);
    setPhase('booting');
    setError(null);
    setExitCode(null);
    setAttempt((value) => value + 1);
  }, []);

  const retry = useCallback(() => {
    bootRef.current = null;
    setError(null);
    setPhase('booting');
    setAttempt((value) => value + 1);
  }, []);

  const onSession = useCallback(
    (info: TerminalSessionInfo) => {
      sessionStore.upsert(info);
      setSession(info);
      setSessionId((current) => current ?? info.id);
      setPhase('ready');
      if (windowId) {
        windowStore.updatePayload(windowId, { sessionId: info.id });
        windowStore.setTitle(windowId, info.title);
      }
    },
    [windowId],
  );

  const onMissingSession = useCallback(() => {
    setSessionId(null);
    setBlocked(true);
    setPhase('ended');
  }, []);

  const shellUnavailable = !sessionStore.shellInfo.available;

  return (
    <div className="ed-terminal-view ed-terminal-single">
      {shellUnavailable && (
        <div className="ed-alert warning">
          未找到可用的登录 Shell（{sessionStore.shellInfo.shell}）。EdgeDesk 不会回退到 /bin/sh，请先在设备上安装 Bash。
        </div>
      )}

      {blocked && (
        <div className="ed-terminal-single-state">
          <span className="ed-terminal-single-icon" aria-hidden="true">
            <Icon icon={TerminalIcon} size={22} />
          </span>
          <h2>会话已结束或不可恢复</h2>
          <p>恢复的 Bash 会话已不存在；EdgeDesk 不会自动创建替代会话。</p>
          <button type="button" className="ed-button primary" onClick={startNewSession} disabled={shellUnavailable}>
            <Icon icon={Plus} size={15} />
            新建终端会话
          </button>
        </div>
      )}

      {phase === 'booting' && !blocked && (
        <div className="ed-terminal-single-state" role="status">
          <span className="ed-terminal-single-spinner" aria-hidden="true" />
          <h2>正在启动 Bash…</h2>
          <p>正在创建真实的登录 Shell 会话。</p>
        </div>
      )}

      {phase === 'error' && !blocked && (
        <div className="ed-terminal-single-state">
          <h2>终端启动失败</h2>
          <p className="ed-alert danger">{error}</p>
          <button type="button" className="ed-button" onClick={retry} disabled={shellUnavailable}>
            <Icon icon={RefreshCw} size={14} />
            重试
          </button>
        </div>
      )}

      {phase === 'ready' && exitCode !== null && (
        <div className="ed-alert warning">
          Shell 进程已退出（退出码 {exitCode}）。关闭窗口不会重新启动会话。
          <button type="button" className="ed-link" onClick={startNewSession} disabled={shellUnavailable}>
            新建终端会话
          </button>
        </div>
      )}

      {phase === 'ready' && sessionId && (
        <TerminalPane
          key={sessionId}
          sessionId={sessionId}
          title={session?.title ?? '终端'}
          cwd={session?.cwd}
          subscribers={session?.subscribers}
          onSessionCreated={onSession}
          onMissingSession={onMissingSession}
          onExited={(_id, code) => setExitCode(code)}
        />
      )}
    </div>
  );
}
