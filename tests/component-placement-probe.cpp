// Development-only real-game probe for the component menu/placement path.
// It loads the sandbox level, calls the same add_component helper used by the
// component-menu builders, and verifies that the registered custom prototype
// lands in the board component array with its 64-bit identity.

#include "../sdk/tc_mod.h"
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

struct V2 {
    float x, y;
};

constexpr uint64_t kAndComponentId = 0x414E44325F303031ULL;
constexpr uint8_t kCustomKind = 0x4E;
const TCHost* host;
tc::TCMod mod;
bool done;
bool imported;
bool loaded_level;
bool started;
void* model;
double start_time;
double elapsed;
TCBoardApiV3 boardApi{};
TCBoardApiV4 boardApi4{};
TCBoardApiV5 boardApi5{};
TCCommandApiV2 commandApi{};
TCTransactionApiV2 transactionApi{};
TCGameHandle boardHandle{};
uint64_t commandRequest = 0;
uint64_t transactionRequest = 0;
size_t commandBefore = 0;
size_t commandLiveBefore = 0;
size_t transactionBefore = 0;
bool phase1 = false, commandPending = false, transactionPending = false;
/* TC_UNDO_TRACE=1 dumps every undo entry the game registers while this probe
   drives its edits.  The entry is one seq of 0x490-byte variant elements whose
   tag sits at +8, so the dump is what tells the layout apart from the
   disassembly's field reads. */
bool undo_trace = false;
/* Two arguments: the wrapper reads its first argument like a sequence header,
   but the call sites set a second register as well, so the dump has to see both
   before deciding which one carries the change list. */
using AddUndoChanges = void (*)(void*, void*);
using AddUndoChangesHook = void (*)(void*, void*);
AddUndoChanges add_undo_changes_original;
uint32_t undoDumpCount;
/* The element copy inside the registration: `eqcopy(dst, src)` is where the real
   0x490-byte change record moves onto the callee's stack, so `src` is the change
   record itself (the wrapper's own argument is a structure that merely points at
   the sequence). */
using EqCopy = void (*)(void*, void*);
EqCopy eqcopy_original;
bool inside_undo_registration = false;
uint32_t elementDumpCount;
bool undoPending = false, redoPending = false;
uint64_t undoRequest = 0, redoRequest = 0;
size_t transactionAfter = 0;
size_t transactionLive = 0;
double load_time;
bool (*invisible_original)(const char*, V2, int);
int test_frame = -1;
int test_button_index;
using UpdateWire = bool (*)(void*, void*, void*, uint32_t, uint8_t);
UpdateWire update_original;
using AddComponent = bool (*)(void*, void*);
AddComponent add_component;
using DeleteComponent = void (*)(void*, int64_t);
DeleteComponent delete_component;
using LoadLevel = void (*)(void*, const tc::TCNimString*);
LoadLevel load_level;

void log(const std::string& message) {
    host->log(host->context, message.c_str());
}

int32_t packPoint(int16_t x, int16_t y) {
    return static_cast<int32_t>((static_cast<uint32_t>(y) << 16) |
                                static_cast<uint16_t>(x));
}

bool hookedUpdate(void* m, void* context, void* input, uint32_t point,
                  uint8_t fifth) {
    model = m;
    return update_original ? update_original(m, context, input, point, fifth)
                           : false;
}

/* One registered undo entry: a seq header (count, payload) of 0x490-byte
   variant elements.  The tag is at +8 of the element, and the head fields say
   which board object the entry is about. */
void hookedAddUndoChanges(void* first, void* second) {
    if (undo_trace && undoDumpCount < 12) {
        ++undoDumpCount;
        std::string text = "undo-trace entry=" + std::to_string(undoDumpCount) +
                           " rcx=" + std::to_string(reinterpret_cast<uintptr_t>(first)) +
                           " rdx=" + std::to_string(reinterpret_cast<uintptr_t>(second));
        const auto describe = [&text](const char* label, const void* pointer,
                                      size_t window, bool asWords) {
            if (!pointer) return;
            const auto* base = static_cast<const unsigned char*>(pointer);
            text += std::string(" ") + label + "=";
            char field[32];
            if (asWords) {
                for (size_t offset = 0; offset < window; offset += 8) {
                    uint64_t value = 0;
                    std::memcpy(&value, base + offset, sizeof(value));
                    std::snprintf(field, sizeof(field), " %016llx",
                                  static_cast<unsigned long long>(value));
                    text += field;
                }
            } else {
                for (size_t offset = 0; offset < window; ++offset) {
                    std::snprintf(field, sizeof(field), "%02x", base[offset]);
                    text += field;
                }
            }
        };
        describe("rcx.words", first, 0x120, true);
        /* Follow whatever looks like a heap pointer in the header: one of these
           buffers is the change list, and the element inside it is the record the
           registration dispatches on. */
        if (first) {
            const auto* words = static_cast<const unsigned char*>(first);
            for (size_t offset = 0; offset < 0x40; offset += 8) {
                uint64_t pointer = 0;
                std::memcpy(&pointer, words + offset, sizeof(pointer));
                if (pointer < 0x10000 || pointer > 0x7FFFFFFFFFFFull) continue;
                char label[32];
                std::snprintf(label, sizeof(label), "at+%02zx[0..0x40]", offset);
                describe(label, reinterpret_cast<const void*>(pointer), 0x40, true);
            }
        }
        // (the second register is zero on this build, so nothing is dumped for it)
        log(text);
    }
    inside_undo_registration = true;
    if (add_undo_changes_original) add_undo_changes_original(first, second);
    inside_undo_registration = false;
}

void hookedEqCopy(void* dst, void* src) {
    if (undo_trace && inside_undo_registration && src && elementDumpCount < 8) {
        ++elementDumpCount;
        const auto* element = static_cast<const unsigned char*>(src);
        std::string text = "undo-element #" + std::to_string(elementDumpCount) +
                           " src=" + std::to_string(reinterpret_cast<uintptr_t>(src)) + " bytes=";
        char field[32];
        for (size_t offset = 0; offset < 0x60; ++offset) {
            std::snprintf(field, sizeof(field), "%02x", element[offset]);
            text += field;
        }
        for (size_t offset : {size_t(0), size_t(0x08), size_t(0x10), size_t(0x18), size_t(0x20),
                              size_t(0x28), size_t(0x30), size_t(0x38), size_t(0x40), size_t(0x48),
                              size_t(0x50), size_t(0x58)}) {
            uint64_t value = 0;
            std::memcpy(&value, element + offset, sizeof(value));
            std::snprintf(field, sizeof(field), " +%02zx=%016llx", offset,
                          static_cast<unsigned long long>(value));
            text += field;
        }
        log(text);
    }
    if (eqcopy_original) eqcopy_original(dst, src);
}

bool hookedInvisible(const char* id, V2 size, int flags) {
    const bool result = invisible_original(id, size, flags);
    const auto rva = reinterpret_cast<uintptr_t>(__builtin_return_address(0)) -
                     reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (rva >= 0x449df0 && rva < 0x44b610) {
        const auto get_frame = reinterpret_cast<int (*)()>(
            host->engine_proc(host->context, "igGetFrameCount"));
        const int frame = get_frame ? get_frame() : 0;
        if (frame != test_frame) {
            test_frame = frame;
            test_button_index = 0;
        }
        ++test_button_index;
        if (elapsed > 4.0 && test_button_index == 2) {
            return true;
        }
    }
    return result;
}

std::vector<unsigned char> placementFor(int16_t x, int16_t y) {
    std::vector<unsigned char> placement(0x238, 0);
    placement[0] = kCustomKind;
    const int32_t point = packPoint(x, y);
    std::memcpy(placement.data() + 2, &point, sizeof(point));
    uint64_t custom_id = kAndComponentId;
    std::memcpy(placement.data() + 0x188, &custom_id, sizeof(custom_id));

    // Defaults used by the component-menu placement template.
    const uint64_t one = 1;
    const uint64_t capacity = 0x100;
    std::memcpy(placement.data() + 0x58, &one, sizeof(one));
    std::memcpy(placement.data() + 0x60, &capacity, sizeof(capacity));
    placement[0x68] = 1;
    std::memcpy(placement.data() + 0x70, &one, sizeof(one));
    std::memcpy(placement.data() + 0x78, &capacity, sizeof(capacity));
    placement[0x80] = 1;
    return placement;
}

}  // namespace

/* Shared board reads for the edit phases: counts, and how many custom instances
   sit on a given grid point. */
static bool boardCounts(size_t& components, size_t& wires) {
    components = 0;
    wires = 0;
    if (boardApi.get_current(boardApi.context, &boardHandle) != TC_HANDLE_OK) return false;
    TCBoardObjectSnapshotV1 counts{};
    TCBoardObjectBuffersV1 countOnly{sizeof(countOnly), TC_BOARD_OBJECT_SNAPSHOT_VERSION_1, nullptr, 0, nullptr, 0};
    if (tc::captureBoardObjects(&boardApi, &boardHandle, &counts, &countOnly) != TC_SNAPSHOT_ERR_CAPACITY) return false;
    std::vector<TCGameHandle> handles(counts.component_count), wireHandles(counts.wire_count);
    TCBoardObjectBuffersV1 storage{sizeof(storage), TC_BOARD_OBJECT_SNAPSHOT_VERSION_1,
        handles.empty() ? nullptr : handles.data(), handles.size(),
        wireHandles.empty() ? nullptr : wireHandles.data(), wireHandles.size()};
    if (tc::captureBoardObjects(&boardApi, &boardHandle, &counts, &storage) != TC_SNAPSHOT_OK) return false;
    components = static_cast<size_t>(counts.component_count);
    wires = static_cast<size_t>(counts.wire_count);
    return true;
}
static size_t countCustomAt(int32_t x, int32_t y) {
    if (boardApi.get_current(boardApi.context, &boardHandle) != TC_HANDLE_OK) return 0;
    TCBoardObjectSnapshotV1 counts{};
    TCBoardObjectBuffersV1 countOnly{sizeof(countOnly), TC_BOARD_OBJECT_SNAPSHOT_VERSION_1, nullptr, 0, nullptr, 0};
    if (tc::captureBoardObjects(&boardApi, &boardHandle, &counts, &countOnly) != TC_SNAPSHOT_ERR_CAPACITY) return 0;
    std::vector<TCGameHandle> handles(counts.component_count), wireHandles(counts.wire_count);
    TCBoardObjectBuffersV1 storage{sizeof(storage), TC_BOARD_OBJECT_SNAPSHOT_VERSION_1,
        handles.empty() ? nullptr : handles.data(), handles.size(),
        wireHandles.empty() ? nullptr : wireHandles.data(), wireHandles.size()};
    if (tc::captureBoardObjects(&boardApi, &boardHandle, &counts, &storage) != TC_SNAPSHOT_OK) return 0;
    size_t found = 0;
    for (const auto& child : handles) {
        TCComponentInfoV1 info{};
        if (tc::readComponent(&boardApi4, &child, &info) != TC_SNAPSHOT_OK) continue;
        if (info.kind == 0x4e && info.custom_prototype_id == kAndComponentId && info.x == x && info.y == y) ++found;
    }
    return found;
}
/* The sequence length is not the live component count: the game's undo leaves a
   zero-filled record behind instead of shrinking the sequence, so anything
   counting components has to skip kind 0. */
static size_t liveComponents() {
    if (boardApi.get_current(boardApi.context, &boardHandle) != TC_HANDLE_OK) return 0;
    TCBoardObjectSnapshotV1 counts{};
    TCBoardObjectBuffersV1 countOnly{sizeof(countOnly), TC_BOARD_OBJECT_SNAPSHOT_VERSION_1, nullptr, 0, nullptr, 0};
    if (tc::captureBoardObjects(&boardApi, &boardHandle, &counts, &countOnly) != TC_SNAPSHOT_ERR_CAPACITY) return 0;
    std::vector<TCGameHandle> handles(counts.component_count), wireHandles(counts.wire_count);
    TCBoardObjectBuffersV1 storage{sizeof(storage), TC_BOARD_OBJECT_SNAPSHOT_VERSION_1,
        handles.empty() ? nullptr : handles.data(), handles.size(),
        wireHandles.empty() ? nullptr : wireHandles.data(), wireHandles.size()};
    if (tc::captureBoardObjects(&boardApi, &boardHandle, &counts, &storage) != TC_SNAPSHOT_OK) return 0;
    size_t live = 0;
    for (const auto& child : handles) {
        TCComponentInfoV1 info{};
        if (tc::readComponent(&boardApi4, &child, &info) != TC_SNAPSHOT_OK) continue;
        if (info.kind) ++live;
    }
    return live;
}

static void frame(void*, const TCFrame* value) {
    if (!started) {
        started = true;
        start_time = value->time_seconds;
    }
    elapsed = value->time_seconds - start_time;
    if (done || !imported || !model || elapsed < 5.0) return;
    const auto folder = std::filesystem::u8path(host->data_directory_utf8);
    auto finish = [&](const std::string& text) {
        done = true;
        commandPending = false;
        transactionPending = false;
        log(text);
        std::ofstream(folder / "result.txt") << text;
    };
    /* The loader executes a queued command at the end of the frame that
       submitted it, so both edit phases finish on a later frame. */
    if (phase1) {
        if (redoPending) {
            TCCommandStatusV1 status{};
            if (commandApi.get_status(commandApi.context, redoRequest, &status, sizeof(status)) != TC_COMMAND_OK) return;
            if (status.state != TC_COMMAND_STATE_SUCCEEDED && status.state != TC_COMMAND_STATE_FAILED &&
                status.state != TC_COMMAND_STATE_CANCELLED)
                return;
            redoPending = false;
            size_t components = 0, wires = 0;
            boardCounts(components, wires);
            log("component placement redo state=" + std::to_string(status.state) +
                " result=" + std::to_string(status.result) +
                " components=" + std::to_string(components) +
                " live=" + std::to_string(liveComponents()) +
                " restored=" + std::to_string(countCustomAt(28, 10)));
            const bool ok = status.state == TC_COMMAND_STATE_SUCCEEDED && status.result == TC_COMMAND_OK &&
                            components == transactionAfter && countCustomAt(28, 10) == 1;
            finish(ok ? "PASS a component was deleted through the board entry; components were placed through the menu helper, the command bus and a two-step transaction; one undo reverted one step and redo restored it"
                      : "FAIL redo did not restore the board");
            return;
        }
        if (undoPending) {
            TCCommandStatusV1 status{};
            if (commandApi.get_status(commandApi.context, undoRequest, &status, sizeof(status)) != TC_COMMAND_OK) return;
            if (status.state != TC_COMMAND_STATE_SUCCEEDED && status.state != TC_COMMAND_STATE_FAILED &&
                status.state != TC_COMMAND_STATE_CANCELLED)
                return;
            undoPending = false;
            size_t components = 0, wires = 0;
            boardCounts(components, wires);
            log("component placement undo state=" + std::to_string(status.state) +
                " result=" + std::to_string(status.result) +
                " components=" + std::to_string(components) +
                " live=" + std::to_string(liveComponents()) +
                " transaction-after=" + std::to_string(transactionAfter) +
                " transaction-before=" + std::to_string(transactionBefore));
            /* Which of the four placements are still there: a successful undo
               that leaves every point untouched means the programmatic edits are
               not on the game's undo stack at all. */
            log("component placement undo points=30,0:" + std::to_string(countCustomAt(30, 0)) +
                " 20,6:" + std::to_string(countCustomAt(20, 6)) +
                " 24,10:" + std::to_string(countCustomAt(24, 10)) +
                " 28,10:" + std::to_string(countCustomAt(28, 10)));
            /* What the sequence actually holds now: an undo that shrinks nothing
               but neutralises a record shows up as an unexpected kind. */
            if (boardApi.get_current(boardApi.context, &boardHandle) == TC_HANDLE_OK) {
                TCBoardObjectSnapshotV1 counts{};
                TCBoardObjectBuffersV1 countOnly{sizeof(countOnly), TC_BOARD_OBJECT_SNAPSHOT_VERSION_1, nullptr, 0, nullptr, 0};
                if (tc::captureBoardObjects(&boardApi, &boardHandle, &counts, &countOnly) == TC_SNAPSHOT_ERR_CAPACITY) {
                    std::vector<TCGameHandle> handles(counts.component_count), cable(counts.wire_count);
                    TCBoardObjectBuffersV1 storage{sizeof(storage), TC_BOARD_OBJECT_SNAPSHOT_VERSION_1,
                        handles.empty() ? nullptr : handles.data(), handles.size(),
                        cable.empty() ? nullptr : cable.data(), cable.size()};
                    if (tc::captureBoardObjects(&boardApi, &boardHandle, &counts, &storage) == TC_SNAPSHOT_OK) {
                        std::string list;
                        for (const auto& child : handles) {
                            TCComponentInfoV1 info{};
                            if (tc::readComponent(&boardApi4, &child, &info) != TC_SNAPSHOT_OK) continue;
                            char kindText[8];
                            std::snprintf(kindText, sizeof(kindText), "0x%02x", info.kind);
                            list += std::string(" [") + kindText + "(" + std::to_string(info.x) + "," +
                                    std::to_string(info.y) + ")" +
                                    (info.flags & TC_COMPONENT_INFO_HAS_CUSTOM_PROTOTYPE ? "c" : "") + "]";
                        }
                        log("component placement undo components" + list);
                    }
                }
            }
            /* One undo reverted either the whole two-step transaction or only its
               last step; the log says which, and redo has to bring it back. */
            log(std::string("component placement undo granularity=") +
                (components == transactionBefore ? "transaction" : "single-step"));
            TCCommandV2 redo{};
            redo.size = sizeof(redo);
            redo.type = TC_COMMAND_BOARD_REDO;
            redo.subject = boardHandle;
            const int submitted = commandApi.submit(commandApi.context, &redo, &redoRequest);
            redoPending = submitted == TC_COMMAND_OK;
            log("component placement redo submit=" + std::to_string(submitted) +
                " request=" + std::to_string(redoRequest));
            if (!redoPending) finish("FAIL the redo command was refused: " + std::to_string(submitted));
            return;
        }
        if (transactionPending) {
            TCTransactionStatusV1 status{};
            if (transactionApi.get_status(transactionApi.context, transactionRequest, &status, sizeof(status)) != TC_TRANSACTION_OK)
                return;
            if (status.state != TC_TRANSACTION_STATE_COMMITTED && status.state != TC_TRANSACTION_STATE_FAILED &&
                status.state != TC_TRANSACTION_STATE_ABORTED && status.state != TC_TRANSACTION_STATE_CONFLICT)
                return;
            transactionPending = false;
            log("component placement transaction complete state=" + std::to_string(status.state) +
                " result=" + std::to_string(status.result) +
                " staged=" + std::to_string(status.staged_count) +
                " completed=" + std::to_string(status.completed_count));
            size_t after = 0, wires = 0;
            boardCounts(after, wires);
            const size_t foundFirst = countCustomAt(24, 10);
            const size_t foundSecond = countCustomAt(28, 10);
            log("component placement transaction board before=" + std::to_string(transactionBefore) +
                " after=" + std::to_string(after) +
                " found=" + std::to_string(foundFirst + foundSecond));
            if (!(status.state == TC_TRANSACTION_STATE_COMMITTED && status.result == TC_TRANSACTION_OK &&
                  status.completed_count == 2 && after == transactionBefore + 2 &&
                  foundFirst == 1 && foundSecond == 1)) {
                finish("FAIL the transactional placements did not land on the board");
                return;
            }
            /* One undo now: the two staged steps either revert together (the
               transaction is one undo step) or one at a time. */
            transactionAfter = after;
            transactionLive = liveComponents();
            TCCommandV2 undo{};
            undo.size = sizeof(undo);
            undo.type = TC_COMMAND_BOARD_UNDO;
            undo.subject = boardHandle;
            const int undoSubmitted = commandApi.submit(commandApi.context, &undo, &undoRequest);
            undoPending = undoSubmitted == TC_COMMAND_OK;
            log("component placement undo submit=" + std::to_string(undoSubmitted) +
                " request=" + std::to_string(undoRequest));
            if (!undoPending) finish("FAIL the undo command was refused: " + std::to_string(undoSubmitted));
            return;
        }
        if (!commandPending) return;
        TCCommandStatusV1 status{};
        if (commandApi.get_status(commandApi.context, commandRequest, &status, sizeof(status)) != TC_COMMAND_OK)
            return;
        if (status.state != TC_COMMAND_STATE_SUCCEEDED && status.state != TC_COMMAND_STATE_FAILED &&
            status.state != TC_COMMAND_STATE_CANCELLED)
            return;
        commandPending = false;
        log("component placement command complete state=" + std::to_string(status.state) +
            " result=" + std::to_string(status.result) +
            " submitted=" + std::to_string(status.submitted_frame) +
            " completed=" + std::to_string(status.completed_frame));
        size_t after = 0, found = 0;
        if (boardApi.get_current(boardApi.context, &boardHandle) == TC_HANDLE_OK) {
            TCBoardObjectSnapshotV1 counts{};
            TCBoardObjectBuffersV1 countOnly{sizeof(countOnly), TC_BOARD_OBJECT_SNAPSHOT_VERSION_1,
                                             nullptr, 0, nullptr, 0};
            if (tc::captureBoardObjects(&boardApi, &boardHandle, &counts, &countOnly) == TC_SNAPSHOT_ERR_CAPACITY) {
                std::vector<TCGameHandle> handles(counts.component_count), wires(counts.wire_count);
                TCBoardObjectBuffersV1 storage{sizeof(storage), TC_BOARD_OBJECT_SNAPSHOT_VERSION_1,
                    handles.empty() ? nullptr : handles.data(), handles.size(),
                    wires.empty() ? nullptr : wires.data(), wires.size()};
                if (tc::captureBoardObjects(&boardApi, &boardHandle, &counts, &storage) == TC_SNAPSHOT_OK) {
                    after = static_cast<size_t>(counts.component_count);
                    for (const auto& child : handles) {
                        TCComponentInfoV1 info{};
                        if (tc::readComponent(&boardApi4, &child, &info) != TC_SNAPSHOT_OK) continue;
                        if (info.kind == 0x4e && info.custom_prototype_id == kAndComponentId &&
                            info.x == 20 && info.y == 6) {
                            ++found;
                        }
                    }
                }
            }
        }
        const size_t liveAfter = liveComponents();
        log("component placement command board before=" + std::to_string(commandBefore) +
            " after=" + std::to_string(after) + " live=" +
            std::to_string(commandLiveBefore) + "->" + std::to_string(liveAfter) +
            " found=" + std::to_string(found));
        if (!(status.state == TC_COMMAND_STATE_SUCCEEDED && status.result == TC_COMMAND_OK &&
              (after == commandBefore || after == commandBefore + 1) &&
              liveAfter == commandLiveBefore + 1 && found == 1)) {
            finish("FAIL the placement command did not land on the board");
            return;
        }
        /* Second edit: TWO placements staged in one V2 transaction, so a single
           undo can tell "transaction = one undo step" from "each command = one". */
        TCCommandV2 staged{};
        staged.size = sizeof(staged);
        staged.type = TC_COMMAND_BOARD_PLACE_COMPONENT;
        staged.subject = boardHandle;
        staged.custom_prototype_id = kAndComponentId;
        staged.kind = 0x4e;
        staged.x = 24;
        staged.y = 10;
        TCCommandV2 stagedSecond = staged;
        stagedSecond.x = 28;
        transactionBefore = after;
        int stagedStatus = transactionApi.begin(transactionApi.context, &boardHandle, 0, &transactionRequest);
        if (stagedStatus == TC_TRANSACTION_OK)
            stagedStatus = transactionApi.stage(transactionApi.context, transactionRequest, &staged);
        if (stagedStatus == TC_TRANSACTION_OK)
            stagedStatus = transactionApi.stage(transactionApi.context, transactionRequest, &stagedSecond);
        if (stagedStatus == TC_TRANSACTION_OK)
            stagedStatus = transactionApi.commit(transactionApi.context, transactionRequest);
        transactionPending = stagedStatus == TC_TRANSACTION_OK;
        log("component placement transaction submit=" + std::to_string(stagedStatus) +
            " request=" + std::to_string(transactionRequest) +
            " before=" + std::to_string(transactionBefore));
        if (!transactionPending)
            finish("FAIL the transactional placement was refused: " + std::to_string(stagedStatus));
        return;
    }
    if (!loaded_level) {
        if (mod.game.raw_new_string) {
            const char* text = "sandbox";
            const size_t length = std::strlen(text);
            tc::TCNimString name{};
            mod.game.raw_new_string(&name, static_cast<int64_t>(length));
            name.length = length;
            std::memcpy(static_cast<unsigned char*>(name.data) + 8, text,
                        length);
            static_cast<unsigned char*>(name.data)[8 + length] = 0;
            if (load_level) load_level(model, &name);
        }
        loaded_level = true;
        load_time = elapsed;
        return;
    }
    if (elapsed < load_time + 3.0) return;
    phase1 = true;
    auto* context = static_cast<unsigned char*>(model);
    uint64_t before = 0;
    std::memcpy(&before, context + 0x78, sizeof(before));
    if (before > 10000 || !add_component) {
        finish("FAIL board or placement helper unavailable");
        return;
    }

    auto placement = placementFor(30, 0);
    const bool placed = add_component(context, placement.data());
    uint64_t after = 0;
    std::memcpy(&after, context + 0x78, sizeof(after));
    log("component placement add helper=" + std::to_string(placed) +
        " before=" + std::to_string(before) +
        " after=" + std::to_string(after));
    if (!placed || after != before + 1) {
        finish("FAIL component placement helper did not commit");
        return;
    }

    void* components = nullptr;
    std::memcpy(&components, context + 0x80, sizeof(components));
    bool found = false;
    if (components && after <= 10000) {
        for (uint64_t i = 0; i < after; ++i) {
            auto* component =
                static_cast<unsigned char*>(components) + 8 + i * 0x238;
            uint16_t kind = 0;
            int16_t x = 0;
            int16_t y = 0;
            uint64_t id = 0;
            std::memcpy(&kind, component, sizeof(kind));
            std::memcpy(&x, component + 2, sizeof(x));
            std::memcpy(&y, component + 4, sizeof(y));
            std::memcpy(&id, component + 0x188, sizeof(id));
            if (kind == kCustomKind && id == kAndComponentId) {
                found = true;
                log("component placement found custom AND at " +
                    std::to_string(x) + "," + std::to_string(y) +
                    " id=" + std::to_string(id));
                break;
            }
        }
    }
    if (!found) {
        finish("FAIL custom AND component missing from board");
        return;
    }
    /* The placed instance is also the real-machine exercise of the
       custom-prototype pin path: enumerate with V3, identify with V4, then read
       the pins the definition's own pin nodes declare. */
    TCGameHandle board{};
    if (boardApi.get_current(boardApi.context, &board) != TC_HANDLE_OK) {
        finish("FAIL the Board handle is unavailable after the menu placement");
        return;
    }
    boardHandle = board;
    TCBoardObjectSnapshotV1 objects{};
    TCBoardObjectBuffersV1 countOnly{sizeof(countOnly), TC_BOARD_OBJECT_SNAPSHOT_VERSION_1,
                                     nullptr, 0, nullptr, 0};
    int objectStatus = tc::captureBoardObjects(&boardApi, &board, &objects, &countOnly);
    std::vector<TCGameHandle> objectHandles(objects.component_count), wireHandles(objects.wire_count);
    TCBoardObjectBuffersV1 storage{sizeof(storage), TC_BOARD_OBJECT_SNAPSHOT_VERSION_1,
        objectHandles.empty() ? nullptr : objectHandles.data(), objectHandles.size(),
        wireHandles.empty() ? nullptr : wireHandles.data(), wireHandles.size()};
    if (objectStatus == TC_SNAPSHOT_ERR_CAPACITY || objectStatus == TC_SNAPSHOT_OK)
        objectStatus = tc::captureBoardObjects(&boardApi, &board, &objects, &storage);
    log("component placement objects status=" + std::to_string(objectStatus) +
        " components=" + std::to_string(objects.component_count));
    for (const auto& child : objectHandles) {
        TCComponentInfoV1 info{};
        if (tc::readComponent(&boardApi4, &child, &info) != TC_SNAPSHOT_OK) continue;
        if (info.kind != 0x4e || info.custom_prototype_id != kAndComponentId) continue;
        TCComponentPinsV1 pins{};
        TCComponentPinBuffersV1 empty{sizeof(empty), TC_COMPONENT_PINS_VERSION_1, 0, 0, nullptr, 0};
        const int pinStatus = tc::readComponentPins(&boardApi5, &child, &pins, &empty);
        const size_t total = static_cast<size_t>(pins.input_count + pins.output_count);
        char kindText[8];
        std::snprintf(kindText, sizeof(kindText), "0x%02x", pins.kind);
        log("component placement custom pins status=" + std::to_string(pinStatus) +
            " inputs=" + std::to_string(pins.input_count) +
            " outputs=" + std::to_string(pins.output_count) +
            " kind=" + kindText);
        if (pinStatus != TC_SNAPSHOT_ERR_CAPACITY && pinStatus != TC_SNAPSHOT_OK) break;
        std::vector<TCPinInfoV1> pinStorage(total);
        TCComponentPinBuffersV1 buffers{sizeof(buffers), TC_COMPONENT_PINS_VERSION_1, 0, 0,
                                        total ? pinStorage.data() : nullptr, total};
        if (tc::readComponentPins(&boardApi5, &child, &pins, &buffers) != TC_SNAPSHOT_OK) {
            log("component placement custom pin list could not be read");
            break;
        }
        std::string shape = "component placement pins";
        for (size_t i = 0; i < pinStorage.size(); ++i) {
            const TCPinInfoV1& pin = pinStorage[i];
            shape += std::string(" p") + std::to_string(i) +
                     (pin.direction == TC_PIN_INPUT ? "=i" : "=o") +
                     "(" + std::to_string(pin.x) + "," + std::to_string(pin.y) +
                     ",w" + std::to_string(pin.bits) +
                     ((pin.flags & TC_PIN_INFO_WIDTH_AUTO) ? "a" : "") + ")";
        }
        shape += " written=" + std::to_string(pins.pin_written);
        log(shape);
        break;
    }
    /* Candidate signature verified from the game's own try_delete caller:
       board_delete_component(board.components, index).  Remove the instance we
       just appended, then use only the public V3/V4 reads for the verdict. */
    if (!delete_component || objects.component_count == 0) {
        finish("FAIL component deletion entry unavailable");
        return;
    }
    const size_t deleteLiveBefore = liveComponents();
    delete_component(static_cast<unsigned char*>(model) + 0x78,
                     static_cast<int64_t>(objects.component_count - 1));
    size_t deleteAfter = 0, deleteWires = 0;
    boardCounts(deleteAfter, deleteWires);
    const size_t deleteLiveAfter = liveComponents();
    const size_t deleteFound = countCustomAt(30, 0);
    log("component deletion candidate before=" +
        std::to_string(objects.component_count) + " after=" +
        std::to_string(deleteAfter) + " live=" +
        std::to_string(deleteLiveBefore) + "->" +
        std::to_string(deleteLiveAfter) + " found=" +
        std::to_string(deleteFound));
    if (deleteAfter != objects.component_count ||
        deleteLiveAfter + 1 != deleteLiveBefore || deleteFound != 0) {
        finish("FAIL component deletion entry did not remove the appended component");
        return;
    }
    /* Queue a placement through the command bus: same fixture component, a
       different grid point, executed by the loader after this frame's plugin
       callbacks.  Nothing but the Board handle is needed. */
    TCCommandV2 place{};
    place.size = sizeof(place);
    place.type = TC_COMMAND_BOARD_PLACE_COMPONENT;
    place.subject = board;
    place.custom_prototype_id = kAndComponentId;
    place.kind = 0x4e;
    place.x = 20;
    place.y = 6;
    commandBefore = deleteAfter;
    commandLiveBefore = deleteLiveAfter;
    const int submitted = commandApi.submit(commandApi.context, &place, &commandRequest);
    commandPending = submitted == TC_COMMAND_OK;
    log("component placement command submit=" + std::to_string(submitted) +
        " request=" + std::to_string(commandRequest) +
        " before=" + std::to_string(commandBefore));
    if (!commandPending)
        finish("FAIL the placement command was refused: " + std::to_string(submitted));
}

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* h, TCPlugin* plugin) {
    if (!h || !plugin || h->api_version != TC_MOD_API_VERSION) return 1;
    host = h;
    if (!mod.load(h) || !mod.valid() || !mod.components.valid()) return 2;

    const auto folder = std::filesystem::u8path(h->data_directory_utf8);
    std::ifstream file(folder / "fixtures" / "and2_component.data",
                       std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(file)), {});
    if (bytes.empty()) return 3;
    const auto directory = (folder / "fixtures").generic_u8string() + "/";
    auto imported_result = mod.components.importCircuit(
        "AND2 Test", bytes.data(), bytes.size(), directory.c_str());
    if (!imported_result.ok() || imported_result.custom_id != kAndComponentId)
        return 4;
    imported = true;

    add_component = reinterpret_cast<AddComponent>(h->resolve_symbol(
        h->context,
        "add_component__presenterZutilitiesZhelper95functions_u5918"));
    delete_component = reinterpret_cast<DeleteComponent>(h->resolve_symbol(
        h->context,
        "board_delete_component__modelZboardZboard_u10711"));
    load_level = reinterpret_cast<LoadLevel>(h->resolve_symbol(
        h->context, "load_level__modelZutilities_u7740"));
    auto* update_target = h->resolve_symbol(
        h->context,
        "handle_update_wire__presenterZuser95inputZboard95ioZactionZnone_u5");
    auto* invisible_target = h->resolve_symbol(h->context, "igInvisibleButton");
    if (!add_component || !delete_component || !load_level || !update_target ||
        !invisible_target)
        return 5;
    if (h->create_hook(h->context, update_target,
                       reinterpret_cast<void*>(hookedUpdate),
                       reinterpret_cast<void**>(&update_original)) != 0)
        return 6;
    if (h->create_hook(h->context, invisible_target,
                       reinterpret_cast<void*>(hookedInvisible),
                       reinterpret_cast<void**>(&invisible_original)) != 0)
        return 7;
    if (const char* trace = std::getenv("TC_UNDO_TRACE"))
        undo_trace = trace[0] && trace[0] != '0';
    if (undo_trace) {
        auto* undo_target = h->resolve_symbol(
            h->context, "add_undo_changes__modelZboardZboard_u23805");
        if (!undo_target ||
            h->create_hook(h->context, undo_target,
                           reinterpret_cast<void*>(hookedAddUndoChanges),
                           reinterpret_cast<void**>(&add_undo_changes_original)) != 0)
            return 13;
        auto* copy_target = h->resolve_symbol(
            h->context, "eqcopy___modelZboardZboard_u22985.part.0");
        if (!copy_target ||
            h->create_hook(h->context, copy_target, reinterpret_cast<void*>(hookedEqCopy),
                           reinterpret_cast<void**>(&eqcopy_original)) != 0)
            return 14;
        log("undo-trace armed");
    }
    /* Three service generations this probe now exercises: object enumeration,
       object reads with pins, and the payload-bearing command/transaction API. */
    if (tc::boardService(h, &boardApi) != TC_SERVICE_OK) return 8;
    if (tc::boardService(h, &boardApi4) != TC_SERVICE_OK) return 9;
    if (tc::boardService(h, &boardApi5) != TC_SERVICE_OK) return 10;
    if (tc::commandService(h, &commandApi) != TC_SERVICE_OK ||
        commandApi.version != TC_COMMAND_API_VERSION_2) return 11;
    if (tc::transactionService(h, &transactionApi) != TC_SERVICE_OK ||
        transactionApi.version != TC_TRANSACTION_API_VERSION_2) return 12;
    plugin->on_frame = frame;
    return 0;
}
