import { ShieldAlert } from 'lucide-react';

export function CorruptStateView() {
  return (
    <div className="ed-login">
      <div className="ed-login-card ed-panel">
        <ShieldAlert className="ed-login-mark" size={52} color="var(--ed-warning)" aria-hidden="true" />
        <h1>
          Edge<span>Desk</span>
        </h1>
        <p>认证状态损坏或不兼容。为防止绕过登录保护，远程初始化和业务 API 已关闭。</p>
        <div className="ed-alert warning">
          请在本机停止服务后执行：
          <code>edgedesk --reset-admin-password --data-dir &lt;数据目录&gt;</code>
          然后重启服务，使用新的初始化码设置密码。业务文件和设置不会被删除。
        </div>
        <small>更多信息请查看服务端启动日志或系统服务日志。</small>
      </div>
    </div>
  );
}
