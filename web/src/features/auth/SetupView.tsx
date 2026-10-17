import { useState } from 'react';
import { api } from '../../services/api';

const asset = (path: string) => `${import.meta.env.BASE_URL}assets/${path}`;

export function SetupView({ onSuccess }: { onSuccess: () => void }) {
  const [code, setCode] = useState('');
  const [password, setPassword] = useState('');
  const [confirm, setConfirm] = useState('');
  const [error, setError] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);

  const passwordLength = [...password].length;
  const submit = async (event: React.FormEvent) => {
    event.preventDefault();
    setError(null);
    if (password !== confirm) {
      setError('两次输入的密码不一致');
      return;
    }
    if (passwordLength < 12) {
      setError('密码至少需要 12 个字符');
      return;
    }
    if (new TextEncoder().encode(password).length > 1024) {
      setError('密码不能超过 1024 个 UTF-8 字节');
      return;
    }
    setBusy(true);
    try {
      await api.setup(code.trim(), password, confirm);
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
        <p>
          首次初始化：请输入本机数据目录中 <code>init-code</code> 文件里的一次性初始化码，并设置管理员密码。
          初始化完成后，该码立即失效。
        </p>
        <label htmlFor="ed-setup-code">初始化码</label>
        <input
          id="ed-setup-code"
          type="password"
          value={code}
          autoComplete="one-time-code"
          onChange={(event) => setCode(event.target.value)}
          required
        />
        <label htmlFor="ed-setup-password">管理员密码（至少 12 个字符）</label>
        <input
          id="ed-setup-password"
          type="password"
          value={password}
          autoComplete="new-password"
          onChange={(event) => setPassword(event.target.value)}
          required
        />
        <label htmlFor="ed-setup-confirm">确认密码</label>
        <input
          id="ed-setup-confirm"
          type="password"
          value={confirm}
          autoComplete="new-password"
          onChange={(event) => setConfirm(event.target.value)}
          required
        />
        {error && <div className="ed-alert danger">{error}</div>}
        <button
          type="submit"
          className="ed-button primary"
          disabled={busy || code.trim().length === 0 || password.length === 0 || confirm.length === 0}
        >
          {busy ? '初始化中…' : '创建管理员密码'}
        </button>
        <small>密码使用 Argon2id 哈希保存；初始化码和明文密码不会写入日志。</small>
      </form>
    </div>
  );
}
