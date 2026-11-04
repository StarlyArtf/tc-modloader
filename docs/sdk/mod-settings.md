# 统一 Mod 设置页：`tc.mod.settings`

`tc.mod.settings` V1 让 Mod 声明字段，不再各自 Hook 游戏设置页或手写页面。宿主复制字段表、从
`tc.mod.storage` 读取当前值，在主菜单提供一个 **Mod settings** 入口，并负责校验、保存与重置。

支持 `BOOL`、`INT`、`FLOAT`、`ENUM`、`TEXT`。字段按 scope 分成 **Global settings** 与
**Current save settings**；后者随当前 profile 复制、切换和删除。

```cpp
#include "tc_mod_settings.h"

tc::mod_settings::Api settings{};
if (!tc::mod_settings::table(host, &settings)) return 2;

TCModSettingFieldV1 field{};
field.size = sizeof(field);
field.type = TC_MOD_SETTING_BOOL;
field.scope = TC_MOD_STORAGE_SCOPE_GLOBAL;
field.id = "grid";
field.label = "Show board grid";
field.description = "Draw integer coordinates behind components.";
field.default_integer = 1;

if (tc::mod_settings::setFields(settings, &field, 1, onChanged, user) !=
    TC_MOD_SETTINGS_OK) return 3;

TCModSettingValueV1 value{};
tc::mod_settings::get(settings, "grid", &value);
```

## 行为契约

- `set_fields` / `clear_fields` 只在游戏线程的 `tc_mod_load` 期间调用；`get` / `reset` 也只允许游戏
  线程。字符串与枚举标签由宿主复制。
- 每个 Mod 最多 64 个字段；id 最长 48 字节，字符集为 `[a-z0-9._-]`。
- `storage_key` 为空时使用 `settings.<id>`；显式 key 可用于接管已有数据。字段 id 和 storage key
  在一张表内都必须唯一。
- INT 使用 32 位范围并声明 min/max/step/default；FLOAT 要求有限数；ENUM 最多 16 项；TEXT 最长
  255 字节。任何一项无效都会拒绝整张表。
- key 不存在时宿主把默认值以 schema 1 写入；已有值无效时先保留原字节、界面使用默认值，日志会说明。
- UI 改动与 `reset` 先调用可选的 changed 回调；非零返回会拒绝改动。通过后宿主原子持久化，再更新
  内存当前值。
- `get` 返回宿主当前值；`reset(id)` 恢复该字段默认值。插件不得保存返回结构里的临时地址。
- 页面只在至少一个活跃 Mod 成功声明字段时出现；Mod 加载失败时它的字段会随插件注册一起撤销。

`examples/board-grid` 展示了完整升级路线：先用 `tc.mod.storage` V2 把旧文本迁到 schema 1，再声明
指向同一 `settings` key 的布尔字段。真机门槛在 `tests/mod-storage-playtest.ps1`：三次重启、两个 profile，
同时断言字段注册日志和主页 `settings` 入口。
