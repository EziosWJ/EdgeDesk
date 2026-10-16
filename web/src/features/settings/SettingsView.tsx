import { useEffect, useState } from 'react';
import { api } from '../../services/api';
import { sessionStore } from '../../stores/sessionStore';

type ServerInfo = {
  version: string;
  shell: string;
  shellAvailable: boolean;
  maxSessions: number;
  idleTimeout: number;
  running: number;
  authMethod: string;
};

export function SettingsView({ onLogout }: { onLogout: () => void }) {
  const [info, setInfo] = useState<ServerInfo | null>(null);

  useEffect(() => {
    Promise.all([api.health(), api.terminalSessions(), api.session()])
      .then(([health, terminals, session]) => {
        setInfo({
          version: health.version,
          shell: terminals.shell,
          shellAvailable: terminals.shell_available,
          maxSessions: terminals.max_sessions,
          idleTimeout: terminals.idle_timeout_seconds,
          running: terminals.running,
          authMethod: session.method,
        });
      })
      .catch(() => undefined);
  }, []);

  return (
    <div className="ed-view">
      <div className="ed-view-header">
        <div>
          <h1>设置与状态</h1>
          <p>运行参数由启动配置决定，界面只做展示；修改配置后重启服务即可生效。</p>
        </div>
        <div className="ed-toolbar">
          <button
            type="button"
            className="ed-button danger"
            onClick={() => {
              void api.logout().finally(onLogout);
            }}
          >
            退出登录
          </button>
        </div>
      </div>

      <section className="ed-card-grid">
        <article className="ed-panel ed-info-card">
          <h2>服务</h2>
          <dl className="ed-metric-list">
            <div>
              <dt>版本</dt>
              <dd>{info?.version ?? '—'}</dd>
            </div>
            <div>
              <dt>认证方式</dt>
              <dd>{info?.authMethod === 'cookie' ? '会话 Cookie' : info?.authMethod === 'bearer' ? 'Bearer Token' : '—'}</dd>
            </div>
            <div>
              <dt>指标采样</dt>
              <dd>{sessionStore.intervalMs} ms / 最多 300 点（内存环形缓冲）</dd>
            </div>
          </dl>
        </article>
        <article className="ed-panel ed-info-card">
          <h2>终端</h2>
          <dl className="ed-metric-list">
            <div>
              <dt>登录 Shell</dt>
              <dd>
                <code>{info?.shell ?? '—'}</code> {info && !info.shellAvailable && <span className="ed-badge warning">不可用</span>}
              </dd>
            </div>
            <div>
              <dt>会话上限</dt>
              <dd>
                {info ? `${info.running} / ${info.maxSessions}` : '—'}
              </dd>
            </div>
            <div>
              <dt>空闲回收</dt>
              <dd>{info ? (info.idleTimeout > 0 ? `${info.idleTimeout} 秒（无客户端连接时）` : '关闭') : '—'}</dd>
            </div>
          </dl>
        </article>
        <article className="ed-panel ed-info-card">
          <h2>安全边界</h2>
          <ul className="ed-plain-list">
            <li>文件 API 限制在配置的沙箱根目录内，符号链接默认不跟随。</li>
            <li>上传、下载与终端均经过认证；Cookie 请求校验 Origin/Referer。</li>
            <li>危险命令由 CommandGuard 提示，但不构成执行权限边界。</li>
            <li>V1.0 不接入数据库，指标与审计均为文件或内存实现。</li>
          </ul>
        </article>
      </section>
    </div>
  );
}
