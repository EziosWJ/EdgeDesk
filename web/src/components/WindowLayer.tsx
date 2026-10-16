import { FloatingWindow } from './FloatingWindow';
import { EditorPane } from '../features/files/EditorPane';
import { PreviewPane } from '../features/files/PreviewPane';
import { TerminalPane } from '../features/terminal/TerminalPane';
import { windowStore, useWindows } from '../stores/windowStore';
import { sessionStore } from '../stores/sessionStore';

export function WindowLayer() {
  const windows = useWindows();
  const minimized = windows.filter((item) => item.state === 'minimized');

  return (
    <>
      {windows.map((item) => (
        <FloatingWindow key={item.id} window={item}>
          {item.kind === 'terminal' && (
            <TerminalPane
              sessionId={item.payload.sessionId ?? null}
              title={item.title}
              onSessionCreated={(session) => sessionStore.upsert(session)}
            />
          )}
          {item.kind === 'editor' && item.payload.path && <EditorPane path={item.payload.path} />}
          {item.kind === 'preview' && item.payload.path && <PreviewPane path={item.payload.path} />}
        </FloatingWindow>
      ))}
      {minimized.length > 0 && (
        <div className="ed-taskbar" role="toolbar" aria-label="最小化的窗口">
          {minimized.map((item) => (
            <button key={item.id} type="button" onClick={() => windowStore.focus(item.id)} title="恢复窗口">
              {item.kind === 'terminal' ? '⌨' : item.kind === 'editor' ? '✎' : '👁'} {item.title}
            </button>
          ))}
        </div>
      )}
    </>
  );
}
