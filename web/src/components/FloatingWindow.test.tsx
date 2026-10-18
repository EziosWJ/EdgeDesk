// @vitest-environment happy-dom
import { act } from 'react';
import { createRoot, type Root } from 'react-dom/client';
import { beforeEach, describe, expect, it, type MockInstance } from 'vitest';
import { FloatingWindow } from './FloatingWindow';
import { windowStore, useWindows } from '../stores/windowStore';

function Harness() {
  const windows = useWindows();
  return (
    <>
      {windows.map((item) => (
        <FloatingWindow key={item.id} window={item}>
          <div className="ed-test-content">content</div>
        </FloatingWindow>
      ))}
    </>
  );
}

function pointerEvent(type: string, init: { clientX: number; clientY: number; pointerId: number }): Event {
  const ctor = (globalThis as unknown as { PointerEvent?: typeof Event }).PointerEvent;
  if (ctor) return new ctor(type, { ...init, bubbles: true });
  const event = new Event(type, { bubbles: true });
  Object.assign(event, init);
  return event;
}

describe('FloatingWindow interactions', () => {
  let container: HTMLDivElement;
  let root: Root;
  let capture: MockInstance | null = null;

  beforeEach(() => {
    windowStore.closeAll();
    document.body.innerHTML = '';
    container = document.createElement('div');
    document.body.appendChild(container);
    root = createRoot(container);
    // happy-dom does not implement pointer capture; the window code calls it.
    const element = Element.prototype as unknown as {
      setPointerCapture?: (id: number) => void;
      releasePointerCapture?: (id: number) => void;
    };
    if (!element.setPointerCapture) {
      capture = null;
      element.setPointerCapture = () => {};
      element.releasePointerCapture = () => {};
    }
  });

  const render = () => {
    act(() => {
      root.render(<Harness />);
    });
  };

  it('drags the window through the title bar', () => {
    const id = windowStore.open({ kind: 'editor', title: '编辑 /a.txt', payload: { path: '/a.txt' } });
    render();
    const before = windowStore.getSnapshot().find((item) => item.id === id)!;
    const titleBar = container.querySelector('.ed-window-title')!;
    act(() => {
      titleBar.dispatchEvent(pointerEvent('pointerdown', { clientX: 100, clientY: 100, pointerId: 1 }));
      titleBar.dispatchEvent(pointerEvent('pointermove', { clientX: 180, clientY: 140, pointerId: 1 }));
      titleBar.dispatchEvent(pointerEvent('pointerup', { clientX: 180, clientY: 140, pointerId: 1 }));
    });
    const after = windowStore.getSnapshot().find((item) => item.id === id)!;
    expect(after.x).toBe(before.x + 80);
    expect(after.y).toBe(before.y + 40);
    expect(capture).toBeNull();
  });

  it('resizes the window from the bottom-right handle', () => {
    const id = windowStore.open({ kind: 'terminal', title: 'Bash 1', payload: { sessionId: 's1' } });
    render();
    const before = windowStore.getSnapshot().find((item) => item.id === id)!;
    const handle = container.querySelector('.ed-window-resize')!;
    act(() => {
      handle.dispatchEvent(pointerEvent('pointerdown', { clientX: 0, clientY: 0, pointerId: 2 }));
      handle.dispatchEvent(pointerEvent('pointermove', { clientX: 60, clientY: 90, pointerId: 2 }));
      handle.dispatchEvent(pointerEvent('pointerup', { clientX: 60, clientY: 90, pointerId: 2 }));
    });
    const after = windowStore.getSnapshot().find((item) => item.id === id)!;
    expect(after.width).toBeGreaterThanOrEqual(before.width + 59);
    expect(after.height).toBeGreaterThanOrEqual(before.height + 89);
  });

  it('minimizes, maximizes, restores and closes through window buttons', () => {
    const id = windowStore.open({ kind: 'preview', title: '预览 /x', payload: { path: '/x' } });
    render();
    const buttons = container.querySelectorAll<HTMLButtonElement>('.ed-window-actions button');
    expect(buttons).toHaveLength(3);
    act(() => buttons[0].click());
    expect(windowStore.getSnapshot().find((item) => item.id === id)?.state).toBe('minimized');
    render();
    // Minimized windows stay mounted (hidden) so editor/terminal state survives.
    const minimized = container.querySelector<HTMLElement>('.ed-window');
    expect(minimized).not.toBeNull();
    expect(minimized?.classList.contains('ed-window--minimized')).toBe(true);
    expect(minimized?.style.display).toBe('none');
    expect(container.querySelector('.ed-test-content')).not.toBeNull();
    // restore via focus (Dock/taskbar behaviour)
    act(() => windowStore.focus(id));
    render();
    const buttonsAgain = container.querySelectorAll<HTMLButtonElement>('.ed-window-actions button');
    act(() => buttonsAgain[1].click());
    expect(windowStore.getSnapshot().find((item) => item.id === id)?.state).toBe('maximized');
    render();
    expect(container.querySelector('.ed-window.maximized')).not.toBeNull();
    const buttonsMax = container.querySelectorAll<HTMLButtonElement>('.ed-window-actions button');
    act(() => buttonsMax[2].click());
    expect(windowStore.getSnapshot()).toHaveLength(0);
  });

  it('uses the editor window title as the accessible name', () => {
    windowStore.open({ kind: 'editor', title: '编辑 /etc/hostname', payload: { path: '/etc/hostname' } });
    render();
    expect(container.querySelector('[role="dialog"]')?.getAttribute('aria-label')).toBe('编辑 /etc/hostname');
  });
});
