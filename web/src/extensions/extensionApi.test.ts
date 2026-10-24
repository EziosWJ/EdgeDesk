// @vitest-environment happy-dom
import { afterEach, describe, expect, it, vi } from 'vitest';
import { extensionApi } from './extensionApi';
import type { BackendInfo } from './extensionTypes';

const BACKEND: BackendInfo = { present: true, state: 'stopped' };

type Call = { url: string; init?: RequestInit };

function jsonResponse(body: unknown): Response {
  return new Response(JSON.stringify(body), {
    status: 200,
    headers: { 'Content-Type': 'application/json' },
  });
}

function stub(body: unknown = { ok: true, backend: BACKEND }): Call[] {
  const calls: Call[] = [];
  vi.stubGlobal('fetch', async (url: string, init?: RequestInit) => {
    calls.push({ url, init });
    return jsonResponse(body);
  });
  return calls;
}

afterEach(() => {
  vi.unstubAllGlobals();
});

describe('extensionApi native backend endpoints', () => {
  const appId = 'io.test/app';

  it('reads backend status with the app id encoded in the path', async () => {
    const calls = stub();
    await extensionApi.getBackend(appId);
    expect(calls[0].url).toBe('/api/extensions/io.test%2Fapp/backend');
    expect(calls[0].init?.method).toBeUndefined();
  });

  it('saves identity, autostart and lifecycle actions', async () => {
    let calls = stub();
    await extensionApi.setBackendIdentity(appId, { run_user: 'u', run_group: 'g', allow_root: true });
    await extensionApi.setBackendAutostart(appId, true);
    expect(calls.map((call) => [call.url, call.init?.method])).toEqual([
      ['/api/extensions/io.test%2Fapp/backend/identity', 'PUT'],
      ['/api/extensions/io.test%2Fapp/backend/autostart', 'PUT'],
    ]);
    expect(JSON.parse(String(calls[0].init?.body))).toEqual({
      run_user: 'u',
      run_group: 'g',
      allow_root: true,
    });
    expect(JSON.parse(String(calls[1].init?.body))).toEqual({ enabled: true });

    calls = stub();
    await extensionApi.startBackend(appId);
    await extensionApi.stopBackend(appId);
    await extensionApi.restartBackend(appId);
    expect(calls.map((call) => call.url)).toEqual([
      '/api/extensions/io.test%2Fapp/backend/start',
      '/api/extensions/io.test%2Fapp/backend/stop',
      '/api/extensions/io.test%2Fapp/backend/restart',
    ]);
    expect(calls.every((call) => call.init?.method === 'POST')).toBe(true);
  });

  it('purges data only with an explicit confirm payload', async () => {
    const calls = stub({ ok: true, purged: true });
    await extensionApi.purgeAppData(appId);
    expect(calls[0].url).toBe('/api/extensions/io.test%2Fapp/data/purge');
    expect(calls[0].init?.method).toBe('POST');
    expect(JSON.parse(String(calls[0].init?.body))).toEqual({ confirm: true, app_id: appId });
  });

  it('posts the runtime contract with the session token in header and body', async () => {
    const calls = stub({ status: 200, headers: { 'content-type': 'application/json' }, body: { ok: true } });
    const response = await extensionApi.runtimeBackendRequest('token-1', {
      method: 'PATCH',
      path: '/items/1',
      body: { name: 'x' },
      contentType: 'application/json',
    });

    expect(calls[0].url).toBe('/api/extensions/runtime/backend/request');
    expect(calls[0].init?.method).toBe('POST');
    expect(new Headers(calls[0].init?.headers).get('X-EdgeDesk-Runtime')).toBe('token-1');
    expect(JSON.parse(String(calls[0].init?.body))).toEqual({
      token: 'token-1',
      method: 'PATCH',
      path: '/items/1',
      body: { name: 'x' },
      contentType: 'application/json',
    });
    expect(response.body).toEqual({ ok: true });
  });
});
