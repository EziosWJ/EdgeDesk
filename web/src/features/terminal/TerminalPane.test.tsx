// @vitest-environment happy-dom
import { act } from 'react';
import { createRoot, type Root } from 'react-dom/client';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

vi.mock('@xterm/xterm', () => ({
  Terminal: class {
    cols = 80;
    rows = 24;
    options: Record<string, unknown> = {};
    output = '';
    loadAddon(): void {}
    open(): void {}
    focus(): void {}
    onData(): void {}
    write(data: string): void {
      this.output += data;
    }
    writeln(data: string): void {
      this.output += `${data}\n`;
    }
    dispose(): void {}
  },
}));
vi.mock('@xterm/addon-fit', () => ({ FitAddon: class { fit(): void {} } }));

import { TerminalPane } from './TerminalPane';
import { sessionStore } from '../../stores/sessionStore';

class FakeWebSocket {
  static OPEN = 1;
  static instances: FakeWebSocket[] = [];
  readyState = 1;
  sent: Array<Record<string, unknown>> = [];
  onopen: (() => void) | null = null;
  onmessage: ((event: { data: string }) => void) | null = null;
  onclose: (() => void) | null = null;
  onerror: (() => void) | null = null;
  url: string;

  constructor(url: string) {
    this.url = url;
    FakeWebSocket.instances.push(this);
  }

  send(raw: string): void {
    this.sent.push(JSON.parse(raw));
  }

  close(): void {
    this.readyState = 3;
    this.onclose?.();
  }

  emit(message: Record<string, unknown>): void {
    this.onmessage?.({ data: JSON.stringify(message) });
  }
}

describe('TerminalPane session restoration', () => {
  let container: HTMLDivElement;
  let root: Root;

  beforeEach(() => {
    FakeWebSocket.instances = [];
    vi.stubGlobal('WebSocket', FakeWebSocket);
    vi.stubGlobal(
      'ResizeObserver',
      class {
        observe(): void {}
        disconnect(): void {}
      },
    );
    sessionStore.setShellInfo({ shell: '/bin/bash', shellAvailable: true, maxSessions: 8 });
    document.body.innerHTML = '';
    container = document.createElement('div');
    document.body.appendChild(container);
    root = createRoot(container);
  });

  afterEach(() => {
    act(() => root.unmount());
    vi.unstubAllGlobals();
  });

  const render = (sessionId: string | null) => {
    act(() => {
      root.render(<TerminalPane sessionId={sessionId} title="Bash" />);
    });
  };

  it('never creates a replacement when a restored session is gone', () => {
    render('removed-session');
    const socket = FakeWebSocket.instances[0];
    expect(socket).toBeDefined();
    act(() => socket.onopen?.());
    expect(socket.sent.some((message) => message.type === 'create')).toBe(false);

    act(() => {
      socket.emit({ type: 'error', code: 'session_not_found', message: 'terminal session is gone' });
    });
    expect(socket.sent.some((message) => message.type === 'create')).toBe(false);
    expect(container.textContent).toContain('会话已结束或不可恢复');
    expect(container.textContent).toContain('不会自动创建替代 Bash 会话');
  });

  it('creates a session only when the pane was opened without a session id', () => {
    render(null);
    const socket = FakeWebSocket.instances[0];
    act(() => socket.onopen?.());
    const create = socket.sent.find((message) => message.type === 'create');
    expect(create).toBeDefined();
    expect(create?.cols).toBe(80);
  });

  it('attaches a real session and updates the UI with its id', () => {
    render('live-session');
    const socket = FakeWebSocket.instances[0];
    act(() => socket.onopen?.());
    act(() => {
      socket.emit({
        type: 'attached',
        session: {
          id: 'live-session',
          title: 'bash · /',
          cwd: '/',
          created_ms: 1,
          last_activity_ms: 1,
          cols: 80,
          rows: 24,
          running: true,
          exited: false,
          exit_code: 0,
          subscribers: 1,
          buffered_bytes: 0,
          pending_confirmation: false,
        },
      });
    });
    expect(socket.sent.some((message) => message.type === 'create')).toBe(false);
    expect(container.textContent).toContain('已连接');
    expect(container.textContent).toContain('live-ses');
  });
});
