import { useEffect, useRef } from 'react';
import { createPortal } from 'react-dom';

type Props = {
  title: string;
  message: string;
  details?: string[];
  confirmText?: string;
  cancelText?: string;
  danger?: boolean;
  busy?: boolean;
  onConfirm: () => void;
  onCancel: () => void;
};

/** Modal confirmation for destructive operations (delete, recursive delete). */
export function ConfirmDialog({
  title,
  message,
  details = [],
  confirmText = '确定',
  cancelText = '取消',
  danger = false,
  busy = false,
  onConfirm,
  onCancel,
}: Props) {
  const confirmRef = useRef<HTMLButtonElement | null>(null);

  useEffect(() => {
    confirmRef.current?.focus();
  }, []);

  return createPortal(
    <div className="ed-modal-backdrop" onPointerDown={onCancel}>
      <div
        className="ed-modal ed-panel"
        role="alertdialog"
        aria-modal="true"
        aria-label={title}
        onPointerDown={(event) => event.stopPropagation()}
      >
        <h2>{title}</h2>
        <p className="ed-modal-text">{message}</p>
        {details.length > 0 && (
          <ul className="ed-modal-list">
            {details.map((item) => (
              <li key={item}>{item}</li>
            ))}
          </ul>
        )}
        <div className="ed-modal-actions">
          <button type="button" className="ed-button" onClick={onCancel} disabled={busy}>
            {cancelText}
          </button>
          <button
            ref={confirmRef}
            type="button"
            className={`ed-button ${danger ? 'danger' : 'primary'}`}
            onClick={onConfirm}
            disabled={busy}
          >
            {busy ? '处理中…' : confirmText}
          </button>
        </div>
      </div>
    </div>,
    document.body,
  );
}
