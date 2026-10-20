import { useMemo, useState, type FormEvent } from 'react';
import { createPortal } from 'react-dom';
import { AlertTriangle } from 'lucide-react';
import { Icon } from '../../components/Icon';
import type { RestartPolicy, ServicePayload, SupervisedService } from '../../types';

type Props = {
  initial?: SupervisedService;
  busy: boolean;
  onSave: (payload: ServicePayload) => void;
  onCancel: () => void;
};

type FormState = {
  name: string;
  executable: string;
  args: string;
  cwd: string;
  env: string;
  run_user: string;
  run_group: string;
  allow_root: boolean;
  autostart: boolean;
  restart_policy: RestartPolicy;
  max_restarts: string;
  restart_window_sec: string;
  restart_delay_ms: string;
  max_restart_delay_ms: string;
  stable_after_sec: string;
  stop_timeout_ms: string;
  log_max_mib: string;
};

function initialState(initial?: SupervisedService): FormState {
  return {
    name: initial?.name ?? '',
    executable: initial?.executable ?? '',
    args: (initial?.args ?? []).join('\n'),
    cwd: initial?.cwd ?? '/',
    env: Object.entries(initial?.env ?? {})
      .map(([key, value]) => `${key}=${value}`)
      .join('\n'),
    run_user: initial?.run_user ?? '',
    run_group: initial?.run_group ?? '',
    allow_root: initial?.allow_root ?? false,
    autostart: initial?.autostart ?? false,
    restart_policy: initial?.restart_policy ?? 'on-failure',
    max_restarts: String(initial?.max_restarts ?? 5),
    restart_window_sec: String(initial?.restart_window_sec ?? 60),
    restart_delay_ms: String(initial?.restart_delay_ms ?? 1000),
    max_restart_delay_ms: String(initial?.max_restart_delay_ms ?? 60000),
    stable_after_sec: String(initial?.stable_after_sec ?? 300),
    stop_timeout_ms: String(initial?.stop_timeout_ms ?? 10000),
    log_max_mib: String(Math.max(1, Math.round((initial?.log_max_bytes ?? 10 * 1024 * 1024) / (1024 * 1024)))),
  };
}

/** One argv element per line. Empty lines in the middle are empty arguments;
 *  the final trailing newline is ignored because textareas always produce one. */
function parseArgs(text: string): string[] {
  const lines = text.split('\n');
  if (lines.length > 0 && lines[lines.length - 1] === '') lines.pop();
  return lines;
}

function parseEnv(text: string): Record<string, string> {
  const result: Record<string, string> = {};
  for (const [index, raw] of text.split('\n').entries()) {
    const line = raw.endsWith('\r') ? raw.slice(0, -1) : raw;
    if (line.trim() === '') continue;
    const separator = line.indexOf('=');
    if (separator <= 0) {
      throw new Error(`环境变量第 ${index + 1} 行缺少 KEY=VALUE 格式`);
    }
    const key = line.slice(0, separator);
    if (/\s/.test(key)) throw new Error(`环境变量名不能包含空白：${key}`);
    if (Object.prototype.hasOwnProperty.call(result, key)) {
      throw new Error(`环境变量重复：${key}`);
    }
    result[key] = line.slice(separator + 1);
  }
  return result;
}

function numberField(value: string, label: string, min: number, max: number): number {
  if (!/^\d+$/.test(value.trim())) throw new Error(`${label}必须是非负整数`);
  const parsed = Number(value);
  if (!Number.isSafeInteger(parsed) || parsed < min || parsed > max) {
    throw new Error(`${label}必须在 ${min} 到 ${max} 之间`);
  }
  return parsed;
}

function looksRoot(user: string): boolean {
  return /^(root|0)$/i.test(user.trim());
}

export function ServiceEditor({ initial, busy, onSave, onCancel }: Props) {
  const [form, setForm] = useState<FormState>(() => initialState(initial));
  const [error, setError] = useState('');
  const rootIdentity = useMemo(() => looksRoot(form.run_user), [form.run_user]);

  const set = <K extends keyof FormState>(key: K, value: FormState[K]) => {
    setForm((current) => ({ ...current, [key]: value }));
  };

  const submit = (event: FormEvent) => {
    event.preventDefault();
    setError('');
    try {
      const name = form.name.trim();
      const executable = form.executable.trim();
      const cwd = form.cwd.trim();
      const runUser = form.run_user.trim();
      const runGroup = form.run_group.trim();
      if (!name) throw new Error('服务名称不能为空');
      if (!executable.startsWith('/')) throw new Error('可执行文件必须是绝对路径');
      if (!cwd.startsWith('/')) throw new Error('工作目录必须是绝对路径');
      if (!runUser) throw new Error('必须显式指定运行用户，不能默认为 root');
      if (!runGroup) throw new Error('必须显式指定运行用户组');
      if (rootIdentity && !form.allow_root) {
        throw new Error('该服务以 root 身份运行，必须勾选明确的确认项');
      }

      const args = parseArgs(form.args);
      if (args.length > 128) throw new Error('参数最多 128 个');
      const env = parseEnv(form.env);
      const restartDelay = numberField(form.restart_delay_ms, '初始重启延迟（毫秒）', 10, 3600000);
      const maxRestartDelay = numberField(
        form.max_restart_delay_ms,
        '最大重启延迟（毫秒）',
        restartDelay,
        3600000,
      );
      const logMib = numberField(form.log_max_mib, '日志上限（MiB）', 1, 1024);

      onSave({
        name,
        executable,
        args,
        cwd,
        env,
        run_user: runUser,
        run_group: runGroup,
        allow_root: rootIdentity ? form.allow_root : false,
        autostart: form.autostart,
        restart_policy: form.restart_policy,
        max_restarts: numberField(form.max_restarts, '最大重启次数', 0, 1000),
        restart_window_sec: numberField(form.restart_window_sec, '重启统计窗口（秒）', 1, 2592000),
        restart_delay_ms: restartDelay,
        max_restart_delay_ms: maxRestartDelay,
        stable_after_sec: numberField(form.stable_after_sec, '稳定运行时间（秒）', 1, 2592000),
        stop_timeout_ms: numberField(form.stop_timeout_ms, '停止宽限（毫秒）', 100, 600000),
        log_max_bytes: logMib * 1024 * 1024,
      });
    } catch (caught) {
      setError(caught instanceof Error ? caught.message : '配置无效');
    }
  };

  return createPortal(
    <div className="ed-modal-backdrop" onPointerDown={busy ? undefined : onCancel}>
      <form
        className="ed-panel ed-proc-editor"
        role="dialog"
        aria-modal="true"
        aria-label={initial ? `编辑服务 ${initial.name}` : '创建服务'}
        onPointerDown={(event) => event.stopPropagation()}
        onSubmit={submit}
      >
        <header className="ed-proc-editor-head">
          <div>
            <span className="ed-proc-eyebrow">{initial ? 'EDIT SERVICE' : 'NEW SERVICE'}</span>
            <h2>{initial ? `编辑 ${initial.name}` : '创建托管服务'}</h2>
            <p>命令以可执行文件和参数数组执行，不经过 Shell。保存配置不会自动重启正在运行的服务。</p>
          </div>
          <button type="button" className="ed-button small" onClick={onCancel} disabled={busy}>
            取消
          </button>
        </header>

        <div className="ed-proc-editor-body">
          {error && (
            <div className="ed-alert danger" role="alert">
              <Icon icon={AlertTriangle} size={15} />
              <span>{error}</span>
            </div>
          )}

          <section className="ed-proc-form-section">
            <h3>基本信息</h3>
            <label>
              服务名称
              <input
                value={form.name}
                onChange={(event) => set('name', event.target.value)}
                placeholder="例如 mihomo"
                maxLength={128}
                required
              />
            </label>
            <label>
              可执行文件（绝对路径）
              <input
                value={form.executable}
                onChange={(event) => set('executable', event.target.value)}
                placeholder="/usr/local/bin/mihomo"
                spellCheck={false}
                required
              />
            </label>
            <label>
              工作目录
              <input
                value={form.cwd}
                onChange={(event) => set('cwd', event.target.value)}
                placeholder="/"
                spellCheck={false}
                required
              />
            </label>
            <label>
              参数（每行一个；中间空行表示空字符串参数，末尾换行忽略）
              <textarea
                value={form.args}
                onChange={(event) => set('args', event.target.value)}
                rows={4}
                spellCheck={false}
                placeholder={'-c\n/etc/mihomo/config.yaml'}
              />
            </label>
            <label>
              环境变量（每行 KEY=VALUE；只传入这里声明的变量和受控基础环境）
              <textarea
                value={form.env}
                onChange={(event) => set('env', event.target.value)}
                rows={3}
                spellCheck={false}
                placeholder="LOG_LEVEL=info"
              />
            </label>
          </section>

          <section className="ed-proc-form-section">
            <h3>运行身份</h3>
            <div className="ed-proc-form-grid">
              <label>
                运行用户
                <input
                  value={form.run_user}
                  onChange={(event) => set('run_user', event.target.value)}
                  placeholder="edgedesk-service"
                  autoComplete="off"
                  required
                />
              </label>
              <label>
                运行用户组
                <input
                  value={form.run_group}
                  onChange={(event) => set('run_group', event.target.value)}
                  placeholder="edgedesk-service"
                  autoComplete="off"
                  required
                />
              </label>
            </div>
            {rootIdentity && (
              <div className="ed-proc-root-warning">
                <Icon icon={AlertTriangle} size={15} />
                <div>
                  <strong>该服务将以 root 身份运行</strong>
                  <p>托管进程将拥有完整系统权限。仅在你信任该可执行文件和参数时继续。</p>
                  <label className="ed-proc-check">
                    <input
                      type="checkbox"
                      checked={form.allow_root}
                      onChange={(event) => set('allow_root', event.target.checked)}
                    />
                    我确认该服务以 root 身份运行
                  </label>
                </div>
              </div>
            )}
          </section>

          <section className="ed-proc-form-section">
            <h3>启动与恢复</h3>
            <div className="ed-proc-form-grid">
              <label>
                重启策略
                <select
                  value={form.restart_policy}
                  onChange={(event) => set('restart_policy', event.target.value as RestartPolicy)}
                >
                  <option value="on-failure">on-failure：异常退出后重启</option>
                  <option value="always">always：任何退出后重启</option>
                  <option value="never">never：不自动重启</option>
                </select>
              </label>
              <label className="ed-proc-check standalone">
                <input
                  type="checkbox"
                  checked={form.autostart}
                  onChange={(event) => set('autostart', event.target.checked)}
                />
                系统启动时自动启动
              </label>
            </div>
            <div className="ed-proc-form-grid three">
              <label>
                最大重启次数
                <input
                  value={form.max_restarts}
                  onChange={(event) => set('max_restarts', event.target.value)}
                  inputMode="numeric"
                />
              </label>
              <label>
                统计窗口（秒）
                <input
                  value={form.restart_window_sec}
                  onChange={(event) => set('restart_window_sec', event.target.value)}
                  inputMode="numeric"
                />
              </label>
              <label>
                稳定重置（秒）
                <input
                  value={form.stable_after_sec}
                  onChange={(event) => set('stable_after_sec', event.target.value)}
                  inputMode="numeric"
                />
              </label>
              <label>
                初始延迟（毫秒）
                <input
                  value={form.restart_delay_ms}
                  onChange={(event) => set('restart_delay_ms', event.target.value)}
                  inputMode="numeric"
                />
              </label>
              <label>
                最大延迟（毫秒）
                <input
                  value={form.max_restart_delay_ms}
                  onChange={(event) => set('max_restart_delay_ms', event.target.value)}
                  inputMode="numeric"
                />
              </label>
              <label>
                停止宽限（毫秒）
                <input
                  value={form.stop_timeout_ms}
                  onChange={(event) => set('stop_timeout_ms', event.target.value)}
                  inputMode="numeric"
                />
              </label>
              <label>
                日志上限（MiB）
                <input
                  value={form.log_max_mib}
                  onChange={(event) => set('log_max_mib', event.target.value)}
                  inputMode="numeric"
                />
              </label>
            </div>
          </section>
        </div>

        <footer className="ed-proc-editor-actions">
          <button type="button" className="ed-button" onClick={onCancel} disabled={busy}>
            取消
          </button>
          <button type="submit" className="ed-button primary" disabled={busy}>
            {busy ? '保存中…' : initial ? '保存配置' : '创建服务'}
          </button>
        </footer>
      </form>
    </div>,
    document.body,
  );
}
