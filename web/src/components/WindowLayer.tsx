import { FloatingWindow } from './FloatingWindow';
import { AppSurface } from '../desktop/AppSurface';
import { useNarrowScreen } from '../desktop/useNarrowScreen';
import { useFocusedWindowId, useWindows } from '../stores/windowStore';

type Props = {
  onLogout: () => void;
  onExtensionsChanged: () => void;
};

/**
 * Renders every open window. Minimized windows stay mounted (hidden), so
 * editor buffers and terminal WebSocket subscriptions are not destroyed by a
 * minimize/restore cycle. Closing a window is the only action that unmounts.
 */
export function WindowLayer({ onLogout, onExtensionsChanged }: Props) {
  const windows = useWindows();
  const focusedId = useFocusedWindowId();
  const narrow = useNarrowScreen();

  return (
    <div className="ed-window-layer">
      {windows.map((item) => (
        <FloatingWindow key={item.id} window={item} narrow={narrow} focused={item.id === focusedId}>
          <AppSurface window={item} onLogout={onLogout} onExtensionsChanged={onExtensionsChanged} />
        </FloatingWindow>
      ))}
    </div>
  );
}
