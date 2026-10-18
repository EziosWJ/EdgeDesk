export type FileEntry = {
  name: string;
  path: string;
  type: 'file' | 'dir' | 'symlink' | 'other';
  size: number;
  mtime: number;
  mode: number;
  permissions: string;
  uid: number;
  gid: number;
  hidden: boolean;
  mime: string;
  text_editable: boolean;
  link_target?: string;
};

export type FileListing = {
  ok: true;
  path: string;
  page: number;
  page_size: number;
  total: number;
  truncated: boolean;
  entries: FileEntry[];
};

export type TerminalSessionInfo = {
  id: string;
  title: string;
  cwd: string;
  created_ms: number;
  last_activity_ms: number;
  cols: number;
  rows: number;
  running: boolean;
  exited: boolean;
  exit_code: number;
  subscribers: number;
  buffered_bytes: number;
  pending_confirmation: boolean;
};

export type MetricsSample = {
  /** Unix timestamp in milliseconds, matching /api/system and /ws/metrics. */
  ts: number;
  uptime_seconds: number;
  hostname: string;
  kernel: string;
  arch: string;
  cpu: {
    cores: number;
    usage_percent: number | null;
    load1: number | null;
    load5: number | null;
    load15: number | null;
  };
  memory: {
    total_kb: number;
    available_kb: number;
    used_kb: number;
    free_kb: number;
    buffers_kb: number;
    cached_kb: number;
    swap_total_kb: number;
    swap_free_kb: number;
    used_percent: number;
  } | null;
  disks: {
    mount: string;
    filesystem: string;
    total_bytes: number;
    used_bytes: number;
    available_bytes: number;
    used_percent: number;
  }[];
  network: {
    name: string;
    addresses: string[];
    rx_bytes_per_second: number;
    tx_bytes_per_second: number;
    rx_bytes_total: number;
    tx_bytes_total: number;
  }[];
  unavailable: string[];
};

export type NetworkSample = MetricsSample['network'][number];
export type DiskSample = MetricsSample['disks'][number];
export type MemorySample = NonNullable<MetricsSample['memory']>;

export type AuthState = 'uninitialized' | 'initialized' | 'corrupt';export type SessionInfo = {
  ok: boolean;
  authenticated: boolean;
  method: 'cookie' | 'bearer' | 'none';
  state: AuthState;
  expires_at?: number;
};

export type SetupStatus = {
  ok: boolean;
  initialized: boolean;
  state: AuthState;
};

export type AppSettings = {
  theme: 'light' | 'dark' | 'system';
  files_view: 'list' | 'grid';
  files_show_hidden: boolean;
  terminal_font_size: number;
  dashboard_metrics_window: number;
};

export type SystemInfo = {
  ok: boolean;
  version: string;
  build: string;
  hostname: string;
  arch: string;
  kernel: string;
  uptime_seconds: number;
  listen: { host: string; port: number };
  file_root: string;
  follow_symlinks: boolean;
  bash: string;
  audit_enabled: boolean;
  audit_log: string;
  data_dir: string;
  state_dir: string;
  max_upload_size: number;
  max_terminal_sessions: number;
  terminal_idle_timeout: number;
  metrics_interval_ms: number;
  api_token_configured: boolean;
};

/** Registered native applications plus the two internal document window types. */
export type NativeAppId =
  | 'dashboard'
  | 'files'
  | 'terminal'
  | 'sessions'
  | 'settings'
  | 'editor'
  | 'preview'
  | 'extensions';

/** Native ids stay literal; installed extension ids are validated at runtime. */
export type AppId = NativeAppId | (string & {});

export type WindowKind = AppId;

export type WindowStatus = 'normal' | 'minimized' | 'maximized';

/** Only non-sensitive, reconnect-relevant context belongs here. */
export type WindowPayload = {
  sessionId?: string;
  path?: string;
  initialPath?: string;
  /** True when a terminal window was restored from local layout. */
  restored?: boolean;
};

export type WindowState = {
  id: string;
  appId: AppId;
  title: string;
  x: number;
  y: number;
  width: number;
  height: number;
  zIndex: number;
  state: WindowStatus;
  payload: WindowPayload;
  maxRestore?: { x: number; y: number; width: number; height: number };
  /** Set when a restored resource (terminal session, directory) is no longer valid. */
  restoreError?: 'session_ended' | 'session_unavailable' | null;
};

export type ViewportSize = { width: number; height: number };
