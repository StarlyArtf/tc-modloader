# 教程：第一个原生 Mod

目标：做一个能在游戏里运行、并出现自己面板的插件。全程只需 MinGW-w64（或 MSVC）。

## 1. 建目录

```text
my-mod/
  mod.json
  native/
    my-plugin.dll       编译产物
  plugin.cpp
```

`mod.json`：

```json
{
  "format": 2,
  "id": "author.my-mod",
  "name": "我的第一个原生 Mod",
  "version": "1.0.0",
  "author": "you",
  "description": "显示当前周期。",
  "native": {"api": 1, "entry": "native/my-plugin.dll"}
}
```

## 2. 最小插件

```cpp
#include "tc_mod.h"
#include <cstdio>

static tc::TCMod mod;
static int frames = 0;

static void frame(void*, const TCFrame* value) {
    ++frames;
    if (frames % 120) return;                       // 别每帧刷屏
    char text[64];
    std::snprintf(text, sizeof(text), "cycle=%lld", static_cast<long long>(mod.simulation.cycle()));
    host_log(text);                                  // 见下文说明
}

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* host, TCPlugin* plugin) {
    if (host->api_version != TC_MOD_API_VERSION) return 1;
    if (!mod.load(host) || !mod.valid()) return 2;
    plugin->on_frame = frame;
    host->log(host->context, "my-mod: loaded");
    return 0;
}
```

`host` 不能存成全局裸指针随便用——把它放进 `tc::TCMod` 之类的结构里，或按
[host-api.md](../sdk/host-api.md) 的约定保存 `TCHost*`（进程内有效）。

## 3. 编译与打包

```powershell
g++ -std=c++17 -O2 -static -shared plugin.cpp -Isdk -o my-mod/native/my-plugin.dll
powershell -NoProfile -ExecutionPolicy Bypass -File tools/Pack-Mod.ps1 -Source my-mod -Output author.my-mod.mod
```

把 `author.my-mod.mod` 放进 `<游戏目录>/mods/`，启动游戏 → 主菜单 Mods → 勾选 →
应用更改 → 重启。

## 4. 加一个面板（可选）

```cpp
static void frame(void*, const TCFrame*) {
    auto* igBegin = reinterpret_cast<bool (*)(const char*, bool*, int)>(
        host->engine_proc(host->context, "igBegin"));
    auto* igText = reinterpret_cast<void (*)(const char*)>(
        host->engine_proc(host->context, "igText"));
    auto* igEnd = reinterpret_cast<void (*)()>(
        host->engine_proc(host->context, "igEnd"));

    bool open = true;
    if (igBegin("My Mod", &open, 0)) igText("hello from C++");
    igEnd();
}
```

约束：使用游戏自身的 ImGui 上下文，不成对 Begin/End 会让界面错乱；不要跨 C ABI 抛异常
或阻塞渲染线程。细节见 [host-api.md](../sdk/host-api.md#每帧回调与界面)。

## 5. 拦一次运行命令（可选）

```cpp
using SimDo = void (*)(void*, uint8_t, int64_t);
static SimDo original;

static void simDo(void* model, uint8_t command, int64_t target) {
    if (command == 0 && target > 1000) target = 1000;   // 最多再跑 1000 周期
    original(model, command, target);
}

// tc_mod_load 中：
auto* address = host->resolve_symbol(host->context,
    "sim_do__modelZsimulationZcompile95thread_u3036");
if (!address) return 3;
if (host->create_hook(host->context, address, reinterpret_cast<void*>(&simDo),
                      reinterpret_cast<void**>(&original)) != 0) return 4;
```

完整实现见 `examples/cycle-guard/plugin.cpp`（含可切换的拦截幅度与调试面板）。

## 6. 自检清单

1. 入口在 ABI 版本或结构长度不符时返回非 0。
2. 所有 Hook 用 `resolve_symbol` 的返回值判空。
3. 回调内不抛异常；需要时捕获并写日志。
4. 插件数据写在 `data_directory_utf8` 下，不要写游戏根目录。
5. 用 `-static` 静态链接运行库，或把依赖 DLL 一起放进 `native/`。

接着可以学 [component-with-cpp-logic.md](component-with-cpp-logic.md)：让游戏里的自定义
元件由这段 C++ 决定行为。
