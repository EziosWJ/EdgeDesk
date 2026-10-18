import { ApiError, apiRequest } from '../services/api';
import type {
  ExtensionApp,
  ExtensionInstallResult,
  ExtensionIntegrityResult,
  ExtensionRevision,
  ExtensionSession,
} from './extensionTypes';

async function parseError(response: Response): Promise<never> {
  const text = await response.text();
  let code = 'http_error';
  let message = response.statusText;
  try {
    const body = JSON.parse(text) as { error?: string; message?: string };
    if (body.error) code = body.error;
    if (body.message) message = body.message;
  } catch {
    if (text) message = text.slice(0, 500);
  }
  throw new ApiError(response.status, code, message);
}

async function extensionRequest<T>(path: string, init: RequestInit = {}): Promise<T> {
  const response = await fetch(path, { credentials: 'same-origin', ...init });
  if (!response.ok) return parseError(response);
  const text = await response.text();
  return (text ? JSON.parse(text) : null) as T;
}

function installQuery(options: {
  restoreData?: boolean;
  overwrite?: boolean;
  allowDowngrade?: boolean;
}): string {
  const params = new URLSearchParams();
  if (options.restoreData) params.set('restore_data', '1');
  if (options.overwrite) params.set('overwrite', '1');
  if (options.allowDowngrade) params.set('allow_downgrade', '1');
  const text = params.toString();
  return text ? `?${text}` : '';
}

export const extensionApi = {
  list: () => apiRequest<{ ok: true; apps: ExtensionApp[] }>('/api/extensions'),

  detail: (appId: string) =>
    apiRequest<{ ok: true; app: ExtensionApp; revisions: ExtensionRevision[] }>(
      `/api/extensions/${encodeURIComponent(appId)}`,
    ),

  install: (
    file: File,
    options: { restoreData?: boolean; overwrite?: boolean; allowDowngrade?: boolean } = {},
  ) =>
    extensionRequest<ExtensionInstallResult>(`/api/extensions/install${installQuery(options)}`, {
      method: 'PUT',
      body: file,
    }),

  upgrade: (
    appId: string,
    file: File,
    options: { overwrite?: boolean; allowDowngrade?: boolean } = {},
  ) =>
    extensionRequest<ExtensionInstallResult>(
      `/api/extensions/${encodeURIComponent(appId)}/upgrade${installQuery(options)}`,
      { method: 'PUT', body: file },
    ),

  approve: (appId: string, permissions: string[], enable: boolean) =>
    apiRequest<{ ok: true; app: ExtensionApp }>(
      `/api/extensions/${encodeURIComponent(appId)}/approve`,
      { method: 'POST', body: JSON.stringify({ permissions, enable }) },
    ),

  enable: (appId: string) =>
    apiRequest<{ ok: true; app: ExtensionApp }>(`/api/extensions/${encodeURIComponent(appId)}/enable`, {
      method: 'POST',
      body: JSON.stringify({}),
    }),

  disable: (appId: string) =>
    apiRequest<{ ok: true; app: ExtensionApp }>(`/api/extensions/${encodeURIComponent(appId)}/disable`, {
      method: 'POST',
      body: JSON.stringify({}),
    }),

  updatePermissions: (appId: string, permissions: string[]) =>
    apiRequest<{ ok: true; app: ExtensionApp }>(
      `/api/extensions/${encodeURIComponent(appId)}/permissions`,
      { method: 'POST', body: JSON.stringify({ permissions }) },
    ),

  uninstall: (appId: string, dataAction: 'keep' | 'delete') =>
    apiRequest<{ ok: true }>(`/api/extensions/${encodeURIComponent(appId)}/uninstall`, {
      method: 'POST',
      body: JSON.stringify({ data_action: dataAction }),
    }),

  activate: (appId: string, revisionId: string) =>
    apiRequest<{ ok: true; app: ExtensionApp }>(
      `/api/extensions/${encodeURIComponent(appId)}/activate`,
      { method: 'POST', body: JSON.stringify({ revision_id: revisionId, confirm: true }) },
    ),

  pin: (appId: string, revisionId: string, pinned: boolean) =>
    apiRequest<{ ok: true }>(`/api/extensions/${encodeURIComponent(appId)}/pin`, {
      method: 'POST',
      body: JSON.stringify({ revision_id: revisionId, pinned }),
    }),

  integrityCheck: (appId: string, revisionId = '') =>
    apiRequest<ExtensionIntegrityResult>(`/api/extensions/${encodeURIComponent(appId)}/integrity-check`, {
      method: 'POST',
      body: JSON.stringify({ revision_id: revisionId }),
    }),

  createSession: (appId: string, instanceId: string) =>
    apiRequest<ExtensionSession>('/api/extensions/runtime/sessions', {
      method: 'POST',
      body: JSON.stringify({ app_id: appId, instance_id: instanceId }),
    }),

  closeSession: (token: string) =>
    fetch('/api/extensions/runtime/sessions', {
      method: 'DELETE',
      credentials: 'same-origin',
      headers: { 'X-EdgeDesk-Runtime': token },
      keepalive: true,
    }).catch(() => undefined),

  runtimePost: <T>(token: string, path: string, body: unknown = {}) =>
    extensionRequest<T>(`/api/extensions/runtime/${path}`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json', 'X-EdgeDesk-Runtime': token },
      body: JSON.stringify(body),
    }),

  runtimeUploadChunk: async (
    token: string,
    transferId: string,
    offset: number,
    data: ArrayBuffer,
  ): Promise<{ ok: true; received: number }> => {
    const response = await fetch(
      `/api/extensions/runtime/files/upload/${encodeURIComponent(transferId)}/chunk?offset=${offset}`,
      {
        method: 'PUT',
        credentials: 'same-origin',
        headers: { 'Content-Type': 'application/octet-stream', 'X-EdgeDesk-Runtime': token },
        body: data,
      },
    );
    if (!response.ok) return parseError(response);
    return (await response.json()) as { ok: true; received: number };
  },

  runtimeDownloadChunk: async (
    token: string,
    transferId: string,
    offset: number,
  ): Promise<{ data: ArrayBuffer; eof: boolean }> => {
    const response = await fetch(
      `/api/extensions/runtime/files/download/${encodeURIComponent(transferId)}/chunk?offset=${offset}`,
      { credentials: 'same-origin', headers: { 'X-EdgeDesk-Runtime': token } },
    );
    if (!response.ok) return parseError(response);
    return { data: await response.arrayBuffer(), eof: response.headers.get('X-EdgeDesk-EOF') === '1' };
  },

  reportError: (token: string, type: string, message: string, stack: string) =>
    fetch('/api/extensions/runtime/errors', {
      method: 'POST',
      credentials: 'same-origin',
      headers: { 'Content-Type': 'application/json', 'X-EdgeDesk-Runtime': token },
      body: JSON.stringify({ type, message, stack }),
    }).catch(() => undefined),

  authStatus: () =>
    apiRequest<{ authenticated: boolean; method: 'cookie' | 'bearer' | 'none'; state: string }>(
      '/api/auth/session',
    ),
};
