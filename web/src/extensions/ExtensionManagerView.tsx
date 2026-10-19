import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import {
  AlertTriangle,
  Check,
  FileArchive,
  Pause,
  Pin,
  Play,
  RefreshCw,
  ShieldCheck,
  Trash2,
  Upload,
} from 'lucide-react';
import { Icon } from '../components/Icon';
import { ConfirmDialog } from '../components/ConfirmDialog';
import { ApiError, formatBytes, formatTime, apiRequest } from '../services/api';
import { windowStore } from '../stores/windowStore';
import { extensionApi } from './extensionApi';
import type { ExtensionApp, ExtensionIntegrityResult, ExtensionRevision } from './extensionTypes';
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

const STATUS_LABEL: Record<string, string> = {
  pending_review: '待审批',
  enabled: '已启用',
  disabled: '已禁用',
  isolated: '已隔离',
  uninstalled: '已卸载',
};

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

export function ExtensionManagerView({ onExtensionsChanged }: Props) {
  const [apps, setApps] = useState<ExtensionApp[]>([]);
  const [selectedId, setSelectedId] = useState<string>('');
  const [detail, setDetail] = useState<{ app: ExtensionApp; revisions: ExtensionRevision[] } | null>(null);
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
  const [refreshing, setRefreshing] = useState(false);
  const installInput = useRef<HTMLInputElement | null>(null);
  const upgradeInput = useRef<HTMLInputElement | null>(null);

  const selected = detail?.app ?? apps.find((item) => item.app_id === selectedId) ?? null;

  const load = useCallback(async (preferred?: string) => {
    setRefreshing(true);
    try {
      const response = await extensionApi.list();
      setApps(response.apps);
      const target = preferred ?? selectedId;
      const next = response.apps.find((item) => item.app_id === target) ?? response.apps[0];
      setSelectedId(next?.app_id ?? '');
      if (next) {
        const info = await apiRequest<{ app: ExtensionApp; revisions: ExtensionRevision[] }>(
          `/api/extensions/${encodeURIComponent(next.app_id)}`,
        );
        setDetail({ app: info.app, revisions: info.revisions });
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

  const select = async (appId: string) => {
    setSelectedId(appId);
    setBusy('load');
    try {
      const info = await extensionApi.detail(appId);
      setDetail({ app: info.app, revisions: info.revisions });
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

  const revisionList = useMemo(() => detail?.revisions ?? [], [detail]);

  return (
    <div className="ed-ext-view">
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
    </div>
  );
}
