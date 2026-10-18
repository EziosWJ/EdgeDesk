export type ExtensionStatus = 'pending_review' | 'enabled' | 'disabled' | 'isolated' | 'uninstalled';

export type ExtensionWindowSpec = {
  defaultWidth?: number;
  defaultHeight?: number;
  minWidth?: number;
  minHeight?: number;
  instances?: 'single' | 'multi';
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
