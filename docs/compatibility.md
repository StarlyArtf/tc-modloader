# 游戏兼容性画像与 SDK ABI

这两道门分别保护加载器与游戏、加载器与原生 Mod 之间的二进制边界。它们都属于发布契约，
不是运行失败后才看的备注。

## 游戏兼容性画像

`compat/profiles.json` 是游戏构建事实的唯一声明来源。一个画像包含：

- 稳定 id、游戏版本、平台和支持状态；
- `Turing Complete.exe` 与原版 `game_engine.dll` 的大小和 SHA-256；
- Loader 注入所依赖的 RVA；
- 该构建必须解析成功的稳定符号别名；
- 安装器允许升级的历史 Loader 哈希。

恰好一个画像标记为 `current: true`。`tools/generate-compat.ps1` 校验画像与
`src/symbol_profile.hpp` 一致，并生成 `src/compat.generated.hpp`。安装器和 Loader 只编译使用
生成的常量，启动时不依赖外部 JSON；JSON 仍是审核、扩展和工具识别的源。

识别本机游戏目录：

```powershell
./tools/compat.ps1 -GameDirectory 'D:\Games\Turing Complete'
./tools/compat.ps1 -GameDirectory 'D:\Games\Turing Complete' -Json
```

已安装 Loader 时，工具校验 `tc_game_engine.dll` 中保存的原引擎；干净安装则校验
`game_engine.dll`。没有画像同时匹配 EXE 与原引擎时返回退出码 2，不能靠关闭哈希检查继续。

适配新游戏构建时，应复制并修改画像、重新测量所有 RVA、逐项核实符号别名，再把状态从
`diagnostic` 改为 `supported`。最后运行生成器、host 层和 game 层测试。不能只替换两个哈希。

## SDK ABI 快照

`abi/windows-x64.json` 是公开 C ABI 在 MinGW UCRT x64 下的审核基线。`tools/abi-snapshot.cpp`
由真实 SDK 头文件编译，记录：

- 所有公开 ABI 结构的大小、对齐和每个字段偏移；
- 结构是否仍是 standard-layout；
- API 版本、能力位、事件位、Hook id 和错误码等稳定常量；
- 数据指针与函数指针宽度。

检查当前头文件：

```powershell
./tools/abi.ps1
```

任何新增、删除或数值变化都会列出具体记录并失败。ABI 确实需要演进时，先判断旧 DLL 是否仍能
工作：`TCHost` 之类可扩展结构只能在尾部追加字段，插件必须按 `size` 检查；既有字段不能改型、
换序或复用，已发布的枚举、能力位和错误码不能重新编号。完成兼容设计和评审后才运行：

```powershell
./tools/abi.ps1 -Update
```

不要为了让测试变绿直接更新基线。更新后的 JSON 差异就是 ABI 评审材料。

## 自动门禁

- fast 层的 `compatibility-contract` 检查画像、生成头和符号目录的一致性；
- fast 层的 `sdk-abi` 重新编译探针并比较公开 ABI 记录；
- host 层的 `compatibility-game` 对仓库旁的已固定游戏文件做真实画像识别；
- `release.json` 记录画像 id、画像清单哈希、ABI 目标和 ABI 基线哈希；
- 玩家包携带画像、离线识别工具、ABI 基线和 ABI 检查工具。

当前只有 `tc-win64-2.1.334` 一个受支持画像。这个机制让新增版本有明确流程，但不会自动声称
相似版本兼容。
