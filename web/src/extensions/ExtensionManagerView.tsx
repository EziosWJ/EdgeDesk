import { useCallback, useEffect, useMemo, useRef, useState, type DragEvent as ReactDragEvent } from 'react';
import {
  AlertTriangle,
  Check,
  FileArchive,
  Pause,
  Pin,
  Play,
  RefreshCw,
  RotateCw,
  ScrollText,
  Server,
  ShieldCheck,
  Square,
  Trash2,
  Upload,
} from 'lucide-react';
import { Icon } from '../components/Icon';
import { ConfirmDialog } from '../components/ConfirmDialog';
import { ApiError, formatBytes, formatTime } from '../services/api';
import { windowStore } from '../stores/windowStore';
import { extensionApi } from './extensionApi';
import type {
  BackendInfo,
  ExtensionApp,
  ExtensionIntegrityResult,
  ExtensionRevision,
} from './extensionTypes';
import { permissionLabel } from './extensionTypes';

type Props = {
  onExtensionsChanged: () => void;
};

type Busy = null | string;

type PendingUpload = {
  file: File;
  appId?: string;
  overwrite: boolean;
  allowDowngrade: boolean;
  restoreData: boolean;
};

type PendingDrop = {
  file: File;
  appId?: string;
  targetName?: string;
};

const STATUS_LABEL: Record<string, string> = {
  pending_review: '待审批',
  enabled: '已启用',
  disabled: '已禁用',
  isolated: '已隔离',
  uninstalled: '已卸载',
};

const BACKEND_STATE_LABEL: Record<string, string> = {
  stopped: '已停止',
  upgrading: '升级中',
  upgrade_failed: '升级失败',
};

const SUPERVISOR_STATE_LABEL: Record<string, string> = {
  STOPPED: '已停止',
  STARTING: '启动中',
  RUNNING: '运行中',
  STOPPING: '停止中',
  BACKOFF: '等待重启',
  FAILED: '失败锁定',
  UNKNOWN: '状态未知',
};

function supervisorState(state: string | undefined): string {
  return (state ?? '').toUpperCase();
}

function supervisorStateLabel(state: string | undefined): string {
  if (!state) return '—';
  return SUPERVISOR_STATE_LABEL[supervisorState(state)] ?? state;
}

function supervisorStateClass(state: string | undefined): string {
  const normalized = supervisorState(state);
  if (normalized === 'RUNNING') return 'ok';
  if (normalized === 'FAILED') return 'danger';
  if (normalized === 'STARTING' || normalized === 'STOPPING' || normalized === 'BACKOFF' || normalized === 'UNKNOWN') {
    return 'pending';
  }
  return 'muted';
}

function backendStateClass(state: string | undefined): string {
  if (state === 'upgrade_failed') return 'danger';
  if (state === 'upgrading') return 'pending';
  return 'subtle';
}

function archMismatch(backend: BackendInfo | null | undefined): boolean {
  if (!backend?.arch || !backend.host_arch) return false;
  return backend.arch.toLowerCase() !== backend.host_arch.toLowerCase();
}

function formatBackendTime(value: number | null | undefined): string {
  if (!value) return '—';
  // Supervisor timestamps are milliseconds; be tolerant of seconds.
  return formatTime(value > 1e11 ? value / 1000 : value);
}

function statusClass(status: string): string {
  if (status === 'enabled') return 'ok';
  if (status === 'pending_review') return 'pending';
  if (status === 'isolated') return 'danger';
  return 'muted';
}

function errorMessage(error: unknown): string {
  if (error instanceof ApiError) return error.message;
  if (error instanceof Error) return error.message;
  return '操作失败，请稍后重试。';
}

function requestedPermissionList(app: ExtensionApp): string[] {
  return [...app.requested_permissions].sort();
}

function isZipFile(file: File): boolean {
  return file.type === 'application/zip' || /\.zip$/i.test(file.name);
}

export function ExtensionManagerView({ onExtensionsChanged }: Props) {
  const [apps, setApps] = useState<ExtensionApp[]>([]);
  const [selectedId, setSelectedId] = useState<string>('');
  const [detail, setDetail] = useState<{
    app: ExtensionApp;
    revisions: ExtensionRevision[];
    backend?: BackendInfo;
  } | null>(null);
  const [selectedPermissions, setSelectedPermissions] = useState<string[]>([]);
  const [busy, setBusy] = useState<Busy>(null);
  const [error, setError] = useState('');
  const [notice, setNotice] = useState('');
  const [installWarnings, setInstallWarnings] = useState<string[]>([]);
  const [integrity, setIntegrity] = useState<ExtensionIntegrityResult | null>(null);
  const [pendingUpload, setPendingUpload] = useState<PendingUpload | null>(null);
  const [restoreData, setRestoreData] = useState(false);
  const [uninstallDialog, setUninstallDialog] = useState<ExtensionApp | null>(null);
  const [activateDialog, setActivateDialog] = useState<{ app: ExtensionApp; revision: ExtensionRevision } | null>(null);
  const [backendForm, setBackendForm] = useState({ run_user: '', run_group: '', allow_root: false });
  const [purgeDialog, setPurgeDialog] = useState<ExtensionApp | null>(null);
  const [purgeConfirm, setPurgeConfirm] = useState('');
  const [refreshing, setRefreshing] = useState(false);
  const [dragActive, setDragActive] = useState(false);
  const [pendingDrop, setPendingDrop] = useState<PendingDrop | null>(null);
  const installInput = useRef<HTMLInputElement | null>(null);
  const upgradeInput = useRef<HTMLInputElement | null>(null);
  const dragDepth = useRef(0);

  const selected = detail?.app ?? apps.find((item) => item.app_id === selectedId) ?? null;
  const backend = detail?.backend ?? selected?.backend ?? null;

  const load = useCallback(async (preferred?: string) => {
    setRefreshing(true);
    try {
      const response = await extensionApi.list();
      setApps(response.apps);
      const target = preferred ?? selectedId;
      const next = response.apps.find((item) => item.app_id === target) ?? response.apps[0];
      setSelectedId(next?.app_id ?? '');
      if (next) {
        const info = await extensionApi.detail(next.app_id);
        setDetail({ app: info.app, revisions: info.revisions, backend: info.backend });
      } else {
        setDetail(null);
      }
      setError('');
    } catch (caught) {
      setError(errorMessage(caught));
    } finally {
      setRefreshing(false);
    }
  }, [selectedId]);

  useEffect(() => {
    void load();
    // Load once when the management window opens.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  useEffect(() => {
    if (!detail) return;
    setSelectedPermissions([...detail.app.granted_permissions].sort());
    setIntegrity(null);
  }, [detail?.app.app_id, detail?.app.updated_at]);

  useEffect(() => {
    setBackendForm({
      run_user: backend?.run_user ?? '',
      run_group: backend?.run_group ?? '',
      allow_root: backend?.allow_root === true,
    });
    // Re-sync only when the server-reported identity changes, not on each render.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [detail?.app.app_id, backend?.run_user, backend?.run_group, backend?.allow_root]);

  const select = async (appId: string) => {
    setSelectedId(appId);
    setBusy('load');
    try {
      const info = await extensionApi.detail(appId);
      setDetail({ app: info.app, revisions: info.revisions, backend: info.backend });
      setError('');
    } catch (caught) {
      setError(errorMessage(caught));
    } finally {
      setBusy(null);
    }
  };

  const afterMutation = async (message: string, appId?: string) => {
    setNotice(message);
    await load(appId ?? selectedId);
    onExtensionsChanged();
  };

  const run = async (label: string, action: () => Promise<void>) => {
    setBusy(label);
    setError('');
    setNotice('');
    try {
      await action();
    } catch (caught) {
      setError(errorMessage(caught));
    } finally {
      setBusy(null);
    }
  };

  const upload = async (
    file: File,
    appId?: string,
    overwrite = false,
    allowDowngrade = false,
    restoreRetained = false,
  ) => {
    await run(appId ? 'upgrade' : 'install', async () => {
      try {
        const result = appId
          ? await extensionApi.upgrade(appId, file, { overwrite, allowDowngrade })
          : await extensionApi.install(file, { overwrite, allowDowngrade, restoreData: restoreRetained });
        // Upgrade/overwrite closes every window of the existing application.
        if (appId) windowStore.closeAllForApp(appId);
        setInstallWarnings(result.warnings ?? []);
        setPendingUpload(null);
        await afterMutation(
          result.approval_required
            ? `已安装 ${result.app.display_name} ${result.revision.version}，等待管理员权限审批。`
            : `已${appId ? '升级' : '安装'} ${result.app.display_name} ${result.revision.version}。`,
          result.app.app_id,
        );
      } catch (caught) {
        if (caught instanceof ApiError && caught.code === 'conflict') {
          const message = caught.message;
          if (message.includes('overwrite') || message.includes('same version')) {
            setPendingUpload({ file, appId, overwrite: true, allowDowngrade, restoreData: restoreRetained });
            return;
          }
          if (message.includes('older')) {
            setPendingUpload({ file, appId, overwrite: false, allowDowngrade: true, restoreData: restoreRetained });
            return;
          }
        }
        throw caught;
      }
    });
  };

  const onInstallFile = (file: File | undefined) => {
    if (!file) return;
    void upload(file, undefined, false, false, restoreData);
    if (installInput.current) installInput.current.value = '';
  };

  const onUpgradeFile = (file: File | undefined) => {
    if (!file || !selected) return;
    void upload(file, selected.app_id);
    if (upgradeInput.current) upgradeInput.current.value = '';
  };

  // 拖放上传：默认作用于当前选中的应用（升级），没有可升级目标时按新应用安装；
  // 实际上传前弹出确认窗口。
  const onDragEnter = (event: ReactDragEvent) => {
    if (!event.dataTransfer.types.includes('Files')) return;
    event.preventDefault();
    dragDepth.current += 1;
    setDragActive(true);
  };

  const onDragOver = (event: ReactDragEvent) => {
    if (!event.dataTransfer.types.includes('Files')) return;
    event.preventDefault();
    event.dataTransfer.dropEffect = 'copy';
  };

  const onDragLeave = (event: ReactDragEvent) => {
    if (!event.dataTransfer.types.includes('Files')) return;
    dragDepth.current = Math.max(0, dragDepth.current - 1);
    if (dragDepth.current === 0) setDragActive(false);
  };

  const onDrop = (event: ReactDragEvent) => {
    event.preventDefault();
    dragDepth.current = 0;
    setDragActive(false);
    if (busy !== null) return;
    const file = event.dataTransfer.files?.[0];
    if (!file) return;
    if (!isZipFile(file)) {
      setError('仅支持上传 ZIP 安装包。');
      return;
    }
    const target = selected && selected.status !== 'uninstalled' ? selected : null;
    setPendingDrop(
      target
        ? { file, appId: target.app_id, targetName: target.display_name }
        : { file },
    );
  };

  const confirmDrop = () => {
    const pending = pendingDrop;
    setPendingDrop(null);
    if (pending) {
      void upload(pending.file, pending.appId, false, false, pending.appId ? false : restoreData);
    }
  };

  const savePermissions = () =>
    selected &&
    run('permissions', async () => {
      const result = await extensionApi.updatePermissions(selected.app_id, selectedPermissions);
      await afterMutation('权限已更新；当前运行实例仍使用创建时的权限快照。', result.app.app_id);
    });

  const approve = (enable: boolean) =>
    selected &&
    run(enable ? 'approve' : 'approve-disabled', async () => {
      const result = await extensionApi.approve(selected.app_id, selectedPermissions, enable);
      await afterMutation(
        enable ? '权限已批准，应用已启用。' : '权限已批准，应用保持禁用。',
        result.app.app_id,
      );
    });

  const toggleEnabled = (enable: boolean) =>
    selected &&
    run(enable ? 'enable' : 'disable', async () => {
      const result = enable
        ? await extensionApi.enable(selected.app_id)
        : await extensionApi.disable(selected.app_id);
      await afterMutation(enable ? '应用已启用。' : '应用已禁用，相关窗口与终端会话已关闭。', result.app.app_id);
    });

  const checkIntegrity = () =>
    selected &&
    run('integrity', async () => {
      const result = await extensionApi.integrityCheck(selected.app_id);
      setIntegrity(result);
      if (!result.ok) {
        if (result.app_isolated) {
          windowStore.closeAllForApp(selected.app_id);
          await afterMutation('完整性校验失败：活动修订异常，应用已隔离并保留异常文件。', selected.app_id);
        } else {
          await afterMutation(
            `完整性校验失败：修订 ${result.revision_id.slice(0, 8)} 已标记为不可运行，应用状态未改变。`,
            selected.app_id,
          );
        }
      } else {
        await afterMutation('完整性校验通过。', selected.app_id);
      }
    });

  const pin = (revision: ExtensionRevision) =>
    selected &&
    run('pin', async () => {
      await extensionApi.pin(selected.app_id, revision.revision_id, !revision.pinned);
      await afterMutation(revision.pinned ? '已取消固定修订。' : '已固定修订，清理时不会被删除。');
    });

  const activate = () => {
    if (!activateDialog) return;
    const target = activateDialog;
    setActivateDialog(null);
    void run('activate', async () => {
      const result = await extensionApi.activate(target.app.app_id, target.revision.revision_id);
      windowStore.closeAllForApp(target.app.app_id);
      await afterMutation(`已切换到修订 ${target.revision.revision_id.slice(0, 8)}。`, result.app.app_id);
    });
  };

  const uninstall = (dataAction: 'keep' | 'delete') => {
    if (!uninstallDialog) return;
    const target = uninstallDialog;
    setUninstallDialog(null);
    void run('uninstall', async () => {
      await extensionApi.uninstall(target.app_id, dataAction);
      windowStore.closeAllForApp(target.app_id);
      setDetail(null);
      await afterMutation(
        dataAction === 'keep' ? '应用已卸载，用户数据已隔离保留。' : '应用与用户数据已删除，审计历史保留。',
      );
    });
  };

  const saveBackendIdentity = () =>
    selected &&
    run('backend-identity', async () => {
      await extensionApi.setBackendIdentity(selected.app_id, {
        run_user: backendForm.run_user.trim(),
        run_group: backendForm.run_group.trim(),
        allow_root: backendForm.allow_root,
      });
      await afterMutation(
        '运行身份已保存；重新启动后端后生效。',
        selected.app_id,
      );
    });

  const backendLifecycle = (kind: 'start' | 'stop' | 'restart') =>
    selected &&
    run(`backend-${kind}`, async () => {
      if (kind === 'start') await extensionApi.startBackend(selected.app_id);
      else if (kind === 'stop') await extensionApi.stopBackend(selected.app_id);
      else await extensionApi.restartBackend(selected.app_id);
      await afterMutation(
        `已提交${kind === 'start' ? '启动' : kind === 'stop' ? '停止' : '重启'}原生后端请求；状态以 Supervisor 为准。`,
        selected.app_id,
      );
    });

  const setAutostart = (enabled: boolean) =>
    selected &&
    run('backend-autostart', async () => {
      await extensionApi.setBackendAutostart(selected.app_id, enabled);
      await afterMutation(
        enabled ? '已启用开机自启，与手动启动/停止相互独立。' : '已关闭开机自启。',
        selected.app_id,
      );
    });

  const openProcessLogs = () => {
    windowStore.launch('processes');
  };

  const openPurgeDialog = (app: ExtensionApp) => {
    setPurgeConfirm('');
    setPurgeDialog(app);
  };

  const confirmPurge = () => {
    if (!purgeDialog || purgeConfirm !== purgeDialog.app_id) return;
    const target = purgeDialog;
    setPurgeDialog(null);
    setPurgeConfirm('');
    void run('purge', async () => {
      await extensionApi.purgeAppData(target.app_id);
      await afterMutation('应用数据已彻底删除；程序文件、修订与审计历史保留。', target.app_id);
    });
  };

  const revisionList = useMemo(() => detail?.revisions ?? [], [detail]);

  return (
    <div
      className="ed-ext-view"
      onDragEnter={onDragEnter}
      onDragOver={onDragOver}
      onDragLeave={onDragLeave}
      onDrop={onDrop}
    >
      {dragActive && (
        <div className="ed-ext-drop-overlay" aria-hidden="true">
          <Icon icon={FileArchive} size={28} />
          <strong>
            {selected && selected.status !== 'uninstalled'
              ? `松开以上传「${selected.display_name}」的升级包`
              : '松开以安装 ZIP 扩展应用'}
          </strong>
          <p>上传前会弹出确认窗口。</p>
        </div>
      )}
      <header className="ed-ext-toolbar">
        <div>
          <p className="ed-ext-eyebrow">EXTENSION PLATFORM</p>
          <h2>应用管理</h2>
          <p>上传 ZIP 静态应用，审批权限并管理不可变修订。发布者身份未经验证。</p>
        </div>
        <div className="ed-ext-toolbar-actions">
          <label
            className="ed-ext-restore"
            title="仅当上传包的 appId 存在已保留数据时生效；未勾选将删除保留数据"
          >
            <input
              type="checkbox"
              checked={restoreData}
              onChange={(event) => setRestoreData(event.target.checked)}
            />
            恢复保留数据
          </label>
          <button
            type="button"
            className="ed-button"
            onClick={() => void load(selectedId)}
            disabled={refreshing}
          >
            <Icon icon={RefreshCw} size={14} />
            刷新
          </button>
          <input
            ref={installInput}
            type="file"
            accept=".zip,application/zip"
            hidden
            onChange={(event) => onInstallFile(event.target.files?.[0])}
          />
          <button
            type="button"
            className="ed-button primary"
            onClick={() => installInput.current?.click()}
            disabled={busy !== null}
          >
            <Icon icon={Upload} size={14} />
            安装 ZIP
          </button>
        </div>
      </header>

      {error && (
        <div className="ed-ext-banner danger" role="alert">
          <Icon icon={AlertTriangle} size={15} />
          <span>{error}</span>
          <button type="button" className="ed-link" onClick={() => setError('')}>关闭</button>
        </div>
      )}
      {notice && (
        <div className="ed-ext-banner ok" role="status">
          <Icon icon={Check} size={15} />
          <span>{notice}</span>
          <button type="button" className="ed-link" onClick={() => setNotice('')}>关闭</button>
        </div>
      )}
      {installWarnings.length > 0 && (
        <div className="ed-ext-banner warning" role="status">
          <Icon icon={AlertTriangle} size={15} />
          <span>{installWarnings.join(' · ')}</span>
        </div>
      )}

      <div className="ed-ext-layout">
        <aside className="ed-ext-list" aria-label="已安装应用">
          {apps.length === 0 && !refreshing && (
            <div className="ed-ext-empty">
              <Icon icon={FileArchive} size={30} />
              <strong>尚未安装扩展应用</strong>
              <p>安装 ZIP 包后，应用会在权限审批并启用后出现在启动器中。</p>
            </div>
          )}
          {apps.map((app) => (
            <button
              key={app.app_id}
              type="button"
              className={`ed-ext-item${app.app_id === selectedId ? ' selected' : ''}`}
              onClick={() => void select(app.app_id)}
            >
              <span className={`ed-ext-status ${statusClass(app.status)}`} aria-hidden="true" />
              <span className="ed-ext-item-main">
                <strong>{app.display_name}</strong>
                <span>{app.app_id}</span>
                <span className="ed-ext-item-meta">
                  {app.active_version || '无活动版本'} · {STATUS_LABEL[app.status] ?? app.status}
                </span>
              </span>
            </button>
          ))}
        </aside>

        <section className="ed-ext-detail" aria-label="应用详情">
          {!selected ? (
            <div className="ed-ext-empty tall">
              <Icon icon={ShieldCheck} size={32} />
              <strong>选择或安装一个应用</strong>
              <p>扩展应用只能访问管理员批准的模块权限，运行在无 same-origin 的 iframe 沙箱中。</p>
            </div>
          ) : (
            <>
              <div className="ed-ext-detail-head">
                <div>
                  <div className="ed-ext-title-line">
                    <h3>{selected.display_name}</h3>
                    <span className={`ed-badge ${selected.status === 'isolated' ? 'warning' : ''}`}>
                      {STATUS_LABEL[selected.status] ?? selected.status}
                    </span>
                    {!selected.publisher_verified && <span className="ed-badge warning">发布者未验证</span>}
                  </div>
                  <p className="ed-ext-id">{selected.app_id}</p>
                  <p className="ed-ext-version">
                    版本 {selected.active_version || '—'}
                    <span>修订 {selected.active_revision_id ? selected.active_revision_id.slice(0, 12) : '—'}</span>
                  </p>
                  {!selected.publisher_verified && (
                    <p className="ed-ext-unverified" role="note">
                      未签名包，发布者身份未验证；SHA-256 仅用于完整性检查。
                    </p>
                  )}
                </div>
                <div className="ed-ext-actions">
                  {(selected.status === 'enabled' || selected.status === 'disabled') && (
                    <button
                      type="button"
                      className="ed-button"
                      onClick={() => toggleEnabled(selected.status !== 'enabled')}
                      disabled={busy !== null}
                    >
                      <Icon icon={selected.status === 'enabled' ? Pause : Play} size={14} />
                      {selected.status === 'enabled' ? '禁用' : '启用'}
                    </button>
                  )}
                  <button
                    type="button"
                    className="ed-button"
                    onClick={checkIntegrity}
                    disabled={busy !== null || selected.status === 'uninstalled'}
                  >
                    <Icon icon={ShieldCheck} size={14} />
                    完整性检查
                  </button>
                  <button
                    type="button"
                    className="ed-button danger"
                    onClick={() => setUninstallDialog(selected)}
                    disabled={busy !== null || selected.status === 'uninstalled'}
                  >
                    <Icon icon={Trash2} size={14} />
                    卸载
                  </button>
                </div>
              </div>

              {selected.status === 'isolated' && (
                <div className="ed-ext-banner danger" role="alert">
                  <Icon icon={AlertTriangle} size={15} />
                  <span>
                    应用因文件哈希不一致被隔离。异常修订与现场已保留；需要重新安装或上传修复包后才能恢复。
                  </span>
                </div>
              )}

              {selected.description && <p className="ed-ext-description">{selected.description}</p>}

              {backend?.present && (
                <section className="ed-ext-section ed-ext-backend" aria-busy={busy !== null} aria-label="原生后端">
                  <div className="ed-ext-backend-head">
                    <h4>
                      <Icon icon={Server} size={14} />
                      原生后端
                    </h4>
                    <span className={`ed-badge ${backendStateClass(backend.state)}`}>
                      {BACKEND_STATE_LABEL[backend.state ?? ''] ?? backend.state ?? '状态未知'}
                    </span>
                    <span className={`ed-ext-backend-live ${supervisorStateClass(backend.supervisor?.state)}`}>
                      <span
                        className={`ed-ext-status ${supervisorStateClass(backend.supervisor?.state)}`}
                        aria-hidden="true"
                      />
                      Supervisor {supervisorStateLabel(backend.supervisor?.state)}
                    </span>
                  </div>
                  <p className="ed-ext-hint">
                    原生后端由独立 Supervisor 服务托管，进程 RUNNING 不代表 HTTP API 已就绪。安装后默认停止，开机自启默认关闭。
                  </p>
                  <p className="ed-ext-unverified" role="note">
                    未签名包，发布者身份未验证；SHA-256 仅用于完整性检查，不证明发布者身份。
                  </p>
                  {archMismatch(backend) && (
                    <div className="ed-ext-banner warning ed-ext-banner-inline" role="alert">
                      <Icon icon={AlertTriangle} size={15} />
                      <span>
                        后端架构 {backend.arch} 与主机架构 {backend.host_arch} 不匹配，启动会被拒绝。
                      </span>
                    </div>
                  )}
                  {backend.error && (
                    <div className="ed-ext-banner danger ed-ext-banner-inline" role="status">
                      <Icon icon={AlertTriangle} size={15} />
                      <span>{backend.error}</span>
                    </div>
                  )}
                  <dl className="ed-ext-backend-facts">
                    <div>
                      <dt>后端架构</dt>
                      <dd>
                        <code>{backend.arch || '—'}</code>
                        {backend.host_arch && <span className="ed-ext-muted"> / 主机 {backend.host_arch}</span>}
                      </dd>
                    </div>
                    <div>
                      <dt>Supervisor PID</dt>
                      <dd>{backend.supervisor?.pid ?? '—'}</dd>
                    </div>
                    <div>
                      <dt>最近退出</dt>
                      <dd>
                        {backend.supervisor?.last_exit_code == null && !backend.supervisor?.last_exit_at
                          ? '—'
                          : `退出码 ${backend.supervisor?.last_exit_code ?? '—'} · ${formatBackendTime(
                              backend.supervisor?.last_exit_at,
                            )}`}
                      </dd>
                    </div>
                    <div>
                      <dt>服务 ID</dt>
                      <dd>
                        <code>{backend.service_id || '—'}</code>
                      </dd>
                    </div>
                    <div className="wide">
                      <dt>可执行文件</dt>
                      <dd>
                        <code>{backend.executable || '—'}</code>
                      </dd>
                    </div>
                    <div className="wide">
                      <dt>程序目录（只读）</dt>
                      <dd>
                        <code>{backend.program_dir || '—'}</code>
                      </dd>
                    </div>
                    <div className="wide">
                      <dt>数据目录（只读）</dt>
                      <dd>
                        <code>{backend.data_dir || '—'}</code>
                      </dd>
                    </div>
                    <div className="wide">
                      <dt>Socket 路径（只读）</dt>
                      <dd>
                        <code>{backend.socket_path || '—'}</code>
                      </dd>
                    </div>
                  </dl>

                  <div className="ed-ext-backend-actions">
                    <button
                      type="button"
                      className="ed-button primary"
                      onClick={() => void backendLifecycle('start')}
                      disabled={
                        busy !== null ||
                        supervisorState(backend.supervisor?.state) === 'RUNNING' ||
                        supervisorState(backend.supervisor?.state) === 'STARTING'
                      }
                    >
                      <Icon icon={Play} size={14} />
                      启动
                    </button>
                    <button
                      type="button"
                      className="ed-button"
                      onClick={() => void backendLifecycle('stop')}
                      disabled={
                        busy !== null ||
                        supervisorState(backend.supervisor?.state) === 'STOPPED' ||
                        supervisorState(backend.supervisor?.state) === 'STOPPING'
                      }
                    >
                      <Icon icon={Square} size={14} />
                      停止
                    </button>
                    <button
                      type="button"
                      className="ed-button"
                      onClick={() => void backendLifecycle('restart')}
                      disabled={busy !== null}
                    >
                      <Icon icon={RotateCw} size={14} />
                      重启
                    </button>
                    <button type="button" className="ed-button" onClick={openProcessLogs}>
                      <Icon icon={ScrollText} size={14} />
                      进程日志
                    </button>
                  </div>

                  <div className="ed-ext-backend-identity">
                    <h5>运行身份</h5>
                    <p className="ed-ext-hint">
                      运行用户与用户组由管理员指定，manifest 不能覆盖。保存后需重新启动后端才会生效。
                    </p>
                    <div className="ed-ext-backend-fields">
                      <label className="ed-ext-field">
                        <span>运行用户</span>
                        <input
                          type="text"
                          value={backendForm.run_user}
                          onChange={(event) =>
                            setBackendForm((current) => ({ ...current, run_user: event.target.value }))
                          }
                          disabled={busy !== null}
                          autoComplete="off"
                          spellCheck={false}
                        />
                      </label>
                      <label className="ed-ext-field">
                        <span>运行组</span>
                        <input
                          type="text"
                          value={backendForm.run_group}
                          onChange={(event) =>
                            setBackendForm((current) => ({ ...current, run_group: event.target.value }))
                          }
                          disabled={busy !== null}
                          autoComplete="off"
                          spellCheck={false}
                        />
                      </label>
                    </div>
                    <label className="ed-ext-check">
                      <input
                        type="checkbox"
                        checked={backendForm.allow_root}
                        onChange={(event) =>
                          setBackendForm((current) => ({ ...current, allow_root: event.target.checked }))
                        }
                        disabled={busy !== null}
                      />
                      允许以 root 运行
                    </label>
                    {backendForm.allow_root && (
                      <div className="ed-ext-banner danger ed-ext-banner-inline" role="alert">
                        <Icon icon={AlertTriangle} size={15} />
                        <span>
                          root 后端拥有主机级权限，可能读写任意系统文件或控制整台设备。仅在完全信任该程序时启用。
                        </span>
                      </div>
                    )}
                    <div className="ed-ext-inline-actions">
                      <button
                        type="button"
                        className="ed-button"
                        onClick={() => void saveBackendIdentity()}
                        disabled={busy !== null}
                      >
                        保存运行身份
                      </button>
                    </div>
                  </div>

                  <div className="ed-ext-autostart">
                    <label className="ed-ext-check">
                      <input
                        type="checkbox"
                        checked={backend.autostart === true}
                        onChange={(event) => void setAutostart(event.target.checked)}
                        disabled={busy !== null}
                      />
                      开机自启
                    </label>
                    <span className="ed-ext-muted">独立设置，默认关闭；手动启动/停止不会修改自启状态。</span>
                  </div>
                </section>
              )}

              <div className="ed-ext-grid">
                <section className="ed-ext-section">
                  <h4>权限审批</h4>
                  <p className="ed-ext-hint">
                    权限按功能模块授权，默认拒绝。撤销单项权限只对下次启动生效；禁用应用会立即终止。
                  </p>
                  {requestedPermissionList(selected).length === 0 && (
                    <p className="ed-ext-muted">该应用未申请任何功能模块权限。</p>
                  )}
                  <div className="ed-ext-permissions">
                    {requestedPermissionList(selected).map((permission) => {
                      const checked = selectedPermissions.includes(permission);
                      const granted = selected.granted_permissions.includes(permission);
                      return (
                        <label key={permission} className="ed-ext-permission">
                          <input
                            type="checkbox"
                            checked={checked}
                            onChange={(event) =>
                              setSelectedPermissions((current) =>
                                event.target.checked
                                  ? [...new Set([...current, permission])].sort()
                                  : current.filter((item) => item !== permission),
                              )
                            }
                          />
                          <span>
                            <strong>{permissionLabel(permission)}</strong>
                            <code>{permission}</code>
                          </span>
                          {granted && <span className="ed-badge subtle">已授权</span>}
                        </label>
                      );
                    })}
                  </div>
                  {selected.requested_permissions.includes('network') &&
                    !selected.granted_permissions.includes('network') && (
                      <p className="ed-ext-hint" role="status">
                        网络来源尚未授权，应用无法加载任何外部资源。升级新增或扩大的来源需要管理员重新审核后才会生效。
                      </p>
                    )}
                  {selected.network_origins.length > 0 && (
                    <div className="ed-ext-origins">
                      <span>已声明的外部来源（共用白名单，http/明文风险自担）</span>
                      <ul>
                        {selected.network_origins.map((origin) => (
                          <li key={origin}><code>{origin}</code></li>
                        ))}
                      </ul>
                    </div>
                  )}
                  <div className="ed-ext-inline-actions">
                    {selected.status === 'pending_review' ? (
                      <>
                        <button
                          type="button"
                          className="ed-button primary"
                          onClick={() => approve(true)}
                          disabled={busy !== null}
                        >
                          批准并启用
                        </button>
                        <button
                          type="button"
                          className="ed-button"
                          onClick={() => approve(false)}
                          disabled={busy !== null}
                        >
                          批准但保持禁用
                        </button>
                      </>
                    ) : (
                      <button
                        type="button"
                        className="ed-button"
                        onClick={savePermissions}
                        disabled={busy !== null || selected.status === 'uninstalled'}
                      >
                        保存权限
                      </button>
                    )}
                  </div>
                </section>

                <section className="ed-ext-section">
                  <h4>版本与修订</h4>
                  <p className="ed-ext-hint">
                    相同 SemVer 可覆盖为不同内容；每次安装都会生成新的不可变 revisionId。
                    {selected.status !== 'uninstalled' && ' 升级会关闭全部应用窗口。'}
                  </p>
                  <input
                    ref={upgradeInput}
                    type="file"
                    accept=".zip,application/zip"
                    hidden
                    onChange={(event) => onUpgradeFile(event.target.files?.[0])}
                  />
                  <div className="ed-ext-inline-actions">
                    <button
                      type="button"
                      className="ed-button"
                      onClick={() => upgradeInput.current?.click()}
                      disabled={busy !== null || selected.status === 'uninstalled'}
                    >
                      <Icon icon={Upload} size={14} />
                      上传升级包
                    </button>
                  </div>
                  <ul className="ed-ext-revisions">
                    {revisionList.map((revision) => (
                      <li key={revision.revision_id} className={revision.active ? 'active' : ''}>
                        <div>
                          <strong>{revision.version}</strong>
                          <code>{revision.revision_id.slice(0, 12)}</code>
                          {revision.active && <span className="ed-badge">活动</span>}
                          {revision.integrity !== 'ok' && <span className="ed-badge warning">完整性异常</span>}
                          {revision.status === 'isolated' && <span className="ed-badge warning">已隔离</span>}
                        </div>
                        <span className="ed-ext-revision-meta">
                          {revision.file_count} 文件 · {formatBytes(revision.total_bytes)} · {formatTime(revision.created_at)}
                        </span>
                        <div className="ed-ext-revision-actions">
                          <button
                            type="button"
                            className={`ed-button small${revision.pinned ? ' primary' : ''}`}
                            onClick={() => pin(revision)}
                            disabled={busy !== null}
                            title={revision.pinned ? '取消固定' : '固定修订'}
                          >
                            <Icon icon={Pin} size={12} />
                            {revision.pinned ? '已固定' : '固定'}
                          </button>
                          {!revision.active && revision.integrity === 'ok' && (
                            <button
                              type="button"
                              className="ed-button small"
                              onClick={() => setActivateDialog({ app: selected, revision })}
                              disabled={busy !== null}
                            >
                              切换到此修订
                            </button>
                          )}
                        </div>
                      </li>
                    ))}
                  </ul>
                </section>
              </div>

              <section className="ed-ext-section ed-ext-data" aria-label="应用数据">
                <h4>应用数据</h4>
                <p className="ed-ext-hint">
                  卸载默认保留应用数据。彻底删除是独立操作，不会随普通卸载执行，也无法撤销。
                </p>
                <p className="ed-ext-data-scope">
                  数据范围：平台 KV 命名空间 <code>{selected.app_id}</code>
                  {backend?.present && backend.data_dir && (
                    <>
                      {' '}· 原生数据目录 <code>{backend.data_dir}</code>
                    </>
                  )}
                </p>
                <div className="ed-ext-inline-actions">
                  <button
                    type="button"
                    className="ed-button danger"
                    onClick={() => openPurgeDialog(selected)}
                    disabled={busy !== null || selected.status === 'uninstalled'}
                  >
                    <Icon icon={Trash2} size={14} />
                    彻底删除应用数据
                  </button>
                </div>
              </section>

              {integrity && (
                <div className={`ed-ext-banner ${integrity.ok ? 'ok' : 'danger'}`} role="status">
                  <Icon icon={integrity.ok ? Check : AlertTriangle} size={15} />
                  <span>
                    {integrity.ok
                      ? `完整性校验通过：${integrity.revision_id}`
                      : integrity.app_isolated
                        ? `完整性校验失败：${integrity.mismatch_path || integrity.revision_id}（应用已隔离）`
                        : `完整性校验失败：${integrity.mismatch_path || integrity.revision_id}（该修订已隔离，应用状态未改变）`}
                  </span>
                </div>
              )}

              <div className="ed-ext-meta">
                <span>安装时间 {formatTime(selected.installed_at)}</span>
                <span>最近更新 {formatTime(selected.updated_at)}</span>
                {selected.data_retained && <span className="ed-badge warning">保留数据</span>}
                {selected.host_api && <span>宿主 API {selected.host_api.min} – {selected.host_api.max}</span>}
              </div>
            </>
          )}
        </section>
      </div>

      {pendingUpload && (
        <ConfirmDialog
          title={pendingUpload.allowDowngrade ? '确认降级到较旧版本？' : '确认覆盖相同版本？'}
          message={
            pendingUpload.allowDowngrade
              ? '新包版本低于当前活动修订。降级可能带来安全与数据兼容风险，用户数据不会回滚。'
              : '相同 SemVer 将创建新的修订，已有修订与缓存不会被复用。'
          }
          confirmText="继续安装"
          onConfirm={() => {
            const pending = pendingUpload;
            setPendingUpload(null);
            if (pending) {
              void upload(
                pending.file,
                pending.appId,
                pending.overwrite,
                pending.allowDowngrade,
                pending.restoreData,
              );
            }
          }}
          onCancel={() => setPendingUpload(null)}
        />
      )}

      {pendingDrop && (
        <ConfirmDialog
          title={pendingDrop.appId ? `升级 ${pendingDrop.targetName}？` : '安装扩展应用？'}
          message={
            pendingDrop.appId
              ? `将把 ${pendingDrop.file.name} 作为「${pendingDrop.targetName}」的升级包上传。每次上传都会生成新的不可变修订，升级会关闭该应用全部窗口。`
              : `将上传 ${pendingDrop.file.name} 并创建新的扩展应用。安装后需管理员审批权限才可启动。`
          }
          confirmText={pendingDrop.appId ? '上传升级包' : '上传并安装'}
          onConfirm={confirmDrop}
          onCancel={() => setPendingDrop(null)}
        />
      )}

      {activateDialog && (
        <ConfirmDialog
          title="切换活动修订"
          message={`将 ${activateDialog.app.display_name} 切换到版本 ${activateDialog.revision.version}。此操作会关闭应用全部窗口，并保留用户数据。`}
          confirmText="切换"
          onConfirm={activate}
          onCancel={() => setActivateDialog(null)}
        />
      )}

      {uninstallDialog && (
        <div className="ed-modal-backdrop" onPointerDown={() => setUninstallDialog(null)}>
          <div
            className="ed-modal ed-panel"
            role="alertdialog"
            aria-modal="true"
            aria-label="卸载应用"
            onPointerDown={(event) => event.stopPropagation()}
          >
            <h2>卸载 {uninstallDialog.display_name}</h2>
            <p className="ed-modal-text">
              卸载会立即撤销全部有效权限并关闭相关窗口。请选择如何处理该应用的用户数据。
            </p>
            <ul className="ed-modal-list">
              <li>保留：数据进入隔离状态，重新安装时需要再次确认才能恢复。</li>
              <li>删除：清理应用 KV 与隔离数据；审计历史仍会保留。</li>
            </ul>
            <div className="ed-modal-actions">
              <button type="button" className="ed-button" onClick={() => setUninstallDialog(null)}>
                取消
              </button>
              <button type="button" className="ed-button" onClick={() => uninstall('keep')}>
                保留数据并卸载
              </button>
              <button type="button" className="ed-button danger" onClick={() => uninstall('delete')}>
                删除数据并卸载
              </button>
            </div>
          </div>
        </div>
      )}
      {purgeDialog && (
        <div
          className="ed-modal-backdrop"
          onPointerDown={() => {
            setPurgeDialog(null);
            setPurgeConfirm('');
          }}
        >
          <div
            className="ed-modal ed-panel"
            role="alertdialog"
            aria-modal="true"
            aria-label="彻底删除应用数据"
            onPointerDown={(event) => event.stopPropagation()}
          >
            <h2>彻底删除 {purgeDialog.display_name} 的应用数据</h2>
            <p className="ed-modal-text">
              此操作不可恢复。程序文件、Web 修订与审计历史会保留，只清理该应用专属的数据。
            </p>
            <ul className="ed-modal-list">
              <li>
                平台 KV 命名空间：<code>{purgeDialog.app_id}</code>
              </li>
              {backend?.present && backend.data_dir && (
                <li>
                  原生数据目录：<code>{backend.data_dir}</code>
                </li>
              )}
              <li>不会删除其他应用的数据、程序文件或 Supervisor 服务配置。</li>
            </ul>
            <label className="ed-ext-field ed-ext-purge-field">
              <span>
                输入应用 ID <code>{purgeDialog.app_id}</code> 以确认：
              </span>
              <input
                type="text"
                value={purgeConfirm}
                onChange={(event) => setPurgeConfirm(event.target.value)}
                autoFocus
                autoComplete="off"
                spellCheck={false}
                aria-label="确认应用 ID"
                onKeyDown={(event) => {
                  if (event.key === 'Enter') confirmPurge();
                }}
              />
            </label>
            <div className="ed-modal-actions">
              <button
                type="button"
                className="ed-button"
                onClick={() => {
                  setPurgeDialog(null);
                  setPurgeConfirm('');
                }}
                disabled={busy !== null}
              >
                取消
              </button>
              <button
                type="button"
                className="ed-button danger"
                onClick={confirmPurge}
                disabled={busy !== null || purgeConfirm !== purgeDialog.app_id}
              >
                {busy === 'purge' ? '删除中…' : '彻底删除数据'}
              </button>
            </div>
          </div>
        </div>
      )}
    </div>
  );
}
