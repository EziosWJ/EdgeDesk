import { useEffect, useRef, useState } from 'react';
import { createPortal } from 'react-dom';

type Props = {
  title: string;
  label?: string;
  initial?: string;
  confirmText?: string;
  onConfirm: (value: string) => void;
  onCancel: () => void;
};

export function PromptDialog({ title, label, initial = '', confirmText = '确定', onConfirm, onCancel }: Props) {
  const [value, setValue] = useState(initial);
  const inputRef = useRef<HTMLInputElement | null>(null);

  useEffect(() => {
    inputRef.current?.focus();
    inputRef.current?.select();
  }, []);

  return createPortal(
    <div className="ed-modal-backdrop" onPointerDown={onCancel}>
      <form
        className="ed-modal ed-panel"
        onPointerDown={(event) => event.stopPropagation()}
        onSubmit={(event) => {
          event.preventDefault();
          if (value.trim()) onConfirm(value.trim());
        }}
      >
        <h2>{title}</h2>
        {label && <label htmlFor="ed-prompt-input">{label}</label>}
        <input
          id="ed-prompt-input"
          ref={inputRef}
          value={value}
          onChange={(event) => setValue(event.target.value)}
          autoComplete="off"
        />
        <div className="ed-modal-actions">
          <button type="button" className="ed-button" onClick={onCancel}>
            取消
          </button>
          <button type="submit" className="ed-button primary" disabled={!value.trim()}>
            {confirmText}
          </button>
        </div>
      </form>
    </div>,
    document.body,
  );
}
