import { useEffect, useState } from 'react';
import { api, formatBytes, formatTime, formatUptime } from '../../services/api';
import { preferencesStore, usePreferences } from '../../stores/preferencesStore';
import type { SystemInfo } from '../../types';

type SessionSnapshot = {
  authenticated: boolean;
  method: string;
  expires_at?: number;
};

function describeError(caught: unknown): string {
  return caught instanceof Error ? caught.message : String(caught);
}

export function SettingsView({ onLogout }: { onLogout: () => void }) {
  const preferences = usePreferences();
  const [system, setSystem] = useState<SystemInfo | null>(null);
  const [session, setSession] = useState<SessionSnapshot | null>(null);
  const [tokenConfigured, setTokenConfigured] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [notice, setNotice] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);

  const [currentPassword, setCurrentPassword] = useState('');
  const [newPassword, setNewPassword] = useState('');
  const [confirmPassword, setConfirmPassword] = useState('');
  const [fontSize, setFontSize] = useState(preferences.terminal_font_size);

  useEffect(() => {
    setFontSize(preferences.terminal_font_size);
  }, [preferences.terminal_font_size]);

  useEffect(() => {
    Promise.all([api.systemInfo(), api.session(), api.settings()])
      .then(([info, current, settings]) => {
        setSystem(info);
        setSession({ authenticated: current.authenticated, method: current.method, expires_at: current.expires_at });
        setTokenConfigured(settings.api_token_configured);
      })
      .catch((caught) => setError(describeError(caught)));
  }, []);

  const savePreference = async (patch: Parameters<typeof preferencesStore.save>[0]) => {
    setError(null);
    setNotice(null);
    try {
      await preferencesStore.save(patch);
      setNotice('偏好已保存');
    } catch (caught) {
      setError(describeError(caught));
    }
  };

  const changePassword = async (event: React.FormEvent) => {
    event.preventDefault();
    setError(null);
    setNotice(null);
    if (newPassword !== confirmPassword) {
      setError('两次输入的新密码不一致');
      return;
    }
    if ([...newPassword].length < 12) {
      setError('新密码至少需要 12 个字符');
      return;
    }
    setBusy(true);
    try {
      await api.changePassword(currentPassword, newPassword, confirmPassword);
      setNotice('密码已修改，所有旧会话已失效，请重新登录');
      setCurrentPassword('');
      setNewPassword('');
      setConfirmPassword('');
      onLogout();
    } catch (caught) {
      setError(describeError(caught));
    } finally {
      setBusy(false);
    }
  };

  const logout = () => {
    void api.logout().finally(onLogout);
  };

  return (
    <div className="ed-view">
      <div className="ed-view-header">
        <div>
          <h1>系统设置</h1>
          <p>安全、系统信息与应用偏好。部署级配置（监听地址、文件根目录、上传上限等）首期只读。</p>
        </div>
        <div className="ed-toolbar">
          <button type="button" className="ed-button danger" onClick={logout}>
            退出登录
          </button>
        </div>
      </div>

      {error && <div className="ed-alert danger">{error}</div>}
      {notice && <div className="ed-alert success">{notice}</div>}

      <section className="ed-card-grid">
        <article className="ed-panel ed-info-card">
          <h2>安全</h2>
          <dl className="ed-metric-list">
            <div>
              <dt>当前会话</dt>
              <dd>{session?.authenticated ? (session.method === 'cookie' ? '浏览器会话' : 'Bearer Token') : '—'}</dd>
            </div>
            <div>
              <dt>会话过期</dt>
              <dd>{session?.expires_at ? formatTime(session.expires_at) : '—'}</dd>
            </div>
            <div>
              <dt>API Token</dt>
              <dd>{tokenConfigured ? '已配置（不显示值）' : '未配置'}</dd>
            </div>
          </dl>

          <h3>修改管理员密码</h3>
          <form className="ed-form" onSubmit={(event) => void changePassword(event)}>
            <label htmlFor="ed-current-password">当前密码</label>
            <input
              id="ed-current-password"
              type="password"
              autoComplete="current-password"
              value={currentPassword}
              onChange={(event) => setCurrentPassword(event.target.value)}
              required
            />
            <label htmlFor="ed-new-password">新密码（至少 12 个字符）</label>
            <input
              id="ed-new-password"
              type="password"
              autoComplete="new-password"
              value={newPassword}
              onChange={(event) => setNewPassword(event.target.value)}
              required
            />
            <label htmlFor="ed-confirm-password">确认新密码</label>
            <input
              id="ed-confirm-password"
              type="password"
              autoComplete="new-password"
              value={confirmPassword}
              onChange={(event) => setConfirmPassword(event.target.value)}
              required
            />
            <button type="submit" className="ed-button primary" disabled={busy}>
              {busy ? '保存中…' : '修改密码并撤销旧会话'}
            </button>
          </form>
        </article>

        <article className="ed-panel ed-info-card">
          <h2>系统信息</h2>
          <dl className="ed-metric-list">
            <div>
              <dt>版本</dt>
              <dd>{system?.version ?? '—'}</dd>
            </div>
            <div>
              <dt>主机名</dt>
              <dd>{system?.hostname ?? '—'}</dd>
            </div>
            <div>
              <dt>架构 / 内核</dt>
              <dd>{system ? `${system.arch} / ${system.kernel}` : '—'}</dd>
            </div>
            <div>
              <dt>运行时间</dt>
              <dd>{system ? formatUptime(system.uptime_seconds) : '—'}</dd>
            </div>
            <div>
              <dt>监听</dt>
              <dd>{system ? `${system.listen.host}:${system.listen.port}` : '—'}</dd>
            </div>
            <div>
              <dt>文件根目录</dt>
              <dd><code>{system?.file_root ?? '—'}</code></dd>
            </div>
            <div>
              <dt>Bash</dt>
              <dd><code>{system?.bash ?? '—'}</code></dd>
            </div>
            <div>
              <dt>审计日志</dt>
              <dd>{system ? (system.audit_enabled ? system.audit_log : '已关闭') : '—'}</dd>
            </div>
          </dl>
        </article>

        <article className="ed-panel ed-info-card">
          <h2>基础偏好</h2>
          <div className="ed-form">
            <label htmlFor="ed-theme">主题</label>
            <select
              id="ed-theme"
              value={preferences.theme}
              onChange={(event) => void savePreference({ theme: event.target.value as typeof preferences.theme })}
            >
              <option value="dark">深色</option>
              <option value="light">浅色</option>
              <option value="system">跟随系统</option>
            </select>

            <label htmlFor="ed-files-view">默认文件视图</label>
            <select
              id="ed-files-view"
              value={preferences.files_view}
              onChange={(event) => void savePreference({ files_view: event.target.value as typeof preferences.files_view })}
            >
              <option value="list">列表</option>
              <option value="grid">网格</option>
            </select>

            <label className="ed-toggle">
              <input
                type="checkbox"
                checked={preferences.files_show_hidden}
                onChange={(event) => void savePreference({ files_show_hidden: event.target.checked })}
              />
              默认显示隐藏文件
            </label>

            <label htmlFor="ed-terminal-font">终端字体大小</label>
            <input
              id="ed-terminal-font"
              type="number"
              min={10}
              max={24}
              value={fontSize}
              onChange={(event) => {
                const value = Number(event.target.value);
                setFontSize(value);
                if (Number.isInteger(value) && value >= 10 && value <= 24) {
                  void savePreference({ terminal_font_size: value });
                }
              }}
            />

            <label htmlFor="ed-metrics-window">仪表盘默认窗口</label>
            <select
              id="ed-metrics-window"
              value={preferences.dashboard_metrics_window}
              onChange={(event) => void savePreference({ dashboard_metrics_window: Number(event.target.value) })}
            >
              <option value={60}>1 分钟</option>
              <option value={300}>5 分钟</option>
              <option value={600}>10 分钟</option>
              <option value={1800}>30 分钟</option>
            </select>
          </div>
        </article>

        <article className="ed-panel ed-info-card">
          <h2>高级配置（只读）</h2>
          <dl className="ed-metric-list">
            <div>
              <dt>数据目录</dt>
              <dd><code>{system?.data_dir ?? '—'}</code></dd>
            </div>
            <div>
              <dt>运行期目录</dt>
              <dd><code>{system?.state_dir ?? '—'}</code></dd>
            </div>
            <div>
              <dt>符号链接策略</dt>
              <dd>{system ? (system.follow_symlinks ? '允许沙箱内跟随' : '禁止跟随') : '—'}</dd>
            </div>
            <div>
              <dt>上传上限</dt>
              <dd>{system ? formatBytes(system.max_upload_size) : '—'}</dd>
            </div>
            <div>
              <dt>终端并发 / 空闲回收</dt>
              <dd>{system ? `${system.max_terminal_sessions} / ${system.terminal_idle_timeout} 秒` : '—'}</dd>
            </div>
            <div>
              <dt>指标采样</dt>
              <dd>{system ? `${system.metrics_interval_ms} ms` : '—'}</dd>
            </div>
          </dl>
          <p className="ed-panel-note">网页设置不会覆盖配置文件、环境变量或命令行提供的部署参数。</p>
        </article>
      </section>
    </div>
  );
}
