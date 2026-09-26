/* Test-only driver: select the first component that has settings, so the game's
   bottom drawer opens on it.  The punch-tape panel draws its squares inside that
   drawer, so a screenshot needs the drawer open - and clicking the component
   from a script depends on the camera, which this avoids.

   It calls the board's own select_component(index, record) entry point, the same
   one the mouse path ends in. */
#include "../sdk/tc_mod_api.h"
#include "../sdk/tc_handle_api.h"
#include <windows.h>
#include <stdint.h>
#include <array>
#include <cstring>
#include <string>

static const TCHost* host;
using SelectFn = void (*)(int64_t index, void* record);
static SelectFn selectComponent;
using ClearSelectionsFn = void (*)();
static ClearSelectionsFn clearSelections;
using AddComponentFn = bool (*)(void* board, void* component);
static AddComponentFn addComponent;
static uint64_t* currentWordSize;
static bool done;
static bool placed;
static bool selectionReported;
static int frames;

static void frame(void*, const TCFrame*) {
    if (done || !selectComponent) return;
    if (++frames < 240) return;
    TCGameHandle handle{};
    if (tc::currentGameHandle(host, TC_GAME_OBJECT_BOARD, &handle) != TC_HANDLE_OK) return;
    const void* raw = nullptr;
    if (tc::resolveGameHandle(host, &handle, &raw) != TC_HANDLE_OK || !raw) return;
    auto* bytes = const_cast<uint8_t*>(static_cast<const uint8_t*>(raw));
    uint64_t count = 0;
    uint8_t* data = nullptr;
    memcpy(&count, bytes + 0x78, sizeof(count));
    memcpy(&data, bytes + 0x80, sizeof(data));
    if (!data || count == 0 || count > 100000) return;
    for (uint64_t index = 0; index < count; ++index) {
        uint8_t* component = data + 8 + index * 0x238;
        if (component[0] != 0x2e) continue;
        int64_t settings = 0;
        memcpy(&settings, component + 0xa8, sizeof(settings));
        if (settings <= 0) continue;
        uint64_t id = 0;
        memcpy(&id, component + 8, sizeof(id));
        if (!selectionReported) {
            const std::string line = "DRIVER: selecting component #" + std::to_string(index) +
                                     " kind=" + std::to_string(component[0]) + " settings=" +
                                     std::to_string(settings) + " id=" + std::to_string(id);
            host->log(host->context, line.c_str());
        }
        clearSelections();
        selectComponent(static_cast<int64_t>(id), component);
        if (!selectionReported) host->log(host->context, "DRIVER: constant selected");
        selectionReported = true;
        return;
    }
    /* The autotest driver may still be replacing the menu's placeholder board;
       keep waiting until the requested level and its constants exist. */
    if (!placed && frames >= 600 && addComponent) {
        std::array<uint8_t, 0x238> component{};
        component[0] = 0x2e;
        const int16_t x = 0, y = 5;
        memcpy(component.data() + 2, &x, sizeof(x));
        memcpy(component.data() + 4, &y, sizeof(y));
        const uint64_t one = 1, capacity = 0x100;
        memcpy(component.data() + 0x58, &one, sizeof(one));
        memcpy(component.data() + 0x60, &capacity, sizeof(capacity));
        component[0x68] = 1;
        memcpy(component.data() + 0x70, &one, sizeof(one));
        memcpy(component.data() + 0x78, &capacity, sizeof(capacity));
        component[0x80] = 1;
        if (currentWordSize) *currentWordSize = 64;
        placed = addComponent(bytes, component.data());
        host->log(host->context, placed ? "DRIVER: placed 64-bit constant"
                                        : "DRIVER: constant placement failed");
    }
}

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* h, TCPlugin* out) {
    if (!h || !out || h->api_version != TC_MOD_API_VERSION) return 1;
    host = h;
    if (!h->resolve_symbol || !h->create_hook) return 2;
    selectComponent = reinterpret_cast<SelectFn>(h->resolve_symbol(
        h->context, "select_component__modelZboardZboard_u9202"));
    clearSelections = reinterpret_cast<ClearSelectionsFn>(h->resolve_symbol(
        h->context, "clear_selections__modelZboardZboard_u8323"));
    addComponent = reinterpret_cast<AddComponentFn>(h->resolve_symbol(
        h->context, "add_component__presenterZutilitiesZhelper95functions_u5918"));
    currentWordSize = static_cast<uint64_t*>(h->resolve_symbol(
        h->context, "current_word_size__modelZmodel95types_u741"));
    if (!selectComponent || !clearSelections || !addComponent || !currentWordSize) return 3;
    out->on_frame = frame;
    h->log(h->context, "DRIVER: armed");
    return 0;
}
