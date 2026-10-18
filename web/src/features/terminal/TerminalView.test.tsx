// @vitest-environment happy-dom
import { act } from 'react';
import { createRoot, type Root } from 'react-dom/client';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

vi.mock('./TerminalPane', () => ({
  TerminalPane: ({ sessionId, onMissingSession }: { sessionId: string | null; onMissingSession?: () => void }) => (
    <div data-testid="terminal-pane">
      <span>{sessionId}</span>
      <button type="button" data-testid="emit-missing" onClick={() => onMissingSession?.()}>
        missing
      </button>
    </div>
  ),
}));

import { TerminalView } from './TerminalView';
import { api } from '../../services/api';
import { sessionStore } from '../../stores/sessionStore';
import { windowStore } from '../../stores/windowStore';
import type { TerminalSessionInfo } from '../../types';

const SESSION: TerminalSessionInfo = {
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
};

async function flush(): Promise<void> {
  for (let index = 0; index < 4; index += 1) {
    await act(async () => {
      await new Promise((resolve) => setTimeout(resolve, 0));
    });
  }
}

describe('TerminalView (standalone terminal)', () => {
  let container: HTMLDivElement;
  let root: Root;

  beforeEach(() => {
    windowStore.reset();
    windowStore.setViewport({ width: 1440, height: 900 });
    sessionStore.setSessions([]);
    sessionStore.setShellInfo({ shell: '/bin/bash', shellAvailable: true, maxSessions: 8 });
    vi.spyOn(api, 'createTerminal');
    document.body.innerHTML = '';
    container = document.createElement('div');
    document.body.appendChild(container);
    root = createRoot(container);
  });

  afterEach(() => {
    vi.restoreAllMocks();
    if (root) act(() => root.unmount());
  });

  it('creates one Bash session when opened without a saved session', async () => {
    vi.mocked(api.createTerminal).mockResolvedValue({ ok: true, session: SESSION });
    const windowId = windowStore.launch('terminal');

    act(() => {
      root.render(<TerminalView windowId={windowId} />);
    });
    expect(container.textContent).toContain('正在启动 Bash');
    await flush();

    expect(api.createTerminal).toHaveBeenCalledTimes(1);
    expect(container.querySelector('[data-testid="terminal-pane"]')?.textContent).toContain('ses-one');
    const window = windowStore.getSnapshot().find((item) => item.id === windowId)!;
    expect(window.payload.sessionId).toBe('ses-one');
    expect(window.title).toBe('bash · /');
  });

  it('attaches an existing session without creating a replacement', async () => {
    const windowId = windowStore.launch('terminal', {
      title: 'bash · /',
      payload: { sessionId: 'ses-one' },
      instanceKey: 'ses-one',
    });

    act(() => {
      root.render(<TerminalView initialSessionId="ses-one" windowId={windowId} />);
    });
    await flush();

    expect(api.createTerminal).not.toHaveBeenCalled();
    expect(container.querySelector('[data-testid="terminal-pane"]')?.textContent).toContain('ses-one');
  });

  it('shows an ended state for a missing restored session and only creates on explicit action', async () => {
    act(() => {
      root.render(<TerminalView initialSessionId="gone" restoredSessionMissing />);
    });
    await flush();

    expect(api.createTerminal).not.toHaveBeenCalled();
    expect(container.textContent).toContain('会话已结束或不可恢复');
    expect(container.textContent).toContain('不会自动创建替代会话');

    vi.mocked(api.createTerminal).mockResolvedValue({ ok: true, session: SESSION });
    const button = [...container.querySelectorAll('button')].find((item) => item.textContent?.includes('新建终端会话'))!;
    act(() => button.dispatchEvent(new MouseEvent('click', { bubbles: true })));
    await flush();

    expect(api.createTerminal).toHaveBeenCalledTimes(1);
    expect(container.querySelector('[data-testid="terminal-pane"]')?.textContent).toContain('ses-one');
  });

  it('surfaces a session that disappears later without fabricating a new shell', async () => {
    vi.mocked(api.createTerminal).mockResolvedValue({ ok: true, session: SESSION });
    const windowId = windowStore.launch('terminal');

    act(() => {
      root.render(<TerminalView windowId={windowId} />);
    });
    await flush();

    act(() => {
      container.querySelector<HTMLButtonElement>('[data-testid="emit-missing"]')!.dispatchEvent(
        new MouseEvent('click', { bubbles: true }),
      );
    });

    expect(container.textContent).toContain('会话已结束或不可恢复');
    expect(api.createTerminal).toHaveBeenCalledTimes(1);
  });

  it('reports a creation failure and allows a retry', async () => {
    vi.mocked(api.createTerminal).mockRejectedValueOnce(new Error('Bash 不可用'));
    act(() => {
      root.render(<TerminalView />);
    });
    await flush();

    expect(container.textContent).toContain('终端启动失败');
    expect(container.textContent).toContain('Bash 不可用');

    vi.mocked(api.createTerminal).mockResolvedValue({ ok: true, session: SESSION });
    const retry = [...container.querySelectorAll('button')].find((item) => item.textContent?.includes('重试'))!;
    act(() => retry.dispatchEvent(new MouseEvent('click', { bubbles: true })));
    await flush();
    expect(container.querySelector('[data-testid="terminal-pane"]')?.textContent).toContain('ses-one');
  });
});
