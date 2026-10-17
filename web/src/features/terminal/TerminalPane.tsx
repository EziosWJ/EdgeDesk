import { useEffect, useRef, useState } from 'react';
import { Terminal } from '@xterm/xterm';
import { FitAddon } from '@xterm/addon-fit';
import '@xterm/xterm/css/xterm.css';
import { TerminalSocket } from '../../services/terminalSocket';
import { usePreferences } from '../../stores/preferencesStore';
import type { TerminalSessionInfo } from '../../types';

type Props = {
  sessionId: string | null;
  autoCreateCwd?: string;
  title?: string;
  onSessionCreated?: (session: TerminalSessionInfo) => void;
  onExited?: (sessionId: string | null, code: number) => void;
};

/**
 * xterm.js pane bound to one backend terminal session.
 *
 * Unmounting only closes the WebSocket; the PTY session stays alive so that
 * navigating, switching tabs or refreshing the browser never kills Bash.
 */
export function TerminalPane({ sessionId, autoCreateCwd, title, onSessionCreated, onExited }: Props) {
  const preferences = usePreferences();
  const containerRef = useRef<HTMLDivElement | null>(null);
  const socketRef = useRef<TerminalSocket | null>(null);
  const terminalRef = useRef<Terminal | null>(null);
  const createdRef = useRef(false);
  const [status, setStatus] = useState<'connecting' | 'open' | 'closed' | 'error'>('connecting');
  const [guard, setGuard] = useState<string | null>(null);
  const [exited, setExited] = useState<number | null>(null);
  const [currentId, setCurrentId] = useState<string | null>(sessionId);

  useEffect(() => {
    const container = containerRef.current;
    if (!container) return;

    const styles = getComputedStyle(document.documentElement);
    const terminal = new Terminal({
      fontFamily: 'JetBrains Mono, Menlo, Consolas, monospace',
      fontSize: preferences.terminal_font_size,
      cursorBlink: true,
      scrollback: 5000,
      allowProposedApi: true,
      theme: {
        background: styles.getPropertyValue('--ed-terminal-bg').trim() || '#08111e',
        foreground: styles.getPropertyValue('--ed-text').trim() || '#eef5ff',
        cursor: styles.getPropertyValue('--ed-primary').trim() || '#338efc',
        selectionBackground: '#274b73',
      },
    });
    terminalRef.current = terminal;
    const fit = new FitAddon();
    terminal.loadAddon(fit);
    terminal.open(container);
    terminal.focus();

    const socket = new TerminalSocket(sessionId);
    socketRef.current = socket;
    socket.onStatus = (next, detail) => {
      setStatus(next);
      if (next === 'open') {
        if (!socket.id && !createdRef.current) {
          createdRef.current = true;
          socket.createSession({ cwd: autoCreateCwd, cols: terminal.cols, rows: terminal.rows });
        } else if (socket.id) {
          socket.resize(terminal.cols, terminal.rows);
        }
      }
      if (next === 'error' && detail) terminal.writeln(`\x1b[31m[edgedesk] ${detail}\x1b[0m`);
      if (next === 'closed') terminal.writeln('\r\n\x1b[33m[edgedesk] 连接已断开，正在尝试重连（会话保持运行）…\x1b[0m');
    };
    socket.onOutput = (data) => terminal.write(data);
    socket.onGuard = (reason) => setGuard(reason);
    socket.onExit = (code) => {
      setExited(code);
      terminal.writeln(`\r\n\x1b[33m[edgedesk] Shell 进程已退出（退出码 ${code}）\x1b[0m`);
      onExited?.(socket.id, code);
    };
    socket.onAttached = (session) => {
      setCurrentId(session.id);
      setExited(session.exited ? session.exit_code : null);
      onSessionCreated?.(session);
    };
    terminal.onData((data) => socket.input(data));

    const doFit = () => {
      try {
        fit.fit();
        socket.resize(terminal.cols, terminal.rows);
      } catch {
        /* container not laid out yet */
      }
    };
    const observer = new ResizeObserver(() => doFit());
    observer.observe(container);
    requestAnimationFrame(doFit);
    socket.connect();

    return () => {
      observer.disconnect();
      socket.disconnect();
      terminal.dispose();
      socketRef.current = null;
      terminalRef.current = null;
    };
    // Session identity is fixed for the lifetime of the pane; changing it
    // creates a new pane in the window manager instead of rebinding.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  useEffect(() => {
    const terminal = terminalRef.current;
    if (!terminal) return;
    terminal.options.fontSize = preferences.terminal_font_size;
  }, [preferences.terminal_font_size]);

  return (
    <div className="ed-terminal-pane">
      <div className="ed-terminal-statusbar">
        <span className={`ed-terminal-status ${status}`}>
          {status === 'open'
            ? exited !== null
              ? `已退出 (${exited})`
              : '已连接'
            : status === 'connecting'
              ? '连接中…'
              : status === 'closed'
                ? '重连中…'
                : '异常'}
        </span>
        {currentId && <code className="ed-session-id">{currentId.slice(0, 8)}</code>}
        <span className="ed-terminal-hint">会话在后端继续运行，切换/刷新不会中断</span>
      </div>
      {guard && (
        <div className="ed-guard-bar">
          <strong>已被 CommandGuard 拦截：</strong>
          <span>{guard}</span>
          <div className="ed-guard-actions">
            <button type="button" className="ed-button danger" onClick={() => { socketRef.current?.confirmGuard(); setGuard(null); }}>
              仍然执行
            </button>
            <button type="button" className="ed-button" onClick={() => { socketRef.current?.cancelGuard(); setGuard(null); }}>
              取消
            </button>
          </div>
        </div>
      )}
      <div className="ed-terminal-host" ref={containerRef} title={title} />
    </div>
  );
}
