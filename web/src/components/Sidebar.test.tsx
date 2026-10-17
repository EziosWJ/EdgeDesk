// @vitest-environment happy-dom
import { act } from 'react';
import { createRoot, type Root } from 'react-dom/client';
import { beforeEach, describe, expect, it } from 'vitest';
import { Sidebar, type HealthState, type ModuleId } from './Sidebar';

const STORAGE_KEY = 'edgedesk.nav.collapsed.v1';

describe('Sidebar', () => {
  let container: HTMLDivElement;
  let root: Root;

  beforeEach(() => {
    window.localStorage.removeItem(STORAGE_KEY);
    document.body.innerHTML = '';
    container = document.createElement('div');
    document.body.appendChild(container);
    root = createRoot(container);
  });

  const render = (active: ModuleId = 'dashboard', health: HealthState = 'online', onSelect: (id: ModuleId) => void = () => {}) => {
    act(() => {
      root.render(<Sidebar active={active} health={health} onSelect={onSelect} />);
    });
  };

  it('renders the modules and marks the active one', () => {
    render('files');
    const items = [...container.querySelectorAll<HTMLButtonElement>('.nav-item')];
    expect(items.map((item) => item.textContent)).toEqual(['仪表盘', '文件管理', '终端工作区', '设置']);
    expect(container.querySelector('.nav-item.active')?.textContent).toBe('文件管理');
  });

  it('reports the selected module', () => {
    const picks: ModuleId[] = [];
    render('dashboard', 'online', (id) => picks.push(id));
    const items = container.querySelectorAll<HTMLButtonElement>('.nav-item');
    act(() => items[2].click());
    expect(picks).toEqual(['terminal']);
  });

  it('collapses and expands through the toggle, persisting the choice', () => {
    render();
    expect(container.querySelector('.sidebar.collapsed')).toBeNull();
    const toggle = () => container.querySelector<HTMLButtonElement>('.sidebar-toggle')!;

    act(() => toggle().click());
    expect(container.querySelector('.sidebar.collapsed')).not.toBeNull();
    expect(window.localStorage.getItem(STORAGE_KEY)).toBe('1');
    expect(toggle().getAttribute('aria-label')).toBe('展开菜单');
    // labels stay reachable for assistive tech and as tooltips while collapsed
    expect(container.querySelector('.nav-item')?.getAttribute('aria-label')).toBe('仪表盘');
    expect(container.querySelector('.nav-item')?.getAttribute('title')).toBe('仪表盘');

    act(() => toggle().click());
    expect(container.querySelector('.sidebar.collapsed')).toBeNull();
    expect(window.localStorage.getItem(STORAGE_KEY)).toBe('0');
    expect(toggle().getAttribute('aria-label')).toBe('收起菜单');
    expect(container.querySelector('.nav-item')?.getAttribute('title')).toBeNull();
  });

  it('restores the persisted collapsed state', () => {
    window.localStorage.setItem(STORAGE_KEY, '1');
    render();
    expect(container.querySelector('.sidebar.collapsed')).not.toBeNull();
  });

  it('shows the service status derived from health', () => {
    render('dashboard', 'offline');
    expect(container.querySelector('.device-dot.offline')).not.toBeNull();
    expect(container.querySelector('.device-status .status')?.textContent).toBe('未连接');
    expect(container.querySelector('.device-status')?.getAttribute('title')).toBe('本机服务：未连接');
  });
});
