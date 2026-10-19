import type {
  AppSettings,
  FileEntry,
  FileListing,
  MetricsSample,
  SessionInfo,
  ServicePayload,
  SupervisorHealth,
  SupervisorLogPage,
  SupervisedService,
  SetupStatus,
  SystemInfo,
  TerminalSessionInfo,
} from '../types';

export class ApiError extends Error {
  status: number;
  code: string;

  constructor(status: number, code: string, message: string) {
    super(message);
    this.status = status;
    this.code = code;
  }
}

export async function apiRequest<T>(path: string, init: RequestInit = {}): Promise<T> {
  const response = await fetch(path, {
    credentials: 'same-origin',
    headers:
      init.body instanceof Blob || init.body instanceof ArrayBuffer
        ? init.headers
        : { 'Content-Type': 'application/json', ...(init.headers ?? {}) },
    ...init,
  });
  const text = await response.text();
  let body: unknown = null;
  if (text) {
    try {
      body = JSON.parse(text);
    } catch {
      body = null;
    }
  }
  if (!response.ok) {
    const data = (body ?? {}) as { error?: string; message?: string };
    throw new ApiError(response.status, data.error ?? 'http_error', data.message ?? response.statusText);
  }
  return body as T;
}

export const api = {
  setupStatus: () => apiRequest<SetupStatus>('/api/auth/setup-status'),
  setup: (password: string, confirmPassword: string) =>
    apiRequest<{ ok: boolean; initialized: boolean }>('/api/auth/setup', {
      method: 'POST',
      body: JSON.stringify({ password, confirm_password: confirmPassword }),
    }),
  session: () => apiRequest<SessionInfo>('/api/auth/session'),
  login: (password: string) =>
    apiRequest<{ ok: boolean; expires_in: number; expires_at: number }>('/api/auth/login', {
      method: 'POST',
      body: JSON.stringify({ password }),
    }),
  logout: () => apiRequest<{ ok: boolean }>('/api/auth/logout', { method: 'POST' }),
  changePassword: (currentPassword: string, newPassword: string, confirmPassword: string) =>
    apiRequest<{ ok: boolean; reauthenticate: boolean }>('/api/auth/password', {
      method: 'POST',
      body: JSON.stringify({
        current_password: currentPassword,
        new_password: newPassword,
        confirm_password: confirmPassword,
      }),
    }),
  settings: () =>
    apiRequest<{ ok: boolean; settings: AppSettings; api_token_configured: boolean }>('/api/settings'),
  updateSettings: (patch: Partial<AppSettings>) =>
    apiRequest<{ ok: boolean; settings: AppSettings }>('/api/settings', {
      method: 'PUT',
      body: JSON.stringify(patch),
    }),
  systemInfo: () => apiRequest<SystemInfo>('/api/system/info'),
  health: () => apiRequest<{ status: string; service: string; phase: number; version: string }>('/api/health'),

  listFiles: (
    path: string,
    options: { page?: number; pageSize?: number; sort?: string; desc?: boolean; hidden?: boolean; dirsOnly?: boolean } = {},
  ) => {
    const params = new URLSearchParams({ path });
    if (options.page) params.set('page', String(options.page));
    if (options.pageSize) params.set('page_size', String(options.pageSize));
    if (options.sort) params.set('sort', options.sort);
    if (options.desc) params.set('order', 'desc');
    if (options.hidden === false) params.set('hidden', '0');
    if (options.dirsOnly) params.set('dirs_only', '1');
    return apiRequest<FileListing>(`/api/files?${params.toString()}`);
  },
  statFile: (path: string) =>
    apiRequest<{ ok: boolean; entry: FileEntry }>(`/api/files/stat?path=${encodeURIComponent(path)}`),
  readText: (path: string) =>
    apiRequest<{ ok: boolean; content: string; entry: FileEntry }>(
      `/api/files/text?path=${encodeURIComponent(path)}`,
    ),
  writeText: (path: string, content: string) =>
    apiRequest<{ ok: boolean; entry: FileEntry }>(`/api/files/text?path=${encodeURIComponent(path)}`, {
      method: 'PUT',
      headers: { 'Content-Type': 'text/plain; charset=utf-8' },
      body: content,
    }),
  mkdir: (path: string) =>
    apiRequest<{ ok: boolean; path: string }>('/api/files/mkdir', {
      method: 'POST',
      body: JSON.stringify({ path }),
    }),
  rename: (from: string, to: string, overwrite = false) =>
    apiRequest<{ ok: boolean }>('/api/files/rename', {
      method: 'POST',
      body: JSON.stringify({ from, to, overwrite }),
    }),
  remove: (paths: string[]) =>
    apiRequest<{ ok: boolean; removed_files: number; removed_dirs: number }>('/api/files/delete', {
      method: 'POST',
      body: JSON.stringify({ paths }),
    }),
  downloadUrl: (path: string) => `/api/files/download?path=${encodeURIComponent(path)}`,

  upload: (path: string, file: File, onProgress?: (percent: number) => void) =>
    new Promise<{ ok: boolean; size: number }>((resolve, reject) => {
      const xhr = new XMLHttpRequest();
      xhr.open('PUT', `/api/files/content?path=${encodeURIComponent(path)}`);
      xhr.withCredentials = true;
      xhr.upload.onprogress = (event) => {
        if (event.lengthComputable && onProgress) {
          onProgress(Math.round((event.loaded / event.total) * 100));
        }
      };
      xhr.onload = () => {
        if (xhr.status >= 200 && xhr.status < 300) {
          resolve(JSON.parse(xhr.responseText || '{}'));
        } else {
          let message = `upload failed (${xhr.status})`;
          try {
            const parsed = JSON.parse(xhr.responseText);
            if (parsed.message) message = parsed.message;
          } catch {
            /* keep default message */
          }
          reject(new ApiError(xhr.status, 'upload_failed', message));
        }
      };
      xhr.onerror = () => reject(new ApiError(0, 'network_error', '上传中断或网络错误'));
      xhr.send(file);
    }),

  terminalSessions: () =>
    apiRequest<{
      ok: boolean;
      shell: string;
      shell_available: boolean;
      max_sessions: number;
      idle_timeout_seconds: number;
      running: number;
      sessions: TerminalSessionInfo[];
    }>('/api/terminal/sessions'),
  createTerminal: (options: { cwd?: string; cols?: number; rows?: number; title?: string }) =>
    apiRequest<{ ok: boolean; session: TerminalSessionInfo }>('/api/terminal/sessions', {
      method: 'POST',
      body: JSON.stringify(options),
    }),
  closeTerminal: (sessionId: string) =>
    apiRequest<{ ok: boolean }>('/api/terminal/sessions/close', {
      method: 'POST',
      body: JSON.stringify({ session_id: sessionId }),
    }),

  systemSummary: (history = 0) =>
    apiRequest<{ ok: boolean; interval_ms: number; sample: MetricsSample; history?: MetricsSample[] }>(
      `/api/system/summary?history=${history}`,
    ),

  supervisorHealth: () => apiRequest<SupervisorHealth>('/api/supervisor/health'),
  supervisorServices: (options: { offset?: number; limit?: number; status?: string } = {}) => {
    const params = new URLSearchParams();
    if (options.offset !== undefined) params.set('offset', String(options.offset));
    if (options.limit !== undefined) params.set('limit', String(options.limit));
    if (options.status) params.set('status', options.status);
    const query = params.toString();
    return apiRequest<{
      ok: boolean;
      services: SupervisedService[];
      total: number;
      count: number;
      offset: number;
      limit: number;
    }>(`/api/supervisor/services${query ? `?${query}` : ''}`);
  },
  supervisorService: (id: string) =>
    apiRequest<SupervisedService & { ok: boolean }>(
      `/api/supervisor/services/${encodeURIComponent(id)}`,
    ),
  createSupervisorService: (payload: ServicePayload) =>
    apiRequest<SupervisedService & { ok: boolean }>('/api/supervisor/services', {
      method: 'POST',
      body: JSON.stringify(payload),
    }),
  updateSupervisorService: (id: string, payload: ServicePayload) =>
    apiRequest<SupervisedService & { ok: boolean }>(
      `/api/supervisor/services/${encodeURIComponent(id)}`,
      { method: 'PUT', body: JSON.stringify(payload) },
    ),
  deleteSupervisorService: (id: string) =>
    apiRequest<{ ok: boolean; deleted: boolean; id: string }>(
      `/api/supervisor/services/${encodeURIComponent(id)}`,
      { method: 'DELETE' },
    ),
  startSupervisorService: (id: string) =>
    apiRequest<SupervisedService & { ok: boolean }>(
      `/api/supervisor/services/${encodeURIComponent(id)}/start`,
      { method: 'POST' },
    ),
  stopSupervisorService: (id: string) =>
    apiRequest<SupervisedService & { ok: boolean }>(
      `/api/supervisor/services/${encodeURIComponent(id)}/stop`,
      { method: 'POST' },
    ),
  restartSupervisorService: (id: string) =>
    apiRequest<SupervisedService & { ok: boolean }>(
      `/api/supervisor/services/${encodeURIComponent(id)}/restart`,
      { method: 'POST' },
    ),
  supervisorLogs: (id: string, options: { tailBytes?: number; cursor?: number; maxBytes?: number } = {}) => {
    const params = new URLSearchParams();
    if (options.tailBytes !== undefined) params.set('tail_bytes', String(options.tailBytes));
    if (options.cursor !== undefined) params.set('cursor', String(options.cursor));
    if (options.maxBytes !== undefined) params.set('max_bytes', String(options.maxBytes));
    const query = params.toString();
    return apiRequest<SupervisorLogPage>(
      `/api/supervisor/services/${encodeURIComponent(id)}/logs${query ? `?${query}` : ''}`,
    );
  },
};

export function formatBytes(value: number, digits = 1): string {
  if (!Number.isFinite(value) || value < 0) return '—';
  const units = ['B', 'KiB', 'MiB', 'GiB', 'TiB'];
  let size = value;
  let unit = 0;
  while (size >= 1024 && unit < units.length - 1) {
    size /= 1024;
    unit += 1;
  }
  return `${size.toFixed(unit === 0 ? 0 : digits)} ${units[unit]}`;
}

export function formatRate(value: number | null | undefined): string {
  if (value === null || value === undefined || !Number.isFinite(value)) return '—';
  return `${formatBytes(value)}/s`;
}

export function formatUptime(seconds: number): string {
  if (!Number.isFinite(seconds) || seconds <= 0) return '—';
  const days = Math.floor(seconds / 86400);
  const hours = Math.floor((seconds % 86400) / 3600);
  const minutes = Math.floor((seconds % 3600) / 60);
  if (days > 0) return `${days} 天 ${hours} 小时`;
  if (hours > 0) return `${hours} 小时 ${minutes} 分`;
  return `${minutes} 分 ${Math.floor(seconds % 60)} 秒`;
}

export function formatTime(unixSeconds: number): string {
  if (!unixSeconds) return '—';
  return new Date(unixSeconds * 1000).toLocaleString('zh-CN', { hour12: false });
}

export function formatPercent(value: number | null | undefined, digits = 1): string {
  if (value === null || value === undefined || !Number.isFinite(value)) return '—';
  return `${value.toFixed(digits)}%`;
}
