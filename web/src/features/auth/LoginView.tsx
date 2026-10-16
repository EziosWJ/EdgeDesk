import { useState } from 'react';
import { api } from '../../services/api';

const asset = (path: string) => `${import.meta.env.BASE_URL}assets/${path}`;

export function LoginView({ onSuccess }: { onSuccess: () => void }) {
  const [token, setToken] = useState('');
  const [error, setError] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);

  const submit = async (event: React.FormEvent) => {
    event.preventDefault();
    setBusy(true);
    setError(null);
    try {
      await api.login(token.trim());
      onSuccess();
    } catch (caught) {
      setError(caught instanceof Error ? caught.message : String(caught));
    } finally {
      setBusy(false);
    }
  };

  return (
    <div className="ed-login">
      <form className="ed-login-card ed-panel" onSubmit={(event) => void submit(event)}>
        <img className="ed-login-mark" src={asset('brand/edgedesk-mark.svg')} alt="EdgeDesk" width={52} height={52} />
        <h1>
          Edge<span>Desk</span>
        </h1>
        <p>输入设备访问密钥以继续。密钥由启动参数、密钥文件或环境变量提供；未配置时服务启动日志会打印一次性密钥。</p>
        <label htmlFor="ed-token">访问密钥</label>
        <input
          id="ed-token"
          type="password"
          value={token}
          autoComplete="current-password"
          onChange={(event) => setToken(event.target.value)}
          placeholder="access token"
          required
        />
        {error && <div className="ed-alert danger">{error}</div>}
        <button type="submit" className="ed-button primary" disabled={busy || token.trim().length === 0}>
          {busy ? '验证中…' : '登录'}
        </button>
        <small>会话使用 HttpOnly + SameSite=Strict Cookie，12 小时后过期；服务重启后需重新登录。</small>
      </form>
    </div>
  );
}
