# Qoplin 轻量级笔记程序

Qoplin 是一个轻量级笔记程序，支持端到端加密和基于 WebDAV 的内容同步。

![截图](https://github.com/user-attachments/assets/10804e89-20cb-432f-b298-cde55e69af0d)

## 功能

- 新建、打开、重命名和移除笔记本，支持从 WebDAV 远程导入。
- 多级逻辑文件夹与笔记目录树，支持重命名、删除、拖放和名称筛选。
- 每篇笔记的 Markdown 正文、逻辑目录和笔记本名称均在本地加密保存。
- 编辑、预览、双列三种模式，支持常用基础 Markdown 语法。
- 支持从剪贴板、编辑菜单和工具栏插入图片，图片在本地加密保存并随 WebDAV 同步；大图采用缩放解码，预览区右键可导出原图。
- 使用 WebDAV 同步，支持端到端加密。
- 不引入重型浏览器组件渲染 Markdown，内存占用低。

## 构建

直接使用 Qt Creator 的 MinGW Kit 打开 `CMakeLists.txt` 即可，无需导入 Preset。手动 Kit 会自动使用本机 vcpkg，并共享 `build/vcpkg_installed` 中的依赖。

命令行 Release 构建使用 `CMakePresets.json`：

```powershell
cmake --preset windows-mingw-release
cmake --build --preset windows-mingw-release
ctest --preset windows-mingw-release
```

Preset 中的 Qt、MinGW、Ninja 和 vcpkg 路径可按本机环境调整。

## 笔记本格式

```text
笔记本目录/
├─ qoplin.json                 # 仅 UUID、格式版本和密钥派生参数
└─ .qoplin/
   ├─ index.bin                # 加密的笔记本名称和逻辑目录
   ├─ index.bin.bak            # 加密备份
   ├─ notes/
   │  └─ <UUID>.bin            # 加密的 Markdown 正文
   └─ assets/
      └─ <UUID>.bin            # 加密的图片资源
```

目录树是逻辑结构，重命名或移动笔记不会重命名磁盘上的 UUID 文件。创建笔记本必须设置主密码，打开时需要解锁；主密码只驻留内存，不写入配置，丢失后无法恢复。索引、正文和备份文件均不包含明文数据。当前版本不支持旧的明文笔记本格式，也不提供迁移。

## 从远程导入

在启动页点击“从远程导入”，填写 WebDAV 服务和远端根目录后，可查询远端笔记本 UUID，也可直接输入 UUID。输入主密码并选择本地父目录后，程序会在暂存目录中解密和校验全部对象，成功后保留原笔记本 UUID 并打开，失败或取消不会注册半成品笔记本。导入后主密码只在当前解锁会话中使用，不会写入配置。

## 同步说明

- 主密码不会保存；WebDAV 服务器密码按当前设计以明文保存在本机配置中。
- 标题、逻辑目录、正文、图片资源及远端清单均加密。
- 实际远端位置为 `远端目录/笔记本UUID/`，不同笔记本相互隔离。
- 同一对象发生冲突时，以对象内 UTC 修改时间较新的版本为准，不做行级合并。
- 删除通过 tombstone 同步。
- 同步过程中会暂时锁定当前编辑区。
- 端到端加密不防御恶意服务器回滚或删除数据，如有需要请保留独立备份。

## 打包

Release 构建后，在程序输出目录运行 Qt 的 `windeployqt` 命令行工具。

```powershell
windeployqt --release .\Qoplin.exe
```
