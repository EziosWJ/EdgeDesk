import type { WindowState } from '../types';
import { DashboardView } from '../features/dashboard/DashboardView';
import { FilesView } from '../features/files/FilesView';
import { TerminalView } from '../features/terminal/TerminalView';
import { TerminalSessionsView } from '../features/terminal/TerminalSessionsView';
import { SettingsView } from '../features/settings/SettingsView';
import { EditorPane } from '../features/files/EditorPane';
import { PreviewPane } from '../features/files/PreviewPane';
import { ExtensionManagerView } from '../extensions/ExtensionManagerView';
import { ExtensionAppView } from '../extensions/ExtensionAppView';
import { getInstalledExtension } from '../extensions/extensionRegistry';

type Props = {
  window: WindowState;
  onLogout: () => void;
  onExtensionsChanged: () => void;
};

/** Maps an app window to its business component. Launch decisions stay in AppRegistry. */
export function AppSurface({ window, onLogout, onExtensionsChanged }: Props) {
  switch (window.appId) {
    case 'dashboard':
      return <DashboardView />;
    case 'extensions':
      return <ExtensionManagerView onExtensionsChanged={onExtensionsChanged} />;
    case 'files':
      return <FilesView initialPath={window.payload.initialPath ?? '/'} windowId={window.id} />;
    case 'terminal':
      return (
        <TerminalView
          initialSessionId={window.payload.sessionId ?? null}
          restoredSessionMissing={window.restoreError === 'session_ended'}
          windowId={window.id}
        />
      );
    case 'sessions':
      return <TerminalSessionsView />;
    case 'settings':
      return <SettingsView onLogout={onLogout} />;
    case 'editor':
      return window.payload.path ? (
        <EditorPane path={window.payload.path} windowId={window.id} />
      ) : (
        <div className="ed-empty small">缺少文件路径，编辑器无法打开。</div>
      );
    case 'preview':
      return window.payload.path ? (
        <PreviewPane path={window.payload.path} />
      ) : (
        <div className="ed-empty small">缺少文件路径，无法预览。</div>
      );
    default:
      return getInstalledExtension(window.appId) ? (
        <ExtensionAppView appId={window.appId} windowId={window.id} />
      ) : (
        <div className="ed-empty small">未知应用：{window.appId}</div>
      );
  }
}
