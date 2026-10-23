// @vitest-environment happy-dom
import { afterEach, describe, expect, it, vi } from 'vitest';
import { ApiError } from '../services/api';
import { extensionApi } from './extensionApi';
import { HostBridge } from './hostBridge';
import type { ExtensionSession } from './extensionTypes';

const RUNTIME: ExtensionSession = {
  ok: true,
  token: 'runtime-token',
  app_id: 'io.test.app',
  instance_id: 'instance-1',
  revision_id: 'revision-1',
  version: '1.0.0',
  permissions: [],
  origins: [],
  host_api_version: '1',
  static_base: '/apps/io.test.app/revisions/revision-1/',
  display_name: '测试应用',
};

type RpcFailure = { code: number; message: string; data?: { slug?: string } };

type RpcResponse = {
  id: number;
  result?: unknown;
  error?: RpcFailure;
};

function makeBridge(): HostBridge {
  return new HostBridge({
    iframe: document.createElement('iframe'),
    runtime: RUNTIME,
    app: {
      appId: RUNTIME.app_id,
      name: RUNTIME.display_name,
      instanceId: RUNTIME.instance_id,
      revisionId: RUNTIME.revision_id,
      version: RUNTIME.version,
      staticBase: RUNTIME.static_base,
    },
    onFatal: vi.fn(),
  });
}

/**
 * Drive the real MessagePort request path (dispatch + API error translation +
 * JSON-RPC response) without a browser handshake.
 */
async function rpc(bridge: HostBridge, method: string, params: unknown): Promise<RpcResponse> {
  const responses: RpcResponse[] = [];
  (bridge as unknown as { port: MessagePort }).port = {
    onmessage: null,
    start: () => undefined,
    close: () => undefined,
    postMessage: (payload: unknown) => {
      responses.push(payload as RpcResponse);
    },
  } as unknown as MessagePort;
  await (
    bridge as unknown as { onPortMessage: (event: { data: unknown }) => Promise<void> }
  ).onPortMessage({ data: { jsonrpc: '2.0', id: 1, method, params } });
  return responses[0];
}

const bridges: HostBridge[] = [];

afterEach(() => {
  for (const bridge of bridges.splice(0)) bridge.dispose();
  vi.restoreAllMocks();
});

describe('HostBridge backend.request', () => {
  it('forwards a validated request and returns status, headers and body', async () => {
    const request = vi.spyOn(extensionApi, 'runtimeBackendRequest').mockResolvedValue({
      status: 201,
      headers: { 'content-type': 'application/json', 'x-request-id': 'abc' },
      body: { id: 7 },
    });
    const bridge = makeBridge();
    bridges.push(bridge);

    const outcome = await rpc(bridge, 'backend.request', {
      method: 'POST',
      path: '/items?limit=2',
      body: { name: 'value' },
      contentType: 'application/json',
    });

    expect(request).toHaveBeenCalledWith(RUNTIME.token, {
      method: 'POST',
      path: '/items?limit=2',
      body: { name: 'value' },
      contentType: 'application/json',
    });
    expect(outcome.result).toEqual({
      status: 201,
      headers: { 'content-type': 'application/json', 'x-request-id': 'abc' },
      body: { id: 7 },
    });
  });

  it('sends GET without a body and keeps the token out of the result', async () => {
    const request = vi
      .spyOn(extensionApi, 'runtimeBackendRequest')
      .mockResolvedValue({ status: 200, headers: {}, body: null });
    const bridge = makeBridge();
    bridges.push(bridge);

    const outcome = await rpc(bridge, 'backend.request', { method: 'GET', path: '/' });

    expect(request).toHaveBeenCalledWith(RUNTIME.token, { method: 'GET', path: '/' });
    expect(JSON.stringify(outcome.result)).not.toContain(RUNTIME.token);
  });

  it.each([
    ['unsupported method', { method: 'HEAD', path: '/' }],
    ['lowercase method', { method: 'get', path: '/' }],
    ['missing path', { method: 'GET' }],
    ['relative path', { method: 'GET', path: 'items' }],
    ['protocol-relative path', { method: 'GET', path: '//evil.example/x' }],
    ['path traversal', { method: 'GET', path: '/items/../secret' }],
    ['backslash', { method: 'GET', path: '/items\\secret' }],
    ['unsupported contentType', { method: 'POST', path: '/', contentType: 'text/plain' }],
  ])('rejects %s with invalid params', async (_label, params) => {
    const request = vi.spyOn(extensionApi, 'runtimeBackendRequest');
    const bridge = makeBridge();
    bridges.push(bridge);

    const error = await rpc(bridge, 'backend.request', params);
    expect(error.error?.code).toBe(-32602);
    expect(request).not.toHaveBeenCalled();
  });

  it.each([
    ['backend_not_configured', -32005],
    ['backend_not_running', -32006],
    ['backend_timeout', -32603],
    ['backend_protocol_error', -32603],
    ['backend_unavailable', -32603],
    ['invalid_argument', -32602],
    ['runtime_session_invalid', -32001],
  ])('maps %s to JSON-RPC code %i', async (slug, code) => {
    vi.spyOn(extensionApi, 'runtimeBackendRequest').mockRejectedValueOnce(
      new ApiError(503, slug, 'backend failure'),
    );
    const bridge = makeBridge();
    bridges.push(bridge);

    const response = await rpc(bridge, 'backend.request', { method: 'GET', path: '/' });
    expect(response.error?.code).toBe(code);
    expect(response.error?.data?.slug).toBe(slug);
  });
});
