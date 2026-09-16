# .mod 格式（资源 format 1 / 原生 format 2）

`.mod` 是 ZIP 压缩包，文件名后缀改为 `.mod`。清单必须位于压缩包根目录，不能多包一层文件夹。接受 `mod.json`、`files/`、`native/` 下的文件。

逻辑插件使用 `format: 2`，增加 `"native": {"api": 1, "entry": "native/my-plugin.dll"}`。入口、Hook 与回调规范见 `SDK-GUIDE.md`。format 1 继续支持纯资源包。

```json
{
  "format": 1,
  "id": "author.example",
  "name": "示例名称",
  "version": "1.0.0",
  "author": "作者",
  "description": "说明用途和兼容条件。",
  "requires": [],
  "patches": []
}
```

必需字段是 `format`、`id`、`name`、`version`。ID 长度为 1–80，只能包含小写英文字母、数字、点、下划线和短横线。同目录内重复 ID 的包均不可启用。`requires` 为必须同时启用的 Mod ID 数组，不包含版本约束。

## 文件替换／新增

```
mod.json
files/
  asset/...
  campaign/...
  translations/...
```

`files/` 之后的路径对应游戏根目录路径。例如 `files/asset/example.txt` 部署到 `游戏目录/asset/example.txt`。

现有文件首次修改前会备份；停用时恢复。新文件停用时删除。禁止部署到 EXE、DLL、存档、加载器目录及其他根目录。

## 精确文本补丁

可以只有 `mod.json`，通过清单中的 `patches` 修改已有文本：

```json
"patches": [
  {
    "path": "translations/Chinese (Simplified).txt",
    "find": "$16857608955464* 沙盒模式",
    "replace": "$16857608955464* 沙盒模式 [MOD]"
  }
]
```

以 UTF-8 字节精确匹配，不做换行转换。`find` 必须非空且恰好出现一次，否则整个包不可启用。多个补丁按清单顺序执行。同一个 Mod 若同时提供完整文件和该文件的补丁，先使用完整文件再打补丁。

已经启用的 Mod 会从保存的原文件重新计算补丁，不会反复叠加。不同 Mod 修改同一个路径时一律冲突，不做补丁级合并。

## 限制

- 单个压缩包最多 128 MiB、4096 个条目。
- 单文件解压最多 64 MiB，总解压大小最多 256 MiB。
- 路径使用 `/`，禁止绝对路径、`..`、Windows 保留文件名、备用数据流和重解析链接。
- 路径冲突不区分英文字母大小写。
- format 1 不执行代码；format 2 执行 native.entry 指定的 DLL。均不执行额外安装脚本。

Mod 分发者应只打包有权分发的内容；只修改少量文本时可以使用精确补丁，避免附带整份游戏文件。
