#pragma once
/* The symbol profile: the one place that maps a stable alias to this game
   build's exact COFF name.

   Why it exists: a plugin used to hard-code strings like
   "sim_do__modelZsimulationZcompile95thread_u3036", which are Nim-mangled names
   that change with every game update, and a typo only showed up as a null
   pointer in the middle of an initialization.  Now the loader resolves the
   whole table once at boot, logs anything that did not resolve, and hands the
   address out by alias (host->resolve_alias).  Adapting to a new game build is
   an edit here, not a hunt through every plugin.

   Adding an entry: only list a name that has been measured against this build
   (the address must be the function or data object the alias claims).  Note the
   evidence in the `note` field, because the next person has to be able to check
   it.  Names come from the EXE's COFF symbol table, which also contains imports
   (that is why "igInvisibleButton" resolves). */
#include <stdint.h>
#include <string>
#include <vector>

namespace tc {

enum SymbolKind : uint32_t {
    /* Executable code: safe to call, and the only kind create_hook accepts. */
    TC_SYM_FUNCTION = 1u,
    /* A data object (often a global pointer).  Read the note before dereferencing. */
    TC_SYM_DATA = 0u,
};

struct SymbolEntry {
    const char* alias;
    const char* coff;
    uint32_t kind;
    const char* note;
};

/* Order is irrelevant; the loader builds a map.  The list covers what the
   shipped examples, the SDK headers and the playtests actually resolve, so a
   plugin can drop the mangled names entirely. */
inline const std::vector<SymbolEntry>& symbol_profile() {
    static const std::vector<SymbolEntry> value{
        {"sim.do", "sim_do__modelZsimulationZcompile95thread_u3036", TC_SYM_FUNCTION,
         "void(void* model, uint8_t command, int64_t target); command 0 run, 1 refresh/stop, 2 mode_reset"},
        {"sim.cycle", "sim_get_cycle__modelZsimulationZcompile95thread_u3041", TC_SYM_FUNCTION,
         "int64_t(void); -1 before the simulation has started"},
        {"sim.settings", "simulation_settings__modelZsimulator95types_u83", TC_SYM_DATA,
         "address of a void* global; dereference only when non-null (the shared settings block)"},
        {"sim.setting.get", "get_command_setting__modelZsimulator95types_u124", TC_SYM_FUNCTION,
         "int64_t(uint8_t); used by the cycle-guard selftest for the command delay setting"},
        {"sim.setting.set", "set_command_setting__modelZsimulator95types_u131", TC_SYM_FUNCTION,
         "void(uint8_t, int64_t)"},
        {"sim.state.read", "sim_state_read_u64__modelZsimulator95types_u159", TC_SYM_FUNCTION,
         "reads simulation state bits; the waveform probe uses it with a wire's offset/width"},
        {"level.load", "load_level__modelZutilities_u7740", TC_SYM_FUNCTION,
         "void(void* boardModel, const TCNimString* name); first argument is the board model"},
        {"level.loaded", "loaded_level__modelZmodel95types_u840", TC_SYM_FUNCTION,
         "the level object the game considers loaded; the board-panel driver compares it across scenes"},
        {"scene.change", "change_scene__presenterZcontext_u2958", TC_SYM_FUNCTION,
         "scene switch; used by the UI drivers to leave a board"},
        {"board.wire.update", "handle_update_wire__presenterZuser95inputZboard95ioZactionZnone_u5", TC_SYM_FUNCTION,
         "bool(void*, void*, void*, uint32_t point, uint8_t colour); the board's per-frame wire handling"},
        {"save.count", "save_count__modelZsave_u11", TC_SYM_DATA,
         "const int64_t*; how many times the game has saved"},
        /* The schematic writer.  The game calls it with the target path right
           before it serializes the board; the loader hooks it so definitions hear
           TC_LOGIC_SAVE before the file is written.  Evidence:
           tests/component-storage-playtest.ps1 (the probe calls it to save) and
           tests/component-placeholder-playtest.ps1 (a save made with a Mod
           missing rewrites the file). */
        {"save.schematic", "save_this_schematic__modelZboardZschematics_u132", TC_SYM_FUNCTION,
         "void(const TCNimString* path, void* model, void* modelField, void* board, uint64_t setting)"},
        {"save.level", "save_level_data__modelZutilities_u5683", TC_SYM_FUNCTION,
         "void(void); the game writes the level save - the save event is derived from this call"},
        {"board.undo", "undo_board__presenterZutilitiesZhelper95functions_u8367", TC_SYM_FUNCTION,
         "uint8_t(void* board); game-native undo using the Board model at rcx (verified by disassembly)"},
        /* The bottom drawer that describes the *selected* component - the panel
           a player edits a Constant's label and value in.  Its fourth argument
           is the component index the drawer is showing (measured by the
           punch-tape example, which hooks the same function).  The loader hooks
           it so a Mod can put its own editor rows inside that panel
           (TC_UI_SLOT_BOARD_COMPONENT_PANEL). */
        {"board.component_panel",
         "build_component_description_panel__presenterZboard95uiZbottom95panelZcomponent95description_u1227",
         TC_SYM_FUNCTION,
         "void(void*, void*, void*, const int64_t* componentIndex, ...); the selected component's drawer"},
        {"board.world_to_screen", "world_pos_to_screen_pos__presenterZrendererZshaderZubo95view95model_u1155",
         TC_SYM_FUNCTION,
         "Vec2(Vec2 world); reads the camera from the global ubo_view_model and returns *normalised* "
         "screen coordinates.  Evidence: the game's own draw_simple_rect (VA 0x1403404f0) takes the "
         "result and multiplies it by ImGuiIO.DisplaySize (io+8) before ImDrawList_AddRect, which is "
         "how the board's drag-selection rectangles are placed"},
        {"options.general", "build_general_options__presenterZmain95menu95uiZoptionsZgeneral95tab_u339",
         TC_SYM_FUNCTION,
         "void(void* presenter); the Options page's General tab - the game's settings panel.  A Mod "
         "hooks it, calls the original and appends its own row"},
        {"board.redo", "redo_board__presenterZutilitiesZhelper95functions_u8374", TC_SYM_FUNCTION,
         "uint8_t(void* board); game-native redo using the Board model at rcx (verified by disassembly)"},
        /* The presenter's state upgrade.  The game's own component placement
           calls this with `context + 0x1a3b8` and 0x30 right after add_component
           succeeds (disassembly of handle_no_action_yet+0xf77 and of the
           component menu), and without that call a component added through the
           command bus is not registered in the board's hit state: it cannot be
           clicked or dragged until something else refreshes the board.
           Evidence: 2026-09-22, the user placed one component by hand and the
           three command-bus-placed ones became draggable with it. */
        {"board.after_place", "upgrade__presenterZcontext_u2766", TC_SYM_FUNCTION,
         "void(void* presenterSlot, uint8_t target); presenterSlot is the change_scene context + 0x1a3b8"},
        {"save.path.level", "__emutls_v.global_save_level_path__modelZmodel95types_u79", TC_SYM_DATA,
         "emutls control block; read through tc::TCSaveModel, not directly"},
        {"save.path.schematic", "__emutls_v.global_save_schematic_path__modelZmodel95types_u81", TC_SYM_DATA,
         "emutls control block for the schematic path"},
        {"runtime.emutls", "__emutls_get_address", TC_SYM_FUNCTION,
         "void*(void* control); resolves an emutls slot for the current thread"},
        {"ui.fonts", "defined_fonts__presenterZimguiZimgui_u7413", TC_SYM_DATA,
         "the game's font table; entry 2 is the body-text face the loader borrows for plugin tools"},
        {"ui.pushFont", "igPushFont__presenterZimguiZimgui_u7614", TC_SYM_FUNCTION,
         "void(unsigned char index); the game's own font push, used with ui.fonts"},
        {"ui.invisibleButton", "igInvisibleButton", TC_SYM_FUNCTION,
         "engine import thunk in the EXE; the UI drivers hook it to find out where the game is"},
        {"cost.gate", "get_gate_cost__modelZscores_u2560", TC_SYM_FUNCTION,
         "gate cost of a component kind"},
        {"cost.total", "get_cost__modelZscores_u2321", TC_SYM_FUNCTION,
         "total cost of a component kind"},
        {"cost.delay", "get_delay_cost__modelZscores_u2316", TC_SYM_FUNCTION,
         "delay cost of a component kind"},
        /* IO values.  The three global-input calls are what the game's own IO
           panel runs (its bit squares flip, its value field writes); the
           constant pair is the setting write plus the refresh a value edit
           performs.  TC_SERVICE_IO_VALUE wraps them for plugins. */
        {"io.input.get", "get_component_global_input__presenterZutilitiesZhelper95functions_u9752", TC_SYM_FUNCTION,
         "int64_t(void* board, int64_t component)"},
        {"io.input.set", "set_component_global_input__presenterZutilitiesZhelper95functions_u5857", TC_SYM_FUNCTION,
         "void(void* board, int64_t component, int64_t value); the native value field's write"},
        {"io.input.flip", "flip_component_global_input__presenterZutilitiesZhelper95functions_u5846", TC_SYM_FUNCTION,
         "void(void* board, int64_t component, int64_t bit); one native bit square's click"},
        {"io.constant.set", "set_setting__presenterZutilitiesZhelper95functions_u2763", TC_SYM_FUNCTION,
         "void(void* board, int64_t setting, int64_t component, int64_t value); setting 0 is a constant's value"},
        {"io.constant.refresh", "sim_stop_and_refresh__modelZsimulationZcompile95thread_u3043", TC_SYM_FUNCTION,
         "void(void* board); refreshes the running simulation in place"},
        /* The custom-tail key/value table.  A 0x4e component record owns one
           `Table[int64, int64]` at +0x190 and the game serializes it inside the
           schematic, which is where a native component's configuration lives.
           The setter is the same function the game's own custom-tail
           deserializer calls while it rebuilds a record from a saved schematic
           (get_component__modelZsave95mongerZversionsZv7_u5+0x4cb), so the
           call shape is (table*, key:i64, value:i64).  Evidence:
           tests/component-persistence-playtest.ps1 -Mode insert inserts into an
           empty table through it, grows the table and reads the value back
           after a save and restart. */
        {"save.custom_tail_set", "X5BX5Deq___modelZsave95mongerZversionsZv7_u70", TC_SYM_FUNCTION,
         "void(void* table, int64_t key, int64_t value); the custom component tail table's own `[]=`"},
        /* The engine's cursor placement.  The game's own panels lay their lists
           out with these two (the left IO panel positions an entry's label with
           igSetCursorPos and the line after it with igSetCursorPosY), so both
           are hook chain points: a Mod draws inside such a panel from the chain
           callback, and a Mod that makes an entry taller moves the lines below
           it by changing the y the game is about to use. */
        {"ig.set_cursor_pos", "igSetCursorPos", TC_SYM_FUNCTION,
         "engine import thunk in the EXE; void(ImVec2 x, y)"},
        {"ig.set_cursor_pos_y", "igSetCursorPosY", TC_SYM_FUNCTION,
         "engine import thunk in the EXE; void(float y)"},
    };
    return value;
}

inline const SymbolEntry* symbol_entry(const char* alias) {
    if (!alias) return nullptr;
    for (const auto& entry : symbol_profile())
        if (entry.alias == std::string(alias)) return &entry;
    return nullptr;
}

}  // namespace tc
