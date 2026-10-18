import { windowStore } from '../../stores/windowStore';
import { FileWorkspace } from './FileWorkspace';

type Props = {
  initialPath?: string;
  windowId?: string;
};

/**
 * File management app. The window chrome already shows the app identity, so
 * the workspace fills the window edge-to-edge with its own command bar.
 */
export function FilesView({ initialPath = '/', windowId }: Props) {
  return (
    <div className="ed-view ed-files-view">
      <h1 className="ed-visually-hidden">文件管理</h1>
      <FileWorkspace
        initialPath={initialPath}
        onPathChange={
          windowId
            ? (path) => windowStore.updatePayload(windowId, { initialPath: path })
            : undefined
        }
      />
    </div>
  );
}
