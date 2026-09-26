/* Offline cases for the gate-delay source transform (src/gate_delay.hpp).

   The transform is judged by the game's own compiler on the real machine, so
   these cases pin down what can be checked without one: the emitted program is
   still one lexical shape (braces stay balanced, nothing is lost), each cycle
   body became one unit loop, no `store(#SIMULATION_STATE ...)` survives inside a
   loop, every component's value is published through the host, the emitter's
   `LATE` commit and the bridge calls run once per cycle, and the bridge's own
   mailbox slots stay direct.

   `--rewrite in out` writes the transformed program for eyeballing against a
   real dump; `--survey` reports on a dump without rewriting it. */

#include "../src/gate_delay.hpp"
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool ok, const std::string& message) {
    if (!ok) throw std::runtime_error(message);
}

size_t count(const std::string& text, const std::string& needle) {
    size_t hits = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size()))
        ++hits;
    return hits;
}

std::string trim(const std::string& line) {
    const size_t begin = line.find_first_not_of(" \t\r");
    if (begin == std::string::npos) return std::string();
    const size_t end = line.find_last_not_of(" \t\r");
    return line.substr(begin, end - begin + 1);
}

/* A direct `store(#SIMULATION_STATE ...)` is allowed only for the emitter's LATE
   commit, which the rewrite leaves inside the unit loop but under the
   `if tc_unit == 0 {` guard so it still happens once per cycle. */
bool guardedStoresOnly(const std::string& text, std::string& offender, size_t& guardedStores) {
    std::istringstream in(text);
    std::string line;
    bool guarded = false;
    guardedStores = 0;
    while (std::getline(in, line)) {
        const std::string text = trim(line);
        if (text.find("tc_delay_cycle_start") != std::string::npos) guarded = true;
        if (text.find("store(#SIMULATION_STATE + ") != std::string::npos) {
            if (!guarded) { offender = text; return false; }
            ++guardedStores;
        }
        if (text == "}") guarded = false;
    }
    return true;
}

/* Host calls (the native-logic bridge, the scope tick) must also be guarded, so
   a callback runs once per cycle and not once per unit.  The mailbox reads that
   assemble a callback's result are direct by design and are not calls. */
std::vector<std::string> unitLoops(const std::string& text);

bool hostCallsGuardedInsideLoops(const std::string& text, std::string& offender) {
    for (const std::string& loop : unitLoops(text)) {
        std::istringstream in(loop);
        std::string line;
        bool guarded = false;
        while (std::getline(in, line)) {
            const std::string one = trim(line);
            if (one.find("tc_delay_cycle_start") != std::string::npos) guarded = true;
            /* tc_delay_* is the mode's own machinery: once per unit by design. */
            if (!guarded && one.find("game_engine.'tc_") != std::string::npos &&
                one.find("'tc_delay_") == std::string::npos) {
                offender = one;
                return false;
            }
            if (one == "}") guarded = false;
        }
    }
    return true;
}

/* The rewrite adds loops and guards, so what has to hold is the *difference*:
   the program must still close every brace it opens. */
long balance(const std::string& text) {
    return static_cast<long>(count(text, "{")) - static_cast<long>(count(text, "}"));
}

/* The shape the emitter produces, reduced to the parts the transform has to
   survive: a state-slot refresh body, and a run body whose combinational values
   travel in local variables, with a LATE commit, a bridge call, a prelude and a
   tail around them. */
const char* kSample = R"SRC(extern windows_x64 game_engine
type ComponentType Enum[com_off, com_nand_bit, com_custom]
const #SIMULATION_STATE               = Ptr 2328140120064
var settings                          = Ptr 2328106696704

def set_setting(idx: StateIndex, value: U64) None {
    store(settings + (Int idx) * 8, value)
}

def reset_sim() None {
    memory_clear(#SIMULATION_STATE, 257)
}

def mode_refresh() None {

    // 5 com_nand_bit 2 3 
    let value_id256 = ~((U1 0x1) & (U1 0x0))
    store(#SIMULATION_STATE + 256, U1 (value_id256))
    store(#SIMULATION_STATE + 257, U1 (value_id256))

    // 6 com_nand_bit 2 9 
    let value_id300 = ~((U1 (load(<U1>, #SIMULATION_STATE + 256))) & (U1 0x1))
    store(#SIMULATION_STATE + 300, U1 (value_id300))
    store(#SIMULATION_STATE + 301, U1 (value_id300))

}

def mode_run(target_cycle: Int) None {

    var halt_run = false
    while get_command(ctl_command_id) == last_command_id && !halt_run && target_cycle > cycle {

        while cycle < burst_target_cycle {

            if in_scope("get_input") {
                level_input = get_input(cycle + 1)
            }

            // 5 com_nand_bit 2 3 
            var vid256 = U1 ~((U1 0x1) & (U1 0x0))

            // 6 com_nand_bit 2 9 
            var vid300 = U1 ~((U1 vid256) & (U1 0x1))

            // 8 com_register_word 32 LATE 11
            if (U1 vid290) == 1 {
                store(#SIMULATION_STATE + 1369, U32 ((U32 vid256)))
            }

            // 9 com_custom 1 13
            var tc_io1 = U1 game_engine.'tc_logic_invoke'(U64 1, U64 (cycle + 1), U64 (vid256), U64 0)
            var vid306 = U1 (load(<U1>, #SIMULATION_STATE + 10092544))

            // 10 com_add 32 15
            let sum_10 = ((U33 (U32 0x0) & 1) + U33 (U32 vid256))
            var vid308 = U32 sum_10
            var vid1368 = U1 (sum_10 >> 32)

            if in_scope("check_output") {
                let result = check_output(cycle + 1, level_input, level_output)
                handle_test_result(result)
            }

            game_engine.'tc_scope_tick'(U64 cycle)
            cycle += 1 // Do this late as it signals to the front end that it can update

        }

        set_setting(sim_cycle, U64 cycle)

    }

}

var manual_input_seen = false

run_sim: while true {
    reset_sim()
    while true {
        mode_refresh()
    }
}
)SRC";

/* A cross-coupled NAND latch the way it has to reach the compiler: one of the two
   feedback wires is cut, and the input it used to drive is held by a placeholder
   constant (component 1001) instead, so the board is acyclic.  Component 1002 is
   the NAND whose input gets re-closed onto component 1003 - the other NAND's
   output, which is state slot 304. */
const char* kRingSample = R"SRC(extern windows_x64 game_engine
type ComponentType Enum[com_off, com_nand_bit, com_constant]
const #SIMULATION_STATE               = Ptr 2328140120064

def reset_sim() None {
    memory_clear(#SIMULATION_STATE, 4096)
}

def mode_refresh() None {

    // 5 com_constant 1 1001 
    let value_id256 = 1
    store(#SIMULATION_STATE + 256, U1 (value_id256))
    store(#SIMULATION_STATE + 257, U1 (value_id256))

    // 6 com_constant 1 1004 S
    let value_id320 = 0
    store(#SIMULATION_STATE + 320, U1 (value_id320))
    store(#SIMULATION_STATE + 321, U1 (value_id320))

    // 7 com_nand_bit 2 1002 
    let value_id300 = ~((U1 (load(<U1>, #SIMULATION_STATE + 256))) & (U1 (load(<U1>, #SIMULATION_STATE + 320))))
    store(#SIMULATION_STATE + 300, U1 (value_id300))
    store(#SIMULATION_STATE + 301, U1 (value_id300))

    // 8 com_constant 1 1005 R
    let value_id322 = 1
    store(#SIMULATION_STATE + 322, U1 (value_id322))
    store(#SIMULATION_STATE + 323, U1 (value_id322))

    // 9 com_nand_bit 2 1003 
    let value_id304 = ~((U1 (load(<U1>, #SIMULATION_STATE + 300))) & (U1 (load(<U1>, #SIMULATION_STATE + 322))))
    store(#SIMULATION_STATE + 304, U1 (value_id304))
    store(#SIMULATION_STATE + 305, U1 (value_id304))

}

def mode_run(target_cycle: Int) None {

    while cycle < burst_target_cycle {

        // 5 com_constant 1 1001 
        var vid256 = U1 1

        // 6 com_constant 1 1004 S
        var vid320 = U1 0

        // 7 com_nand_bit 2 1002 
        var vid300 = U1 ~((U1 vid256) & (U1 vid320))

        // 8 com_constant 1 1005 R
        var vid322 = U1 1

        // 9 com_nand_bit 2 1003 
        var vid304 = U1 ~((U1 vid300) & (U1 vid322))

        cycle += 1 // Do this late as it signals to the front end that it can update

    }

}
)SRC";

/* Where the unit loops live, so "inside the loop" can be tested rather than
   guessed: everything between `while tc_unit > 0 {` and its closing brace. */
std::vector<std::string> unitLoops(const std::string& text) {
    std::vector<std::string> loops;
    size_t at = 0;
    while ((at = text.find("while tc_unit > 0 {", at)) != std::string::npos) {
        size_t start = at;
        int depth = 0;
        size_t end = start;
        for (size_t i = at; i < text.size(); ++i) {
            if (text[i] == '{') ++depth;
            else if (text[i] == '}') {
                if (--depth == 0) { end = i; break; }
            }
        }
        loops.push_back(text.substr(start, end + 1 - start));
        at = end + 1;
    }
    return loops;
}

/* A board the loader cut by itself: the wire is gone from the model, so the
   consumer's input reaches the emitter with no driver and comes out folded to a
   constant - the shape every dump of a dangling input has.  This is the ring
   sample with the placeholder constant and its two wires removed, and with one
   operand of the first NAND folded to 0x0. */
const char* kCutSample = R"SRC(extern windows_x64 game_engine
const #SIMULATION_STATE               = Ptr 2328140120064

def reset_sim() None {
    memory_clear(#SIMULATION_STATE, 4096)
}

def mode_refresh() None {

    // 6 com_constant 1 1004 S
    let value_id320 = 1
    store(#SIMULATION_STATE + 320, U1 (value_id320))
    store(#SIMULATION_STATE + 321, U1 (value_id320))

    // 7 com_nand_bit 2 1002 
    let value_id300 = ~((U1 0x0) & (U1 (load(<U1>, #SIMULATION_STATE + 320))))
    store(#SIMULATION_STATE + 300, U1 (value_id300))
    store(#SIMULATION_STATE + 301, U1 (value_id300))

    // 8 com_constant 1 1005 R
    let value_id322 = 1
    store(#SIMULATION_STATE + 322, U1 (value_id322))
    store(#SIMULATION_STATE + 323, U1 (value_id322))

    // 9 com_nand_bit 2 1003 
    let value_id304 = ~((U1 (load(<U1>, #SIMULATION_STATE + 300))) & (U1 (load(<U1>, #SIMULATION_STATE + 322))))
    store(#SIMULATION_STATE + 304, U1 (value_id304))
    store(#SIMULATION_STATE + 305, U1 (value_id304))

}

def mode_run(target_cycle: Int) None {

    while cycle < burst_target_cycle {

        // 6 com_constant 1 1004 S
        var vid320 = U1 1

        // 7 com_nand_bit 2 1002 
        var vid300 = U1 ~((U1 0x0) & (U1 vid320))

        // 8 com_constant 1 1005 R
        var vid322 = U1 1

        // 9 com_nand_bit 2 1003 
        var vid304 = U1 ~((U1 vid300) & (U1 vid322))

        cycle += 1 // Do this late as it signals to the front end that it can update

    }

}
)SRC";

void checkSample() {
    tc::gate_delay::Runtime& stats = tc::gate_delay::store();
    stats.log = nullptr;
    std::string text = kSample;
    const long balanceBefore = balance(text);
    const size_t stepsBefore = count(text, "cycle += 1");
    const size_t storesBefore = count(text, "store(#SIMULATION_STATE + ");
    require(tc::gate_delay::rewrite(text, 8), "the sample program was not rewritten");

    const std::vector<std::string> loops = unitLoops(text);
    require(loops.size() == 2, "expected one unit loop per cycle body, got " + std::to_string(loops.size()));
    require(count(text, "tc_delay_begin") == 2, "each cycle body must open one unit");
    require(text.find("game_engine.'tc_delay_begin'(U64 0)") != std::string::npos &&
                text.find("game_engine.'tc_delay_begin'(U64 1)") != std::string::npos,
            "the two cycle bodies must be told apart when they report their units");
    require(text.find("game_engine.'tc_delay_units_this_pass'(U64 0)") != std::string::npos &&
                text.find("game_engine.'tc_delay_units_this_pass'(U64 1)") != std::string::npos,
            "the two cycle bodies must request a site-specific unit budget");
    require(count(text, "cycle += 1") == stepsBefore, "the cycle step changed");
    require(balance(text) == balanceBefore,
            "the rewrite changed the program's brace balance: " + std::to_string(balanceBefore) + " -> " +
                std::to_string(balance(text)));

    const size_t publishes = count(text, "tc_delay_write");
    require(publishes >= 6, "every node's value must be published, found " + std::to_string(publishes));
    require(count(text, "tc_delay_read") >= 3, "cross-component reads must go through the snapshot");
    /* The LATE commit, the bridge call, and the loader's per-cycle scope tick
       that the emitter's tail carries. */
    require(count(text, "tc_delay_cycle_start") == 3,
            "the LATE commit, the bridge call and the tail's host call must each run once per cycle: " +
                std::to_string(count(text, "tc_delay_cycle_start")));
    std::string unguarded;
    const bool callsGuarded = hostCallsGuardedInsideLoops(text, unguarded);
    require(callsGuarded, "a host call was left unguarded inside a unit loop: " + unguarded);
    require(count(text, "game_engine.'tc_scope_tick'") == 1,
            "the loader's per-cycle scope tick must survive the rewrite exactly once");

    std::string offender;
    size_t guardedStores = 0;
    /* Two statements: argument evaluation order is unspecified, and the message
       needs what the call fills in. */
    const bool guardedOnly = guardedStoresOnly(text, offender, guardedStores);
    require(guardedOnly, "a direct state store escaped the cycle boundary: " + offender);
    require(guardedStores == 1, "the LATE commit is the only direct store, found " +
                                    std::to_string(guardedStores));
    for (const std::string& loop : loops) {
        require(loop.find("#SIMULATION_STATE") == std::string::npos ||
                    loop.find("#SIMULATION_STATE + 10092544") != std::string::npos,
                "the bridge mailbox should be the only direct state access inside a loop");
    }
    require(storesBefore == 5, "the sample should start with five component stores");

    /* The run body's wave front must now cross units: node 5 reads node 6's
       committed slot, not the same unit's local. */
    require(text.find("var vid300 = U1 ~((U1 ((U1 game_engine.'tc_delay_read'(U64 256, U64 1))))") !=
                std::string::npos,
            "node 6 still reads node 5's local variable directly instead of its committed slot");
    require(text.find("game_engine.'tc_delay_write'(U64 256, U64 1, U64 (vid256), U64 1)") !=
                std::string::npos,
            "node 5's output is not published to its own slot");
    /* The delay value itself is the delay table's business (an add is several
       units now); what this case pins down is that the second output is
       published at all. */
    require(text.find("game_engine.'tc_delay_write'(U64 1368, U64 1, U64 (vid1368), U64 ") !=
                std::string::npos,
            "the second output of a multi-output component is not published");
    /* The adder's own delay is the delay table's business; what this pins down
       is that its 32-bit output is published as a 32-bit slot. */
    require(text.find("game_engine.'tc_delay_write'(U64 308, U64 32, U64 (vid308), U64 ") !=
                std::string::npos,
            "the adder's 32-bit output is not published as a 32-bit slot");
}

/* One of the real dumps, if the caller points at one: the transform must survive
   the shapes a whole board produces (hundreds of blocks, RAM, ports, probes). */
void checkDump(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + path);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    std::string text = buffer.str();
    if (text.find("def mode_refresh() None {") == std::string::npos) {
        std::cout << "SKIP " << path << ": not a schematic program\n";
        return;
    }
    const long balanceBefore = balance(text);
    const size_t stepsBefore = count(text, "cycle += 1");
    tc::gate_delay::store().log = nullptr;
    if (!tc::gate_delay::rewrite(text, 8)) {
        /* No component blocks at all (an empty board): nothing to phase. */
        std::cout << "SKIP " << path << ": no component blocks\n";
        return;
    }
    const std::vector<std::string> loops = unitLoops(text);
    require(loops.size() == 2, path + ": expected two unit loops");
    require(balance(text) == balanceBefore, path + ": brace balance changed");
    require(count(text, "cycle += 1") == stepsBefore, path + ": the cycle step changed");
    std::string offender;
    size_t guardedStores = 0;
    const bool guardedOnly = guardedStoresOnly(text, offender, guardedStores);
    require(guardedOnly, path + ": a direct state store escaped the cycle boundary: " + offender);
    std::string unguarded;
    const bool callsGuarded = hostCallsGuardedInsideLoops(text, unguarded);
    require(callsGuarded, path + ": a host call was left unguarded inside a unit loop: " + unguarded);
    std::cout << "PASS dump " << path << ": publishes=" << count(text, "tc_delay_write")
              << " reads=" << count(text, "tc_delay_read") << " guarded_stores=" << guardedStores << "\n";
}

/* The re-close path: a cut feedback wire is put back by pointing the consumer at
   the producer's slot, in both cycle bodies. */
void checkReclose() {
    tc::gate_delay::Runtime& stats = tc::gate_delay::store();
    std::vector<std::string> log;
    stats.log = [&log](const std::string& line) { log.push_back(line); };
    std::string text = kRingSample;
    tc::gate_delay::Reclose record;
    record.consumer = 1002;      /* the NAND whose feedback input was cut */
    record.placeholder = 1001;   /* the constant that stood in for the wire */
    record.producer = 1003;      /* the NAND whose output it should read */
    record.bits = 1;
    const bool rewritten = tc::gate_delay::rewrite(text, 8, std::vector<tc::gate_delay::Reclose>{record});
    stats.log = nullptr;
    require(rewritten, "the ring sample was not rewritten");

    require(count(text, "tc_delay_read'(U64 304, U64 1)") >= 2,
            "both cycle bodies must read the producer's slot once the wire is back");
    require(count(text, "tc_delay_read'(U64 256, U64 1)") == 0,
            "the placeholder's slot is still being read after the re-close");
    require(text.find("load(<U1>, #SIMULATION_STATE + 256)") == std::string::npos,
            "a direct read of the placeholder's slot survived the re-close");
    require(text.find("tc_delay_read'(U64 300, U64 1)") != std::string::npos,
            "the other direction of the loop was lost");
    bool reported = false;
    for (const std::string& line : log) {
        require(line.find("unsupported") == std::string::npos, "the re-close reported a problem: " + line);
        if (line.find("re-closed") != std::string::npos) reported = true;
    }
    require(reported, "the re-close was not reported");
    require(unitLoops(text).size() == 2, "the re-closed sample did not become two unit loops");
    std::cout << "PASS gate delay: the cut feedback wire is re-closed through the producer's slot\n";
}

/* M1b: the event-scheduled pass evaluates a component only when one of its
   inputs moved.  What has to hold in the text: every block that reads something
   is behind a `tc_delay_dirty` guard, that guard also contains the block's own
   publish, and a source block (a constant) is left unconditional - it is the one
   that publishes the value everything else goes dirty from. */
void checkEventSchedule() {
    tc::gate_delay::Runtime& stats = tc::gate_delay::store();
    stats.log = nullptr;
    _putenv_s("TC_GATE_DELAY_SCHED", "event");
    /* The ring is the better witness: each gate reads two slots, so its guard
       has to name both of them. */
    std::string text = kRingSample;
    const bool rewritten = tc::gate_delay::rewrite(text, 8);
    _putenv_s("TC_GATE_DELAY_SCHED", "");
    require(rewritten, "the sample program was not rewritten in the event pass");

    const size_t guards = count(text, "game_engine.'tc_delay_dirty'(U64 ");
    /* Two gates, two bodies, two inputs each. */
    require(guards == 8, "expected two gates x two bodies x two inputs of guards, got " +
                             std::to_string(guards));
    require(count(text, "sched=") == 0, "the schedule mode leaked into the program text");
    /* The constants have no inputs, so they stay unconditional and keep
       publishing - that is what goes dirty for everyone else. */
    require(text.find("if ((U64 game_engine.'tc_delay_dirty'(U64 256)) != 0 || "
                      "(U64 game_engine.'tc_delay_dirty'(U64 320)) != 0) {") != std::string::npos,
            "the first gate is not guarded by both of its input slots");
    require(text.find("if ((U64 game_engine.'tc_delay_dirty'(U64 300)) != 0 || "
                      "(U64 game_engine.'tc_delay_dirty'(U64 322)) != 0) {") != std::string::npos,
            "the second gate is not guarded by both of its input slots");
    const std::vector<std::string> loops = unitLoops(text);
    require(loops.size() == 2, "the event pass lost a unit loop");
    for (const std::string& loop : loops) {
        /* Every guard the pass emits ends with the publish of the block it
           protects: a guarded block that computed but never published would
           never wake anyone. */
        size_t guardCount = 0, publishInside = 0;
        bool inGuard = false;
        std::istringstream stream(loop);
        std::string line;
        while (std::getline(stream, line)) {
            const std::string text = trim(line);
            if (text.rfind("if ((U64 game_engine.'tc_delay_dirty'", 0) == 0) {
                ++guardCount;
                inGuard = true;
                continue;
            }
            if (!inGuard) continue;
            if (text == "}") { inGuard = false; continue; }
            if (text.find("tc_delay_write") != std::string::npos) ++publishInside;
        }
        /* Each guarded block publishes inside its guard (one guard may publish
           to more than one slot, which is why this is a lower bound). */
        require(guardCount == 2 && publishInside >= guardCount,
                "a guarded block does not publish inside its guard (" + std::to_string(publishInside) + "/" +
                    std::to_string(guardCount) + ")");
    }
    std::cout << "PASS gate delay: the event pass guards every reader with its inputs\n";
}

/* The loader-cut path: no placeholder exists, the consumer's input arrived
   folded, and the transform has to put the connection back by pointing that
   operand at the producer's slot - in both cycle bodies. */
void checkCutReclose() {
    tc::gate_delay::Runtime& stats = tc::gate_delay::store();
    std::vector<std::string> log;
    stats.log = [&log](const std::string& line) { log.push_back(line); };
    tc::gate_delay::clearCuts();
    tc::gate_delay::addCut(1002, 1003, 1);          /* the wire the loader detached */
    std::string text = kCutSample;
    const bool rewritten = tc::gate_delay::rewrite(text, 8);
    tc::gate_delay::clearCuts();
    stats.log = nullptr;
    require(rewritten, "the cut sample was not rewritten");

    /* The folded operand is gone from both bodies and replaced by a read of the
       producer's slot (304), written the way the emitter writes its own reads. */
    require(count(text, "(U1 0x0)") == 0, "a folded operand survived the re-close");
    /* Phased into the mode's own read, in both bodies. */
    require(count(text, "tc_delay_read'(U64 304, U64 1)") >= 2,
            "the consumer does not read the producer's slot in both bodies");
    require(text.find("tc_delay_read'(U64 304, U64 1)") != std::string::npos,
            "the re-closed read was not phased");
    require(unitLoops(text).size() == 2, "the cut sample lost a unit loop");
    bool reported = false;
    for (const std::string& line : log) {
        require(line.find("unsupported") == std::string::npos, "the cut re-close reported a problem: " + line);
        if (line.find("cut wire re-closed") != std::string::npos) reported = true;
    }
    require(reported, "the cut re-close was not reported");
    std::cout << "PASS gate delay: a wire the loader cut is re-closed through the producer's slot\n";
}

/* A proximity-only graph can mistake an ordinary reconvergent path for a
   feedback edge.  If neither endpoint owns the one folded input created by a
   real cut, the transform must reject the whole pass and leave the compiler's
   source untouched; compiling a partial rewrite caused an infinite retry loop
   in a real 91-component sandbox. */
void checkAmbiguousCutRejected() {
    tc::gate_delay::Runtime& stats = tc::gate_delay::store();
    std::vector<std::string> log;
    stats.log = [&log](const std::string& line) { log.push_back(line); };
    tc::gate_delay::clearCuts();
    tc::gate_delay::addCut(1002, 1003, 1);
    std::string text = kRingSample;
    const std::string original = text;
    const bool rewritten = tc::gate_delay::rewrite(text, 8);
    stats.log = nullptr;

    require(!rewritten, "an ambiguous automatic cut was accepted");
    require(text == original, "an ambiguous automatic cut changed the compiler source");
    require(tc::gate_delay::pendingCuts().empty(),
            "a rejected automatic cut remained pending for the next compile");
    require(!tc::gate_delay::cutState().active,
            "a rejected automatic cut left a wire detached");
    bool skipped = false;
    for (const std::string& line : log)
        if (line.find("rewrite skipped") != std::string::npos) skipped = true;
    require(skipped, "an ambiguous automatic cut was not reported as skipped");
    std::cout << "PASS gate delay: an ambiguous automatic cut leaves compiler source untouched\n";
}

void checkMultiDriverAccumulator() {
    std::string text = R"SRC(const #SIMULATION_STATE = Ptr 1
def mode_refresh() None {
    // 1 com_nand_bit 1 1
    let value_id256 = U1 0
    store(#SIMULATION_STATE + 256, U1 (value_id256))
    // 2 com_nand_bit 1 2
    let value_id258 = U1 1
    store(#SIMULATION_STATE + 258, U1 (value_id258))
}
def mode_run(target_cycle: Int) None {
    while cycle < burst_target_cycle {
        // 1 com_nand_bit 1 1
        var vid256 = U1 0
        // 2 com_nand_bit 1 2
        var vid258 = U1 1
        // 3 com_switch_bit 1 3
        if vid258 != 0 {
            vid256 |= U1 1
            store(#SIMULATION_STATE + 256, U1 (vid256))
        }
        cycle += 1
    }
}
)SRC";
    require(tc::gate_delay::rewrite(text, 8), "the multi-driver sample was not rewritten");
    require(text.find("vid256 |= U1 1") != std::string::npos,
            "a multi-driver accumulator stopped being a writable local");
    require(text.find("tc_delay_read'(U64 256, U64 1))) |=") == std::string::npos,
            "a delay read was emitted as the left side of a compound assignment");
    require(text.find("tc_delay_read'(U64 258, U64 1)") != std::string::npos,
            "the multi-driver block's real input did not become a snapshot read");
    std::cout << "PASS gate delay: multi-driver accumulators remain writable locals\n";
}

template <class T>
void put(std::vector<unsigned char>& bytes, size_t offset, T value) {
    require(offset + sizeof(value) <= bytes.size(), "synthetic board write is out of range");
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

/* The normalized wire copy handed to preorder already contains each full
   polyline's two ends.  Pin ownership must ignore the board's kind-0 tombstone,
   otherwise a real component at (0,0) loses the nearest-point tie and the cut
   is recorded against component id 0, which cannot be re-closed. */
void checkCycleCut() {
    using namespace tc::gate_delay;
    restoreCut();
    clearCuts();

    std::vector<unsigned char> componentPayload(
        kTableHeader + 4 * kComponentStride, 0);
    std::vector<unsigned char> wirePayload(kTableHeader + 3 * kWireStride, 0);
    auto component = [&](size_t index, uint8_t kind, int16_t x, int16_t y, uint64_t id) {
        const size_t at = kTableHeader + index * kComponentStride;
        put(componentPayload, at, kind);
        put(componentPayload, at + 2, x);
        put(componentPayload, at + 4, y);
        put(componentPayload, at + 8, id);
    };
    auto wire = [&](size_t index, int16_t x1, int16_t y1, int16_t x2, int16_t y2) {
        const size_t at = kTableHeader + index * kWireStride;
        put(wirePayload, at + 0x18, x1);
        put(wirePayload, at + 0x1a, y1);
        put(wirePayload, at + 0x1c, x2);
        put(wirePayload, at + 0x1e, y2);
        put<uint32_t>(wirePayload, at + 0x30, 1);
    };
    component(0, 0, 0, 0, 0);             /* the tombstone */
    component(1, 6, -20, 0, 1001);
    component(2, 6, 0, 0, 1002);          /* same origin as the tombstone */
    component(3, 6, 20, 0, 1003);
    wire(0, -21, -1, 22, 0);               /* A <-> C */
    wire(1, -1, -1, -18, 0);               /* B <-> A */
    wire(2, 19, -1, 2, 0);                 /* C <-> B */

    struct Sequence { uint64_t count; const unsigned char* payload; };
    Sequence components{4, componentPayload.data()};
    Sequence wires{3, wirePayload.data()};
    std::vector<std::string> log;
    const bool cut = detachCycleEdge(&components, &wires,
        [&log](const std::string& line) { log.push_back(line); });
    require(cut, "the normalized three-gate ring was not cut");
    require(cutState().active, "the ring cut was not kept until preorder returns");
    require(pendingCuts().size() == 1, "the ring cut did not produce one re-close record");
    const CutEdge edge = pendingCuts().front();
    require(edge.first != 0 && edge.second != 0,
            "the kind-0 tombstone was mistaken for the middle gate");
    require((edge.first == 1001 || edge.first == 1002 || edge.first == 1003) &&
                (edge.second == 1001 || edge.second == 1002 || edge.second == 1003),
            "the cut does not join two real ring gates");
    afterCompileCut();
    require(!cutState().active, "the temporary wire cut was not restored");
    require(pendingCuts().size() == 1,
            "restoring the temporary wire consumed the re-close record too early");
    clearCuts();

    /* The editor can briefly expose a small mixed/custom circuit while a part
       is being changed.  That shape triggered the production crash: an
       undirected proximity cycle was cut even though it was not feedback.
       Direction now comes from exact NAND/constant pins, so a foreign kind can
       break the loop without switching automatic cutting off for the board. */
    component(3, 0x4e, 20, 0, 1003);
    const bool mixedCut = detachCycleEdge(&components, &wires,
        [&log](const std::string& line) { log.push_back(line); });
    require(!mixedCut, "a mixed editor circuit was automatically cut");
    require(!cutState().active && pendingCuts().empty(),
            "skipping a mixed editor circuit left cut state behind");
    std::cout << "PASS gate delay: the normalized wire copy cuts a real gate, not the tombstone\n";
    std::cout << "PASS gate delay: a loop that runs into a foreign kind has no cuttable edge\n";
}

/* The board the two-NAND regression is built from: a player's sandbox mid-edit,
   where the latch is closed but the same board also carries unrelated kinds.
   Automatic cutting has to find the feedback edge between the two NANDs in
   spite of those parts, and it has to name the consumer's own input pin, because
   a half-built latch can have both of its inputs folded.

       A at (-3,12)   in0 (-4,11)  in1 (-4,13)  out (-1,12)
       B at (-3,19)   in0 (-4,18)  in1 (-4,20)  out (-1,19)  */
void checkTwoNandLatchCut() {
    using namespace tc::gate_delay;
    restoreCut();
    clearCuts();

    std::vector<unsigned char> componentPayload(kTableHeader + 6 * kComponentStride, 0);
    std::vector<unsigned char> wirePayload(kTableHeader + 6 * kWireStride, 0);
    auto component = [&](size_t index, uint8_t kind, int16_t x, int16_t y, uint64_t id) {
        const size_t at = kTableHeader + index * kComponentStride;
        put(componentPayload, at, kind);
        put(componentPayload, at + 2, x);
        put(componentPayload, at + 4, y);
        put(componentPayload, at + 8, id);
    };
    auto wire = [&](size_t index, int16_t x1, int16_t y1, int16_t x2, int16_t y2) {
        const size_t at = kTableHeader + index * kWireStride;
        put(wirePayload, at + 0x18, x1);
        put(wirePayload, at + 0x1a, y1);
        put(wirePayload, at + 0x1c, x2);
        put(wirePayload, at + 0x1e, y2);
        put<uint32_t>(wirePayload, at + 0x30, 1);
    };
    component(0, 0x4e, -6, 21, 5001);      /* a custom instance, the way the sandbox had one */
    component(1, 0x06, -3, 12, 2001);      /* A */
    component(2, 0x06, -3, 19, 2002);      /* B */
    component(3, 0x04, -12, 25, 5002);     /* AND, another unrelated part */
    component(4, 0x2e, -10, 13, 2003);     /* A's reset constant */
    component(5, 0x2e, -10, 20, 2004);     /* B's set constant */
    wire(0, -1, 12, -4, 18);               /* A out -> B in0 */
    wire(1, -1, 19, -4, 11);               /* B out -> A in0 */
    wire(2, -7, 13, -4, 13);               /* reset constant -> A in1 */
    wire(3, -7, 20, -4, 20);               /* set constant -> B in1 */
    wire(4, -6, 21, -3, 21);               /* touches only the custom instance */
    wire(5, -12, 25, -10, 25);             /* touches only the AND */

    struct Sequence { uint64_t count; const unsigned char* payload; };
    Sequence components{6, componentPayload.data()};
    Sequence wires{6, wirePayload.data()};
    std::vector<std::string> log;
    const bool cut = detachCycleEdge(&components, &wires,
        [&log](const std::string& line) { log.push_back(line); });
    require(cut, "the closed two-NAND latch was not cut");
    require(cutState().active, "the latch cut was not kept until preorder returns");
    require(pendingCuts().size() == 1, "the latch cut did not produce one re-close record");
    const CutEdge edge = pendingCuts().front();
    require(edge.directed, "the latch cut did not record a direction");
    require((edge.first == 2001 && edge.second == 2002 && edge.input == 0) ||
                (edge.first == 2002 && edge.second == 2001 && edge.input == 0),
            "the latch cut does not join the two NANDs on the feedback pin");
    require(edge.bits == 1, "the latch cut did not carry the wire's width");
    /* The detached end is the far end of the wire the record names, and
       restoring has to put it back byte for byte. */
    const unsigned char* cutWire = cutState().wire;
    require(cutWire != nullptr, "the latch cut kept no wire pointer");
    require(readI16(cutWire, 0x1c) == -68,
            "the latch cut did not push the consumer's wire end out of reach");
    afterCompileCut();
    require(!cutState().active, "the latch cut was not restored");
    require(pendingCuts().size() == 1,
            "restoring the latch wire consumed the re-close record too early");
    require(readI16(cutWire, 0x1c) == -4, "the restored wire end is not back on the pin");
    clearCuts();

    /* Nothing about the latch may be cut once the loop only passes through a
       kind whose pins are unknown: that is the editor's half-drawn part, and a
       guess here is what made a real board stop compiling. */
    put(wirePayload, kTableHeader + 1 * kWireStride + 0x1c, static_cast<int16_t>(-6));
    put(wirePayload, kTableHeader + 1 * kWireStride + 0x1e, static_cast<int16_t>(21));
    const bool foreign = detachCycleEdge(&components, &wires,
        [&log](const std::string& line) { log.push_back(line); });
    require(!foreign, "a loop through an unknown kind was cut");
    require(!cutState().active && pendingCuts().empty(),
            "skipping an unknown-kind loop left cut state behind");

    /* The old boundary skipped every board above sixteen components, because the
       undirected graph could not tell a real latch from an ordinary fan-out.
       Direction removes that reason, so a crowded board has to be cut too - the
       remaining bound is about how much work one compile may spend. */
    const size_t crowded = 40;
    std::vector<unsigned char> crowdedComponents(kTableHeader + crowded * kComponentStride, 0);
    for (size_t index = 0; index + 2 < crowded; ++index) {
        const size_t at = kTableHeader + index * kComponentStride;
        put(crowdedComponents, at, static_cast<uint8_t>(0x04));
        put(crowdedComponents, at + 2, static_cast<int16_t>(100 + static_cast<int>(index) * 4));
        put(crowdedComponents, at + 4, static_cast<int16_t>(100));
        put(crowdedComponents, at + 8, static_cast<uint64_t>(6000 + index));
    }
    for (size_t index = crowded - 2; index < crowded; ++index) {
        const size_t at = kTableHeader + index * kComponentStride;
        put(crowdedComponents, at, static_cast<uint8_t>(0x06));
        put(crowdedComponents, at + 2, static_cast<int16_t>(-3));
        put(crowdedComponents, at + 4, static_cast<int16_t>(index == crowded - 2 ? 12 : 19));
        put(crowdedComponents, at + 8, static_cast<uint64_t>(index == crowded - 2 ? 2001 : 2002));
    }
    std::vector<unsigned char> crowdedWires(kTableHeader + 2 * kWireStride, 0);
    put(crowdedWires, kTableHeader + 0x18, static_cast<int16_t>(-1));
    put(crowdedWires, kTableHeader + 0x1a, static_cast<int16_t>(12));
    put(crowdedWires, kTableHeader + 0x1c, static_cast<int16_t>(-4));
    put(crowdedWires, kTableHeader + 0x1e, static_cast<int16_t>(18));
    put<uint32_t>(crowdedWires, kTableHeader + 0x30, 1);
    put(crowdedWires, kTableHeader + kWireStride + 0x18, static_cast<int16_t>(-1));
    put(crowdedWires, kTableHeader + kWireStride + 0x1a, static_cast<int16_t>(19));
    put(crowdedWires, kTableHeader + kWireStride + 0x1c, static_cast<int16_t>(-4));
    put(crowdedWires, kTableHeader + kWireStride + 0x1e, static_cast<int16_t>(11));
    put<uint32_t>(crowdedWires, kTableHeader + kWireStride + 0x30, 1);
    Sequence crowdedComponentsSequence{crowded, crowdedComponents.data()};
    Sequence crowdedWiresSequence{2, crowdedWires.data()};
    const bool crowdedCut = detachCycleEdge(&crowdedComponentsSequence, &crowdedWiresSequence,
        [&log](const std::string& line) { log.push_back(line); });
    require(crowdedCut, "a board above the old sixteen-component boundary was not cut");
    require(pendingCuts().size() == 1 && pendingCuts().front().directed &&
                pendingCuts().front().first != 0 && pendingCuts().front().second != 0,
            "the crowded board's cut was not a directed record between the two gates");
    afterCompileCut();
    clearCuts();

    std::cout << "PASS gate delay: a closed two-NAND latch is cut between the gates on the "
                 "feedback pin\n";
    std::cout << "PASS gate delay: unrelated kinds on the same board do not stop the cut\n";
    std::cout << "PASS gate delay: a crowded board is cut too, not skipped by a size boundary\n";
}

/* Automatic cutting is not NAND-only: the pin geometry comes from the game's own
   prototype table, so a loop built from NOR/AND/OR/XOR/NOT/Mux/... is a cycle
   too.  The one thing it must not do is cut an edge whose producer has more
   than one output (the re-close record names a component, not an output). */
void checkOtherGateKindsCut() {
    using namespace tc::gate_delay;
    restoreCut();
    clearCuts();

    const size_t count = 2;
    std::vector<unsigned char> componentPayload(kTableHeader + count * kComponentStride, 0);
    std::vector<unsigned char> wirePayload(kTableHeader + count * kWireStride, 0);
    auto component = [&](size_t index, uint8_t kind, int16_t x, int16_t y, uint64_t id) {
        const size_t at = kTableHeader + index * kComponentStride;
        put(componentPayload, at, kind);
        put(componentPayload, at + 2, x);
        put(componentPayload, at + 4, y);
        put(componentPayload, at + 8, id);
    };
    auto wire = [&](size_t index, int16_t x1, int16_t y1, int16_t x2, int16_t y2) {
        const size_t at = kTableHeader + index * kWireStride;
        put(wirePayload, at + 0x18, x1);
        put(wirePayload, at + 0x1a, y1);
        put(wirePayload, at + 0x1c, x2);
        put(wirePayload, at + 0x1e, y2);
        put<uint32_t>(wirePayload, at + 0x30, 1);
    };
    struct Sequence { uint64_t count; const unsigned char* payload; };
    Sequence components{count, componentPayload.data()};
    Sequence wires{count, wirePayload.data()};
    std::vector<std::string> log;

    /* A NOR latch: the classic replacement for the two-NAND one. */
    component(0, 0x09, -3, 12, 2001);
    component(1, 0x09, -3, 19, 2002);
    wire(0, -1, 12, -4, 18);
    wire(1, -1, 19, -4, 11);
    require(detachCycleEdge(&components, &wires,
                            [&log](const std::string& line) { log.push_back(line); }),
            "a two-NOR latch was not cut");
    require(pendingCuts().size() == 1 && pendingCuts().front().directed &&
                pendingCuts().front().first != 0 &&
                pendingCuts().front().first != pendingCuts().front().second,
            "the NOR latch cut was not a directed record between two gates");
    afterCompileCut();
    clearCuts();

    /* A loop through a Splitter: every node on the cycle is a producer, but only
       the NAND has a single output - the pass has to find that edge. */
    component(0, 0x06, 0, 10, 3002);       /* NAND: in0 (-1,9), out (2,10) */
    component(1, 0x2f, 0, 0, 3001);        /* Splitter: in0 (-1,0), out0 (1,-1) */
    wire(0, 1, -1, -1, 9);                 /* splitter out -> NAND in0 */
    wire(1, 2, 10, -1, 0);                 /* NAND out -> splitter in0 */
    require(detachCycleEdge(&components, &wires,
                            [&log](const std::string& line) { log.push_back(line); }),
            "a loop through a multi-output component was not cut at its single-output edge");
    require(pendingCuts().size() == 1 && pendingCuts().front().first == 3001 &&
                pendingCuts().front().second == 3002,
            "the splitter loop was cut on the wrong edge");
    afterCompileCut();
    clearCuts();

    /* Two Splitters: every producer has several outputs, so there is no edge the
       re-close record could name.  Refusing is the correct answer. */
    component(0, 0x2f, 0, 0, 4001);
    component(1, 0x2f, 0, 10, 4002);
    wire(0, 1, -1, -1, 10);                /* s1 out0 -> s2 in0 */
    wire(1, 1, 9, -1, 0);                  /* s2 out0 -> s1 in0 */
    require(!detachCycleEdge(&components, &wires,
                             [&log](const std::string& line) { log.push_back(line); }),
            "a loop with no single-output producer was cut anyway");
    require(!cutState().active && pendingCuts().empty(),
            "refusing the splitter loop left cut state behind");
    std::cout << "PASS gate delay: a loop of NOR/other built-in gates is cut too\n";
    std::cout << "PASS gate delay: a multi-output producer is never named by a cut record\n";
}

/* The clock source (examples/clock).  The Mod registers a pure source whose
   value is a per-cycle square wave; with the mode on, the delay model answers
   reads of that component's slot with a one-unit pulse instead, which is what
   makes the mode testable.  The rewrite learns the slot from the `com_custom`
   header the emitter writes for the instance, so this case drives the whole
   path: instance set in, slot marked, read answering with the pulse. */
/* The tie-break rule: when every member of a feedback loop publishes the same
   new value in one unit, the board-order-first member keeps its old value for
   that unit.  That is what lets a two-NAND latch settle instead of flipping in
   lockstep, and it must never fire for a settled loop. */
/* The delay table: composite gates cost an extra unit, the way a CMOS AND is a
   NAND plus an inverter.  The numbers have to reach the emitted program, or the
   model is still "everything is one unit". */
void checkDelayTable() {
    using namespace tc::gate_delay;
    require(kindDelay("com_not_bit", 1) == 1, "a NOT stopped being one level");
    require(kindDelay("com_nand_bit", 1) == 1, "a NAND stopped being one level");
    require(kindDelay("com_and_bit", 1) == 2, "an AND is not two levels");
    require(kindDelay("com_or_bit", 1) == 2, "an OR is not two levels");
    require(kindDelay("com_xor_bit", 1) == 3, "an XOR is not three levels");
    require(kindDelay("com_ram", 1) == 4, "RAM lost its four-unit delay");
    require(kindDelay("com_add", 8) == 4, "an 8-bit add lost its carry stage");
    require(kindDelay("com_add", 1) == 3, "a one-bit add stopped being three levels");

    std::string text = R"SRC(extern windows_x64 game_engine
const #SIMULATION_STATE               = Ptr 2328140120064

def mode_refresh() None {

    // 1 com_not_bit 1 3
    let value_id256 = ~(U1 0x0)
    store(#SIMULATION_STATE + 256, U1 (value_id256))

    // 2 com_and_bit 1 9
    let value_id258 = (U1 (load(<U1>, #SIMULATION_STATE + 256))) & (U1 0x1)
    store(#SIMULATION_STATE + 258, U1 (value_id258))
}

def mode_run(target_cycle: Int) None {
    while cycle < burst_target_cycle {

    // 1 com_not_bit 1 3
    var vid256 = U1 ~0

    // 2 com_and_bit 1 9
    var vid258 = U1 (vid256 & 1)
        cycle += 1
    }
}
)SRC";
    require(rewrite(text, 8), "the delay-table sample was not rewritten");
    require(count(text, ", U64 2)") >= 2, "the AND's two-unit delay did not reach the program");
    require(count(text, ", U64 1)") >= 2, "the NOT's one-unit delay did not reach the program");
    std::cout << "PASS gate delay: a composite gate costs an extra unit (NOT 1, AND/OR 2, XOR 3)\n";
}

void checkTieBreak() {
    using namespace tc::gate_delay;
    Runtime& rt = store();
    static std::vector<unsigned char> backing(4096, 0);
    static unsigned char* statePointer = backing.data();
    std::fill(backing.begin(), backing.end(), 0);
    rt.state = &statePointer;
    rt.scratch.assign(4096, 0);
    rt.dirty.assign(4096, 0);
    rt.dirtyNow.assign(4096, 0);
    rt.reset();
    tieGroups().clear();
    tieBreaks() = 0;
    tieGroups().push_back(TieGroup{{100, 102}});      /* board order: 100 first */
    rt.unitsPerCycle = 8;
    rt.time = 0;
    write(100, 1, 1, 1);
    write(102, 1, 1, 1);
    rt.time = 1;
    commitPending();
    /* Commits land in the state array; reads answer from the unit's snapshot. */
    require(backing[100] == 0, "the tie-break member did not hold its old value");
    require(backing[102] == 1, "the other member of the tie did not commit");
    require(tieBreaks() == 1, "the tie-break rule did not fire exactly once");
    /* A settled latch keeps publishing what it already holds: nobody moves, so
       nothing may be held. */
    rt.time = 2;
    write(100, 1, 0, 1);
    write(102, 1, 1, 1);
    rt.time = 3;
    commitPending();
    require(backing[100] == 0 && backing[102] == 1, "the settled latch changed value");
    require(tieBreaks() == 1, "the rule fired without a lockstep transition");
    tieGroups().clear();
    rt.reset();
    std::cout << "PASS gate delay: a symmetric loop resolves instead of flipping in lockstep\n";
}

/* A wire is not a device.  The emitter turns a wire into a node of its own
   (`com_cc_input_buffer` and the same shape wherever a wire is mirrored for a
   consumer), and every node used to cost a unit of delay.  For a wire that unit
   is visible: the board draws the copy slot while the gate's own pin (what the
   component panel shows) is the source, so the wire sat one unit behind its pin
   for as long as the circuit kept changing.  Such a block mirrors its source in
   the same unit instead of publishing a delay of its own. */
void checkWireMirror() {
    using namespace tc::gate_delay;
    restoreCut();
    clearCuts();
    mirrors().clear();
    std::string text = R"SRC(extern windows_x64 game_engine
const #SIMULATION_STATE               = Ptr 2328140120064

def mode_refresh() None {

    // 1 com_not_bit 1 3
    let value_id256 = ~(U1 0x0)
    store(#SIMULATION_STATE + 256, U1 (value_id256))

    // 2 com_cc_input_buffer 1 0
    let value_id258 = (U1 (load(<U1>, #SIMULATION_STATE + 256)))
    store(#SIMULATION_STATE + 258, U1 (value_id258))
    store(#SIMULATION_STATE + 259, U1 (value_id258))
}
)SRC";
    require(rewrite(text, 8), "the wire-copy sample was not rewritten");
    require(text.find("tc_delay_mirror'(U64 258, U64 256, U64 1)") != std::string::npos,
            "the wire copy did not become a mirror of its source");
    require(text.find("tc_delay_mirror'(U64 259, U64 256, U64 1)") != std::string::npos,
            "the wire copy's second slot did not become a mirror");
    require(text.find("tc_delay_write'(U64 258") == std::string::npos,
            "the wire copy still publishes with a delay of its own");
    require(text.find("tc_delay_write'(U64 256, U64 1, U64 (U1 (value_id256)), U64 1)") !=
                std::string::npos,
            "the gate next to the wire lost its own delay");

    /* Runtime: the copy carries the source's value in the *same* unit, so a
       frame, the gate that reads it and the panel that shows the pin agree. */
    Runtime& rt = store();
    static std::vector<unsigned char> backing(512, 0);
    static unsigned char* statePointer = backing.data();
    std::fill(backing.begin(), backing.end(), 0);
    rt.state = &statePointer;
    rt.stateSize = backing.size();
    rt.scratch.assign(backing.size(), 0);
    rt.dirty.assign(backing.size(), 0);
    rt.dirtyNow.assign(backing.size(), 0);
    rt.reset();
    setUnitsPerPass(1);
    rt.activeSite = rt.currentSite = -1;
    write(256, 1, 1, 1);         /* the gate next to the wire publishes its output */
    registerMirror(258, 256, 1); /* what the rewritten copy block calls every pass */
    registerMirror(259, 256, 1);
    begin(0);                    /* commit, mirror, snapshot - one unit */
    require(backing[256] == 1, "the source did not publish");
    require(backing[258] == 1 && backing[259] == 1,
            "the wire copy did not follow its source in the same unit");
    /* The next change reaches the copy in the same unit too; a copy that
       published a delay of its own would still show the old value here. */
    write(256, 1, 0, 1);
    begin(0);
    require(backing[256] == 0 && backing[258] == 0 && backing[259] == 0,
            "the wire copy lagged its source after the source changed");
    setUnitsPerPass(0);
    mirrors().clear();
    rt.reset();
    std::cout << "PASS gate delay: a wire copy mirrors its source instead of adding a unit\n";
}

/* The emitter's reporting scaffold keeps one slot per net as an "any field"
   accumulator: several neighbouring blocks write it inside one pass and the
   block that reads the pin reads the same slot back, expecting what the previous
   statements just computed - a pass-local value, not a delayed node.  Delaying
   those writes hands the reader *another net's* value from an earlier unit,
   which is what made a NAND's own output stay wrong while the component panel
   (which the game computes by itself) showed the right one. */
void checkClockPulse() {
    using namespace tc::gate_delay;
    restoreCut();
    clearCuts();
    clockSlots().clear();
    clockInstances().clear();
    const uint64_t clock = 4849338083909316657ULL;          /* "CLOK_001" */
    std::string text = R"SRC(extern windows_x64 game_engine
const #SIMULATION_STATE               = Ptr 2328140120064

def mode_refresh() None {

    // 1 com_custom 1 4849338083909316657
    let value_id256 = (U1 0x0)
    store(#SIMULATION_STATE + 256, U1 (value_id256))
    store(#SIMULATION_STATE + 257, U1 (value_id256))

    // 2 com_and_bit 2 7
    let value_id258 = (U1 (load(<U1>, #SIMULATION_STATE + 256))) & (U1 0x1)
    store(#SIMULATION_STATE + 258, U1 (value_id258))
}

def mode_run(target_cycle: Int) None {
    while cycle < burst_target_cycle {

    // 1 com_custom 1 4849338083909316657
    var vid256 = U1 0

    // 2 com_and_bit 2 7
    var vid258 = U1 (vid256 & 1)
        cycle += 1
    }
}
)SRC";
    clockInstances().insert(clock);
    std::vector<std::string> log;
    store().log = [&log](const std::string& line) { log.push_back(line); };
    require(rewrite(text, 8), "the clock sample was not rewritten");
    Runtime& rt = store();
    require(clockSlots().count(256) == 1, "the clock instance's slot was not marked");
    require(clockSlots().count(257) == 1, "the clock instance's visible alias was not marked");
    bool reported = false;
    for (const std::string& line : log)
        if (line.find("clock slot(s) answer with a ") != std::string::npos)
            reported = true;
    require(reported, "the clock slot was marked without saying so");
    store().log = nullptr;

    /* By default the clock is high for half the cycle (a square wave; see
       clockWidthUnits) and it repeats: the unit counter is monotone, so a later
       cycle's first unit is high again. */
    rt.unitsPerCycle = 8;
    rt.time = 0;
    require(read(256, 1) == 1, "the clock was low at the first unit of a cycle");
    rt.time = 3;
    require(read(256, 1) == 1, "the clock ended its half cycle too early");
    rt.time = 4;
    require(read(256, 1) == 0, "the clock stayed high past its half cycle");
    rt.time = 7;
    require(read(256, 1) == 0, "the clock was high at the last unit of a cycle");
    rt.time = 8;
    require(read(256, 1) == 1, "the clock did not pulse again in the next cycle");
    /* The same pulse must be present in the real state buffer because that is
       what the board's wire shader reads; logic and drawing cannot have two
       different clocks. */
    static std::vector<unsigned char> backing(512, 0);
    static unsigned char* statePointer = backing.data();
    std::fill(backing.begin(), backing.end(), 0);
    rt.state = &statePointer;
    rt.stateSize = backing.size();
    rt.scratch.assign(backing.size(), 0);
    rt.dirty.assign(backing.size(), 0);
    rt.dirtyNow.assign(backing.size(), 0);
    rt.time = 0;
    rt.activeSite = rt.currentSite = -1;
    setUnitsPerPass(1);
    begin(0);
    require(backing[256] == 1 && backing[257] == 1,
            "the first clock unit was not published to every visible slot");
    begin(0);
    begin(0);
    begin(0);
    require(backing[256] == 0 && backing[257] == 0,
            "the displayed clock stayed high after the logic clock fell");
    /* A slot that is not a clock keeps answering with what it holds. */
    rt.time = 3;
    require(read(258, 1) == 0, "a non-clock read picked up the pulse");
    rt.time = 0;
    setUnitsPerPass(0);
    rt.reset();
    clockSlots().clear();
    clockInstances().clear();
    std::cout << "PASS gate delay: a clock source answers with a one-unit pulse per cycle\n";
}

/* The game may enter both generated bodies: mode_refresh while painting the
   board and mode_run for a burst.  They describe the same circuit, so letting
   both advance Runtime::time makes rendering an extra clock source.  The first
   body that runs owns unit propagation; the other body is read-only, except
   that mode_run may observe one already-reached cycle boundary. */
void checkSingleTimeOwner() {
    using namespace tc::gate_delay;
    Runtime& rt = store();
    static std::vector<unsigned char> backing(4096, 0);
    static unsigned char* statePointer = backing.data();
    std::fill(backing.begin(), backing.end(), 0);
    rt.state = &statePointer;
    rt.scratch.assign(backing.size(), 0);
    rt.dirty.clear();
    rt.dirtyNow.clear();
    rt.log = nullptr;
    setUnitsPerPass(1);
    rt.reset();

    require(unitsThisPass(0) == 1, "the refresh owner did not receive a unit budget");
    begin(0);
    require(rt.activeSite == 0 && rt.time == 1 && rt.perSite[0] == 1,
            "the first refresh body did not become the unit-clock owner");
    write(100, 1, 1, 1);
    const size_t pending = rt.pending.size();

    require(unitsThisPass(1) == 0,
            "the non-owner burst evaluated combinational locals between boundaries");
    require(rt.time == 1 && rt.perSite[1] == 0 && rt.suppressedPerSite[1] == 0,
            "checking the non-owner burst changed runtime state");
    require(rt.pending.size() == pending,
            "the skipped non-owner burst changed the delayed-write queue");

    for (unsigned i = 1; i < 8; ++i) begin(0);
    require(rt.time == 8 && cycleStart() == 1, "the refresh owner did not reach the K-unit boundary");
    require(unitsThisPass(1) == 1,
            "mode_run did not receive its one pass at the reached cycle boundary");
    begin(1);
    require(cycleStart() == 1, "mode_run could not synchronize the reached cycle boundary");
    require(unitsThisPass(1) == 0, "mode_run consumed the same cycle boundary twice");

    rt.reset();
    require(unitsThisPass(1) == 1, "the burst-only path did not receive a unit budget");
    begin(1);
    require(rt.activeSite == 1 && rt.time == 1 && rt.perSite[1] == 1,
            "a burst-only program could not own the unit clock");
    begin(0);
    require(rt.time == 1 && rt.suppressedPerSite[0] == 1,
            "a render refresh advanced the burst-owned unit clock");

    setUnitsPerPass(0);
    rt.reset();
    std::cout << "PASS gate delay: refresh and burst share one unit-clock owner\n";
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc >= 3 && std::string(argv[1]) == "--sample") {
            std::ofstream out(argv[2], std::ios::binary);
            out << kSample;
            return 0;
        }
        if (argc >= 3 && std::string(argv[1]) == "--sample-ring") {
            std::ofstream out(argv[2], std::ios::binary);
            out << kRingSample;
            return 0;
        }
        if (argc >= 3 && std::string(argv[1]) == "--sample-cut") {
            std::ofstream out(argv[2], std::ios::binary);
            out << kCutSample;
            return 0;
        }
        if (argc >= 4 && std::string(argv[1]) == "--rewrite") {
            std::ifstream in(argv[2], std::ios::binary);
            if (!in) throw std::runtime_error(std::string("cannot read ") + argv[2]);
            std::ostringstream buffer;
            buffer << in.rdbuf();
            std::string text = buffer.str();
            tc::gate_delay::store().log = [](const std::string& line) { std::cout << line << "\n"; };
            /* A fourth argument is a cut the loader would have made:
               `first:second[:bits]`. */
            tc::gate_delay::clearCuts();
            if (argc >= 5) {
                int64_t first = 0, second = 0;
                unsigned bits = 1;
                if (std::sscanf(argv[4], "%lld:%lld:%u", &first, &second, &bits) >= 2)
                    tc::gate_delay::addCut(first, second, bits);
            }
            /* TC_GATE_DELAY_CLOSE reaches the re-close path the way it does
               inside the loader. */
            if (!tc::gate_delay::rewrite(text, 8, tc::gate_delay::environmentReclosures()))
                throw std::runtime_error("not a simulation program");
            std::ofstream out(argv[3], std::ios::binary);
            out << text;
            return 0;
        }
        checkSample();
        std::cout << "PASS gate delay: the sample program becomes two unit loops with "
                     "guarded boundary blocks\n";
        checkReclose();
        checkEventSchedule();
        checkCutReclose();
        checkAmbiguousCutRejected();
        checkMultiDriverAccumulator();
        checkCycleCut();
        checkTwoNandLatchCut();
        checkOtherGateKindsCut();
        checkClockPulse();
        checkWireMirror();
        checkSingleTimeOwner();
        checkTieBreak();
        checkDelayTable();
        for (int i = 1; i < argc; ++i) {
            if (std::string(argv[i]).rfind("--rewrite", 0) == 0) continue;
            checkDump(argv[i]);
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL gate delay: " << error.what() << "\n";
        return 1;
    }
}
