import { ApiError } from '../services/api';
import { extensionApi } from './extensionApi';
import type { ExtensionSession } from './extensionTypes';

// Host side of the EdgeDesk extension bridge.
//
// The iframe is sandboxed without allow-same-origin, so no host cookie,
// localStorage or DOM is reachable from application code. The host performs a
// postMessage handshake bound to the exact WindowProxy, then transfers a
// dedicated MessagePort. Every privileged call is checked against the runtime
// permission snapshot and again by the C++ runtime endpoints.

const PROTOCOL_VERSION = 1;
const HANDSHAKE_TIMEOUT_MS = 15_000;
const RPC_ERROR = {
  invalidRequest: -32600,
  methodNotFound: -32601,
  invalidParams: -32602,
  internal: -32603,
  permissionDenied: -32000,
  sessionInvalid: -32001,
  fileLocked: -32002,
  cursorStale: -32003,
  quotaExceeded: -32004,
  notFound: -32005,
  conflict: -32006,
  transferError: -32007,
  tooLarge: -32008,
  unsupported: -32010,
  ioError: -32011,
} as const;

type RuntimeAppInfo = {
  appId: string;
  name: string;
  instanceId: string;
  revisionId: string;
  version: string;
  staticBase: string;
  iconUrl?: string;
};

type BridgeOptions = {
  iframe: HTMLIFrameElement;
  runtime: ExtensionSession;
  app: RuntimeAppInfo;
  onFatal: (message: string) => void;
  onReady?: () => void;
  onAppError?: (type: string, message: string) => void;
  onEvent?: (name: string, data: unknown) => void;
};

type JsonRpcId = string | number;

class RpcError extends Error {
  code: number;
  data?: unknown;
  constructor(code: number, message: string, data?: unknown) {
    super(message);
    this.code = code;
    this.data = data;
  }
}

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === 'object' && value !== null && !Array.isArray(value);
}

function randomToken(bytes = 16): string {
  const buffer = new Uint8Array(bytes);
  crypto.getRandomValues(buffer);
  return [...buffer].map((value) => value.toString(16).padStart(2, '0')).join('');
}

function errorFromApi(error: unknown): RpcError {
  if (error instanceof RpcError) return error;
  if (error instanceof ApiError) {
    const map: Record<string, number> = {
      permission_denied: RPC_ERROR.permissionDenied,
      runtime_session_invalid: RPC_ERROR.sessionInvalid,
      file_locked: RPC_ERROR.fileLocked,
      cursor_stale: RPC_ERROR.cursorStale,
      quota_exceeded: RPC_ERROR.quotaExceeded,
      not_found: RPC_ERROR.notFound,
      app_not_found: RPC_ERROR.notFound,
      backend_not_configured: RPC_ERROR.notFound,
      backend_not_running: RPC_ERROR.conflict,
      conflict: RPC_ERROR.conflict,
      invalid_argument: RPC_ERROR.invalidParams,
      too_large: RPC_ERROR.tooLarge,
      unsupported: RPC_ERROR.unsupported,
      io_error: RPC_ERROR.ioError,
    };
    const code = map[error.code] ?? (error.status === 401 ? RPC_ERROR.sessionInvalid : RPC_ERROR.internal);
    return new RpcError(code, error.message, { slug: error.code, status: error.status });
  }
  return new RpcError(RPC_ERROR.internal, error instanceof Error ? error.message : 'internal error');
}

const BACKEND_METHODS = new Set(['GET', 'POST', 'PUT', 'PATCH', 'DELETE']);
const BACKEND_PATH_MAX = 2048;

/** Reject anything that is not an application-relative HTTP path. */
function validateBackendPath(path: string): void {
  if (!path.startsWith('/') || path.startsWith('//')) {
    throw new RpcError(RPC_ERROR.invalidParams, 'path must be an application-relative path starting with /');
  }
  if (path.length > BACKEND_PATH_MAX) {
    throw new RpcError(RPC_ERROR.invalidParams, 'path is too long');
  }
  if (path.includes('\\') || /[\u0000-\u001f\u007f]/.test(path)) {
    throw new RpcError(RPC_ERROR.invalidParams, 'path contains invalid characters');
  }
  if (path.split('?')[0].split('/').some((segment) => segment === '..' || segment === '.')) {
    throw new RpcError(RPC_ERROR.invalidParams, 'path traversal is not allowed');
  }
}

export class HostBridge {
  private readonly options: BridgeOptions;
  private messageListener: ((event: MessageEvent) => void) | null = null;
  private port: MessagePort | null = null;
  private handshakeTimer: number | null = null;
  private initialized = false;
  private disposed = false;
  private handshakeToken = '';
  private readonly transfers = new Set<MessagePort>();
  private readonly terminals = new Map<string, WebSocket>();
  private readonly subscriptions = new Set<string>();
  private readonly listeners: Array<() => void> = [];
  private themeObserver: MutationObserver | null = null;

  constructor(options: BridgeOptions) {
    this.options = options;
  }

  start(): void {
    this.messageListener = (event: MessageEvent) => this.onWindowMessage(event);
    window.addEventListener('message', this.messageListener);
    this.handshakeTimer = window.setTimeout(() => {
      if (!this.initialized) this.fatal('应用未在超时时间内完成沙箱握手。');
    }, HANDSHAKE_TIMEOUT_MS);
  }

  private fatal(message: string): void {
    if (this.disposed) return;
    this.options.onFatal(message);
  }

  private onWindowMessage(event: MessageEvent): void {
    if (this.disposed || this.initialized) return;
    const iframeWindow = this.options.iframe.contentWindow;
    if (!iframeWindow || event.source !== iframeWindow) return;
    // A sandboxed iframe without allow-same-origin reports an opaque origin.
    if (event.origin !== 'null') return;
    const data = event.data;
    if (!isRecord(data) || data.type !== 'edgedesk:hello' || data.protocol !== PROTOCOL_VERSION) return;
    this.initialized = true;
    if (this.handshakeTimer !== null) {
      window.clearTimeout(this.handshakeTimer);
      this.handshakeTimer = null;
    }
    this.handshakeToken = randomToken(24);
    const channel = new MessageChannel();
    this.port = channel.port1;
    this.port.onmessage = (message) => this.onPortMessage(message);
    this.port.start();
    const init = {
      type: 'edgedesk:init',
      protocol: PROTOCOL_VERSION,
      token: this.handshakeToken,
      app: {
        appId: this.options.app.appId,
        name: this.options.app.name,
        instanceId: this.options.app.instanceId,
        revisionId: this.options.app.revisionId,
        version: this.options.app.version,
        staticBase: this.options.app.staticBase,
        iconUrl: this.options.app.iconUrl,
      },
      hostApiVersion: this.options.runtime.host_api_version,
      permissions: this.options.runtime.permissions,
      networkOrigins: this.options.runtime.origins,
      window: this.options.runtime.window ?? {},
    };
    iframeWindow.postMessage(init, '*', [channel.port2]);
    this.installHostEvents();
    this.options.onReady?.();
  }

  private hasPermission(permission: string): boolean {
    return this.options.runtime.permissions.includes(permission);
  }

  private requirePermission(permission: string): void {
    if (!this.hasPermission(permission)) {
      throw new RpcError(RPC_ERROR.permissionDenied, `permission is not granted: ${permission}`, {
        slug: 'permission_denied',
      });
    }
  }

  private async onPortMessage(event: MessageEvent): Promise<void> {
    if (this.disposed) return;
    const request = event.data;
    if (!isRecord(request) || request.jsonrpc !== '2.0' || typeof request.method !== 'string') {
      this.respond(null, undefined, new RpcError(RPC_ERROR.invalidRequest, 'invalid JSON-RPC request'));
      return;
    }
    const id = request.id;
    const notification = id === undefined;
    if (!notification && typeof id !== 'string' && typeof id !== 'number') {
      this.respond(null, undefined, new RpcError(RPC_ERROR.invalidRequest, 'invalid request id'));
      return;
    }
    try {
      const outcome = await this.dispatch(request.method, request.params);
      if (!notification) this.respond(id as JsonRpcId, outcome.result, undefined, outcome.transfer);
    } catch (error) {
      const rpcError = errorFromApi(error);
      if (rpcError.code === RPC_ERROR.sessionInvalid) {
        this.fatal('应用运行会话已失效（应用可能已被禁用、升级或隔离）。');
      }
      if (!notification) {
        this.respond(id as JsonRpcId, undefined, rpcError);
      } else if (request.method === 'platform.reportError') {
        // Notifications have no response; errors are intentionally dropped.
      }
    }
  }

  private respond(
    id: JsonRpcId | null,
    result?: unknown,
    error?: RpcError,
    transfer: MessagePort[] = [],
  ): void {
    if (!this.port || this.disposed) return;
    const payload =
      error !== undefined
        ? {
            jsonrpc: '2.0',
            id,
            error: { code: error.code, message: error.message, ...(error.data ? { data: error.data } : {}) },
          }
        : { jsonrpc: '2.0', id, result: result ?? null };
    this.port.postMessage(payload, transfer);
  }

  private notify(method: string, params: unknown): void {
    if (!this.port || this.disposed) return;
    this.port.postMessage({ jsonrpc: '2.0', method, params });
  }

  private params(raw: unknown): Record<string, unknown> {
    if (raw === undefined) return {};
    if (!isRecord(raw)) throw new RpcError(RPC_ERROR.invalidParams, 'params must be an object');
    return raw;
  }

  private stringParam(body: Record<string, unknown>, name: string, fallback = ''): string {
    const value = body[name];
    if (value === undefined) return fallback;
    if (typeof value !== 'string') throw new RpcError(RPC_ERROR.invalidParams, `${name} must be a string`);
    return value;
  }

  private numberParam(body: Record<string, unknown>, name: string, fallback: number): number {
    const value = body[name];
    if (value === undefined) return fallback;
    if (typeof value !== 'number' || !Number.isFinite(value)) {
      throw new RpcError(RPC_ERROR.invalidParams, `${name} must be a number`);
    }
    return value;
  }

  private async dispatch(
    method: string,
    rawParams: unknown,
  ): Promise<{ result: unknown; transfer?: MessagePort[] }> {
    const token = this.options.runtime.token;
    switch (method) {
      case 'platform.getInfo':
        return {
          result: {
            hostApiVersion: this.options.runtime.host_api_version,
            appId: this.options.app.appId,
            instanceId: this.options.app.instanceId,
            revisionId: this.options.app.revisionId,
            version: this.options.app.version,
          },
        };
      case 'platform.reportError': {
        const body = this.params(rawParams);
        const type = this.stringParam(body, 'type', 'error').slice(0, 120);
        const message = this.stringParam(body, 'message', '').slice(0, 1024);
        const stack = this.stringParam(body, 'stack', '').slice(0, 2048);
        this.options.onAppError?.(type, message);
        await extensionApi.reportError(token, type, message, stack);
        return { result: { ok: true } };
      }
      case 'auth.getStatus': {
        const status = await extensionApi.authStatus();
        return { result: { authenticated: status.authenticated, method: status.method } };
      }
      case 'backend.request': {
        // No manifest permission: an application may only reach its own backend.
        // The runtime token stays host-side and identifies the app on the server.
        const body = this.params(rawParams);
        const method = this.stringParam(body, 'method');
        if (!BACKEND_METHODS.has(method)) {
          throw new RpcError(RPC_ERROR.invalidParams, `method must be one of ${[...BACKEND_METHODS].join(', ')}`);
        }
        const path = this.stringParam(body, 'path');
        validateBackendPath(path);
        if (body.contentType !== undefined && body.contentType !== 'application/json') {
          throw new RpcError(RPC_ERROR.invalidParams, 'contentType must be application/json');
        }
        const response = await extensionApi.runtimeBackendRequest(token, {
          method,
          path,
          ...(Object.prototype.hasOwnProperty.call(body, 'body') ? { body: body.body } : {}),
          ...(body.contentType === 'application/json' ? { contentType: 'application/json' } : {}),
        });
        return {
          result: {
            status: response.status,
            headers: response.headers ?? {},
            body: response.body ?? null,
          },
        };
      }
      case 'storage.get': {
        this.requirePermission('storage');
        const body = this.params(rawParams);
        const response = await extensionApi.runtimePost<{
          value: unknown;
          size_bytes: number;
          updated_at: number;
        }>(token, 'storage/get', { key: this.stringParam(body, 'key') });
        return {
          result: { key: this.stringParam(body, 'key'), value: response.value, sizeBytes: response.size_bytes, updatedAt: response.updated_at },
        };
      }
      case 'storage.set': {
        this.requirePermission('storage');
        const body = this.params(rawParams);
        if (!('value' in body)) throw new RpcError(RPC_ERROR.invalidParams, 'value is required');
        const response = await extensionApi.runtimePost<{ size_bytes: number; usage_bytes: number }>(
          token,
          'storage/set',
          { key: this.stringParam(body, 'key'), value: body.value },
        );
        return { result: { key: this.stringParam(body, 'key'), sizeBytes: response.size_bytes, usageBytes: response.usage_bytes } };
      }
      case 'storage.delete': {
        this.requirePermission('storage');
        const body = this.params(rawParams);
        const response = await extensionApi.runtimePost<{ deleted: boolean; usage_bytes: number }>(
          token,
          'storage/delete',
          { key: this.stringParam(body, 'key') },
        );
        return { result: { key: this.stringParam(body, 'key'), deleted: response.deleted, usageBytes: response.usage_bytes } };
      }
      case 'storage.list': {
        this.requirePermission('storage');
        const body = this.params(rawParams);
        const response = await extensionApi.runtimePost<{
          entries: Array<{ key: string; size_bytes: number; updated_at: number }>;
          next_cursor: string;
          usage_bytes: number;
          quota_bytes: number;
        }>(token, 'storage/list', {
          cursor: this.stringParam(body, 'cursor'),
          limit: this.numberParam(body, 'limit', 100),
        });
        return {
          result: {
            entries: response.entries.map((entry) => ({
              key: entry.key,
              sizeBytes: entry.size_bytes,
              updatedAt: entry.updated_at,
            })),
            nextCursor: response.next_cursor,
            usageBytes: response.usage_bytes,
            quotaBytes: response.quota_bytes,
          },
        };
      }
      case 'storage.transaction': {
        this.requirePermission('storage');
        const body = this.params(rawParams);
        if (!Array.isArray(body.operations)) {
          throw new RpcError(RPC_ERROR.invalidParams, 'operations must be an array');
        }
        const response = await extensionApi.runtimePost<{ applied: number; usage_bytes: number }>(
          token,
          'storage/transaction',
          { operations: body.operations },
        );
        return { result: { applied: response.applied, usageBytes: response.usage_bytes } };
      }
      case 'files.list': {
        this.requirePermission('files.manage');
        const body = this.params(rawParams);
        const response = await extensionApi.runtimePost<{
          path: string;
          entries: unknown[];
          next_cursor: string;
          sort: string;
        }>(token, 'files/list', {
          path: this.stringParam(body, 'path', '/'),
          cursor: this.stringParam(body, 'cursor'),
          limit: this.numberParam(body, 'limit', 100),
        });
        return {
          result: { path: response.path, entries: response.entries, nextCursor: response.next_cursor, sort: response.sort },
        };
      }
      case 'files.stat': {
        this.requirePermission('files.manage');
        const body = this.params(rawParams);
        const response = await extensionApi.runtimePost<{ entry: unknown }>(token, 'files/stat', {
          path: this.stringParam(body, 'path'),
        });
        return { result: response.entry };
      }
      case 'files.read': {
        this.requirePermission('files.manage');
        const body = this.params(rawParams);
        return {
          result: await extensionApi.runtimePost(token, 'files/read', {
            path: this.stringParam(body, 'path'),
          }),
        };
      }
      case 'files.mkdir': {
        this.requirePermission('files.manage');
        const body = this.params(rawParams);
        return {
          result: await extensionApi.runtimePost(token, 'files/mkdir', {
            path: this.stringParam(body, 'path'),
          }),
        };
      }
      case 'files.rename': {
        this.requirePermission('files.manage');
        const body = this.params(rawParams);
        return {
          result: await extensionApi.runtimePost(token, 'files/rename', {
            from: this.stringParam(body, 'from'),
            to: this.stringParam(body, 'to'),
            overwrite: body.overwrite === true,
          }),
        };
      }
      case 'files.delete': {
        this.requirePermission('files.manage');
        const body = this.params(rawParams);
        if (!Array.isArray(body.paths) || !body.paths.every((item) => typeof item === 'string')) {
          throw new RpcError(RPC_ERROR.invalidParams, 'paths must be an array of strings');
        }
        return { result: await extensionApi.runtimePost(token, 'files/delete', { paths: body.paths }) };
      }
      case 'files.upload.begin': {
        this.requirePermission('files.manage');
        const body = this.params(rawParams);
        const path = this.stringParam(body, 'path');
        const size = this.numberParam(body, 'size', -1);
        if (!path || size < 0) throw new RpcError(RPC_ERROR.invalidParams, 'path and non-negative size are required');
        const response = await extensionApi.runtimePost<{
          transfer_id: string;
          chunk_size: number;
          total_size: number;
        }>(token, 'files/upload/begin', { path, size });
        return this.startUpload(path, response.transfer_id, response.total_size, response.chunk_size);
      }
      case 'files.upload.cancel': {
        this.requirePermission('files.manage');
        const body = this.params(rawParams);
        await extensionApi.runtimePost(token, 'files/upload/cancel', {
          transfer_id: this.stringParam(body, 'transferId'),
        });
        return { result: { ok: true } };
      }
      case 'files.download.begin': {
        this.requirePermission('files.manage');
        const body = this.params(rawParams);
        const path = this.stringParam(body, 'path');
        if (!path) throw new RpcError(RPC_ERROR.invalidParams, 'path is required');
        const response = await extensionApi.runtimePost<{
          transfer_id: string;
          chunk_size: number;
          total_size: number;
        }>(token, 'files/download/begin', { path });
        return this.startDownload(path, response.transfer_id, response.total_size, response.chunk_size);
      }
      case 'files.download.cancel': {
        this.requirePermission('files.manage');
        const body = this.params(rawParams);
        await extensionApi.runtimePost(token, 'files/download/cancel', {
          transfer_id: this.stringParam(body, 'transferId'),
        });
        return { result: { ok: true } };
      }
      case 'terminal.sessions.list': {
        this.requirePermission('terminal.access');
        const response = await extensionApi.runtimePost<{ sessions: unknown[]; shell: string; shell_available: boolean; max_sessions: number }>(
          token,
          'terminal/sessions',
        );
        return { result: response };
      }
      case 'terminal.sessions.create': {
        this.requirePermission('terminal.access');
        const body = this.params(rawParams);
        const response = await extensionApi.runtimePost<{ session: { id: string } }>(
          token,
          'terminal/sessions',
          {
            title: this.stringParam(body, 'title', ''),
            cwd: this.stringParam(body, 'cwd', ''),
            cols: this.numberParam(body, 'cols', 100),
            rows: this.numberParam(body, 'rows', 30),
          },
        );
        await this.attachTerminal(response.session.id);
        return { result: response.session };
      }
      case 'terminal.sessions.input': {
        this.requirePermission('terminal.access');
        const body = this.params(rawParams);
        const sessionId = this.stringParam(body, 'sessionId');
        const data = this.stringParam(body, 'data');
        const socket = this.terminals.get(sessionId);
        if (!socket || socket.readyState !== WebSocket.OPEN) {
          throw new RpcError(RPC_ERROR.notFound, 'terminal session is not attached');
        }
        socket.send(JSON.stringify({ type: 'input', data }));
        return { result: { ok: true } };
      }
      case 'terminal.sessions.resize': {
        this.requirePermission('terminal.access');
        const body = this.params(rawParams);
        const sessionId = this.stringParam(body, 'sessionId');
        const socket = this.terminals.get(sessionId);
        if (!socket || socket.readyState !== WebSocket.OPEN) {
          throw new RpcError(RPC_ERROR.notFound, 'terminal session is not attached');
        }
        socket.send(
          JSON.stringify({
            type: 'resize',
            cols: this.numberParam(body, 'cols', 80),
            rows: this.numberParam(body, 'rows', 24),
          }),
        );
        return { result: { ok: true } };
      }
      case 'terminal.sessions.close': {
        this.requirePermission('terminal.access');
        const body = this.params(rawParams);
        const sessionId = this.stringParam(body, 'sessionId');
        this.terminals.get(sessionId)?.close();
        this.terminals.delete(sessionId);
        await extensionApi.runtimePost(token, 'terminal/close', { session_id: sessionId });
        return { result: { ok: true } };
      }
      case 'events.subscribe': {
        const body = this.params(rawParams);
        if (!Array.isArray(body.events) || !body.events.every((item) => typeof item === 'string')) {
          throw new RpcError(RPC_ERROR.invalidParams, 'events must be an array of strings');
        }
        const allowed = new Set(['visibility', 'theme', 'window.focus']);
        const accepted: string[] = [];
        for (const name of body.events as string[]) {
          if (!allowed.has(name)) throw new RpcError(RPC_ERROR.invalidParams, `unknown event: ${name}`);
          this.subscriptions.add(name);
          accepted.push(name);
        }
        return { result: { subscribed: accepted } };
      }
      case 'events.unsubscribe': {
        const body = this.params(rawParams);
        if (!Array.isArray(body.events)) {
          throw new RpcError(RPC_ERROR.invalidParams, 'events must be an array');
        }
        for (const name of body.events) {
          if (typeof name === 'string') this.subscriptions.delete(name);
        }
        return { result: { ok: true } };
      }
      default:
        throw new RpcError(RPC_ERROR.methodNotFound, `method not found: ${method}`);
    }
  }

  private startUpload(
    path: string,
    transferId: string,
    totalSize: number,
    chunkSize: number,
  ): { result: unknown; transfer: MessagePort[] } {
    const channel = new MessageChannel();
    this.transfers.add(channel.port1);
    const token = this.options.runtime.token;
    let received = 0;
    let chain = Promise.resolve();
    const fail = (error: unknown) => {
      const rpcError = errorFromApi(error);
      channel.port1.postMessage({ type: 'error', code: rpcError.code, message: rpcError.message });
      channel.port1.close();
      this.transfers.delete(channel.port1);
      void extensionApi.runtimePost(token, 'files/upload/cancel', { transfer_id: transferId }).catch(() => undefined);
    };
    channel.port1.onmessage = (event) => {
      const message = event.data;
      if (!isRecord(message)) return;
      chain = chain
        .then(async () => {
          if (message.type === 'chunk') {
            const data = message.data;
            if (!(data instanceof ArrayBuffer)) throw new RpcError(RPC_ERROR.invalidParams, 'chunk data must be an ArrayBuffer');
            if (data.byteLength < 1 || data.byteLength > chunkSize) {
              throw new RpcError(RPC_ERROR.invalidParams, 'chunk must contain 1..1048576 bytes');
            }
            const offset = Number(message.offset);
            if (!Number.isSafeInteger(offset) || offset !== received) {
              throw new RpcError(RPC_ERROR.invalidParams, 'chunk offset is not sequential');
            }
            if (offset + data.byteLength > totalSize) {
              throw new RpcError(RPC_ERROR.invalidParams, 'chunk exceeds the declared length');
            }
            const response = await extensionApi.runtimeUploadChunk(token, transferId, offset, data);
            received = response.received;
            channel.port1.postMessage({ type: 'ack', offset, received });
          } else if (message.type === 'complete') {
            if (received !== totalSize) {
              throw new RpcError(RPC_ERROR.transferError, 'upload length does not match the declared size');
            }
            const response = await extensionApi.runtimePost<{ entry: unknown }>(token, 'files/upload/complete', {
              transfer_id: transferId,
              expected_size: totalSize,
            });
            channel.port1.postMessage({ type: 'done', entry: response.entry });
            channel.port1.close();
            this.transfers.delete(channel.port1);
          } else if (message.type === 'cancel') {
            await extensionApi.runtimePost(token, 'files/upload/cancel', { transfer_id: transferId });
            channel.port1.postMessage({ type: 'cancelled' });
            channel.port1.close();
            this.transfers.delete(channel.port1);
          } else {
            throw new RpcError(RPC_ERROR.invalidParams, 'unknown transfer message');
          }
        })
        .catch(fail);
    };
    channel.port1.start();
    return {
      result: { transferId, totalSize, chunkSize, path, port: channel.port2 },
      transfer: [channel.port2],
    };
  }

  private startDownload(
    path: string,
    transferId: string,
    totalSize: number,
    chunkSize: number,
  ): { result: unknown; transfer: MessagePort[] } {
    const channel = new MessageChannel();
    this.transfers.add(channel.port1);
    const token = this.options.runtime.token;
    let offset = 0;
    let closed = false;
    const close = () => {
      if (closed) return;
      closed = true;
      channel.port1.close();
      this.transfers.delete(channel.port1);
    };
    const fail = (error: unknown) => {
      const rpcError = errorFromApi(error);
      channel.port1.postMessage({ type: 'error', code: rpcError.code, message: rpcError.message });
      close();
      void extensionApi.runtimePost(token, 'files/download/cancel', { transfer_id: transferId }).catch(() => undefined);
    };
    const sendNext = () => {
      if (closed) return;
      if (offset >= totalSize) {
        void extensionApi
          .runtimePost(token, 'files/download/complete', { transfer_id: transferId })
          .then(() => {
            channel.port1.postMessage({ type: 'done', totalSize });
            close();
          })
          .catch(fail);
        return;
      }
      void extensionApi
        .runtimeDownloadChunk(token, transferId, offset)
        .then((response) => {
          if (closed) return;
          const data = response.data;
          if (data.byteLength === 0 && !response.eof) {
            throw new RpcError(RPC_ERROR.ioError, 'empty download chunk');
          }
          offset += data.byteLength;
          channel.port1.postMessage({ type: 'chunk', data, offset, eof: response.eof }, [data]);
        })
        .catch(fail);
    };
    channel.port1.onmessage = (event) => {
      const message = event.data;
      if (!isRecord(message)) return;
      if (message.type === 'start') {
        sendNext();
      } else if (message.type === 'ack') {
        if (Number(message.offset) !== offset) {
          fail(new RpcError(RPC_ERROR.transferError, 'download acknowledgement offset mismatch'));
          return;
        }
        sendNext();
      } else if (message.type === 'cancel') {
        void extensionApi.runtimePost(token, 'files/download/cancel', { transfer_id: transferId }).catch(() => undefined);
        channel.port1.postMessage({ type: 'cancelled' });
        close();
      }
    };
    channel.port1.start();
    return {
      result: { transferId, totalSize, chunkSize, path, port: channel.port2 },
      transfer: [channel.port2],
    };
  }

  private attachTerminal(sessionId: string): Promise<void> {
    return new Promise((resolve, reject) => {
      const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
      const socket = new WebSocket(
        `${protocol}//${window.location.host}/ws/terminal?session_id=${encodeURIComponent(sessionId)}`,
      );
      const timer = window.setTimeout(() => {
        socket.close();
        reject(new RpcError(RPC_ERROR.ioError, 'terminal websocket did not open'));
      }, 8000);
      socket.onopen = () => {
        window.clearTimeout(timer);
        this.terminals.set(sessionId, socket);
        resolve();
      };
      socket.onmessage = (event) => {
        let message: unknown;
        try {
          message = JSON.parse(String(event.data));
        } catch {
          return;
        }
        if (!isRecord(message) || typeof message.type !== 'string') return;
        if (message.type === 'output' && typeof message.data === 'string') {
          this.notify('terminal.output', { sessionId, data: message.data });
        } else if (message.type === 'exit') {
          this.notify('terminal.exit', { sessionId, code: Number(message.code ?? 0) });
        } else if (message.type === 'guard') {
          this.notify('terminal.guard', { sessionId, reason: String(message.reason ?? '') });
        } else if (message.type === 'error') {
          this.notify('terminal.error', {
            sessionId,
            code: String(message.code ?? 'terminal_error'),
            message: String(message.message ?? ''),
          });
        }
      };
      socket.onerror = () => {
        window.clearTimeout(timer);
        reject(new RpcError(RPC_ERROR.ioError, 'terminal websocket failed'));
      };
      socket.onclose = () => {
        this.terminals.delete(sessionId);
      };
    });
  }

  private installHostEvents(): void {
    if (this.listeners.length > 0) return;
    const visibility = () => {
      if (!this.subscriptions.has('visibility')) return;
      this.emitEvent('visibility', { visible: document.visibilityState === 'visible' });
    };
    const focus = () => {
      if (this.subscriptions.has('window.focus')) this.emitEvent('window.focus', { focused: true });
    };
    const blur = () => {
      if (this.subscriptions.has('window.focus')) this.emitEvent('window.focus', { focused: false });
    };
    document.addEventListener('visibilitychange', visibility);
    window.addEventListener('focus', focus);
    window.addEventListener('blur', blur);
    this.themeObserver = new MutationObserver(() => {
      if (!this.subscriptions.has('theme')) return;
      this.emitEvent('theme', { theme: document.documentElement.dataset.theme ?? 'dark' });
    });
    this.themeObserver.observe(document.documentElement, { attributes: true, attributeFilter: ['data-theme'] });
    this.listeners.push(() => {
      document.removeEventListener('visibilitychange', visibility);
      window.removeEventListener('focus', focus);
      window.removeEventListener('blur', blur);
      this.themeObserver?.disconnect();
      this.themeObserver = null;
    });
  }

  private emitEvent(name: string, data: unknown): void {
    this.notify('event', { name, data });
    this.options.onEvent?.(name, data);
  }

  /** Close every PTY this instance created. Called before the runtime session
   * is removed so the app-instance terminal cleanup is not lost. */
  async closeTerminalSessions(): Promise<void> {
    const ids = [...this.terminals.keys()];
    await Promise.all(
      ids.map((sessionId) =>
        extensionApi
          .runtimePost(this.options.runtime.token, 'terminal/close', { session_id: sessionId })
          .catch(() => undefined),
      ),
    );
    for (const socket of this.terminals.values()) socket.close();
    this.terminals.clear();
  }

  dispose(): void {
    if (this.disposed) return;
    this.disposed = true;
    if (this.handshakeTimer !== null) window.clearTimeout(this.handshakeTimer);
    if (this.messageListener) window.removeEventListener('message', this.messageListener);
    for (const port of this.transfers) port.close();
    this.transfers.clear();
    for (const socket of this.terminals.values()) socket.close();
    this.terminals.clear();
    for (const remove of this.listeners) remove();
    this.listeners.length = 0;
    this.themeObserver?.disconnect();
    this.themeObserver = null;
    if (this.port) {
      this.port.onmessage = null;
      this.port.close();
      this.port = null;
    }
  }
}
