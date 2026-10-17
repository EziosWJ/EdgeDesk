import { useEffect, useState } from 'react';
import type { LucideIcon } from 'lucide-react';
import {
  FolderTree,
  LayoutDashboard,
  PanelLeftClose,
  PanelLeftOpen,
  Settings,
  SquareTerminal,
} from 'lucide-react';
import { Icon } from './Icon';

export type ModuleId = 'dashboard' | 'files' | 'terminal' | 'settings';
export type HealthState = 'checking' | 'online' | 'offline';

const modules: { id: ModuleId; label: string; icon: LucideIcon }[] = [
  { id: 'dashboard', label: '仪表盘', icon: LayoutDashboard },
  { id: 'files', label: '文件管理', icon: FolderTree },
  { id: 'terminal', label: '终端工作区', icon: SquareTerminal },
  { id: 'settings', label: '设置', icon: Settings },
];

const STORAGE_KEY = 'edgedesk.nav.collapsed.v1';

const asset = (path: string) => `${import.meta.env.BASE_URL}assets/${path}`;

/** Collapsed state is a desktop-only preference; on narrow screens the layout wins. */
function loadCollapsed(): boolean {
  try {
    return window.localStorage.getItem(STORAGE_KEY) === '1';
  } catch {
    return false;
  }
}

type SidebarProps = {
  active: ModuleId;
  health: HealthState;
  onSelect: (id: ModuleId) => void;
};

export function Sidebar({ active, health, onSelect }: SidebarProps) {
  const [collapsed, setCollapsed] = useState(loadCollapsed);

  useEffect(() => {
    try {
      window.localStorage.setItem(STORAGE_KEY, collapsed ? '1' : '0');
    } catch {
      // localStorage can be unavailable (private mode); persistence is optional.
    }
  }, [collapsed]);

  const statusText = health === 'online' ? '运行中 · 已认证' : health === 'offline' ? '未连接' : '检测中';
  const toggleLabel = collapsed ? '展开菜单' : '收起菜单';

  return (
    <aside className={`sidebar ${collapsed ? 'collapsed' : ''}`}>
      <div className="sidebar-head">
        <img className="brand-mark" src={asset('brand/edgedesk-mark.svg')} width="30" height="30" alt="" />
        <div className="brand-text">
          <strong>
            Edge<span>Desk</span>
          </strong>
          <small>嵌入式 Linux 运维工作台</small>
        </div>
        <button type="button" className="sidebar-toggle" onClick={() => setCollapsed((value) => !value)} title={toggleLabel} aria-label={toggleLabel}>
          <Icon icon={collapsed ? PanelLeftOpen : PanelLeftClose} size={17} />
        </button>
      </div>

      <nav aria-label="主导航" className="main-nav">
        {modules.map((item) => (
          <button
            key={item.id}
            type="button"
            className={`nav-item ${active === item.id ? 'active' : ''}`}
            aria-current={active === item.id ? 'page' : undefined}
            aria-label={collapsed ? item.label : undefined}
            title={collapsed ? item.label : undefined}
            onClick={() => onSelect(item.id)}
          >
            <Icon icon={item.icon} size={18} className="nav-icon" />
            <span className="nav-label">{item.label}</span>
          </button>
        ))}
      </nav>

      <div className="device-status" title={`本机服务：${statusText}`}>
        <span className={`device-dot ${health}`} aria-hidden="true" />
        <div className="device-status-text">
          <strong>本机服务</strong>
          <span className={`status ${health}`}>{statusText}</span>
        </div>
      </div>
    </aside>
  );
}
