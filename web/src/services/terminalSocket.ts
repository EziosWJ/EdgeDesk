import type { TerminalSessionInfo } from '../types';
import { api } from './api';

type OutputHandler = (data: string) => void;
type ExitHandler = (code: number) => void;
type StatusHandler = (status: 'connecting' | 'open' | 'closed' | 'error', detail?: string) => void;
type GuardHandler = (reason: string) => void;

/**
 * Terminal WebSocket client. One socket per terminal view.
 *
 * Disconnecting never destroys the backend session: the server keeps the PTY
 * and the replay buffer, so a reconnecting client receives the buffered output
 * again. Session ids live in the terminal store (persisted to localStorage).
 */
export class TerminalSocket {
  private socket: WebSocket | null = null;
  private sessionId: string | null;
  private closedByUser = false;
  private reconnectAttempts = 0;
  private reconnectTimer: number | null = null;
  private pendingResize: { cols: number; rows: number } | null = null;

  onOutput: OutputHandler = () => {};
  onExit: ExitHandler = () => {};
  onStatus: StatusHandler = () => {};
  onGuard: GuardHandler = () => {};
  onAttached: (session: TerminalSessionInfo) => void = () => {};

  constructor(sessionId: string | null) {
    this.sessionId = sessionId;
  }

  connect(): void {
    if (this.socket) return;
    this.closedByUser = false;
    this.onStatus('connecting');
    const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
    const url = new URL(`${protocol}//${window.location.host}/ws/terminal`);
    if (this.sessionId) url.searchParams.set('session_id', this.sessionId);
    const socket = new WebSocket(url.toString());
    this.socket = socket;

    socket.onopen = () => {
      this.reconnectAttempts = 0;
      this.onStatus('open');
      if (this.pendingResize) {
        this.resize(this.pendingResize.cols, this.pendingResize.rows);
      }
    };
    socket.onmessage = (event) => {
      let message: Record<string, unknown>;
      try {
        message = JSON.parse(String(event.data));
      } catch {
        return;
      }
      switch (message.type) {
        case 'ready':
          if (this.sessionId) {
            this.send({ type: 'attach', session_id: this.sessionId });
          }
          break;
        case 'created':
        case 'attached': {
          const session = message.session as TerminalSessionInfo | undefined;
          if (session) {
            this.sessionId = session.id;
            this.onAttached(session);
          }
          break;
        }
        case 'output':
          this.onOutput(String(message.data ?? ''));
          break;
        case 'exit':
          this.onExit(Number(message.code ?? 0));
          break;
        case 'guard':
          this.onGuard(String(message.reason ?? '危险命令'));
          break;
        case 'error': {
          const code = String(message.code ?? 'error');
          this.onStatus('error', String(message.message ?? code));
          if (code === 'session_not_found') {
            this.sessionId = null;
          }
          break;
        }
        default:
          break;
      }
    };
    socket.onclose = () => {
      this.socket = null;
      this.onStatus('closed');
      if (!this.closedByUser) this.scheduleReconnect();
    };
    socket.onerror = () => {
      this.onStatus('error', 'websocket error');
    };
  }

  private scheduleReconnect(): void {
    if (this.reconnectTimer !== null) return;
    this.reconnectAttempts += 1;
    const delay = Math.min(8000, 400 * 2 ** Math.min(this.reconnectAttempts, 4));
    this.reconnectTimer = window.setTimeout(() => {
      this.reconnectTimer = null;
      this.connect();
    }, delay);
  }

  private send(payload: Record<string, unknown>): void {
    if (this.socket && this.socket.readyState === WebSocket.OPEN) {
      this.socket.send(JSON.stringify(payload));
    }
  }

  createSession(options: { cwd?: string; cols: number; rows: number }): void {
    this.send({ type: 'create', ...options });
  }

  input(data: string): void {
    this.send({ type: 'input', data });
  }

  resize(cols: number, rows: number): void {
    this.pendingResize = { cols, rows };
    this.send({ type: 'resize', cols, rows });
  }

  confirmGuard(): void {
    this.send({ type: 'confirm' });
  }

  cancelGuard(): void {
    this.send({ type: 'cancel' });
  }

  /** Closes the socket only; the backend session keeps running. */
  disconnect(): void {
    this.closedByUser = true;
    if (this.reconnectTimer !== null) {
      window.clearTimeout(this.reconnectTimer);
      this.reconnectTimer = null;
    }
    this.socket?.close();
    this.socket = null;
  }

  get id(): string | null {
    return this.sessionId;
  }
}

const STORAGE_KEY = 'edgedesk.terminals.v1';

export type StoredTerminal = { id: string; title: string };

/** Persisted session metadata. Never stores tokens or secrets. */
export function loadStoredTerminals(): StoredTerminal[] {
  try {
    const raw = window.localStorage.getItem(STORAGE_KEY);
    if (!raw) return [];
    const parsed = JSON.parse(raw);
    if (!Array.isArray(parsed)) return [];
    return parsed
      .filter((item) => item && typeof item.id === 'string')
      .map((item) => ({ id: String(item.id), title: String(item.title ?? 'Bash') }));
  } catch {
    return [];
  }
}

export function storeTerminals(sessions: StoredTerminal[]): void {
  try {
    window.localStorage.setItem(STORAGE_KEY, JSON.stringify(sessions));
  } catch {
    // localStorage can be unavailable (private mode); persistence is optional.
  }
}

export async function closeTerminalSession(sessionId: string): Promise<void> {
  try {
    await api.closeTerminal(sessionId);
  } catch {
    // Session may already be gone; the UI state is refreshed from the server.
  }
}
