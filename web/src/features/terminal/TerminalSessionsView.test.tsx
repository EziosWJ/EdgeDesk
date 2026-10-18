// @vitest-environment happy-dom
import { act } from 'react';
import { createRoot, type Root } from 'react-dom/client';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

vi.mock('./TerminalPane', () => ({
  TerminalPane: ({ sessionId }: { sessionId: string }) => <div data-testid="terminal-pane">{sessionId}</div>,
}));

import { TerminalSessionsView } from './TerminalSessionsView';
import { api } from '../../services/api';
import { sessionStore } from '../../stores/sessionStore';
import { windowStore } from '../../stores/windowStore';
import type { TerminalSessionInfo } from '../../types';

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

describe('TerminalSessionsView', () => {
  let container: HTMLDivElement;
  let root: Root;

  beforeEach(() => {
    windowStore.reset();
    windowStore.setViewport({ width: 1440, height: 900 });
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
    vi.spyOn(api, 'createTerminal');
    vi.spyOn(api, 'closeTerminal').mockResolvedValue({ ok: true });
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
      root.render(<TerminalSessionsView />);
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

  it('hands a session to a standalone terminal window instead of creating a tab-only view', async () => {
    await render();
    const open = container.querySelector<HTMLButtonElement>(
      '[aria-label="在独立终端窗口中打开 bash · /"]',
    )!;
    act(() => open.dispatchEvent(new MouseEvent('click', { bubbles: true })));

    const terminal = windowStore.getSnapshot().find((item) => item.appId === 'terminal');
    expect(terminal?.payload.sessionId).toBe('ses-one');
    expect(terminal?.title).toBe('bash · /');
  });

  it('focuses an existing standalone window for the same session', async () => {
    const first = windowStore.launch('terminal', {
      title: 'bash · /',
      payload: { sessionId: 'ses-one' },
      instanceKey: 'ses-one',
    });
    await render();
    const open = container.querySelector<HTMLButtonElement>(
      '[aria-label="在独立终端窗口中打开 bash · /"]',
    )!;
    act(() => open.dispatchEvent(new MouseEvent('click', { bubbles: true })));

    const terminals = windowStore.getSnapshot().filter((item) => item.appId === 'terminal');
    expect(terminals).toHaveLength(1);
    expect(terminals[0].id).toBe(first);
  });

  it('explicitly ends a session and closes any window showing it', async () => {
    const terminalWindow = windowStore.launch('terminal', {
      title: 'bash · /',
      payload: { sessionId: 'ses-one' },
      instanceKey: 'ses-one',
    });
    await render();
    const close = container.querySelector<HTMLButtonElement>('[aria-label="结束并销毁 bash · /"]')!;
    act(() => close.dispatchEvent(new MouseEvent('click', { bubbles: true })));
    await act(async () => {
      await new Promise((resolve) => setTimeout(resolve, 0));
    });

    expect(api.closeTerminal).toHaveBeenCalledWith('ses-one');
    expect(windowStore.getSnapshot().some((item) => item.id === terminalWindow)).toBe(false);
  });
});
