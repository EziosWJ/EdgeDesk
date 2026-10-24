export type ExtensionStatus = 'pending_review' | 'enabled' | 'disabled' | 'isolated' | 'uninstalled';

export type ExtensionWindowSpec = {
  defaultWidth?: number;
  defaultHeight?: number;
  minWidth?: number;
  minHeight?: number;
  instances?: 'single' | 'multi';
};

/** Supervisor-reported live state for a native backend. */
export type BackendSupervisorState = {
  state?: string;
  pid?: number | null;
  last_exit_code?: number | null;
  last_exit_at?: number | null;
};

/**
 * Native backend descriptor from SPEC-0011. `present=false` means the package
 * is a pure Web application and no backend controls may be shown.
 * Persisted `state` only tracks manager state (stopped/upgrading/upgrade_failed);
 * process liveness is always read from `supervisor`, never copied.
 */
export type BackendInfo = {
  present: boolean;
  service_id?: string;
  arch?: string;
  host_arch?: string;
  executable?: string;
  program_dir?: string;
  data_dir?: string;
  socket_path?: string;
  run_user?: string;
  run_group?: string;
  allow_root?: boolean;
  autostart?: boolean;
  state?: 'stopped' | 'upgrading' | 'upgrade_failed' | string;
  error?: string;
  supervisor?: BackendSupervisorState | null;
};

export type ExtensionRevision = {
  revision_id: string;
  version: string;
  status: 'pending' | 'active' | 'historical' | 'isolated';
  active: boolean;
  pinned: boolean;
  integrity: 'ok' | 'mismatch';
  created_at: number;
  file_count: number;
  total_bytes: number;
};

export type ExtensionApp = {
  app_id: string;
  display_name: string;
  description?: string;
  publisher?: string;
  status: ExtensionStatus;
  active_revision_id: string;
  active_version: string;
  requested_permissions: string[];
  granted_permissions: string[];
  data_retained: boolean;
  publisher_verified: boolean;
  installed_at: number;
  updated_at: number;
  icon?: string;
  icon_url?: string;
  host_api?: { min: string; max: string };
  window?: ExtensionWindowSpec;
  network_origins: string[];
  revisions?: ExtensionRevision[];
  /** Present on detail responses; omitted or `present=false` for pure Web apps. */
  backend?: BackendInfo;
};

export type ExtensionDetail = {
  app: ExtensionApp;
  revisions: ExtensionRevision[];
  backend?: BackendInfo;
};

export type ExtensionSession = {
  ok: true;
  token: string;
  app_id: string;
  instance_id: string;
  revision_id: string;
  version: string;
  permissions: string[];
  origins: string[];
  host_api_version: string;
  static_base: string;
  display_name: string;
  window?: ExtensionWindowSpec;
};

export type ExtensionInstallResult = {
  ok: true;
  app: ExtensionApp;
  revision: ExtensionRevision;
  warnings: string[];
  approval_required: boolean;
  retained_data_restored: boolean;
};

export type ExtensionIntegrityResult = {
  ok: boolean;
  revision_id: string;
  mismatch_path: string;
  app_isolated: boolean;
};

/** Metadata needed by the sandboxed iframe runtime. */
export type ExtensionRuntimeApp = {
  appId: string;
  name: string;
  iconUrl?: string;
  description: string;
};

export const EXTENSION_PERMISSIONS = ['storage', 'files.manage', 'terminal.access', 'network'] as const;

export function permissionLabel(permission: string): string {
  switch (permission) {
    case 'storage':
      return '平台存储（5 MiB 配额）';
    case 'files.manage':
      return '文件读写与传输';
    case 'terminal.access':
      return '终端会话';
    case 'network':
      return '外部网络来源';
    default:
      return permission;
  }
}
