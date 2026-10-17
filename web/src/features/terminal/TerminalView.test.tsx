// @vitest-environment happy-dom
import { act } from 'react';
import { createRoot, type Root } from 'react-dom/client';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { TerminalView } from './TerminalView';
import { api } from '../../services/api';
import { sessionStore } from '../../stores/sessionStore';
import { windowStore } from '../../stores/windowStore';
import type { TerminalSessionInfo } from '../../types';

vi.mock('./TerminalPane', () => ({
  TerminalPane: ({ sessionId }: { sessionId: string }) => <div data-testid="terminal-pane">{sessionId}</div>,
}));

const SESSIONS: TerminalSessionInfo[] = [
  {
    id: 'ses-one',
    title: 'bash · /',
    cwd: '/',
    created_ms: 1_760_000_000_000,
    last_activity_ms: 1_760_000_000_000,
    cols: 100,
    rows: 30,
    running: true,
    exited: false,
    exit_code: 0,
    subscribers: 1,
    buffered_bytes: 0,
    pending_confirmation: false,
  },
  {
    id: 'ses-two',
    title: 'bash · /var/log',
    cwd: '/var/log',
    created_ms: 1_760_000_001_000,
    last_activity_ms: 1_760_000_001_000,
    cols: 100,
    rows: 30,
    running: false,
    exited: true,
    exit_code: 0,
    subscribers: 1,
    buffered_bytes: 0,
    pending_confirmation: false,
  },
];

describe('TerminalView', () => {
  let container: HTMLDivElement;
  let root: Root;

  beforeEach(() => {
    windowStore.closeAll();
    sessionStore.setSessions([]);
    sessionStore.setShellInfo({ shell: '/bin/bash', shellAvailable: true, maxSessions: 8 });
    window.localStorage.removeItem('edgedesk.terminals.v1');
    vi.spyOn(api, 'terminalSessions').mockResolvedValue({
      ok: true,
      shell: '/bin/bash',
      shell_available: true,
      max_sessions: 8,
      idle_timeout_seconds: 3600,
      running: 1,
      sessions: SESSIONS,
    });
    document.body.innerHTML = '';
    container = document.createElement('div');
    document.body.appendChild(container);
    root = createRoot(container);
  });

  afterEach(() => {
    vi.restoreAllMocks();
    if (root) act(() => root.unmount());
  });

  const render = async () => {
    act(() => {
      root.render(<TerminalView />);
    });
    for (let index = 0; index < 3; index += 1) {
      await act(async () => {
        await new Promise((resolve) => setTimeout(resolve, 0));
      });
    }
  };

  it('renders compact tabs with ARIA state and supports arrow-key switching', async () => {
    await render();
    const tabs = [...container.querySelectorAll<HTMLElement>('[role="tab"]')];
    expect(tabs).toHaveLength(2);
    expect(tabs[0].getAttribute('aria-selected')).toBe('true');
    expect(tabs[0].tabIndex).toBe(0);
    expect(tabs[1].getAttribute('aria-selected')).toBe('false');
    expect(tabs[1].tabIndex).toBe(-1);
    expect(container.querySelector('[role="tabpanel"]')?.getAttribute('aria-labelledby')).toBe(
      'ed-terminal-tab-ses-one',
    );

    act(() => {
      tabs[0].focus();
      tabs[0].dispatchEvent(new KeyboardEvent('keydown', { key: 'ArrowRight', bubbles: true }));
    });
    expect(tabs[1].getAttribute('aria-selected')).toBe('true');
    expect(tabs[1].tabIndex).toBe(0);
    expect(container.querySelector('[role="tabpanel"]')?.getAttribute('aria-labelledby')).toBe(
      'ed-terminal-tab-ses-two',
    );
  });
});
