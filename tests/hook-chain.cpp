/* Offline test for the loader-owned hook chains and the symbol profile.

   It plays the part of the game: the fake sim_do / sim_get_cycle / settings
   symbols carry the same COFF names as the real build, so tc::Symbols resolves
   them and the loader can install a real (MinHook) chain.  The probe packages in
   <own folder>/mods join the chain; the host asserts the order the links ran in,
   the arguments they saw, that a skipping link keeps the game's function from
   running, and that a raw hook on a chain point is refused.

   Usage: hook-chain.exe <engine.dll> [chain|skip|raw] */
#include "../src/native.hpp"
#include <iostream>
#include <string>
#include <vector>

namespace {

std::vector<std::string> notes;
int64_t observed = -999;
uint8_t observedCommand = 255;
int calls = 0;
int64_t fakeSettings[32]{};
/* Distinct counters keep the linker from folding these tiny fakes into one
   address, which would make the loader see them as the same hook target. */
int64_t levelLoads = 0;
int64_t sceneChanges = 0;
int64_t saveCalls = 0;

}  // namespace

/* The host's own note channel: the probes find it with resolve_symbol and call
   it, which is how the test sees the order the links ran in. */
extern "C" void tc_test_note(const char* text) {
    if (text) notes.push_back(text);
}

/* Fake game functions, labelled with the real build's COFF names. */
extern "C" void* simulationSettings asm("simulation_settings__modelZsimulator95types_u83");
void* simulationSettings = fakeSettings;
extern "C" __attribute__((noinline)) void fakeSim(void*, uint8_t, int64_t)
    asm("sim_do__modelZsimulationZcompile95thread_u3036");
void fakeSim(void*, uint8_t command, int64_t target) {
    observed = target;
    observedCommand = command;
    ++calls;
}
extern "C" __attribute__((noinline)) int64_t fakeCycle()
    asm("sim_get_cycle__modelZsimulationZcompile95thread_u3041");
int64_t fakeCycle() { return 100; }
extern "C" __attribute__((noinline)) void fakeLevelLoad(void*, const void*)
    asm("load_level__modelZutilities_u7740");
void fakeLevelLoad(void*, const void*) { ++levelLoads; }
extern "C" int64_t tcSaveCount asm("save_count__modelZsave_u11");
int64_t tcSaveCount = 0;
extern "C" __attribute__((noinline)) void fakeSave()
    asm("save_level_data__modelZutilities_u5683");
void fakeSave() { ++saveCalls; }
extern "C" __attribute__((noinline)) void fakeSceneChange(void*, int)
    asm("change_scene__presenterZcontext_u2958");
void fakeSceneChange(void*, int) { ++sceneChanges; }

namespace {

bool sawNote(const std::string& needle) {
    for (const auto& note : notes)
        if (note.find(needle) != std::string::npos) return true;
    return false;
}

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

/* Index of the first note containing `needle`, or -1. */
int indexOfNote(const std::string& needle) {
    for (size_t i = 0; i < notes.size(); ++i)
        if (notes[i].find(needle) != std::string::npos) return static_cast<int>(i);
    return -1;
}

void printNotes() {
    for (const auto& note : notes) std::cout << "NOTE " << note << "\n";
}

/* The order test: two links from one package (priorities 0 and 10) plus one link
   from a second package (priority 0).  Priority wins, then mod id, so the order
   must be a0, b0, a10 whatever order the packages were enabled in - and every
   link adds 1 to the target before the game's own function sees it. */
void runChain() {
    using Fn = void (*)(void*, uint8_t, int64_t);
    Fn call = fakeSim;
    call(nullptr, 0, 1000);
    printNotes();
    require(sawNote("alias sim.do=1 sim.cycle=1 bogus=0"), "the symbol profile did not resolve");
    const int a0 = indexOfNote("link a0");
    const int b0 = indexOfNote("link b0");
    const int a10 = indexOfNote("link a10");
    require(a0 >= 0 && b0 >= 0 && a10 >= 0, "not every link ran");
    require(a0 < b0 && b0 < a10, "chain order is not (priority, mod id)");
    require(sawNote("link a0 target=1000"), "the link did not see the game's own argument");
    require(sawNote("link a10 target=1002"), "an edit from an earlier link did not reach the next one");
    require(observed == 1003 && observedCommand == 0 && calls == 1,
            "the game's own function did not run exactly once with the edited argument");
    std::cout << "PASS hook chain: three links ran in priority/mods order, edits reached the game "
                 "function, and it still ran once\n";
}

/* A link that swallows the call stops the chain and keeps the game's function
   from running: mods replace behaviour through the same mechanism. */
void runSkip() {
    using Fn = void (*)(void*, uint8_t, int64_t);
    Fn call = fakeSim;
    call(nullptr, 0, 7);
    printNotes();
    require(sawNote("skip swallow=1"), "the skipping link did not run");
    require(!sawNote("skip later-link-ran"), "a later link ran after the chain was stopped");
    require(calls == 0 && observed == -999, "the game's own function ran even though the link swallowed it");
    std::cout << "PASS hook chain: a skipping link stopped the chain and suppressed the game function\n";
}

/* A raw hook on a chain point is refused with a message that says what to use
   instead; a raw hook on any other EXE symbol still works. */
void runRaw() {
    printNotes();
    require(sawNote("raw plain-hook ok=1"),
            "a raw hook on a plain target was refused");
    require(sawNote("raw sim.do ok=0"),
            "a raw hook on a loader chain point was accepted");
    std::cout << "PASS hook chain: raw hooks still work on plain targets and are refused on chain points\n";
}

/* The event bus: the loader watches the points its listeners asked for and
   reports them as typed events, so a plugin never has to hook them itself. */
void runEvents() {
    printNotes();
    require(sawNote("events subscribed=ok"), "the event subscription was refused");
    /* Drive the fake game: every one of these is a point the loader armed. */
    fakeSim(nullptr, 0, 5);
    fakeLevelLoad(nullptr, nullptr);
    /* A non-null context on purpose: the scene-change event has to carry it
       through, because that is how a mod (or the board-panel driver) gets the
       value it must hand back to change_scene. */
    fakeSceneChange(&sceneChanges, 1);
    tcSaveCount = 3;
    fakeSave();
    printNotes();
    const int command = indexOfNote("event sim.command command=0");
    const int load = indexOfNote("event level.load subject=0");
    const int scene = indexOfNote("event scene.change");
    const int save = indexOfNote("event save count=3");
    require(command >= 0, "the simulation command event never arrived");
    require(load >= 0, "the level load event never arrived");
    require(scene >= 0, "the scene change event never arrived");
    require(sawNote("event scene.change scene=1 subject=1"),
            "the scene change event did not carry the change_scene context");
    require(save >= 0, "the save event never arrived");
    require(command < load || command < scene, "event order does not follow the calls");
    /* The events must not disturb the game's own functions. */
    require(observed == 5 && observedCommand == 0,
            "the event listener changed what the simulation command did");
    std::cout << "PASS event bus: sim command, level load, scene change and save reached one "
                 "listener, in order, without disturbing the game\n";
}

/* The fault journal: a plugin link that dereferences a bad pointer kills the
   process (containment is not offered - see src/fault_guard.hpp), and the journal
   must name the plugin, the hook point and the reason.  This function therefore
   does not return; tests/hook-chain.ps1 checks fault.log and the exit code. */
void runCrash() {
    std::cout << "CRASH: calling the faulting link on purpose; fault.log must name it\n";
    notes.clear();
    using Fn = void (*)(void*, uint8_t, int64_t);
    Fn call = fakeSim;
    call(nullptr, 0, 42);
    printNotes();
    std::cout << "NOTE: the faulting link returned - containment is active, re-evaluate the docs\n";
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        /* Unbuffered: this test deliberately makes a plugin fault, and a harness
           that loses every line it printed before the fault cannot say where the
           fault happened. */
        std::cout << std::unitbuf;
        if (argc < 2) return 1;
        const std::string mode = argc >= 3 ? tc::fs::path(argv[2]).string() : "chain";
        wchar_t own[32768];
        GetModuleFileNameW(nullptr, own, 32768);
        tc::Core core(tc::fs::path(own).parent_path());
        core.scan();
        std::set<std::string> selected;
        for (auto& mod : core.mods) {
            if (!mod.error.empty()) continue;
            selected.insert(mod.id);
            std::cout << "MOD " << mod.id << " " << mod.version << "\n";
        }
        core.apply(selected);
        HMODULE engine = LoadLibraryExW(argv[1], nullptr,
                                        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                            LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!engine) throw std::runtime_error("test engine unavailable");
        /* Every loader line also lands in host.log: the crash scenario makes a
           plugin fault on purpose, and an abnormal exit must not hide the lines
           that say how far the loader got. */
        tc::NativeRuntime runtime(core, engine, [](const std::string& line) {
            std::cout << line << "\n";
            std::ofstream log("host.log", std::ios::app);
            log << line << "\n";
        });
        runtime.boot();
        if (mode == "skip") runSkip();
        else if (mode == "raw") runRaw();
        else if (mode == "events") runEvents();
        else if (mode == "crash") runCrash();
        else runChain();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
        return 1;
    }
}
