import type { AppId } from '../types';

export type AppTone = 'blue' | 'yellow' | 'dark' | 'purple' | 'green' | 'gray';
export type InstancePolicy = 'single' | 'multi';

export type AppSize = { width: number; height: number };

export type AppDefinition = {
  id: AppId;
  name: string;
  description: string;
  /** Extra search terms for the launcher (Chinese and English). */
  keywords: readonly string[];
  /** Symbol id in /assets/desktop/app-icons.svg. */
  sprite: string;
  tone: AppTone;
  instancePolicy: InstancePolicy;
  defaultSize: AppSize;
  minSize: AppSize;
  /** Show a desktop icon on a fresh layout. */
  desktopShortcut: boolean;
  /** Pinned to the Dock. */
  dockPinned: boolean;
  /** Listed in the application launcher. Internal editor/preview windows are not. */
  launcherVisible: boolean;
};

export class AppRegistryError extends Error {
  code: 'invalid_definition' | 'duplicate_app' | 'unknown_app';

  constructor(code: AppRegistryError['code'], message: string) {
    super(message);
    this.name = 'AppRegistryError';
    this.code = code;
  }
}

function assertSize(size: AppSize, field: string): void {
  if (!Number.isFinite(size.width) || !Number.isFinite(size.height) || size.width <= 0 || size.height <= 0) {
    throw new AppRegistryError('invalid_definition', `${field} must contain positive finite dimensions`);
  }
}

const REQUIRED_FIELDS: ReadonlyArray<keyof AppDefinition> = [
  'id',
  'name',
  'description',
  'sprite',
  'tone',
  'instancePolicy',
  'defaultSize',
  'minSize',
];

/** Central metadata for every window application; no duplicate app lists elsewhere. */
export class AppRegistry {
  private apps = new Map<AppId, AppDefinition>();

  register(definition: AppDefinition): void {
    const missing = REQUIRED_FIELDS.filter((field) => definition[field] === undefined || definition[field] === null);
    if (missing.length > 0 || !definition.id || !definition.name.trim()) {
      throw new AppRegistryError('invalid_definition', `missing app metadata: ${missing.join(', ') || 'id/name'}`);
    }
    if (this.apps.has(definition.id)) {
      throw new AppRegistryError('duplicate_app', `app already registered: ${definition.id}`);
    }
    if (definition.instancePolicy !== 'single' && definition.instancePolicy !== 'multi') {
      throw new AppRegistryError('invalid_definition', `invalid instance policy for ${definition.id}`);
    }
    assertSize(definition.defaultSize, 'defaultSize');
    assertSize(definition.minSize, 'minSize');
    if (definition.minSize.width > definition.defaultSize.width || definition.minSize.height > definition.defaultSize.height) {
      throw new AppRegistryError('invalid_definition', `minSize exceeds defaultSize for ${definition.id}`);
    }
    this.apps.set(definition.id, Object.freeze({ ...definition, keywords: [...definition.keywords] }));
  }

  has(id: string): id is AppId {
    return this.apps.has(id as AppId);
  }

  get(id: string): AppDefinition | undefined {
    return this.apps.get(id as AppId);
  }

  require(id: string): AppDefinition {
    const app = this.get(id);
    if (!app) throw new AppRegistryError('unknown_app', `unknown appId: ${id}`);
    return app;
  }

  list(): readonly AppDefinition[] {
    return [...this.apps.values()];
  }

  desktopApps(): readonly AppDefinition[] {
    return this.list().filter((app) => app.desktopShortcut);
  }

  dockApps(): readonly AppDefinition[] {
    return this.list().filter((app) => app.dockPinned);
  }

  launcherApps(): readonly AppDefinition[] {
    return this.list().filter((app) => app.launcherVisible);
  }

  /** Search matches name, description and keywords; empty query lists everything. */
  search(query: string): readonly AppDefinition[] {
    const needle = query.trim().toLocaleLowerCase();
    const apps = this.launcherApps();
    if (!needle) return apps;
    return apps.filter((app) =>
      [app.name, app.description, ...app.keywords].some((value) => value.toLocaleLowerCase().includes(needle)),
    );
  }

  /** Tests re-create the registry without leaking registrations between cases. */
  reset(): void {
    this.apps.clear();
  }
}

export function createDefaultRegistry(): AppRegistry {
  const registry = new AppRegistry();
  const apps: AppDefinition[] = [
    {
      id: 'dashboard',
      name: '设备概览',
      description: 'CPU、内存、存储、网卡与服务状态',
      keywords: ['dashboard', 'cpu', '内存', '监控', 'metrics', 'network'],
      sprite: 'dashboard',
      tone: 'blue',
      instancePolicy: 'single',
      defaultSize: { width: 940, height: 700 },
      minSize: { width: 520, height: 360 },
      desktopShortcut: true,
      dockPinned: true,
      launcherVisible: true,
    },
    {
      id: 'files',
      name: '文件管理',
      description: '浏览、上传、下载和编辑设备文件',
      keywords: ['files', 'folder', '目录', '文件', 'upload', '下载'],
      sprite: 'files',
      tone: 'yellow',
      instancePolicy: 'multi',
      defaultSize: { width: 1000, height: 680 },
      minSize: { width: 520, height: 360 },
      desktopShortcut: true,
      dockPinned: true,
      launcherVisible: true,
    },
    {
      id: 'terminal',
      name: '终端',
      description: '打开即用的单页面 Bash 终端',
      keywords: ['terminal', 'bash', 'shell', '命令', 'pty', '终端'],
      sprite: 'terminal',
      tone: 'dark',
      instancePolicy: 'multi',
      defaultSize: { width: 900, height: 600 },
      minSize: { width: 420, height: 300 },
      desktopShortcut: true,
      dockPinned: true,
      launcherVisible: true,
    },
    {
      id: 'sessions',
      name: '会话管理',
      description: '管理 Bash 会话、标签页与生命周期',
      keywords: ['sessions', 'session', '会话', 'terminal', 'bash', '标签页'],
      sprite: 'sessions',
      tone: 'green',
      instancePolicy: 'single',
      defaultSize: { width: 940, height: 620 },
      minSize: { width: 460, height: 320 },
      desktopShortcut: true,
      dockPinned: true,
      launcherVisible: true,
    },
    {
      id: 'settings',
      name: '系统设置',
      description: '外观、会话安全与系统信息',
      keywords: ['settings', '设置', '主题', '密码', 'system'],
      sprite: 'settings',
      tone: 'gray',
      instancePolicy: 'single',
      defaultSize: { width: 780, height: 640 },
      minSize: { width: 480, height: 360 },
      desktopShortcut: false,
      dockPinned: true,
      launcherVisible: true,
    },
    {
      id: 'editor',
      name: '文本编辑器',
      description: '编辑设备上的 UTF-8 文本文件',
      keywords: ['editor', '编辑', '文本'],
      sprite: 'editor',
      tone: 'purple',
      instancePolicy: 'multi',
      defaultSize: { width: 780, height: 560 },
      minSize: { width: 380, height: 240 },
      desktopShortcut: false,
      dockPinned: false,
      launcherVisible: false,
    },
    {
      id: 'preview',
      name: '文件预览',
      description: '预览文本与图片',
      keywords: ['preview', '预览'],
      sprite: 'logs',
      tone: 'green',
      instancePolicy: 'multi',
      defaultSize: { width: 640, height: 520 },
      minSize: { width: 320, height: 240 },
      desktopShortcut: false,
      dockPinned: false,
      launcherVisible: false,
    },
  ];
  apps.forEach((app) => registry.register(app));
  return registry;
}

export const appRegistry = createDefaultRegistry();
