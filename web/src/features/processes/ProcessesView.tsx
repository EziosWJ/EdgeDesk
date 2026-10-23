import { useCallback, useEffect, useRef, useState } from 'react';
import {
  AlertTriangle,
  Cpu,
  MemoryStick,
  Pencil,
  Play,
  Plus,
  RefreshCw,
  RotateCw,
  ScrollText,
  Server,
  Square,
  Trash2,
} from 'lucide-react';
import { Icon } from '../../components/Icon';
import { ConfirmDialog } from '../../components/ConfirmDialog';
import {
  ApiError,
  api,
  formatPercent,
  formatTime,
  formatUptime,
  formatBytes,
} from '../../services/api';
import type {
  ServicePayload,
  ServiceLifecycle,
  SupervisedService,
  SupervisorHealth,
  SupervisorOperation,
} from '../../types';
import { ServiceEditor } from './ServiceEditor';
import './processes.css';

const POLL_MS = 3000;
const LOG_POLL_MS = 2000;
const OPERATION_POLL_MS = 1000;
// Longer than the maximum server-side stop timeout (600s) plus the SIGKILL
// grace period. Waiting here only bounds the UI; the Supervisor keeps running
// the operation if the page is refreshed or closed.
const MAX_OPERATION_WAIT_MS = 11 * 60 * 1000;
const MAX_LOG_CHARS = 256 * 1024;

type ConfirmAction = {
  kind: 'start' | 'stop' | 'restart' | 'delete';
  service: SupervisedService;
};

const STATE_LABEL: Record<ServiceLifecycle, string> = {
  STOPPED: '已停止',
  STARTING: '启动中',
  RUNNING: '运行中',
  STOPPING: '停止中',
  BACKOFF: '等待重启',
  FAILED: '失败锁定',
  UNKNOWN: '状态未知',
};

const POLICY_LABEL: Record<string, string> = {
  always: 'always · 总是重启',
  'on-failure': 'on-failure · 异常退出重启',
  never: 'never · 不自动重启',
};

function errorMessage(error: unknown): string {
  if (error instanceof ApiError) {
    if (error.code === 'supervisor_unavailable') {
      return '无法连接进程 Supervisor；请确认 edgedesk-supervisor 正在运行且 Socket 权限正确。';
    }
    return error.message;
  }
  if (error instanceof Error) return error.message;
  return '操作失败，请稍后重试。';
}

function stateClass(state: ServiceLifecycle): string {
  if (state === 'RUNNING') return 'ok';
  if (state === 'STARTING' || state === 'STOPPING' || state === 'BACKOFF' || state === 'UNKNOWN') {
    return 'pending';
  }
  if (state === 'FAILED') return 'danger';
  return 'muted';
}

function isRootService(service: SupervisedService): boolean {
  return /^(root|0)$/i.test(service.run_user.trim());
}

function uptimeText(service: SupervisedService): string {
  if (service.uptime_seconds === null || service.state !== 'RUNNING') return '—';
  return formatUptime(service.uptime_seconds);
}

function exitText(service: SupervisedService): string {
  if (service.last_signal !== null) return `信号 ${service.last_signal}`;
  if (service.last_exit_code !== null) return `退出码 ${service.last_exit_code}`;
  return '—';
}

async function waitForOperation(initial: SupervisorOperation): Promise<SupervisorOperation> {
  let operation = initial;
  let failures = 0;
  const deadline = Date.now() + MAX_OPERATION_WAIT_MS;
  while (operation.state === 'pending' && Date.now() < deadline) {
    await new Promise((resolve) => setTimeout(resolve, OPERATION_POLL_MS));
    try {
      operation = await api.supervisorOperation(operation.operation_id);
      failures = 0;
    } catch (caught) {
      if (caught instanceof ApiError && caught.status === 404) throw caught;
      failures += 1;
      if (failures >= 5) throw caught;
    }
  }
  return operation;
}

export function ProcessesView() {
  const [services, setServices] = useState<SupervisedService[]>([]);
  const [health, setHealth] = useState<SupervisorHealth | null>(null);
  const [selectedId, setSelectedId] = useState('');
  const [loading, setLoading] = useState(true);
  const [connectionError, setConnectionError] = useState('');
  const [actionError, setActionError] = useState('');
  const [notice, setNotice] = useState('');
  const [busy, setBusy] = useState<string | null>(null);
  const [editor, setEditor] = useState<{ mode: 'create' | 'edit'; service?: SupervisedService } | null>(null);
  const [confirm, setConfirm] = useState<ConfirmAction | null>(null);

  const [logText, setLogText] = useState('');
  const [logCursor, setLogCursor] = useState(0);
  const [logPaused, setLogPaused] = useState(false);
  const [logLoading, setLogLoading] = useState(false);
  const [logError, setLogError] = useState('');
  const [logTruncated, setLogTruncated] = useState(false);
  const [logTrimmed, setLogTrimmed] = useState(false);
  const cursorRef = useRef(0);
  const logRequestRef = useRef(0);

  const selected = services.find((item) => item.id === selectedId) ?? null;

  const loadServices = useCallback(async () => {
    try {
      const [healthResponse, listResponse] = await Promise.all([
        api.supervisorHealth(),
        api.supervisorServices({ limit: 500 }),
      ]);
      setHealth(healthResponse);
      setServices(listResponse.services);
      setSelectedId((current) => {
        if (current && listResponse.services.some((item) => item.id === current)) return current;
        return listResponse.services[0]?.id ?? '';
      });
      setConnectionError('');
    } catch (caught) {
      setHealth(null);
      setConnectionError(errorMessage(caught));
      throw caught;
    } finally {
      setLoading(false);
    }
  }, []);

  useEffect(() => {
    let cancelled = false;
    const load = async () => {
      try {
        await loadServices();
      } catch {
        // The banner already carries the supervisor error.
      }
      if (cancelled) return;
    };
    void load();
    const timer = window.setInterval(() => {
      if (document.hidden) return;
      void load();
    }, POLL_MS);
    return () => {
      cancelled = true;
      window.clearInterval(timer);
    };
  }, [loadServices]);

  const fetchLogs = useCallback(
    async (reset: boolean) => {
      const id = selectedId;
      if (!id) return;
      const request = ++logRequestRef.current;
      setLogLoading(true);
      try {
        const page = await api.supervisorLogs(
          id,
          reset
            ? { tailBytes: 16 * 1024, maxBytes: 64 * 1024 }
            : { cursor: cursorRef.current, maxBytes: 64 * 1024 },
        );
        if (request !== logRequestRef.current) return;
        cursorRef.current = page.next_cursor;
        setLogCursor(page.next_cursor);
        setLogTruncated(page.truncated);
        if (reset || page.reset) {
          setLogText(page.content);
          setLogTrimmed(false);
        } else if (page.content) {
          setLogText((current) => {
            const combined = current + page.content;
            if (combined.length > MAX_LOG_CHARS) {
              setLogTrimmed(true);
              return combined.slice(combined.length - MAX_LOG_CHARS);
            }
            return combined;
          });
        }
        setLogError('');
      } catch (caught) {
        if (request === logRequestRef.current) setLogError(errorMessage(caught));
      } finally {
        if (request === logRequestRef.current) setLogLoading(false);
      }
    },
    [selectedId],
  );

  useEffect(() => {
    logRequestRef.current += 1;
    cursorRef.current = 0;
    setLogCursor(0);
    setLogText('');
    setLogTruncated(false);
    setLogTrimmed(false);
    setLogError('');
    if (selectedId) void fetchLogs(true);
  }, [selectedId, fetchLogs]);

  useEffect(() => {
    if (!selectedId || logPaused) return;
    const timer = window.setInterval(() => {
      if (!document.hidden) void fetchLogs(false);
    }, LOG_POLL_MS);
    return () => window.clearInterval(timer);
  }, [fetchLogs, logPaused, selectedId]);

  const refresh = async () => {
    setActionError('');
    try {
      await loadServices();
      if (selectedId) await fetchLogs(true);
    } catch (caught) {
      setConnectionError(errorMessage(caught));
    }
  };

  const applyRecord = (record: SupervisedService) => {
    setServices((current) => {
      const exists = current.some((item) => item.id === record.id);
      return exists ? current.map((item) => (item.id === record.id ? record : item)) : [...current, record];
    });
  };

  const saveService = async (payload: ServicePayload) => {
    const editing = editor?.mode === 'edit' ? editor.service : undefined;
    setBusy('save');
    setActionError('');
    setNotice('');
    try {
      const record = editing
        ? await api.updateSupervisorService(editing.id, payload)
        : await api.createSupervisorService(payload);
      applyRecord(record);
      setSelectedId(record.id);
      setEditor(null);
      setNotice(
        editing
          ? `已保存 ${record.name} 的配置；正在运行的进程不会自动重启。`
          : `已创建 ${record.name}。点击“启动”后才会运行。`,
      );
      await loadServices();
    } catch (caught) {
      setActionError(errorMessage(caught));
    } finally {
      setBusy(null);
    }
  };

  const perform = async (action: ConfirmAction) => {
    const { kind, service } = action;
    setBusy(kind);
    setActionError('');
    setNotice('');
    try {
      if (kind === 'start') {
        applyRecord(await api.startSupervisorService(service.id));
        setNotice(`已请求启动 ${service.name}。`);
      } else if (kind === 'stop' || kind === 'restart') {
        // Stop/restart are asynchronous Supervisor operations: the HTTP request
        // only submits them. Poll the operation ID until it settles; the
        // service keeps stopping even if this page goes away.
        const submitted =
          kind === 'stop'
            ? await api.stopSupervisorService(service.id)
            : await api.restartSupervisorService(service.id);
        setNotice(
          kind === 'stop'
            ? `已提交停止 ${service.name} 的请求，正在等待清理完成…`
            : `已提交重启 ${service.name} 的请求，正在等待完成…`,
        );
        const operation = await waitForOperation(submitted);
        if (operation.state === 'failed') {
          throw new ApiError(
            409,
            operation.error?.slug ?? 'operation_failed',
            operation.error?.message ?? 'Supervisor 操作失败。',
          );
        }
        if (operation.state === 'pending') {
          setNotice(
            `操作 ${operation.operation_id} 仍在进行；刷新或关闭页面不会取消停止/清理。`,
          );
        } else {
          if (operation.result) applyRecord(operation.result);
          setNotice(
            kind === 'stop' ? `已停止 ${service.name}。` : `已重启 ${service.name}。`,
          );
        }
      } else {
        await api.deleteSupervisorService(service.id);
        setServices((current) => current.filter((item) => item.id !== service.id));
        setSelectedId('');
        setNotice(`已删除 ${service.name}。`);
      }
      setConfirm(null);
      await loadServices();
      if (kind === 'start' || kind === 'restart') void fetchLogs(true);
    } catch (caught) {
      setActionError(errorMessage(caught));
      setConfirm(null);
    } finally {
      setBusy(null);
    }
  };

  const confirmText = confirm
    ? confirm.kind === 'delete'
      ? '删除服务'
      : confirm.kind === 'stop'
        ? '停止服务'
        : confirm.kind === 'restart'
          ? '重启服务'
          : '启动服务'
    : '';

  return (
    <div className="ed-proc-view">
      <header className="ed-proc-toolbar">
        <div>
          <span className="ed-proc-eyebrow">PROCESS SUPERVISOR</span>
          <h2>进程管理</h2>
          <p>管理本机常驻服务。托管进程独立于 EdgeDesk Web 后端和当前窗口生命周期。</p>
        </div>
        <div className="ed-proc-toolbar-actions">
          <span className={`ed-proc-connection ${health ? (health.degraded ? 'pending' : 'ok') : 'muted'}`}>
            <span className="ed-proc-dot" aria-hidden="true" />
            {health ? (health.degraded ? 'Supervisor 降级' : 'Supervisor 已连接') : connectionError ? 'Supervisor 不可用' : '检测中'}
          </span>
          <button type="button" className="ed-button small" onClick={() => void refresh()} disabled={busy !== null}>
            <Icon icon={RefreshCw} size={14} />
            刷新
          </button>
          <button type="button" className="ed-button primary small" onClick={() => setEditor({ mode: 'create' })}>
            <Icon icon={Plus} size={14} />
            新建服务
          </button>
        </div>
      </header>

      {connectionError && (
        <div className="ed-alert warning" role="alert">
          <Icon icon={AlertTriangle} size={15} />
          <span>{connectionError} 其他 EdgeDesk 功能不受影响。</span>
        </div>
      )}
      {health?.degraded && (
        <div className="ed-alert warning" role="status">
          <Icon icon={AlertTriangle} size={15} />
          <span>
            Supervisor 报告 {health.persistence_errors} 个持久化错误、{health.log_errors} 个日志错误；状态可能不完整。
          </span>
        </div>
      )}
      {actionError && (
        <div className="ed-alert danger" role="alert">
          <Icon icon={AlertTriangle} size={15} />
          <span>{actionError}</span>
        </div>
      )}
      {notice && (
        <div className="ed-alert info" role="status">
          <Icon icon={Server} size={15} />
          <span>{notice}</span>
        </div>
      )}

      <div className="ed-proc-layout">
        <aside className="ed-proc-list" aria-label="服务列表">
          {loading && services.length === 0 && <div className="ed-proc-list-empty">正在读取服务…</div>}
          {!loading && services.length === 0 && !connectionError && (
            <div className="ed-proc-list-empty">
              <Icon icon={Server} size={20} />
              <span>还没有托管服务</span>
              <button type="button" className="ed-link" onClick={() => setEditor({ mode: 'create' })}>
                创建第一个服务
              </button>
            </div>
          )}
          {services.map((service) => (
            <button
              key={service.id}
              type="button"
              className={`ed-proc-item state-${stateClass(service.state)}${service.id === selectedId ? ' selected' : ''}`}
              aria-pressed={service.id === selectedId}
              onClick={() => {
                setSelectedId(service.id);
                setNotice('');
                setActionError('');
              }}
            >
              <span className={`ed-proc-status ${stateClass(service.state)}`} aria-hidden="true" />
              <span className="ed-proc-item-main">
                <strong>{service.name}</strong>
                <span>
                  {STATE_LABEL[service.state]} · PID {service.pid ?? '—'} · 重启 {service.restart_count}
                </span>
                <span className="ed-proc-item-metrics">
                  CPU {formatPercent(service.cpu_percent)} · RSS{' '}
                  {service.rss_bytes === null ? '—' : formatBytes(service.rss_bytes)}
                </span>
              </span>
              {service.autostart && <span className="ed-badge subtle">自启</span>}
              {isRootService(service) && <span className="ed-badge warning">root</span>}
            </button>
          ))}
        </aside>

        <section className="ed-proc-main">
          {!selected ? (
            <div className="ed-empty small">
              <Icon icon={Server} size={28} />
              <h2>{connectionError ? 'Supervisor 暂不可用' : '选择一个服务'}</h2>
              <p>
                {connectionError
                  ? '进程管理暂时无法读取数据。文件、终端、仪表盘、认证和扩展平台继续可用。'
                  : '从左侧选择服务查看状态与日志，或新建一个托管服务。'}
              </p>
            </div>
          ) : (
            <>
              <header className="ed-proc-detail-head">
                <div>
                  <div className="ed-proc-title-line">
                    <h3>{selected.name}</h3>
                    <span className={`ed-proc-state-pill ${stateClass(selected.state)}`}>
                      <span className="ed-proc-dot" aria-hidden="true" />
                      {STATE_LABEL[selected.state]}
                    </span>
                    {selected.failed_locked && <span className="ed-badge warning">自动恢复已锁定</span>}
                    {isRootService(selected) && <span className="ed-badge warning">root 身份</span>}
                  </div>
                  <p className="ed-proc-id">
                    <span className="ed-proc-id-value">ID {selected.id}</span>
                    {' · '}
                    <span className="ed-proc-updated">更新 {formatTime(selected.updated_at)}</span>
                  </p>
                </div>
                <div className="ed-proc-actions">
                  {selected.state !== 'RUNNING' && selected.state !== 'STARTING' && selected.state !== 'STOPPING' && (
                    <button
                      type="button"
                      className="ed-button small primary"
                      onClick={() => setConfirm({ kind: 'start', service: selected })}
                      disabled={busy !== null}
                    >
                      <Icon icon={Play} size={14} />
                      启动
                    </button>
                  )}
                  {selected.state !== 'STOPPED' && (
                    <button
                      type="button"
                      className="ed-button small"
                      onClick={() => setConfirm({ kind: 'stop', service: selected })}
                      disabled={busy !== null}
                    >
                      <Icon icon={Square} size={14} />
                      停止
                    </button>
                  )}
                  <button
                    type="button"
                    className="ed-button small"
                    onClick={() => setConfirm({ kind: 'restart', service: selected })}
                    disabled={busy !== null || selected.state === 'STOPPING'}
                  >
                    <Icon icon={RotateCw} size={14} />
                    重启
                  </button>
                  <button
                    type="button"
                    className="ed-button small"
                    onClick={() => setEditor({ mode: 'edit', service: selected })}
                    disabled={busy !== null}
                  >
                    <Icon icon={Pencil} size={14} />
                    编辑
                  </button>
                  <button
                    type="button"
                    className="ed-button small danger"
                    onClick={() => setConfirm({ kind: 'delete', service: selected })}
                    disabled={busy !== null || (selected.state !== 'STOPPED' && selected.state !== 'FAILED')}
                    title={
                      selected.state !== 'STOPPED' && selected.state !== 'FAILED'
                        ? '必须先停止服务才能删除'
                        : undefined
                    }
                  >
                    <Icon icon={Trash2} size={14} />
                    删除
                  </button>
                </div>
              </header>

              {selected.last_error && (
                <div className="ed-alert danger" role="status">
                  <Icon icon={AlertTriangle} size={15} />
                  <span>最近错误：{selected.last_error}</span>
                </div>
              )}

              <div className="ed-proc-stats">
                <div>
                  <span>PID</span>
                  <strong>{selected.pid ?? '—'}</strong>
                </div>
                <div>
                  <span>运行时间</span>
                  <strong>{uptimeText(selected)}</strong>
                </div>
                <div>
                  <span>CPU（主进程）</span>
                  <strong>
                    <Icon icon={Cpu} size={13} />
                    {formatPercent(selected.cpu_percent)}
                  </strong>
                </div>
                <div>
                  <span>RSS（主进程）</span>
                  <strong>
                    <Icon icon={MemoryStick} size={13} />
                    {selected.rss_bytes === null ? '—' : formatBytes(selected.rss_bytes)}
                  </strong>
                </div>
                <div>
                  <span>退出原因</span>
                  <strong>{exitText(selected)}</strong>
                </div>
                <div>
                  <span>重启次数</span>
                  <strong>{selected.restart_count}</strong>
                </div>
              </div>
              {!selected.metrics_available && selected.state === 'RUNNING' && (
                <p className="ed-proc-note">主进程 CPU/RSS 指标当前不可用，界面不会把它们显示为 0。</p>
              )}

              <section className="ed-proc-section">
                <h4>服务配置</h4>
                <dl className="ed-proc-config">
                  <div>
                    <dt>命令</dt>
                    <dd>
                      <code>{selected.executable}</code>
                    </dd>
                  </div>
                  <div>
                    <dt>参数</dt>
                    <dd>
                      {selected.args.length === 0 ? (
                        '（无参数）'
                      ) : (
                        <div className="ed-proc-args">
                          {selected.args.map((arg, index) => (
                            <code key={`${index}-${arg}`}>{arg === '' ? '""' : arg}</code>
                          ))}
                        </div>
                      )}
                    </dd>
                  </div>
                  <div>
                    <dt>工作目录</dt>
                    <dd>
                      <code>{selected.cwd}</code>
                    </dd>
                  </div>
                  <div>
                    <dt>运行身份</dt>
                    <dd>
                      {selected.run_user}:{selected.run_group}
                      {selected.allow_root && isRootService(selected) && '（管理员已确认 root）'}
                    </dd>
                  </div>
                  <div>
                    <dt>环境变量</dt>
                    <dd>
                      {Object.keys(selected.env).length === 0
                        ? '（仅受控基础环境）'
                        : `${Object.keys(selected.env).length} 个键：${Object.keys(selected.env).sort().join(', ')}`}
                    </dd>
                  </div>
                  <div>
                    <dt>重启策略</dt>
                    <dd>{POLICY_LABEL[selected.restart_policy] ?? selected.restart_policy}</dd>
                  </div>
                  <div>
                    <dt>自动启动</dt>
                    <dd>{selected.autostart ? '系统启动时自动启动' : '不自动启动'}</dd>
                  </div>
                  <div>
                    <dt>最后退出</dt>
                    <dd>{selected.last_exit_at ? formatTime(selected.last_exit_at / 1000) : '—'}</dd>
                  </div>
                  <div>
                    <dt>进程组</dt>
                    <dd>{selected.pgid ?? '—'}</dd>
                  </div>
                  <div>
                    <dt>引导 ID</dt>
                    <dd className="ed-proc-boot-id">{selected.boot_id || '—'}</dd>
                  </div>
                </dl>
                <p className="ed-proc-note">
                  环境变量值不在界面和审计日志中展示；如需修改请使用“编辑”。保存配置不会重启运行中的服务。
                </p>
              </section>

              <section className="ed-proc-section ed-proc-logs">
                <div className="ed-proc-logs-head">
                  <h4>
                    <Icon icon={ScrollText} size={15} />
                    日志（stdout / stderr）
                  </h4>
                  <div className="ed-proc-logs-actions">
                    <label className="ed-proc-check">
                      <input
                        type="checkbox"
                        checked={!logPaused}
                        onChange={(event) => setLogPaused(!event.target.checked)}
                      />
                      自动刷新
                    </label>
                    <button
                      type="button"
                      className="ed-button small"
                      onClick={() => void fetchLogs(true)}
                      disabled={logLoading}
                    >
                      刷新尾部
                    </button>
                    <button
                      type="button"
                      className="ed-button small"
                      onClick={() => {
                        setLogText('');
                        cursorRef.current = logCursor;
                        setLogTrimmed(false);
                      }}
                    >
                      清空显示
                    </button>
                  </div>
                </div>
                {(logTruncated || logTrimmed) && (
                  <p className="ed-proc-note">
                    {logTruncated ? '日志读取已达到返回上限。' : ''}
                    {logTrimmed ? ' 页面仅保留最近 256 KiB 内容。' : ''}
                  </p>
                )}
                {logError && (
                  <div className="ed-alert warning" role="status">
                    <Icon icon={AlertTriangle} size={14} />
                    <span>{logError}</span>
                  </div>
                )}
                <pre className="ed-proc-log" aria-label={`${selected.name} 日志`}>
                  {logText || (logLoading ? '正在读取日志…' : '暂无日志输出。')}
                </pre>
                <p className="ed-proc-note">
                  光标 {logCursor} · 单文件上限 {formatBytes(selected.log_max_bytes)} · 关闭窗口不会停止日志采集。
                </p>
              </section>
            </>
          )}
        </section>
      </div>

      {editor && (
        <ServiceEditor
          initial={editor.service}
          busy={busy === 'save'}
          onCancel={() => setEditor(null)}
          onSave={(payload) => void saveService(payload)}
        />
      )}

      {confirm && (
        <ConfirmDialog
          title={confirm.kind === 'delete' ? '删除托管服务' : confirmText}
          message={
            confirm.kind === 'delete'
              ? `删除“${confirm.service.name}”及其日志。该操作不可恢复。`
              : confirm.kind === 'stop'
                ? `停止“${confirm.service.name}”会先向整个进程组发送 SIGTERM，超时后发送 SIGKILL。`
                : confirm.kind === 'restart'
                  ? `重启“${confirm.service.name}”会先停止再启动，服务会短暂中断。`
                  : `启动“${confirm.service.name}”？`
          }
          details={
            confirm.kind === 'start' && isRootService(confirm.service)
              ? ['该服务以 root 身份运行，将拥有完整系统权限。']
              : confirm.kind === 'delete'
                ? [`服务 ID：${confirm.service.id}`, '运行中的服务必须先停止。']
                : []
          }
          confirmText={confirmText}
          danger={confirm.kind === 'delete' || confirm.kind === 'stop' || confirm.kind === 'restart'}
          busy={busy !== null}
          onConfirm={() => void perform(confirm)}
          onCancel={() => setConfirm(null)}
        />
      )}
    </div>
  );
}
