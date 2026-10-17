import { ShieldCheck } from 'lucide-react';
import { Icon } from '../../components/Icon';
import { FileWorkspace } from './FileWorkspace';

/** File management page: compact header plus the tree/list workspace. */
export function FilesView({ initialPath = '/' }: { initialPath?: string }) {
  return (
    <div className="ed-view ed-files-view">
      <div className="ed-view-header ed-view-header-compact">
        <div>
          <h1>文件管理</h1>
          <p>浏览、编辑和管理设备上的文件；所有操作限制在配置的根目录内。</p>
        </div>
        <span
          className="ed-badge subtle ed-sandbox-badge"
          title="目录树按需加载；上传、下载、编辑与删除均经过统一路径沙箱；文本保存采用原子替换并保留原权限。"
        >
          <Icon icon={ShieldCheck} size={13} />
          路径沙箱已启用
        </span>
      </div>
      <FileWorkspace initialPath={initialPath} />
    </div>
  );
}
