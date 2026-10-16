import { useEffect, useState } from 'react';

type ModuleId = 'dashboard' | 'files' | 'terminal';

type Health = {
  status: 'ok';
  service: 'edgedesk';
  phase: number;
};

const modules: { id: ModuleId; label: string; summary: string; icon: string }[] = [
  { id: 'dashboard', label: '仪表盘', summary: '硬件资源概览与短时曲线', icon: 'dashboard' },
  { id: 'files', label: '文件管理', summary: '文件浏览、上传、编辑及浮动窗口', icon: 'files' },
  { id: 'terminal', label: '终端工作区', summary: 'Bash PTY、多 Tab 与会话重连', icon: 'terminal' },
];

const asset = (path: string) => `${import.meta.env.BASE_URL}assets/${path}`;

function NavIcon({ name }: { name: string }) {
  return <img className="nav-icon" src={asset(`icons/${name}.svg`)} alt="" />;
}

export default function App() {
  const [active, setActive] = useState<ModuleId>('dashboard');
  const [health, setHealth] = useState<'checking' | 'online' | 'offline'>('checking');
  useEffect(() => {
    const controller = new AbortController();
    fetch(`${import.meta.env.BASE_URL}api/health`, { signal: controller.signal, cache: 'no-store' })
      .then(async (response) => {
        if (!response.ok) throw new Error(String(response.status));
        const data: Health = await response.json();
        if (data.status !== 'ok' || data.service !== 'edgedesk') throw new Error('Bad health');
        setHealth('online');
      })
      .catch(() => {
        if (!controller.signal.aborted) setHealth('offline');
      });
    return () => controller.abort();
  }, []);

  const current = modules.find((item) => item.id === active)!;
  return (
    <div className="shell">
      <aside className="sidebar">
        <div className="brand">
          <img src={asset('brand/edgedesk-mark.svg')} width="44" height="44" alt="" />
          <div><strong>Edge<span>Desk</span></strong><small>嵌入式 Linux 运维面板</small></div>
        </div>
        <nav aria-label="主导航" className="main-nav">
          {modules.map((item) => (
            <button key={item.id} type="button" className={`nav-item ${active === item.id ? 'active' : ''}`}
              aria-current={active === item.id ? 'page' : undefined} onClick={() => setActive(item.id)}>
              <NavIcon name={item.icon} />{item.label}
            </button>
          ))}
        </nav>
        <div className="device-status ed-panel">
          <img src={asset('illustrations/edge-device.svg')} alt="" width="62" height="52" />
          <div><strong>本机服务</strong><span className={`status ${health}`}>
            {health === 'online' ? 'HTTP 在线' : health === 'offline' ? '未连接' : '检测中'}
          </span></div>
        </div>
      </aside>
      <div className="content-shell">
        <header className="topbar"><div className="topbar-title">EdgeDesk <span>/ Phase 1</span></div><span className="version">v0.1 · 工程骨架</span></header>
        <main className="main-area">
          <div className="heading"><p className="eyebrow">EMBEDDED LINUX CONTROL PANEL</p><h1>{current.label}</h1><p>{current.summary}</p></div>
          <section className="overview">
            <div className="ed-panel intro-card">
              <div className="mini-label">当前阶段</div>
              <h2>工程初始化已就绪</h2>
              <p>已建立 React 页面骨架、统一视觉素材、C++ 服务接口与资源嵌入构建链路。本阶段不展示虚构设备数据，也不提供未完成的运维操作。</p>
              <div className="tag-row"><span className="tag">C++17 / libhv</span><span className="tag">GCC 15+ #embed</span><span className="tag">React + TypeScript</span></div>
            </div>
            <div className="ed-panel health-card">
              <div className="mini-label">服务健康检查</div>
              <h2>GET /api/health</h2>
              <div className={`health-value ${health}`}><span className="health-dot" />{health === 'online' ? '服务正常' : health === 'offline' ? '服务不可达' : '检测中'}</div>
              <p>用于验证 HTTP 服务与前端资源是否正确集成。</p>
            </div>
          </section>
          <section className="module-area"><h2>功能模块 <span>后续阶段实现</span></h2><div className="module-grid">
            {modules.map(item => <article className="ed-panel module-card" key={item.id}>
              <div className="module-icon"><NavIcon name={item.icon} /></div><h3>{item.label}</h3><p>{item.summary}</p><span className="planned">尚未实现</span>
            </article>)}
          </div></section>
          <p className="footnote">设计资源基于 EdgeDesk 原型图提炼；功能状态以 ADR、Spec 和自动化测试为准。</p>
        </main>
      </div>
    </div>
  );
}
