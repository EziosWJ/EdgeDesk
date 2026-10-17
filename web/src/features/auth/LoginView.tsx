import { useState } from 'react';
import { api } from '../../services/api';

const asset = (path: string) => `${import.meta.env.BASE_URL}assets/${path}`;

export function LoginView({ onSuccess }: { onSuccess: () => void }) {
  const [password, setPassword] = useState('');
  const [error, setError] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);

  const submit = async (event: React.FormEvent) => {
    event.preventDefault();
    setBusy(true);
    setError(null);
    try {
      await api.login(password);
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
        <p>输入管理员密码以继续。EdgeDesk 为单管理员、仅密码登录；脚本客户端可使用独立的 Bearer API Token。</p>
        <label htmlFor="ed-password">管理员密码</label>
        <input
          id="ed-password"
          type="password"
          value={password}
          autoComplete="current-password"
          onChange={(event) => setPassword(event.target.value)}
          placeholder="administrator password"
          required
        />
        {error && <div className="ed-alert danger">{error}</div>}
        <button type="submit" className="ed-button primary" disabled={busy || password.length === 0}>
          {busy ? '验证中…' : '登录'}
        </button>
        <small>会话使用 HttpOnly + SameSite=Strict Cookie，默认 12 小时后过期；服务重启后需重新登录。</small>
      </form>
    </div>
  );
}
