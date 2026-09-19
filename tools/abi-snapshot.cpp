#include <cstddef>
#include <cstdint>
#include <iostream>
#include <type_traits>
#include "../sdk/tc_mod_api.h"

static void record(const char* name, std::uint64_t value) {
    std::cout << name << '=' << value << '\n';
}
static void record_signed(const char* name, std::int64_t value) {
    std::cout << name << '=' << value << '\n';
}

#define ABI_SIZE(type) record("sizeof." #type, sizeof(type))
#define ABI_ALIGN(type) record("alignof." #type, alignof(type))
#define ABI_LAYOUT(type) record("standard-layout." #type, std::is_standard_layout<type>::value ? 1u : 0u)
#define ABI_OFFSET(type, field) record("offsetof." #type "." #field, offsetof(type, field))
#define ABI_CONSTANT(name) record("constant." #name, static_cast<std::uint64_t>(name))
#define ABI_SIGNED_CONSTANT(name) record_signed("constant." #name, static_cast<std::int64_t>(name))

int main() {
    record("meta.pointer-size", sizeof(void*));
    record("meta.function-pointer-size", sizeof(TCModLoad));

    ABI_SIZE(TCFrame); ABI_ALIGN(TCFrame); ABI_LAYOUT(TCFrame);
    ABI_OFFSET(TCFrame, size); ABI_OFFSET(TCFrame, frame_number); ABI_OFFSET(TCFrame, time_seconds);

    ABI_SIZE(TCUiPageDefinition); ABI_ALIGN(TCUiPageDefinition); ABI_LAYOUT(TCUiPageDefinition);
    ABI_OFFSET(TCUiPageDefinition, size); ABI_OFFSET(TCUiPageDefinition, page_id);
    ABI_OFFSET(TCUiPageDefinition, title); ABI_OFFSET(TCUiPageDefinition, draw); ABI_OFFSET(TCUiPageDefinition, user);

    ABI_SIZE(TCUiSlotDefinition); ABI_ALIGN(TCUiSlotDefinition); ABI_LAYOUT(TCUiSlotDefinition);
    ABI_OFFSET(TCUiSlotDefinition, size); ABI_OFFSET(TCUiSlotDefinition, kind); ABI_OFFSET(TCUiSlotDefinition, slot_id);
    ABI_OFFSET(TCUiSlotDefinition, title); ABI_OFFSET(TCUiSlotDefinition, draw); ABI_OFFSET(TCUiSlotDefinition, user);
    ABI_OFFSET(TCUiSlotDefinition, preferred_width); ABI_OFFSET(TCUiSlotDefinition, preferred_height);

    ABI_SIZE(TCUiTexturePixels); ABI_ALIGN(TCUiTexturePixels); ABI_LAYOUT(TCUiTexturePixels);
    ABI_OFFSET(TCUiTexturePixels, size); ABI_OFFSET(TCUiTexturePixels, width); ABI_OFFSET(TCUiTexturePixels, height);
    ABI_OFFSET(TCUiTexturePixels, filter); ABI_OFFSET(TCUiTexturePixels, rgba); ABI_OFFSET(TCUiTexturePixels, byte_count);
    ABI_SIZE(TCUiTexture); ABI_ALIGN(TCUiTexture); ABI_LAYOUT(TCUiTexture);
    ABI_OFFSET(TCUiTexture, size); ABI_OFFSET(TCUiTexture, width); ABI_OFFSET(TCUiTexture, height);
    ABI_OFFSET(TCUiTexture, reserved); ABI_OFFSET(TCUiTexture, handle); ABI_OFFSET(TCUiTexture, renderer_id);

    ABI_SIZE(TCHost); ABI_ALIGN(TCHost); ABI_LAYOUT(TCHost);
    ABI_OFFSET(TCHost, size); ABI_OFFSET(TCHost, api_version); ABI_OFFSET(TCHost, context);
    ABI_OFFSET(TCHost, game_build); ABI_OFFSET(TCHost, mod_id); ABI_OFFSET(TCHost, data_directory_utf8);
    ABI_OFFSET(TCHost, log); ABI_OFFSET(TCHost, resolve_symbol); ABI_OFFSET(TCHost, engine_proc);
    ABI_OFFSET(TCHost, create_hook); ABI_OFFSET(TCHost, register_logic); ABI_OFFSET(TCHost, register_component);
    ABI_OFFSET(TCHost, register_ui_page); ABI_OFFSET(TCHost, create_ui_texture); ABI_OFFSET(TCHost, load_ui_texture);
    ABI_OFFSET(TCHost, release_ui_texture); ABI_OFFSET(TCHost, register_ui_slot); ABI_OFFSET(TCHost, host_version);
    ABI_OFFSET(TCHost, host_version_reserved); ABI_OFFSET(TCHost, capabilities); ABI_OFFSET(TCHost, report_status);
    ABI_OFFSET(TCHost, resolve_alias); ABI_OFFSET(TCHost, register_hook_chain); ABI_OFFSET(TCHost, add_event_listener);
    ABI_OFFSET(TCHost, get_current_game_handle); ABI_OFFSET(TCHost, validate_game_handle); ABI_OFFSET(TCHost, resolve_game_handle);

    ABI_SIZE(TCGameHandle); ABI_ALIGN(TCGameHandle); ABI_LAYOUT(TCGameHandle);
    ABI_OFFSET(TCGameHandle, size); ABI_OFFSET(TCGameHandle, kind); ABI_OFFSET(TCGameHandle, generation); ABI_OFFSET(TCGameHandle, token);

    ABI_SIZE(TCPlugin); ABI_ALIGN(TCPlugin); ABI_LAYOUT(TCPlugin);
    ABI_OFFSET(TCPlugin, size); ABI_OFFSET(TCPlugin, user); ABI_OFFSET(TCPlugin, on_frame); ABI_OFFSET(TCPlugin, on_unload);

    ABI_SIZE(TCLogicIO); ABI_ALIGN(TCLogicIO); ABI_LAYOUT(TCLogicIO);
    ABI_OFFSET(TCLogicIO, size); ABI_OFFSET(TCLogicIO, phase); ABI_OFFSET(TCLogicIO, instance_id);
    ABI_OFFSET(TCLogicIO, cycle); ABI_OFFSET(TCLogicIO, input_count); ABI_OFFSET(TCLogicIO, output_count);
    ABI_OFFSET(TCLogicIO, inputs); ABI_OFFSET(TCLogicIO, outputs); ABI_OFFSET(TCLogicIO, state); ABI_OFFSET(TCLogicIO, user);
    ABI_SIZE(TCLogicDefinition); ABI_ALIGN(TCLogicDefinition); ABI_LAYOUT(TCLogicDefinition);
    ABI_OFFSET(TCLogicDefinition, size); ABI_OFFSET(TCLogicDefinition, version); ABI_OFFSET(TCLogicDefinition, custom_id);
    ABI_OFFSET(TCLogicDefinition, callback); ABI_OFFSET(TCLogicDefinition, user);
    ABI_SIZE(TCComponentPin); ABI_ALIGN(TCComponentPin); ABI_LAYOUT(TCComponentPin);
    ABI_OFFSET(TCComponentPin, name); ABI_OFFSET(TCComponentPin, bits);
    ABI_SIZE(TCNativeComponentDefinition); ABI_ALIGN(TCNativeComponentDefinition); ABI_LAYOUT(TCNativeComponentDefinition);
    ABI_OFFSET(TCNativeComponentDefinition, size); ABI_OFFSET(TCNativeComponentDefinition, custom_id);
    ABI_OFFSET(TCNativeComponentDefinition, name); ABI_OFFSET(TCNativeComponentDefinition, description);
    ABI_OFFSET(TCNativeComponentDefinition, shape_svg); ABI_OFFSET(TCNativeComponentDefinition, input_count);
    ABI_OFFSET(TCNativeComponentDefinition, output_count); ABI_OFFSET(TCNativeComponentDefinition, inputs);
    ABI_OFFSET(TCNativeComponentDefinition, outputs); ABI_OFFSET(TCNativeComponentDefinition, gate_cost);
    ABI_OFFSET(TCNativeComponentDefinition, delay); ABI_OFFSET(TCNativeComponentDefinition, callback);
    ABI_OFFSET(TCNativeComponentDefinition, user);

    ABI_SIZE(TCHookSimDoArgs); ABI_ALIGN(TCHookSimDoArgs); ABI_LAYOUT(TCHookSimDoArgs);
    ABI_OFFSET(TCHookSimDoArgs, size); ABI_OFFSET(TCHookSimDoArgs, command);
    ABI_OFFSET(TCHookSimDoArgs, model); ABI_OFFSET(TCHookSimDoArgs, target);
    ABI_SIZE(TCHookLevelLoadArgs); ABI_ALIGN(TCHookLevelLoadArgs); ABI_LAYOUT(TCHookLevelLoadArgs);
    ABI_OFFSET(TCHookLevelLoadArgs, size); ABI_OFFSET(TCHookLevelLoadArgs, reserved);
    ABI_OFFSET(TCHookLevelLoadArgs, board_model); ABI_OFFSET(TCHookLevelLoadArgs, name);
    ABI_SIZE(TCHookCall); ABI_ALIGN(TCHookCall); ABI_LAYOUT(TCHookCall);
    ABI_OFFSET(TCHookCall, size); ABI_OFFSET(TCHookCall, hook_id); ABI_OFFSET(TCHookCall, cycle);
    ABI_OFFSET(TCHookCall, user); ABI_OFFSET(TCHookCall, args); ABI_OFFSET(TCHookCall, skip_original);
    ABI_OFFSET(TCHookCall, reserved); ABI_OFFSET(TCHookCall, run_chain); ABI_OFFSET(TCHookCall, loader_state);

    ABI_SIZE(TCEvent); ABI_ALIGN(TCEvent); ABI_LAYOUT(TCEvent);
    ABI_OFFSET(TCEvent, size); ABI_OFFSET(TCEvent, kind); ABI_OFFSET(TCEvent, flags); ABI_OFFSET(TCEvent, reserved);
    ABI_OFFSET(TCEvent, cycle); ABI_OFFSET(TCEvent, subject); ABI_OFFSET(TCEvent, name); ABI_OFFSET(TCEvent, user);

    ABI_CONSTANT(TC_MOD_API_VERSION); ABI_CONSTANT(TC_HOST_BASE_SIZE);
    ABI_CONSTANT(TC_CAP_LOG); ABI_CONSTANT(TC_CAP_SYMBOL); ABI_CONSTANT(TC_CAP_HOOK); ABI_CONSTANT(TC_CAP_LOGIC);
    ABI_CONSTANT(TC_CAP_COMPONENT); ABI_CONSTANT(TC_CAP_UI_PAGE); ABI_CONSTANT(TC_CAP_UI_SLOT);
    ABI_CONSTANT(TC_CAP_TEXTURE); ABI_CONSTANT(TC_CAP_STATUS); ABI_CONSTANT(TC_CAP_SYMBOL_ALIAS);
    ABI_CONSTANT(TC_CAP_HOOK_CHAIN); ABI_CONSTANT(TC_CAP_EVENTS);
    ABI_CONSTANT(TC_CAP_GAME_HANDLES);
    ABI_CONSTANT(TC_UI_SLOT_BOARD_SIDE); ABI_CONSTANT(TC_UI_SLOT_BOARD_TOOLBAR);
    ABI_CONSTANT(TC_LOGIC_RESET); ABI_CONSTANT(TC_LOGIC_REFRESH); ABI_CONSTANT(TC_LOGIC_CYCLE);
    ABI_CONSTANT(TC_HOOK_SIM_DO); ABI_CONSTANT(TC_HOOK_LEVEL_LOAD); ABI_CONSTANT(TC_HOOK_OK);
    ABI_SIGNED_CONSTANT(TC_HOOK_ERR_UNAVAILABLE); ABI_SIGNED_CONSTANT(TC_HOOK_ERR_ID);
    ABI_SIGNED_CONSTANT(TC_HOOK_ERR_ARGUMENT); ABI_SIGNED_CONSTANT(TC_HOOK_ERR_CAPACITY); ABI_SIGNED_CONSTANT(TC_HOOK_ERR_TARGET);
    ABI_CONSTANT(TC_EVENT_LEVEL_LOAD); ABI_CONSTANT(TC_EVENT_SCENE_CHANGE); ABI_CONSTANT(TC_EVENT_SIM_COMMAND);
    ABI_CONSTANT(TC_EVENT_SAVE); ABI_CONSTANT(TC_EVENT_OK); ABI_SIGNED_CONSTANT(TC_EVENT_ERR_UNAVAILABLE);
    ABI_SIGNED_CONSTANT(TC_EVENT_ERR_ARGUMENT); ABI_SIGNED_CONSTANT(TC_EVENT_ERR_CAPACITY);
    ABI_CONSTANT(TC_GAME_OBJECT_BOARD); ABI_CONSTANT(TC_GAME_OBJECT_COMPONENT); ABI_CONSTANT(TC_GAME_OBJECT_WIRE); ABI_CONSTANT(TC_GAME_OBJECT_LEVEL);
    ABI_CONSTANT(TC_HANDLE_OK); ABI_SIGNED_CONSTANT(TC_HANDLE_ERR_UNAVAILABLE); ABI_SIGNED_CONSTANT(TC_HANDLE_ERR_ARGUMENT);
    ABI_SIGNED_CONSTANT(TC_HANDLE_ERR_KIND); ABI_SIGNED_CONSTANT(TC_HANDLE_ERR_STALE);
    return 0;
}
