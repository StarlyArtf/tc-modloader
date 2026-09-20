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
        {"save.level", "save_level_data__modelZutilities_u5683", TC_SYM_FUNCTION,
         "void(void); the game writes the level save - the save event is derived from this call"},
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
