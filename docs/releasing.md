# 发布流程

发布内容由 `release/manifest.json` 声明；`tools/release.ps1` 是唯一的完整发布入口。
`package.ps1` 仅作为兼容入口，用已有构建产物调用同一条流水线。

## 正式发布

正式发布要求工作树干净，默认依次运行 fast、host、game 三层验证：

```powershell
./tools/release.ps1
```

如需把开发诊断探针也跑一遍：

```powershell
./tools/release.ps1 -TestTier all
```

先查看将发布的版本、文件名和 Mod，不构建、不写文件：

```powershell
./tools/release.ps1 -Plan
```

本地验证流水线时可以使用已有构建，并明确允许脏工作树；这种产物的 `release.json` 会记录
`"dirty": true`，不应作为正式发布：

```powershell
./tools/release.ps1 -UseExistingBuild -SkipTests -AllowDirty -OutputRoot build/release-smoke
```

`package.ps1` 等价于使用已有构建且跳过测试，但仍默认拒绝脏工作树：

```powershell
./package.ps1
./package.ps1 -AllowDirty -OutputRoot build/release-smoke
```

## 清单

`release/manifest.json` 明确列出：

- 源码包的根文件、目录和排除项；
- 玩家包中的安装器、CLI、SDK、文档和许可证；
- 每个发布 `.mod` 的来源、包内文件名和预期 Mod id；
- 单独放在发布目录供直接下载的产物。

清单要求的文件缺失、目标路径重复、Mod id 不一致、原生入口缺失或包内路径不安全时，发布
立即失败。玩家包只从空 staging 收集清单内容，不扫描已有 `dist/` 猜测该发布什么，因此旧版
压缩包和开发探针不会混进新版本。

## 确定性

`tools/ReleaseTools.psm1` 与 `tools/Pack-Mod.ps1` 使用同一套 ZIP 实现：

- 条目按 UTF-8 路径的 ordinal 顺序写入；
- 时间戳固定为 `SOURCE_DATE_EPOCH`，未设置时使用 ZIP 最小时间 1980-01-01；
- `build.ps1` 同样把未显式设置的 `SOURCE_DATE_EPOCH` 固定为 1980-01-01，避免 GNU ld
  把当前时间写入 PE/COFF 头；
- 压缩级别、路径分隔符与条目属性固定；
- 路径不区分大小写检查冲突；
- 源码包和玩家包都会各生成两次并比较 SHA-256。

需要指定发布日期时，传 Unix 秒数；为了 ZIP 的两秒精度会向下对齐：

```powershell
$env:SOURCE_DATE_EPOCH = '1789747200'
./tools/release.ps1
```

同一源码、同一工具链和同一 `SOURCE_DATE_EPOCH` 应生成字节一致的安装器、`.mod` 与发布 ZIP。

## 产物

默认写入 `dist/releases/<VERSION>/`：

```text
TCModLoader-Setup.exe
TCModLoader-<VERSION>-source.zip
TCModLoader-<VERSION>-win64.zip
release.json
SHA256SUMS.txt
```

玩家 ZIP 内另有一份 `SHA256SUMS.txt`，覆盖其中每一个安装器、工具、文档、SDK、许可证和
示例包。外层 `release.json` 记录源码提交、脏工作树标记、清单哈希、当前兼容性画像、ABI
基线哈希以及每个发布产物的大小和 SHA-256。已有同版本目录默认拒绝覆盖；确认替换这个精确
版本时使用 `-Force`。

## 自动验证

`tests/release.ps1` 属于 fast 层，验证相同 Mod 源生成相同哈希、固定 ZIP 时间戳与条目顺序、
发布目标不重复，以及清单中的全部公开 Mod 都具有预期 id、有效入口和固定 PE/COFF 时间戳。
fast 层还会检查兼容性画像与生成头同步，并重新编译 SDK ABI 探针；host 层会用固定游戏文件
验证画像识别。细节见 [compatibility.md](compatibility.md)。
