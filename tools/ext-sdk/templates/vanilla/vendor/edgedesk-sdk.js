/**
 * EdgeDesk extension SDK 1.0.
 *
 * This module runs inside the sandboxed application iframe. It performs the
 * postMessage handshake with the host desktop, then sends JSON-RPC 2.0 requests
 * over the dedicated MessagePort. It never receives host cookies, bearer
 * tokens, absolute filesystem paths, or another application's data.
 *
 * Host API version: 1.0.0
 */

export const HOST_API_VERSION = '1.0.0';
export const PROTOCOL_VERSION = 1;
export const CHUNK_SIZE = 1024 * 1024;

export class RpcError extends Error {
  constructor(code, message, data) {
    super(message);
    this.name = 'RpcError';
    this.code = code;
    this.data = data;
  }
}

function randomId() {
  const bytes = new Uint8Array(8);
  crypto.getRandomValues(bytes);
  return [...bytes].map((value) => value.toString(16).padStart(2, '0')).join('');
}

function isRecord(value) {
  return typeof value === 'object' && value !== null && !Array.isArray(value);
}

function originIsDeclared(parsed, declared) {
  if (parsed.protocol !== 'http:' && parsed.protocol !== 'https:') return false;
  const port = parsed.port ? Number(parsed.port) : parsed.protocol === 'https:' ? 443 : 80;
  for (const entry of declared ?? []) {
    let candidate;
    try {
      candidate = new URL(entry.replace('://*.', '://wildcard.'));
    } catch {
      continue;
    }
    const wildcard = entry.includes('://*.');
    const scheme = entry.split('://')[0];
    const candidatePort = candidate.port ? Number(candidate.port) : scheme === 'https' ? 443 : 80;
    if (candidatePort !== port || scheme + ':' !== parsed.protocol) continue;
    const host = parsed.hostname.toLowerCase();
    const expected = candidate.hostname.toLowerCase().replace(/^wildcard\./, '');
    if (host === expected) return true;
    if (wildcard && host.endsWith('.' + expected)) return true;
  }
  return false;
}

export class EdgeDeskClient {
  constructor(port, info) {
    this.port = port;
    this.info = Object.freeze({ ...info });
    this.pending = new Map();
    this.nextId = 1;
    this.listeners = new Map();
    this.closed = false;
    port.onmessage = (event) => this.#onMessage(event.data);
    port.start();
  }

  #emit(name, payload) {
    const callbacks = this.listeners.get(name);
    if (!callbacks) return;
    for (const callback of [...callbacks]) {
      try {
        callback(payload);
      } catch {
        // Listener failures must not break protocol dispatch.
      }
    }
  }

  #onMessage(message) {
    if (!isRecord(message)) return;
    if (Object.prototype.hasOwnProperty.call(message, 'id') && this.pending.has(message.id)) {
      const pending = this.pending.get(message.id);
      this.pending.delete(message.id);
      clearTimeout(pending.timer);
      if (message.error) {
        pending.reject(
          new RpcError(
            Number(message.error.code ?? -32603),
            String(message.error.message ?? 'host error'),
            message.error.data,
          ),
        );
      } else {
        pending.resolve(message.result);
      }
      return;
    }
    if (typeof message.method === 'string') {
      this.#emit(message.method, message.params);
    }
  }

  on(name, callback) {
    let callbacks = this.listeners.get(name);
    if (!callbacks) {
      callbacks = new Set();
      this.listeners.set(name, callbacks);
    }
    callbacks.add(callback);
    return () => callbacks.delete(callback);
  }

  /**
   * Caller-side waiting timeout. A timeout rejects this promise; it does not
   * cancel an operation that is already executing on the host.
   */
  call(method, params = {}, options = {}) {
    if (this.closed) return Promise.reject(new RpcError(-32001, 'runtime connection is closed'));
    const id = `r${this.nextId++}-${randomId()}`;
    const timeoutMs = Number(options.timeoutMs ?? 30_000);
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(id);
        reject(
          new RpcError(
            -32000,
            'timed out while waiting for the host; the operation may still be running',
            { timeout: true },
          ),
        );
      }, timeoutMs);
      this.pending.set(id, { resolve, reject, timer });
      this.port.postMessage({ jsonrpc: '2.0', id, method, params });
    });
  }

  notify(method, params = {}) {
    if (this.closed) return;
    this.port.postMessage({ jsonrpc: '2.0', method, params });
  }

  // ---- platform ----
  getInfo() {
    return this.call('platform.getInfo');
  }

  getAuthStatus() {
    return this.call('auth.getStatus');
  }

  reportError(type, message, stack = '') {
    this.notify('platform.reportError', {
      type: String(type).slice(0, 120),
      message: String(message).slice(0, 1024),
      stack: String(stack).slice(0, 2048),
    });
  }

  // ---- storage ----
  storageGet(key, options) {
    return this.call('storage.get', { key }, options);
  }

  storageSet(key, value, options) {
    return this.call('storage.set', { key, value }, options);
  }

  storageDelete(key, options) {
    return this.call('storage.delete', { key }, options);
  }

  storageList(options = {}) {
    return this.call('storage.list', { cursor: options.cursor, limit: options.limit ?? 100 }, options);
  }

  storageTransaction(operations, options) {
    return this.call('storage.transaction', { operations }, options);
  }

  // ---- files ----
  filesList(path = '/', options = {}) {
    return this.call('files.list', { path, cursor: options.cursor, limit: options.limit ?? 100 }, options);
  }

  filesStat(path, options) {
    return this.call('files.stat', { path }, options);
  }

  filesRead(path, options) {
    return this.call('files.read', { path }, options);
  }

  filesMkdir(path, options) {
    return this.call('files.mkdir', { path }, options);
  }

  filesRename(from, to, overwrite = false, options) {
    return this.call('files.rename', { from, to, overwrite }, options);
  }

  filesDelete(paths, options) {
    return this.call('files.delete', { paths }, options);
  }

  async uploadFile(path, source, options = {}) {
    const bytes =
      source instanceof Blob
        ? new Uint8Array(await source.arrayBuffer())
        : new Uint8Array(source.buffer ?? source, source.byteOffset ?? 0, source.byteLength ?? source.length);
    const begin = await this.call(
      'files.upload.begin',
      { path, size: bytes.byteLength },
      options,
    );
    const port = begin.port;
    if (!port) throw new RpcError(-32007, 'host did not provide a transfer port');
    return new Promise((resolve, reject) => {
      let offset = 0;
      let settled = false;
      const finish = (error, result) => {
        if (settled) return;
        settled = true;
        port.close();
        if (error) reject(error);
        else resolve(result);
      };
      port.onmessage = (event) => {
        const message = event.data;
        if (!isRecord(message)) return;
        if (message.type === 'ack') {
          if (Number(message.received) !== offset) {
            finish(new RpcError(-32007, 'host acknowledgement offset mismatch'));
            return;
          }
          if (offset < bytes.byteLength) send();
          else port.postMessage({ type: 'complete' });
        } else if (message.type === 'done') {
          finish(null, message.entry);
        } else if (message.type === 'error') {
          finish(new RpcError(Number(message.code ?? -32007), String(message.message ?? 'transfer failed')));
        } else if (message.type === 'cancelled') {
          finish(new RpcError(-32007, 'upload cancelled'));
        }
      };
      const send = () => {
        const chunk = bytes.slice(offset, offset + CHUNK_SIZE);
        const copy = chunk.buffer.slice(chunk.byteOffset, chunk.byteOffset + chunk.byteLength);
        port.postMessage({ type: 'chunk', offset, data: copy }, [copy]);
        offset += chunk.byteLength;
      };
      port.start();
      if (bytes.byteLength === 0) {
        port.postMessage({ type: 'complete' });
      } else {
        send();
      }
    });
  }

  async downloadFile(path, options = {}) {
    const begin = await this.call('files.download.begin', { path }, options);
    const port = begin.port;
    if (!port) throw new RpcError(-32007, 'host did not provide a transfer port');
    return new Promise((resolve, reject) => {
      const chunks = [];
      let offset = 0;
      let settled = false;
      const finish = (error, result) => {
        if (settled) return;
        settled = true;
        port.close();
        if (error) reject(error);
        else resolve(result);
      };
      port.onmessage = (event) => {
        const message = event.data;
        if (!isRecord(message)) return;
        if (message.type === 'chunk') {
          if (!(message.data instanceof ArrayBuffer)) {
            finish(new RpcError(-32007, 'host sent a non-binary chunk'));
            return;
          }
          chunks.push(message.data);
          offset += message.data.byteLength;
          port.postMessage({ type: 'ack', offset });
        } else if (message.type === 'done') {
          finish(
            null,
            new Blob(chunks, { type: options.mime ?? 'application/octet-stream' }),
          );
        } else if (message.type === 'error') {
          finish(new RpcError(Number(message.code ?? -32007), String(message.message ?? 'transfer failed')));
        } else if (message.type === 'cancelled') {
          finish(new RpcError(-32007, 'download cancelled'));
        }
      };
      port.start();
      port.postMessage({ type: 'start' });
    });
  }

  // ---- terminal ----
  terminalList(options) {
    return this.call('terminal.sessions.list', {}, options);
  }

  terminalCreate(options = {}) {
    return this.call(
      'terminal.sessions.create',
      { title: options.title ?? '', cwd: options.cwd ?? '', cols: options.cols ?? 100, rows: options.rows ?? 30 },
      options,
    );
  }

  terminalInput(sessionId, data, options) {
    return this.call('terminal.sessions.input', { sessionId, data }, options);
  }

  terminalResize(sessionId, cols, rows, options) {
    return this.call('terminal.sessions.resize', { sessionId, cols, rows }, options);
  }

  terminalClose(sessionId, options) {
    return this.call('terminal.sessions.close', { sessionId }, options);
  }

  // ---- events ----
  subscribe(events, options) {
    return this.call('events.subscribe', { events }, options);
  }

  unsubscribe(events, options) {
    return this.call('events.unsubscribe', { events }, options);
  }

  /**
   * Opens an external page in a popup. Returns a validator that only accepts a
   * callback from that exact popup, with an allowed origin, matching random
   * nonce and state, and an object payload. Host API commands are never read
   * from opener messages.
   */
  openExternal(url, options = {}) {
    let parsed;
    try {
      parsed = new URL(url, window.location.href);
    } catch {
      throw new RpcError(-32602, 'invalid external URL');
    }
    if (!originIsDeclared(parsed, this.info.networkOrigins)) {
      throw new RpcError(-32000, `external origin is not declared in the manifest: ${parsed.origin}`);
    }
    const nonce = options.nonce ?? randomId() + randomId();
    const state = options.state ?? randomId() + randomId();
    const features = options.features ?? 'width=520,height=720,menubar=no,toolbar=no';
    const popup = window.open(parsed.toString(), options.name ?? '_blank', features);
    if (!popup) throw new RpcError(-32007, 'popup was blocked by the browser');
    const expected = new Set([
      parsed.origin,
      ...(Array.isArray(options.expectedOrigins) ? options.expectedOrigins : []),
    ]);
    const waitForCallback = (waitOptions = {}) =>
      new Promise((resolve, reject) => {
        const timeoutMs = Number(waitOptions.timeoutMs ?? 120_000);
        const timer = setTimeout(() => {
          window.removeEventListener('message', listener);
          reject(new RpcError(-32000, 'timed out waiting for the external callback'));
        }, timeoutMs);
        const listener = (event) => {
          if (event.source !== popup) return;
          if (!expected.has(event.origin) && event.origin !== parsed.origin) return;
          const data = event.data;
          if (!isRecord(data)) return;
          if (data.nonce !== nonce || data.state !== state) return;
          clearTimeout(timer);
          window.removeEventListener('message', listener);
          resolve({ origin: event.origin, data });
        };
        window.addEventListener('message', listener);
      });
    return { popup, nonce, state, waitForCallback };
  }

  dispose() {
    this.closed = true;
    for (const pending of this.pending.values()) {
      clearTimeout(pending.timer);
      pending.reject(new RpcError(-32001, 'runtime connection is closed'));
    }
    this.pending.clear();
    try {
      this.port.close();
    } catch {
      // already closed
    }
  }
}

/**
 * Perform the sandbox handshake. Rejects when the page is not hosted by the
 * EdgeDesk desktop or the host does not answer before the timeout.
 */
export function connect(options = {}) {
  const timeoutMs = Number(options.timeoutMs ?? 15_000);
  if (typeof window === 'undefined' || window.parent === window) {
    return Promise.reject(new RpcError(-32001, 'this application must run inside the EdgeDesk desktop'));
  }
  return new Promise((resolve, reject) => {
    let settled = false;
    let hello = null;
    const finish = (error, client) => {
      if (settled) return;
      settled = true;
      if (hello !== null) clearInterval(hello);
      clearTimeout(timer);
      window.removeEventListener('message', onMessage);
      if (error) reject(error);
      else resolve(client);
    };
    const onMessage = (event) => {
      if (event.source !== window.parent) return;
      const data = event.data;
      if (!isRecord(data) || data.type !== 'edgedesk:init') return;
      if (data.protocol !== PROTOCOL_VERSION) {
        finish(new RpcError(-32600, `unsupported protocol version: ${data.protocol}`));
        return;
      }
      if (typeof data.token !== 'string' || data.token.length < 16) {
        finish(new RpcError(-32600, 'host handshake token is invalid'));
        return;
      }
      const port = event.ports && event.ports[0];
      if (!port) {
        finish(new RpcError(-32600, 'host did not transfer a MessagePort'));
        return;
      }
      const info = {
        token: data.token,
        app: data.app ?? {},
        permissions: Array.isArray(data.permissions) ? data.permissions : [],
        networkOrigins: Array.isArray(data.networkOrigins) ? data.networkOrigins : [],
        window: isRecord(data.window) ? data.window : {},
        hostApiVersion: String(data.hostApiVersion ?? ''),
      };
      finish(null, new EdgeDeskClient(port, info));
    };
    const timer = setTimeout(() => {
      finish(new RpcError(-32000, 'timed out waiting for the EdgeDesk host handshake'));
    }, timeoutMs);
    window.addEventListener('message', onMessage);
    const announce = () => {
      window.parent.postMessage({ type: 'edgedesk:hello', protocol: PROTOCOL_VERSION }, '*');
    };
    announce();
    hello = setInterval(announce, 500);
  });
}

/** Report unhandled errors without capturing page contents or business data. */
export function installErrorReporting(client) {
  const report = (type, message, stack) => {
    try {
      client.reportError(type, message, stack);
    } catch {
      // Reporting must never throw from an error handler.
    }
  };
  window.addEventListener(
    'error',
    (event) => {
      const target = event.target;
      if (target && target !== window && typeof target.tagName === 'string') {
        const source = target.currentSrc || target.src || target.href || target.tagName;
        report('resource_error', String(source));
        return;
      }
      report('uncaught_exception', event.message ?? String(event.error ?? 'error'), event.error?.stack ?? '');
    },
    true,
  );
  window.addEventListener('unhandledrejection', (event) => {
    const reason = event.reason;
    report(
      'unhandled_rejection',
      reason instanceof Error ? reason.message : String(reason ?? 'unhandled rejection'),
      reason instanceof Error ? reason.stack ?? '' : '',
    );
  });
}
