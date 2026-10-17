import { useCallback, useEffect, useRef, useState } from 'react';
import {
  Download,
  Eye,
  FolderOpen,
  Link2,
  MoreVertical,
  PenLine,
  TextCursorInput,
  Trash2,
} from 'lucide-react';
import { Icon } from '../../components/Icon';
import { api } from '../../services/api';
import type { FileEntry } from '../../types';

type Props = {
  entry: FileEntry;
  onOpen: (entry: FileEntry) => void;
  onPreview: (entry: FileEntry) => void;
  onEdit: (entry: FileEntry) => void;
  onRename: (entry: FileEntry) => void;
  onDelete: (entry: FileEntry) => void;
  className?: string;
};

const MENU_WIDTH = 188;

/** Kebab menu shared by the list rows and the grid cards. */
export function FileEntryActions({
  entry,
  onOpen,
  onPreview,
  onEdit,
  onRename,
  onDelete,
  className,
}: Props) {
  const [position, setPosition] = useState<{ top: number; left: number } | null>(null);
  const rootRef = useRef<HTMLDivElement | null>(null);
  const triggerRef = useRef<HTMLButtonElement | null>(null);
  const open = position !== null;

  const close = useCallback(() => setPosition(null), []);

  const toggle = () => {
    if (open) {
      close();
      return;
    }
    const rect = triggerRef.current?.getBoundingClientRect();
    if (!rect) return;
    const left = Math.max(8, Math.min(rect.right - MENU_WIDTH, window.innerWidth - MENU_WIDTH - 8));
    setPosition({ top: rect.bottom + 4, left });
  };

  useEffect(() => {
    if (!open) return;
    const onPointerDown = (event: PointerEvent) => {
      const target = event.target;
      if (rootRef.current && target instanceof Node && rootRef.current.contains(target)) return;
      close();
    };
    const onKeyDown = (event: KeyboardEvent) => {
      if (event.key === 'Escape') close();
    };
    window.addEventListener('pointerdown', onPointerDown);
    window.addEventListener('keydown', onKeyDown);
    window.addEventListener('scroll', close, true);
    window.addEventListener('resize', close);
    return () => {
      window.removeEventListener('pointerdown', onPointerDown);
      window.removeEventListener('keydown', onKeyDown);
      window.removeEventListener('scroll', close, true);
      window.removeEventListener('resize', close);
    };
  }, [open, close]);

  const run = (action: () => void) => () => {
    close();
    action();
  };

  return (
    // Clicks inside the menu must never reach the card/row open handler.
    <div
      className={`ed-actions${className ? ` ${className}` : ''}`}
      ref={rootRef}
      onClick={(event) => event.stopPropagation()}
    >
      <button
        ref={triggerRef}
        type="button"
        className="ed-icon-button ed-kebab"
        title="文件操作"
        aria-label={`${entry.name} 操作`}
        aria-haspopup="menu"
        aria-expanded={open}
        onClick={toggle}
      >
        <Icon icon={MoreVertical} size={15} />
      </button>
      {open && position && (
        <div
          className="ed-menu"
          role="menu"
          aria-label={`${entry.name} 操作`}
          style={{ top: position.top, left: position.left }}
        >
          {entry.type === 'dir' && (
            <button type="button" role="menuitem" onClick={run(() => onOpen(entry))}>
              <Icon icon={FolderOpen} size={14} />
              打开
            </button>
          )}
          {entry.type === 'file' && (
            <a role="menuitem" href={api.downloadUrl(entry.path)} onClick={close}>
              <Icon icon={Download} size={14} />
              下载
            </a>
          )}
          {entry.type === 'file' && (
            <button type="button" role="menuitem" onClick={run(() => onPreview(entry))}>
              <Icon icon={Eye} size={14} />
              预览
            </button>
          )}
          {entry.type === 'file' && entry.text_editable && (
            <button type="button" role="menuitem" onClick={run(() => onEdit(entry))}>
              <Icon icon={PenLine} size={14} />
              编辑
            </button>
          )}
          {entry.type === 'symlink' && (
            <button type="button" role="menuitem" onClick={run(() => onOpen(entry))}>
              <Icon icon={Link2} size={14} />
              查看链接目标
            </button>
          )}
          <div className="ed-menu-sep" />
          <button type="button" role="menuitem" onClick={run(() => onRename(entry))}>
            <Icon icon={TextCursorInput} size={14} />
            重命名
          </button>
          <button type="button" role="menuitem" className="danger" onClick={run(() => onDelete(entry))}>
            <Icon icon={Trash2} size={14} />
            删除
          </button>
        </div>
      )}
    </div>
  );
}
