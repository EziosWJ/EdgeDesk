import { FileWorkspace } from './FileWorkspace';

/** File management page: page header plus the tree/list workspace. */
export function FilesView({ initialPath = '/' }: { initialPath?: string }) {
  return (
    <div className="ed-view ed-files-view">
      <div className="ed-view-header">
        <div>
          <h1>文件管理</h1>
          <p>
            目录树按需加载；浏览、上传、下载、编辑与删除都经过统一的路径沙箱，文本保存为原子替换并保留原权限。
          </p>
        </div>
      </div>
      <FileWorkspace initialPath={initialPath} />
    </div>
  );
}
