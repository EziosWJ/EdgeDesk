// @vitest-environment happy-dom
import { act } from 'react';
import { createRoot, type Root } from 'react-dom/client';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { LoginView } from './LoginView';
import { SetupView } from './SetupView';

function jsonResponse(body: unknown, status = 200): Response {
  return new Response(JSON.stringify(body), {
    status,
    headers: { 'Content-Type': 'application/json' },
  });
}

function typeInto(input: HTMLInputElement, value: string): void {
  act(() => {
    const descriptor = Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value');
    if (descriptor?.set) descriptor.set.call(input, value);
    else input.value = value;
    input.dispatchEvent(new Event('input', { bubbles: true }));
  });
}

describe('auth views', () => {
  let container: HTMLDivElement;
  let root: Root;

  beforeEach(() => {
    document.body.innerHTML = '';
    container = document.createElement('div');
    document.body.appendChild(container);
    root = createRoot(container);
  });

  afterEach(() => {
    act(() => root.unmount());
    vi.unstubAllGlobals();
  });

  it('logs in with a password and calls the success callback', async () => {
    const fetchMock = vi.fn(async () => jsonResponse({ ok: true, expires_in: 43200, expires_at: 1 }));
    vi.stubGlobal('fetch', fetchMock);
    const onSuccess = vi.fn();
    act(() => root.render(<LoginView onSuccess={onSuccess} />));
    const input = container.querySelector<HTMLInputElement>('#ed-password')!;
    typeInto(input, 'correct horse battery');
    await act(async () => {
      container.querySelector('form')!.dispatchEvent(new Event('submit', { bubbles: true, cancelable: true }));
    });
    expect(fetchMock).toHaveBeenCalledWith(
      '/api/auth/login',
      expect.objectContaining({ body: JSON.stringify({ password: 'correct horse battery' }) }),
    );
    expect(onSuccess).toHaveBeenCalled();
  });

  it('rejects a mismatched setup confirmation before calling the API', async () => {
    const fetchMock = vi.fn();
    vi.stubGlobal('fetch', fetchMock);
    act(() => root.render(<SetupView onSuccess={() => {}} />));
    typeInto(container.querySelector<HTMLInputElement>('#ed-setup-password')!, 'correct horse battery');
    typeInto(container.querySelector<HTMLInputElement>('#ed-setup-confirm')!, 'different password!!');
    await act(async () => {
      container.querySelector('form')!.dispatchEvent(new Event('submit', { bubbles: true, cancelable: true }));
    });
    expect(fetchMock).not.toHaveBeenCalled();
    expect(container.querySelector('.ed-alert.danger')?.textContent).toContain('不一致');
  });

  it('submits a valid setup request', async () => {
    const fetchMock = vi.fn(async () => jsonResponse({ ok: true, initialized: true }, 201));
    vi.stubGlobal('fetch', fetchMock);
    const onSuccess = vi.fn();
    act(() => root.render(<SetupView onSuccess={onSuccess} />));
    typeInto(container.querySelector<HTMLInputElement>('#ed-setup-password')!, 'correct horse battery');
    typeInto(container.querySelector<HTMLInputElement>('#ed-setup-confirm')!, 'correct horse battery');
    await act(async () => {
      container.querySelector('form')!.dispatchEvent(new Event('submit', { bubbles: true, cancelable: true }));
    });
    expect(fetchMock).toHaveBeenCalledWith(
      '/api/auth/setup',
      expect.objectContaining({
        body: JSON.stringify({
          password: 'correct horse battery',
          confirm_password: 'correct horse battery',
        }),
      }),
    );
    expect(onSuccess).toHaveBeenCalled();
  });
});
