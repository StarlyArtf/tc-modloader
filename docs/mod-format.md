# .mod 包格式（资源 format 1 / 原生 format 2）

`.mod` 是 ZIP 压缩包，仅把后缀改为 `.mod`。清单必须位于压缩包根目录，不能多包一层
文件夹。接受 `mod.json`、`files/`、`native/` 下的文件。

```json
{
  "format": 2,
  "id": "author.example",
  "name": "示例名称",
  "version": "1.0.0",
  "author": "作者",
  "description": "说明用途和兼容条件。",
  "requires": [],
  "optional": [],
  "capabilities": [],
  "patches": [],
  "native": {"api": 1, "entry": "native/my-plugin.dll"}
}
```

必需字段是 `format`、`id`、`name`、`version`；`native` 仅 `format: 2` 需要。

- `id` 长度 1–80，只能包含小写英文字母、数字、点、下划线和短横线；同目录内重复 ID 的包
  均不可启用。
- `requires` 列出必须同时启用的 Mod ID：可以是 id 数组，也可以是
  `{"其他包.id": ">=1.2.0"}` 这样的**对象形式**，值写版本约束。约束只在对方**被启用**
  时检查，失败时不会写入任何文件。
- `optional` 同样两种写法：可选的包没被启用（或根本不在 mods 文件夹里）不报错；
  一旦启用，它带上的版本约束照样生效。
- `capabilities` 声明本包需要加载器提供哪些能力（`log`、`hook`、`component`、
  `ui_page`、`ui_slot`、`texture`、`status` 等）。名字写错是"Unknown loader
  capability"，加载器太旧是"This loader does not provide the capability"，两种拒绝
  都发生在扫描阶段，玩家在 Mods 页能直接看到。名单与语义见
  [reference/capabilities.md](reference/capabilities.md)。
- `native.api` 当前为 1，`entry` 是包内 DLL 路径（Windows x64）。入口、Hook 与回调规范见
  [sdk/host-api.md](sdk/host-api.md)。
- format 1 继续支持纯资源包；format 1 不执行代码，format 2 执行 `native.entry`，两者都
  不执行额外安装脚本。

DLL 及私有依赖会解压到按包 SHA-256 区分的缓存目录，再以绝对路径加载；一个包里可以
带多个 DLL。

## 文件替换／新增

```text
mod.json
files/
  asset/...
  campaign/...
  translations/...
native/            可选
  my-plugin.dll
```

`files/` 之后的路径对应游戏根目录路径，例如 `files/asset/example.txt` 部署到
`<游戏目录>/asset/example.txt`。

现有文件首次修改前会备份，停用时恢复；新文件停用时删除。禁止部署到 EXE、DLL、存档、
加载器目录及其他根目录。

## 精确文本补丁

可以只提供 `mod.json`，用 `patches` 修改已有文本：

```json
"patches": [
  {
    "path": "translations/Chinese (Simplified).txt",
    "find": "$16857608955464* 沙盒模式",
    "replace": "$16857608955464* 沙盒模式 [MOD]"
  }
]
```

- 以 UTF-8 字节精确匹配，不做换行转换。
- `find` 必须非空且恰好出现一次，否则整个包不可启用；多个补丁按清单顺序执行。
- 同一个 Mod 同时提供完整文件和该文件的补丁时，先部署完整文件再打补丁。
- 已启用的 Mod 从保存的原文件重新计算补丁，不会反复叠加。
- 不同 Mod 修改同一路径时一律冲突，不做补丁级合并。

## 限制

| 项目 | 上限 |
|---|---|
| 单个压缩包 | 128 MiB / 4096 条目 |
| 单文件解压 | 64 MiB |
| 解压总量 | 256 MiB |

路径使用 `/`，禁止绝对路径、`..`、Windows 保留文件名、备用数据流和重解析链接；
路径冲突不区分英文字母大小写。

## 发布建议

- 只打包有权分发的内容；只改少量文本时用精确补丁，避免附带整份游戏文件。
- 原生包应用后记录指纹；替换包内容后必须重新应用并重启，见
  [install.md](install.md#启停更新与恢复)。
- 发布前按 [verification.md](verification.md#作者自检清单) 的清单自检。
