# Mod 配置与存档数据：`tc.mod.storage`

`tc.mod.storage` V1/V2 给每个原生 Mod 两个由宿主管理、互相隔离的命名空间。插件只传 scope、key
和字节，不接触路径，也不能传另一个 Mod 的 id。

| scope | 用途 | 实际归属 |
|---|---|---|
| `TC_MOD_STORAGE_SCOPE_GLOBAL` | 用户设置、缓存、窗口偏好 | 当前安装，切换存档不变 |
| `TC_MOD_STORAGE_SCOPE_PROFILE` | 跟随存档的数据、进度、存档级选项 | 当前独立存档；复制存档时一起复制 |

## 最小用法

```cpp
#include "tc_mod_storage.h"

tc::mod_storage::Api storage{};
if (!tc::mod_storage::table(host, &storage)) return 2;

std::string theme;
int status = tc::mod_storage::readText(
    storage, TC_MOD_STORAGE_SCOPE_GLOBAL, "theme", &theme);
if (status == TC_MOD_STORAGE_ERR_NOT_FOUND) theme = "dark";

tc::mod_storage::writeText(
    storage, TC_MOD_STORAGE_SCOPE_GLOBAL, "theme", theme);
```

二进制值使用 `read` / `write`；`read` 的第一次调用可以只问大小：

```cpp
uint32_t bytes = 0;
int status = storage.read(storage.context, TC_MOD_STORAGE_SCOPE_PROFILE,
                          "state", nullptr, 0, &bytes);
std::vector<unsigned char> value(bytes);
if (status == TC_MOD_STORAGE_OK && bytes)
    status = storage.read(storage.context, TC_MOD_STORAGE_SCOPE_PROFILE,
                          "state", value.data(), bytes, &bytes);
```

## 契约

- 服务在 `tc_mod_load` 中已经可用，也可在插件运行期间调用；宿主内部串行化文件操作。
- key 是 1–64 字节的小写 ASCII 名称，只能包含 `a-z 0-9 . _ -`；它不是路径。
- `write` 是整个值的原子替换；零字节值有效，与不存在不同。
- `read` 在缓冲太小时返回 `ERR_SIZE`，同时通过 `bytes` 告知所需大小。
- `has` 返回 1/0 或负错误；`remove` 删除不存在的 key 时返回 `ERR_NOT_FOUND`。
- 每项最多 1 MiB；每个 Mod、每个 scope 最多 256 项、合计 8 MiB。
- GLOBAL 文件位于加载器自己的 `plugin-data/<mod id>/storage/`；PROFILE 文件位于当前独立
  存档内部的 `tc-mod-data/<mod id>/`。这些是诊断位置，不是公共 ABI，插件不得自行拼路径。
- 原生 Mod 与游戏同进程，存储服务只负责命名空间、路径检查、配额和原子替换，不是安全沙箱。

## V2：schema 迁移

V2 在同一个原子值中保存宿主管理的 schema 版本；旧 V1/raw 值自动视为版本 0。V1 读写继续只看
payload，V1 写入已经版本化的 key 时保留其 schema 版本。

```cpp
tc::mod_storage::ApiV2 storage{};
if (!tc::mod_storage::tableV2(host, &storage)) return 2;

uint32_t resulting = 0;
const char initial[] = "{}";
int status = tc::mod_storage::migrate(
    storage, TC_MOD_STORAGE_SCOPE_GLOBAL, "settings", 2,
    initial, 2, migrateSettings, user, &resulting);
```

- key 不存在时直接用 default payload 创建目标版本，不调用回调。
- 当前版本等于目标版本时不做事；当前版本更高时返回 `ERR_VERSION`，不降级。
- 旧版本低于目标版本时，回调一次完成 `from → target`。只有 `MIGRATE_OK` 才提交。
- 回调拒绝、抛异常、输出超限或迁移期间同一 key 被改动，旧 payload/版本都保持不变。
- 版本与 payload 在一个 envelope 中一次原子替换，因此不存在“数据已换、版本号没换”的半迁移状态。

V1 仍是稳定 ABI；不知道 V2 的旧插件无需改动。统一设置页由相邻的
[`tc.mod.settings`](mod-settings.md) 提供。

真机验证会启动游戏三次并切换 A → B → A：全局计数必须是 1/2/3，存档计数必须是 1/1/2。
复现见 `tests/mod-storage-playtest.ps1`。`examples/board-grid` 是第一个实际消费者：它把旧
`board-grid.txt` / schema 0 的 `grid=…` 迁为 GLOBAL `settings` schema 1，再把字段交给统一设置页。
