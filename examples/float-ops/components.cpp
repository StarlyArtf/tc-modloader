/* FP32 Constant, FP32 Add, FP32 Display (plan 6.1 and 7.2): configuration,
   arithmetic, formatting, drawing and registration.

   What these three prove together: a decimal text an editor accepts becomes a
   bit pattern in the instance's configuration; the arithmetic component runs
   the kernel with the rounding mode stored in its own configuration and
   publishes the five flags; the display formats on the CPU from a cache written
   by the logic callback, so a paused board still shows values; and the body is
   drawn like the stock parts, with every configured value visible on the board
   instead of only in a tooltip. */

#include "components_internal.hpp"

#include "fp/decimal.hpp"
#include "fp/fp32.hpp"

#include <cstdio>
#include <atomic>
#include <cmath>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>

namespace floatops {
namespace internals {

/* Declared here so the configuration migration (which runs long before the
   table below is defined) can ask whether an id belongs to the M3/M4
   catalogue. */
struct CatalogueType;
const CatalogueType* catalogueEntry(uint64_t id);

const TCHost* host = nullptr;
tc::component_types::Api types{};
tc::component_render::ApiV2 render{};
tc::component_render::ApiV6 renderV6{};
tc::component_geometry::Api geometry{};
tc::component_geometry::ApiV3 geometryV3{};
tc::component_storage::ApiV2 storage{};
tc::component_storage::Api storageV1{};
tc::component_instances::Api instances{};
TCCommandApiV1 commands{};
bool uiReady = false;
bool renderReady = false;
bool placementPreviewReady = false;
bool commandsReady = false;

void (*getMousePos)(MousePoint*) = nullptr;
bool (*mouseClicked)(int, bool) = nullptr;
bool (*mouseReleased)(int) = nullptr;

std::mutex hitMutex;
std::map<uint64_t, InstanceBoxes> boxes;
int frameCounter = 0;

std::mutex valueMutex;
std::map<uint64_t, uint32_t> values;
std::map<uint64_t, std::string> texts;

EditorState editor;

void note(const std::string& text) {
    if (host && host->log) host->log(host->context, text.c_str());
}

uint32_t rgba(int r, int g, int b, int a = 255) {
    return static_cast<uint32_t>((a << 24) | (b << 16) | (g << 8) | r);
}
/* The stock *shape* is the game's word-width part (docs/research/
   component-appearance.md): 4.92 x 2.93 cells, the width box in the top-left
   corner, the name centred near the top, the value in the middle.  The colour is
   this family's own: every Float Ops component is purple, so a float part is
   never mistaken for a word-width integer part at a glance. */
uint32_t bodyFill() { return rgba(122, 79, 192); }
uint32_t bodyFillHover() { return rgba(140, 96, 208); }
uint32_t bodyBorder() { return rgba(67, 39, 111); }
uint32_t bodyBorderHover() { return rgba(198, 172, 240); }
uint32_t bodyText() { return rgba(255, 255, 255); }
uint32_t boxFill() { return rgba(28, 32, 36); }
uint32_t boxBorder() { return rgba(67, 39, 111); }
uint32_t boxText() { return rgba(235, 240, 245); }
uint32_t accent() { return rgba(255, 208, 110); }
uint32_t pinColor() { return rgba(214, 84, 84); }

/* ---- configuration ------------------------------------------------------ */

/* The game's compiler never learns that a *custom* component's configuration
   moved - it only knows its own parts - so after an edit a paused board keeps
   showing what it computed before.  The same is true just after re-entering a
   level: the Constant is rebound with its saved configuration, but the paused
   board may still contain the zero/default value from before that bind.

   Do not call sim.do from inside the storage or compile callback.  At those
   moments the game is still finishing the edit/new program, so the evaluation
   can run against the old program and the next frame overwrites its result.
   Instead, both paths arm a small request which the Mod's next-frame callback
   services.  ERR_STATE is retried while the new simulation model is becoming
   available; a successful pause is the same refresh the player's button uses. */
std::atomic<unsigned> pendingRefreshDelay{0};
std::atomic<unsigned> pendingRefreshAttempts{0};
std::atomic<unsigned> pendingSaveDelay{0};
std::atomic<unsigned> pendingSaveAttempts{0};
uint64_t pendingSaveRequest = 0;

void requestBoardRefresh() {
    pendingRefreshAttempts.store(120, std::memory_order_release);
    pendingRefreshDelay.store(1, std::memory_order_release);
}

void servicePendingBoardRefresh() {
    unsigned delay = pendingRefreshDelay.load(std::memory_order_acquire);
    while (delay) {
        if (pendingRefreshDelay.compare_exchange_weak(
                delay, delay - 1, std::memory_order_acq_rel, std::memory_order_acquire))
            return;
    }
    unsigned attempts = pendingRefreshAttempts.load(std::memory_order_acquire);
    if (!attempts) return;

    static tc::simulation::Api sim{};
    static bool ready = false;
    static bool probed = false;
    if (!probed) {
        probed = true;
        ready = tc::simulation::table(host, &sim);
        if (!ready)
            note("float-ops: this loader has no simulation control; a paused board "
                 "will keep its old values until it is refreshed");
    }
    if (!ready) {
        pendingRefreshAttempts.store(0, std::memory_order_release);
        return;
    }
    const int status = tc::simulation::pause(sim);
    if (status == TC_SIMULATION_OK) {
        pendingRefreshAttempts.store(0, std::memory_order_release);
        static int logged = 0;
        if (logged < 6) {
            ++logged;
            note("float-ops: the board was refreshed after a Constant bind/configuration edit");
        }
        return;
    }
    if (attempts > 1) {
        pendingRefreshAttempts.store(attempts - 1, std::memory_order_release);
        return;
    }
    pendingRefreshAttempts.store(0, std::memory_order_release);
    char detail[176] = {};
    std::snprintf(detail, sizeof(detail),
                  "float-ops: the pending board refresh expired (status %d)", status);
    note(detail);
}

void requestCircuitSave() {
    pendingSaveAttempts.store(120, std::memory_order_release);
    pendingSaveDelay.store(1, std::memory_order_release);
}

void servicePendingCircuitSave() {
    if (!commandsReady || !host) return;

    /* A queued save is asynchronous.  Do not enqueue duplicates while the
       previous commit is still being written by the game. */
    if (pendingSaveRequest) {
        TCCommandStatusV1 status{};
        status.size = sizeof(status);
        const int queried = commands.get_status(
            commands.context, pendingSaveRequest, &status, sizeof(status));
        if (queried == TC_COMMAND_OK &&
            (status.state == TC_COMMAND_STATE_QUEUED ||
             status.state == TC_COMMAND_STATE_RUNNING))
            return;
        const bool saved = queried == TC_COMMAND_OK &&
                           status.state == TC_COMMAND_STATE_SUCCEEDED &&
                           status.result == TC_COMMAND_OK;
        pendingSaveRequest = 0;
        if (saved) {
            static int logged = 0;
            if (logged++ < 6)
                note("float-ops: component configuration saved in the game's circuit file");
            /* A later edit may have arrived while this save was queued.  Its
               pending attempt stays armed and will enqueue the next save. */
        } else {
            char detail[176] = {};
            std::snprintf(detail, sizeof(detail),
                          "float-ops: the game did not complete the circuit save "
                          "(query %d, state %u, result %d)",
                          queried, status.state, status.result);
            note(detail);
        }
    }

    unsigned delay = pendingSaveDelay.load(std::memory_order_acquire);
    while (delay) {
        if (pendingSaveDelay.compare_exchange_weak(
                delay, delay - 1, std::memory_order_acq_rel, std::memory_order_acquire))
            return;
    }
    unsigned attempts = pendingSaveAttempts.load(std::memory_order_acquire);
    if (!attempts) return;

    TCGameHandle board{};
    const int current = tc::currentGameHandle(host, TC_GAME_OBJECT_BOARD, &board);
    if (current != TC_HANDLE_OK) {
        if (attempts > 1) {
            pendingSaveAttempts.store(attempts - 1, std::memory_order_release);
            return;
        }
        pendingSaveAttempts.store(0, std::memory_order_release);
        note("float-ops: configuration reached the component record, but no current "
             "Board handle was available to save the circuit");
        return;
    }

    TCCommandV1 save{};
    save.size = sizeof(save);
    save.type = TC_COMMAND_SAVE;
    save.subject = board;
    uint64_t request = 0;
    const int submitted = commands.submit(commands.context, &save, &request);
    if (submitted == TC_COMMAND_OK) {
        pendingSaveRequest = request;
        pendingSaveAttempts.store(0, std::memory_order_release);
        return;
    }
    if (attempts > 1) {
        pendingSaveAttempts.store(attempts - 1, std::memory_order_release);
        return;
    }
    pendingSaveAttempts.store(0, std::memory_order_release);
    char detail[144] = {};
    std::snprintf(detail, sizeof(detail),
                  "float-ops: could not queue the circuit save (status %d)", submitted);
    note(detail);
}

/* ---- the board the rows belong to ---------------------------------------- */

/* Leaving a level and entering it again hands the new board the *same* component
   ids, so "the instance changed" says nothing about whether the rows still
   describe this component: after a re-entry the drawer kept the rows it had
   filled on the board that was gone, and a Constant came back showing the
   default value without ever being re-read (player report of 2026-09-26).  What
   really changes is the Board handle, so it is watched once per frame and
   everything that belonged to the old board is dropped with it.

   Dropping the rows is what makes the next drawer pass re-read the instance's
   configuration - the loader restores a saved record on the first frame that can
   see it, which can be later than the frame that opened the drawer. */
bool sameBoardHandle(const TCGameHandle& left, const TCGameHandle& right) {
    return left.kind == right.kind && left.generation == right.generation &&
           left.token == right.token;
}

void forgetBoardState() {
    editor = EditorState{};
    {
        std::lock_guard<std::mutex> lock(hitMutex);
        boxes.clear();
    }
    {
        std::lock_guard<std::mutex> lock(valueMutex);
        values.clear();
        texts.clear();
    }
    /* A save queued against the old Board would be refused by its own handle
       check anyway, and the instance it names no longer exists. */
    pendingSaveRequest = 0;
    pendingSaveAttempts.store(0, std::memory_order_release);
}

void serviceBoardChange() {
    if (!host) return;
    static TCGameHandle seen{};
    static bool have = false;
    TCGameHandle board{};
    const int status = tc::currentGameHandle(host, TC_GAME_OBJECT_BOARD, &board);
    if (status != TC_HANDLE_OK) {
        if (!have) return;
        /* No board at all (the menu, or the moment between a level's teardown
           and the next entry): the rows belong to nothing, and the board that
           appears afterwards announces itself through its own handle. */
        have = false;
        seen = TCGameHandle{};
        forgetBoardState();
        note("float-ops: no board is up; the editor rows and the board caches "
             "were dropped");
        return;
    }
    if (have && sameBoardHandle(seen, board)) return;
    const bool replacing = have;
    have = true;
    seen = board;
    forgetBoardState();
    char detail[208] = {};
    std::snprintf(detail, sizeof(detail),
                  "float-ops: board entered (handle %llu/%llu)%s; the editor rows "
                  "were dropped and the next read comes from the new records",
                  static_cast<unsigned long long>(board.generation),
                  static_cast<unsigned long long>(board.token),
                  replacing ? " replacing the previous one" : "");
    note(detail);
}

ConstantConfig constantDefault() {
    ConstantConfig config{};
    config.format = kFormatBinary32;
    config.display = 0;
    config.bits = 0x3F800000u; /* 1.0: something readable on a fresh board */
    return config;
}

AddConfig addDefault() {
    AddConfig config{};
    config.format = kFormatBinary32;
    config.rounding = static_cast<uint8_t>(tcfp::FPRounding::nearest_even);
    return config;
}

DisplayConfig displayDefault() {
    DisplayConfig config{};
    config.format = kFormatBinary32;
    config.mode = static_cast<uint8_t>(tcfp::DisplayMode::decimal_and_hex);
    return config;
}

OpsConfig opsDefault() {
    OpsConfig config{};
    config.format = kFormatBinary32;
    config.rounding = static_cast<uint8_t>(tcfp::FPRounding::nearest_even);
    return config;
}

template <typename T>
T decodeOr(const uint8_t* data, uint32_t size, uint32_t schema, const T& fallback) {
    if (!data || size < sizeof(T) || schema != kConfigSchema) return fallback;
    T config{};
    std::memcpy(&config, data, sizeof(T));
    if (config.format != kFormatBinary32) return fallback;
    return config;
}

tcfp::FPRounding roundingOf(uint8_t code) {
    if (code > static_cast<uint8_t>(tcfp::FPRounding::toward_positive))
        return tcfp::FPRounding::nearest_even;
    return static_cast<tcfp::FPRounding>(code);
}

tcfp::DisplayMode displayModeOf(uint8_t code) {
    if (code > static_cast<uint8_t>(tcfp::DisplayMode::hex_only))
        return tcfp::DisplayMode::decimal_and_hex;
    return static_cast<tcfp::DisplayMode>(code);
}

const char* roundingCode(tcfp::FPRounding rounding) {
    switch (rounding) {
        case tcfp::FPRounding::nearest_even: return "RNE";
        case tcfp::FPRounding::ties_away: return "RNA";
        case tcfp::FPRounding::toward_zero: return "RTZ";
        case tcfp::FPRounding::toward_negative: return "RDN";
        case tcfp::FPRounding::toward_positive: return "RUP";
    }
    return "RNE";
}

const char* roundingName(tcfp::FPRounding rounding) {
    switch (rounding) {
        case tcfp::FPRounding::nearest_even: return "nearest, ties to even (RNE)";
        case tcfp::FPRounding::ties_away: return "nearest, ties away (RNA)";
        case tcfp::FPRounding::toward_zero: return "toward zero (RTZ)";
        case tcfp::FPRounding::toward_negative: return "toward -inf (RDN)";
        case tcfp::FPRounding::toward_positive: return "toward +inf (RUP)";
    }
    return "nearest, ties to even (RNE)";
}

const char* displayModeName(tcfp::DisplayMode mode) {
    switch (mode) {
        case tcfp::DisplayMode::decimal_and_hex: return "decimal + hex";
        case tcfp::DisplayMode::shortest: return "shortest decimal";
        case tcfp::DisplayMode::scientific: return "scientific";
        case tcfp::DisplayMode::hex_only: return "hex only";
    }
    return "decimal + hex";
}

/* A configuration an earlier release wrote: the same payload without the format
   byte.  The migration inserts the format this build implements and refuses a
   payload that is not a legal value, in which case the host keeps the record and
   the instance runs on the registered default. */
int migrateLegacyConfig(void* user, uint32_t from_schema, const void* from_data,
                        uint32_t from_bytes, uint32_t to_schema, void* out,
                        uint32_t capacity) {
    const uint64_t type = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(user));
    if (to_schema != kConfigSchema) return TC_COMPONENT_CONFIG_MIGRATE_REJECT;
    if (from_schema == kConfigSchema && from_bytes == capacity) {
        std::memcpy(out, from_data, capacity);
        return TC_COMPONENT_CONFIG_MIGRATE_OK;
    }
    /* Schema 2 is this release minus the label: the same fields, then an empty
       label where "CONST"/"ADD"/"DISP" is shown - which is exactly what the
       board showed before the label existed. */
    if (from_schema == kLabelSchema) {
        if (type == kConstantId && from_bytes == sizeof(ConstantConfig) - kLabelBytes) {
            ConstantConfig config = constantDefault();
            std::memcpy(&config, from_data, from_bytes);
            config.format = kFormatBinary32;
            std::memcpy(out, &config, sizeof(config));
            return TC_COMPONENT_CONFIG_MIGRATE_OK;
        }
        if (type == kAddId && from_bytes == sizeof(AddConfig) - kLabelBytes) {
            AddConfig config = addDefault();
            std::memcpy(&config, from_data, from_bytes);
            config.format = kFormatBinary32;
            std::memcpy(out, &config, sizeof(config));
            return TC_COMPONENT_CONFIG_MIGRATE_OK;
        }
        if (type == kDisplayId && from_bytes == sizeof(DisplayConfig) - kLabelBytes) {
            DisplayConfig config = displayDefault();
            std::memcpy(&config, from_data, from_bytes);
            config.format = kFormatBinary32;
            std::memcpy(out, &config, sizeof(config));
            return TC_COMPONENT_CONFIG_MIGRATE_OK;
        }
        /* The catalogue types are new in this release: the only payload an
           older build could have written for one of them is schema 2, this
           release's layout without the label. */
        if (catalogueEntry(type)) {
            if (from_bytes != sizeof(OpsConfig) - kLabelBytes ||
                capacity != sizeof(OpsConfig))
                return TC_COMPONENT_CONFIG_MIGRATE_REJECT;
            OpsConfig config = opsDefault();
            std::memcpy(&config, from_data, from_bytes);
            config.format = kFormatBinary32;
            std::memcpy(out, &config, sizeof(config));
            return TC_COMPONENT_CONFIG_MIGRATE_OK;
        }
        return TC_COMPONENT_CONFIG_MIGRATE_REJECT;
    }
    if (from_schema != kLegacyConfigSchema) return TC_COMPONENT_CONFIG_MIGRATE_REJECT;
    if (type == kConstantId && from_bytes == sizeof(uint32_t) &&
        capacity == sizeof(ConstantConfig)) {
        ConstantConfig config = constantDefault();
        std::memcpy(&config.bits, from_data, sizeof(config.bits));
        std::memcpy(out, &config, sizeof(config));
        return TC_COMPONENT_CONFIG_MIGRATE_OK;
    }
    if (type == kAddId && from_bytes == sizeof(uint8_t) && capacity == sizeof(AddConfig)) {
        uint8_t rounding = 0;
        std::memcpy(&rounding, from_data, sizeof(rounding));
        if (rounding > static_cast<uint8_t>(tcfp::FPRounding::toward_positive))
            return TC_COMPONENT_CONFIG_MIGRATE_REJECT;
        AddConfig config = addDefault();
        config.rounding = rounding;
        std::memcpy(out, &config, sizeof(config));
        return TC_COMPONENT_CONFIG_MIGRATE_OK;
    }
    if (type == kDisplayId && from_bytes == sizeof(uint8_t) &&
        capacity == sizeof(DisplayConfig)) {
        uint8_t mode = 0;
        std::memcpy(&mode, from_data, sizeof(mode));
        if (mode > static_cast<uint8_t>(tcfp::DisplayMode::hex_only))
            return TC_COMPONENT_CONFIG_MIGRATE_REJECT;
        DisplayConfig config = displayDefault();
        config.mode = mode;
        std::memcpy(out, &config, sizeof(config));
        return TC_COMPONENT_CONFIG_MIGRATE_OK;
    }
    return TC_COMPONENT_CONFIG_MIGRATE_REJECT;
}

/* The host's enumerate answers OK only when every matching instance fitted in
   the caller's buffer (tc_component_instances.h), so a fixed eight-handle array
   silently turns into "no instances" as soon as a board carries more native
   components than that: the player's own board had 22 of them and *every* write
   answered "写入元件配置失败" (measured 2026-09-26 - the loader's log showed the
   refused instances while the same code worked on the four-instance fixture).
   Start at a comfortable size and grow once to the total the host reports, the
   way examples/clock does it. */
bool instanceHandles(uint64_t custom_id, std::vector<TCComponentInstanceHandle>* out,
                     std::string* why = nullptr) {
    out->clear();
    size_t capacity = 16;
    uint32_t written = 0, total = 0;
    int attempt = 0;
    for (; attempt < 5; ++attempt) {
        out->assign(capacity, TCComponentInstanceHandle{});
        written = total = 0;
        const int status = tc::component_instances::enumerate(
            instances, custom_id, out->data(), static_cast<uint32_t>(out->size()), &written,
            &total);
        if (status == TC_COMPONENT_INSTANCES_OK) {
            out->resize(written);
            return true;
        }
        if (status != TC_COMPONENT_INSTANCES_ERR_RANGE) break;
        /* ERR_RANGE means "all of them did not fit", and the set can still be
           growing (the game binds a compiled board's instances over several
           frames), so ask again with room for one more than it just reported. */
        capacity = static_cast<size_t>(total) + 1u;
    }
    out->clear();
    if (why) {
        char detail[160] = {};
        std::snprintf(detail, sizeof(detail),
                      "the host reports %u instance(s) and this Mod could see %u of them "
                      "in %d attempt(s)",
                      total, written, attempt + 1);
        *why = detail;
    }
    return false;
}

const TCComponentInstanceHandle* liveHandle(const std::vector<TCComponentInstanceHandle>& handles,
                                            uint64_t instance) {
    for (const TCComponentInstanceHandle& handle : handles)
        if (handle.instance_id == instance) return &handle;
    return nullptr;
}

bool writeConfig(uint64_t instance, const void* data, uint32_t bytes) {
    if (!storage.write_config || !instances.enumerate) return false;
    std::vector<TCComponentInstanceHandle> handles;
    std::string why;
    if (!instanceHandles(0, &handles, &why)) {
        note("float-ops: this write was not attempted because " + why);
        return false;
    }
    const TCComponentInstanceHandle* handle = liveHandle(handles, instance);
    if (!handle) {
        char detail[176] = {};
        std::snprintf(detail, sizeof(detail),
                      "float-ops: no live handle for instance 0x%llx (the host lists %zu "
                      "instance(s))",
                      static_cast<unsigned long long>(instance), handles.size());
        note(detail);
        return false;
    }
    TCComponentStorageInfoV1 info{};
    info.size = sizeof(info);
    const int infoStatus = tc::component_storage::info(storageV1, *handle, &info);
    if (infoStatus != TC_COMPONENT_STORAGE_OK ||
        !(info.flags & TC_COMPONENT_STORAGE_HAS_PERSISTENCE)) {
        char detail[192] = {};
        std::snprintf(detail, sizeof(detail),
                      "float-ops: instance 0x%llx has no persistent circuit record "
                      "(storage status %d, flags 0x%X)",
                      static_cast<unsigned long long>(instance), infoStatus, info.flags);
        note(detail);
        return false;
    }
    const bool transactional = storage.begin_edit != nullptr;
    if (transactional) (void)tc::component_storage::beginEdit(storage, *handle);
    const int result = tc::component_storage::writeConfig(storageV1, *handle, kConfigSchema,
                                                         data, bytes);
    if (transactional) {
        if (result == TC_COMPONENT_STORAGE_OK)
            (void)tc::component_storage::commitEdit(storage, *handle);
        else
            (void)tc::component_storage::abortEdit(storage, *handle);
    }
    if (result != TC_COMPONENT_STORAGE_OK) {
        char detail[176] = {};
        std::snprintf(detail, sizeof(detail),
                      "float-ops: the host refused the configuration for instance 0x%llx: %s",
                      static_cast<unsigned long long>(instance),
                      tc::component_storage::errorText(result));
        note(detail);
        return false;
    }
    requestBoardRefresh();
    requestCircuitSave();
    return true;
}

bool currentConfig(uint64_t instance, uint64_t custom_id, void* out, uint32_t bytes) {
    if (!storage.read_config || !instances.enumerate) return false;
    std::vector<TCComponentInstanceHandle> handles;
    if (!instanceHandles(custom_id, &handles)) return false;
    const TCComponentInstanceHandle* handle = liveHandle(handles, instance);
    if (!handle) {
        /* The caller's idea of the type does not match the instance; the id is
           what identifies it, so one more pass over every type is enough. */
        if (!instanceHandles(0, &handles)) return false;
        handle = liveHandle(handles, instance);
        if (!handle) return false;
    }
    uint32_t read = 0;
    return tc::component_storage::readConfig(storageV1, *handle, out, bytes, &read) ==
               TC_COMPONENT_STORAGE_OK &&
           read >= bytes;
}

bool boxesOf(uint64_t instance, InstanceBoxes* out) {
    std::lock_guard<std::mutex> lock(hitMutex);
    const auto found = boxes.find(instance);
    if (found == boxes.end()) return false;
    *out = found->second;
    return true;
}

/* The drawer names the instance it is showing, and the panel has to draw its rows
   from that name alone.  The painted boxes answer for every component on screen,
   but a board that was just entered has none yet, and a component that is off
   screen never gets one - so the host's own instance list is the fallback: it is
   the same table the writes go through, and it survives a frame with no render
   pass. */
uint64_t typeOfLiveInstance(uint64_t instance) {
    if (!instances.enumerate || !instance) return 0;
    std::vector<TCComponentInstanceHandle> handles;
    if (!instanceHandles(0, &handles)) return 0;
    const TCComponentInstanceHandle* handle = liveHandle(handles, instance);
    return handle ? handle->custom_id : 0;
}

/* ---- drawing ------------------------------------------------------------ */

ScreenBox screenBox(const TCComponentRenderFrameV1& frame, float x0, float y0, float x1,
                    float y1) {
    float xs[4] = {}, ys[4] = {};
    tc::component_render::localToScreen(frame, x0, y0, &xs[0], &ys[0]);
    tc::component_render::localToScreen(frame, x1, y0, &xs[1], &ys[1]);
    tc::component_render::localToScreen(frame, x1, y1, &xs[2], &ys[2]);
    tc::component_render::localToScreen(frame, x0, y1, &xs[3], &ys[3]);
    ScreenBox box{xs[0], ys[0], xs[0], ys[0]};
    for (int i = 1; i < 4; ++i) {
        if (xs[i] < box.min_x) box.min_x = xs[i];
        if (xs[i] > box.max_x) box.max_x = xs[i];
        if (ys[i] < box.min_y) box.min_y = ys[i];
        if (ys[i] > box.max_y) box.max_y = ys[i];
    }
    return box;
}

HitBox hitBox(const TCComponentRenderFrameV1& frame, float x0, float y0, float x1,
              float y1) {
    const ScreenBox box = screenBox(frame, x0, y0, x1, y1);
    HitBox hit{};
    hit.min_x = box.min_x;
    hit.min_y = box.min_y;
    hit.max_x = box.max_x;
    hit.max_y = box.max_y;
    hit.valid = true;
    return hit;
}

void drawBox(const TCComponentRenderFrameV1& frame, float x0, float y0, float x1, float y1,
             uint32_t fill, uint32_t border, float rounding) {
    const ScreenBox box = screenBox(frame, x0, y0, x1, y1);
    if (frame.draw->rect_filled)
        frame.draw->rect_filled(frame.draw->context, box.min_x, box.min_y, box.max_x,
                                box.max_y, fill, rounding);
    if (frame.draw->rect)
        frame.draw->rect(frame.draw->context, box.min_x, box.min_y, box.max_x, box.max_y,
                         border, rounding, 1.f);
}

/* ---- text in the stock part's own measurements --------------------------

   Everything below expresses a text run the way the original parts are drawn -
   a digit height in board cells - because that is what makes a Mod component
   sit next to a CONST without looking foreign.  Two conversions are needed and
   both are measured, not guessed:

     * board cells to screen pixels: one cell is the length of the frame's x
       axis (the axes carry camera zoom, DPI and the part's rotation);
     * the size AddText takes to the digit it paints: the game paints a face at
       `kPaintedPerAsked` of the size it is given, and a painted digit is
       `kCapPerPainted` of that painted size high, so a digit is `kCapPerAsked`
       of the size asked for.  All three come from the reference capture of a
       stock CONST (18 px digit at 30.3 px per cell), cross-checked against the
       face's own metrics.

   The size itself is passed through the draw table's sized-text entry point,
   which exists for exactly this reason: `ImDrawList::AddText_Vec2` has no size
   argument, and the size the board's own text uses changes with the camera. */
float cellPixels(const TCComponentRenderFrameV1& frame) {
    const float cell = std::sqrt(frame.axis_x_x * frame.axis_x_x +
                                 frame.axis_x_y * frame.axis_x_y);
    return cell > 0.001f ? cell : 1.f;
}

float fontPixelsForCap(const TCComponentRenderFrameV1& frame, float capCells) {
    return capCells * cellPixels(frame) / kCapPerAsked;
}

/* The width a run will occupy, in board cells.  The host measures it with the
   game's own font functions, and that measurement is what the paint produces
   (checked against a capture: "DISP" measured 42 px and the ink box came out
   39 px wide on a 17.5 px face), so it is used as it stands; a loader without
   that entry point falls back to the face's advance, 0.60 of the painted
   size. */
float textWidthCells(const TCComponentRenderFrameV1& frame, float size, bool bold,
                     const char* text) {
    float width = 0.f, height = 0.f;
    if (tc::component_render::measureText(frame, size, bold, text, &width, &height))
        return width / cellPixels(frame);
    return 0.60f * size * kPaintedPerAsked * static_cast<float>(std::strlen(text)) /
           cellPixels(frame);
}

enum class TextAlign { left, centre, right };

/* One run, anchored on its ink box: (localX, localY) is the left/centre/right
   end of the ink's vertical middle, in board cells. */
void drawTextCells(const TCComponentRenderFrameV1& frame, float localX, float localY,
                   float capCells, TextAlign align, bool bold, uint32_t color,
                   const char* text) {
    if (!text || !*text) return;
    const float size = fontPixelsForCap(frame, capCells);
    const float cell = cellPixels(frame);
    const float width = textWidthCells(frame, size, bold, text);
    float x = localX;
    if (align == TextAlign::centre) x -= width * 0.5f;
    else if (align == TextAlign::right) x -= width;
    /* The ink's middle sits kCapPerAsked of the size below the origin, so the
       origin belongs that far above the row this run is centred on. */
    const float y = localY - kCapPerAsked * size / cell;
    float screenX = 0.f, screenY = 0.f;
    tc::component_render::localToScreen(frame, x, y, &screenX, &screenY);
    const bool sized = tc::component_render::drawV2(frame) != nullptr;
    if (sized) {
        tc::component_render::textSized(frame, screenX, screenY, size, color, bold, text);
        return;
    }
    /* An older loader has no way to set a size; the run still lands in the
       right place, it is simply set in whatever size the frame draws with.
       Said once, because the fix is "update the loader", not "reload the Mod". */
    static bool notedPlainText = false;
    if (!notedPlainText) {
        notedPlainText = true;
        note("float-ops: this loader has no sized text in tc.component.render; "
             "board text will not match the stock parts (update the loader)");
    }
    if (frame.draw->text) frame.draw->text(frame.draw->context, screenX, screenY, color, text);
}

/* A value that must not spill out of the body: the stock parts draw their value
   at full size and clip, which is not an option for a 9-digit float, so a run
   wider than `maxCells` is scaled down until it fits. */
void drawFittedValue(const TCComponentRenderFrameV1& frame, float localX, float localY,
                     float capCells, float maxCells, uint32_t color, const char* text) {
    if (!text || !*text) return;
    const float cell = cellPixels(frame);
    float size = capCells * cell / kCapPerAsked;
    const float width = textWidthCells(frame, size, true, text);
    if (width > maxCells) {
        size *= maxCells / width;
        size = size < 0.5f ? 0.5f : size;
    }
    const float x = localX - textWidthCells(frame, size, true, text) * 0.5f;
    const float y = localY - kCapPerAsked * size / cell;
    float screenX = 0.f, screenY = 0.f;
    tc::component_render::localToScreen(frame, x, y, &screenX, &screenY);
    if (tc::component_render::drawV2(frame))
        tc::component_render::textSized(frame, screenX, screenY, size, color, true, text);
    else if (frame.draw->text)
        frame.draw->text(frame.draw->context, screenX, screenY, color, text);
}

/* The stock part draws its pins as filled circles on the pin lane, with a short
   stub from the body edge.  The lane is fixed at x = +-2 whatever the footprint
   is, so the same two numbers work for every type here. */
void drawPin(const TCComponentRenderFrameV1& frame, float x, float y, float bodyEdge) {
    if (frame.draw->line) {
        float x1 = 0, y1 = 0, x2 = 0, y2 = 0;
        tc::component_render::localToScreen(frame, bodyEdge, y, &x1, &y1);
        tc::component_render::localToScreen(frame, x, y, &x2, &y2);
        frame.draw->line(frame.draw->context, x1, y1, x2, y2, pinColor(), 1.6f);
    }
    if (frame.draw->circle_filled) {
        float cx = 0, cy = 0;
        tc::component_render::localToScreen(frame, x, y, &cx, &cy);
        /* The radius is in screen pixels: one cell is about |axis_x| long. */
        const float cell = std::sqrt(frame.axis_x_x * frame.axis_x_x +
                                     frame.axis_x_y * frame.axis_x_y);
        frame.draw->circle_filled(frame.draw->context, cx, cy, kPinRadius * cell,
                                  pinColor());
    }
}

/* Where the body is drawn, in local cells.  It is centred on the *pins*, not on
   the instance origin: the loader puts a two-pin side on rows 0 and +1, so a
   body centred on the origin would look half a cell low and lopsided. */
struct BodyRect {
    float x0, y0, x1, y1, centerY;
};

/* The body is centred on the instance origin for every type: the pin rows only
   decide where the pins are drawn, and the stock parts do not shift their face
   for a pin row either. */
BodyRect bodyRect(float halfHeight) {
    BodyRect body{};
    body.centerY = 0.f;
    body.x0 = -kBodyHalfWidth;
    body.x1 = kBodyHalfWidth;
    body.y0 = -halfHeight;
    body.y1 = halfHeight;
    return body;
}

/* The face a type with a given pin span needs.  One or two rows on each side is
   what the stock Constant/Add/Static Value look like, and those keep the stock
   2.93-cell face unchanged; a type that puts more pins on a side (Compare has
   four outputs, Split Bits three) grows taller and centres on its pins, because
   the alternative - keeping 2.93 cells - would leave the outer pins hanging in
   the air with nothing behind them.  The width, the top-left badge, the name's
   corner and the centred symbol are the same in either case (plan 7.2). */
BodyRect bodyRectForPins(int minRow, int maxRow) {
    /* Anything that fits inside the stock face keeps exactly that face, centred
       on the component's own cell: the stock look, and (because the face is
       2.93 of the three cells the host reserves) two of them sit flush. */
    const float lowest = static_cast<float>(minRow) - kPinRadius;
    const float highest = static_cast<float>(maxRow) + kPinRadius;
    if (lowest >= -kBodyHalfHeight && highest <= kBodyHalfHeight)
        return bodyRect(kBodyHalfHeight);
    /* A side with more pins grows the face, centred on the pin span.  The face
       has to land inside whole cells, because the host stores whole cells: the
       extra rows are paid for once, and the outline stays tight to the outermost
       pins (four outputs span rows 0..3, so their face runs -0.33..3.33, which
       is exactly the four cells the declaration below asks for).  Growing by a
       fixed margin instead - the old `+ 0.6` - made a 3.2-cell face land in a
       four-cell box and left a visible band between every pair of parts. */
    const float centre = 0.5f * static_cast<float>(minRow + maxRow);
    const float needed = 0.5f * (highest - lowest);
    const float halfHeight = needed > kBodyHalfHeight ? needed : kBodyHalfHeight;
    BodyRect body{};
    body.centerY = centre;
    body.x0 = -kBodyHalfWidth;
    body.x1 = kBodyHalfWidth;
    body.y0 = centre - halfHeight;
    body.y1 = centre + halfHeight;
    return body;
}

/* The whole cells a type's face and pins occupy: the box the game hit-tests,
   drags by and reserves.  The pins' own cells count (measured: a two-pin side
   needs one more cell of clearance than a lone output does), and a multi-pin
   face is not centred on the component's own cell, so the box has to keep the
   offset the face has - which is what geometry V3's cell form is for. */
struct FootprintCells {
    int x = -2, y = -1, width = 5, height = 3;
};

/* The whole cell that holds this board-space edge.  Cells are one wide with the
   component's grid position at the centre of cell 0, so an edge lands in the
   cell above when it sits exactly on the boundary - the faces above are sized to
   stay off the boundary. */
int containingCell(float edge) { return static_cast<int>(std::floor(edge + 0.5f)); }

FootprintCells footprintCellsFor(const BodyRect& body, int minRow, int maxRow) {
    const int top = std::min(containingCell(body.y0), minRow);
    const int bottom = std::max(containingCell(body.y1), maxRow);
    FootprintCells cells{};
    cells.x = containingCell(body.x0);
    cells.width = containingCell(body.x1) - cells.x + 1;
    cells.y = top;
    cells.height = bottom - top + 1;
    return cells;
}

/* Where the generated pins sit, in board cells: the loader writes an input's y
   as `i*8 - (n-1)*4` circuit units and an output's as `i*8`, and the game turns
   a circuit coordinate into a cell with `round(x/8)` - measured in
   docs/research/custom-component-pins.md ("in0=(-3,0) in1=(-3,1)" for a
   two-pin side, "-1,0,1" for three).  These two functions are that rule. */
int inputPinRow(uint32_t index, uint32_t count) {
    return static_cast<int>(index) - static_cast<int>(count ? count - 1 : 0) / 2;
}
int outputPinRow(uint32_t index) { return static_cast<int>(index); }

/* The width box in the top left corner, the same size and inset the stock
   picture uses.  It is inert on purpose: FP16/FP64 do not exist yet, so it
   shows what the instance is instead of offering a choice that cannot be
   honoured (plan 7.2). */
float widthBoxX0(const BodyRect& body) { return body.x0 + kBoxInset; }
float widthBoxY0(const BodyRect& body) { return body.y0 + kBoxInset; }

/* Body, width box and label, in the stock colours and the stock places. */
void drawChrome(const TCComponentRenderFrameV1& frame, const BodyRect& body,
                const char* typeName, const char* label, bool hovered) {
    drawBox(frame, body.x0, body.y0, body.x1, body.y1,
            hovered ? bodyFillHover() : bodyFill(),
            hovered ? bodyBorderHover() : bodyBorder(), 0.28f);
    const float boxX0 = widthBoxX0(body);
    const float boxY0 = widthBoxY0(body);
    drawBox(frame, boxX0, boxY0, boxX0 + kBoxWidth, boxY0 + kBoxHeight, boxFill(),
            boxBorder(), 0.10f);
    /* The width digits sit in the middle of their box, exactly like the stock
       part's "32"/"8" badge. */
    drawTextCells(frame, boxX0 + kBoxWidth * 0.5f, boxY0 + kBoxHeight * 0.5f,
                  kWidthCapCells, TextAlign::centre, true, boxText(), "32");
    /* The name slot shows the component's own label when it has one, and the
       type's name otherwise - the same rule the stock parts follow (an
       unlabelled Constant prints CONST, a labelled one prints its label). */
    const char* shown = (label && *label) ? label : typeName;
    if (shown) {
        /* The name is right-aligned in the top right corner, on its own row, the
           way the stock parts print CONST/STATIC/REG - never over the value. */
        drawTextCells(frame, body.x1 - kLabelInset, boxY0 + kLabelCapCells * 0.5f,
                      kLabelCapCells, TextAlign::right, true, bodyText(), shown);
    }
}

void drawRoundingBox(const TCComponentRenderFrameV1& frame, const BodyRect& body,
                     tcfp::FPRounding rounding, bool hovered, bool open) {
    const float x0 = widthBoxX0(body) + kBoxWidth + 0.14f;
    const float x1 = x0 + 1.24f;
    const float y0 = widthBoxY0(body);
    const float y1 = y0 + kBoxHeight;
    const uint32_t border =
        open ? accent() : (hovered ? bodyBorderHover() : boxBorder());
    drawBox(frame, x0, y0, x1, y1, boxFill(), border, 0.10f);
    drawTextCells(frame, (x0 + x1) * 0.5f, (y0 + y1) * 0.5f, kWidthCapCells,
                  TextAlign::centre, true, open ? accent() : boxText(),
                  roundingCode(rounding));
}

/* ---- logic callbacks ---------------------------------------------------- */

/* Which simulation phase calls a component's callback - counted, not assumed.

   A generated component's callback runs from a node in the game's compiled
   program.  A node whose output nothing reads can be dropped from the *cycle*
   code (the board still runs, but the callback then only happens on refresh,
   which is what a player reports as "the display updates only when I refresh").
   A source and an adder keep an output that the rest of the board reads; a pure
   sink has nothing, so the census below prints the first few calls of each
   instance by phase and makes that difference visible in the log. */
void censusNote(uint64_t instance, uint32_t phase, const char* kind) {
    struct Census {
        unsigned cycle = 0;
        unsigned refresh = 0;
        unsigned nextLog = 8;   /* first line after 8 calls, then every 256 */
        unsigned logged = 0;
    };
    static std::mutex censusMutex;
    static std::map<uint64_t, Census> census;
    std::lock_guard<std::mutex> lock(censusMutex);
    Census& entry = census[instance];
    if (phase == TC_LOGIC_CYCLE) ++entry.cycle;
    else ++entry.refresh;
    if (entry.logged >= 3 || entry.cycle + entry.refresh < entry.nextLog) return;
    ++entry.logged;
    entry.nextLog += 256;
    char line[192] = {};
    std::snprintf(line, sizeof(line),
                  "float-ops M2: %s 0x%llx call census cycle=%u refresh=%u",
                  kind, static_cast<unsigned long long>(instance), entry.cycle,
                  entry.refresh);
    note(line);
}

void constantLogic(TCLogicIOV2* io) {
    if (!io) return;
    censusNote(io->instance_id, io->phase, "constant");
    const ConstantConfig config = decodeOr<ConstantConfig>(
        io->config, io->config_size, io->config_schema, constantDefault());
    if (io->outputs && io->output_count) io->outputs[0] = config.bits;
}

/* Prime the host-side output as soon as a Constant is rebound or changed, then
   defer one whole-board evaluation so wires and downstream components observe
   it too.  on_create covers every re-entry (including configurations restored
   from the level); on_config_changed covers edits without requiring the player
   to select/click the Constant afterwards. */
void constantLifecycle(TCLogicIOV2* io) {
    if (!io) return;
    constantLogic(io);
    requestBoardRefresh();
}

const TCComponentLifecycleV1 kConstantLifecycle = {
    sizeof(TCComponentLifecycleV1), TC_COMPONENT_LIFECYCLE_VERSION_1,
    &constantLifecycle, nullptr, &constantLifecycle, &constantLifecycle,
    &constantLifecycle, nullptr};

void addLogic(TCLogicIOV2* io) {
    if (!io) return;
    censusNote(io->instance_id, io->phase, "add");
    const AddConfig config = decodeOr<AddConfig>(io->config, io->config_size,
                                                io->config_schema, addDefault());
    if (io->phase == TC_LOGIC_RESET) return;
    const uint32_t a =
        io->inputs && io->input_count > 0 ? static_cast<uint32_t>(io->inputs[0]) : 0u;
    const uint32_t b =
        io->inputs && io->input_count > 1 ? static_cast<uint32_t>(io->inputs[1]) : 0u;
    const tcfp::FP32Result result = tcfp::fp32_add(a, b, roundingOf(config.rounding));
    if (io->outputs && io->output_count) io->outputs[0] = result.bits;
    if (io->outputs && io->output_count > 1) io->outputs[1] = result.flags;
}

void displayLogic(TCLogicIOV2* io) {
    if (!io) return;
    censusNote(io->instance_id, io->phase, "display");
    if (io->phase == TC_LOGIC_DESTROY || io->phase == TC_LOGIC_RESET) {
        std::lock_guard<std::mutex> lock(valueMutex);
        values.erase(io->instance_id);
        texts.erase(io->instance_id);
        return;
    }
    const uint32_t bits =
        io->inputs && io->input_count ? static_cast<uint32_t>(io->inputs[0]) : 0u;
    const DisplayConfig config = decodeOr<DisplayConfig>(
        io->config, io->config_size, io->config_schema, displayDefault());
    char text[96] = {};
    tcfp::format_value(bits, displayModeOf(config.mode), text, sizeof(text));
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(valueMutex);
        changed = values[io->instance_id] != bits || texts[io->instance_id] != text;
        values[io->instance_id] = bits;
        texts[io->instance_id] = text;
    }
    /* One line per change is the evidence the true-game case reads: the display
       saw exactly what the arithmetic produced. */
    if (changed) {
        char detail[192] = {};
        std::snprintf(detail, sizeof(detail),
                      "float-ops M2: display 0x%llx shows %s (bits=0x%08X)",
                      static_cast<unsigned long long>(io->instance_id), text, bits);
        note(detail);
    }
}

/* ---- the M3/M4 catalogue ------------------------------------------------ */

/* One table drives everything the catalogue types have in common: what the
   loader registers (pins, description), what the board prints (board name,
   symbol, whether a rounding badge belongs on the body) and what the drawer
   shows (rounding choice or a one-line explanation).  Adding a type is a row
   here plus its logic callback - the drawing, the panel rows and the
   registration path do not have to be touched again (plan 6.2, 6.3). */
struct CatalogueType {
    CatalogueInfo info;
    const TCComponentPinV2* inputs;
    uint32_t input_count;
    const TCComponentPinV2* outputs;
    uint32_t output_count;
    TCLogicCallbackV2 callback;
};

/* The pin sets, as the game's own parts name them: A/B/C in, R and Flags out,
   32-bit words, one-bit relations and the 10-bit classification bus. */
const TCComponentPinV2 kInA[] = {{"a", "A", 32, 0}};
const TCComponentPinV2 kInAB[] = {{"a", "A", 32, 0}, {"b", "B", 32, 0}};
const TCComponentPinV2 kInABC[] = {{"a", "A", 32, 0}, {"b", "B", 32, 0},
                                   {"c", "C", 32, 0}};
const TCComponentPinV2 kInI32[] = {{"i", "I", 32, 0}};
const TCComponentPinV2 kInU32[] = {{"i", "U", 32, 0}};
const TCComponentPinV2 kOutR[] = {{"r", "R", 32, 0}};
const TCComponentPinV2 kOutRFlags[] = {{"r", "R", 32, 0}, {"f", "Flags", 5, 0}};
const TCComponentPinV2 kOutI32Flags[] = {{"i", "I", 32, 0}, {"f", "Flags", 5, 0}};
const TCComponentPinV2 kOutU32Flags[] = {{"u", "U", 32, 0}, {"f", "Flags", 5, 0}};
const TCComponentPinV2 kOutRelation[] = {{"lt", "LT", 1, 0},
                                         {"eq", "EQ", 1, 0},
                                         {"gt", "GT", 1, 0},
                                         {"f", "Flags", 5, 0}};
const TCComponentPinV2 kOutClass[] = {{"c", "Class", 10, 0}};
const TCComponentPinV2 kOutSplit[] = {{"s", "Sign", 1, 0},
                                      {"e", "Exponent", 8, 0},
                                      {"m", "Significand", 23, 0}};
const TCComponentPinV2 kInSign[] = {{"s", "Sign", 1, 0},
                                    {"e", "Exponent", 8, 0},
                                    {"m", "Significand", 23, 0}};

uint32_t inputBits(const TCLogicIOV2* io, uint32_t index) {
    return io->inputs && io->input_count > index ? static_cast<uint32_t>(io->inputs[index])
                                                 : 0u;
}

void publishBits(const TCLogicIOV2* io, uint32_t bits, uint32_t index = 0) {
    if (io->outputs && io->output_count > index) io->outputs[index] = bits;
}

void publishResult(const TCLogicIOV2* io, const tcfp::FP32Result& result,
                   bool with_flags = true) {
    publishBits(io, result.bits, 0);
    if (with_flags) publishBits(io, result.flags, 1);
}

void publishInt(const TCLogicIOV2* io, const tcfp::FP32IntResult& result) {
    publishBits(io, result.value, 0);
    publishBits(io, result.flags, 1);
}

OpsConfig opsConfigOf(const TCLogicIOV2* io) {
    return decodeOr<OpsConfig>(io->config, io->config_size, io->config_schema,
                               opsDefault());
}

using BinaryRoundingOp = tcfp::FP32Result (*)(uint32_t, uint32_t, tcfp::FPRounding);
using UnaryRoundingOp = tcfp::FP32Result (*)(uint32_t, tcfp::FPRounding);

void binaryRoundingLogic(TCLogicIOV2* io, BinaryRoundingOp op, const char* kind) {
    if (!io) return;
    censusNote(io->instance_id, io->phase, kind);
    if (io->phase == TC_LOGIC_RESET) return;
    const OpsConfig config = opsConfigOf(io);
    publishResult(io, op(inputBits(io, 0), inputBits(io, 1), roundingOf(config.rounding)));
}

void unaryRoundingLogic(TCLogicIOV2* io, UnaryRoundingOp op, const char* kind) {
    if (!io) return;
    censusNote(io->instance_id, io->phase, kind);
    if (io->phase == TC_LOGIC_RESET) return;
    const OpsConfig config = opsConfigOf(io);
    publishResult(io, op(inputBits(io, 0), roundingOf(config.rounding)));
}

/* ---- M3: the operations that round ------------------------------------- */

void subtractLogic(TCLogicIOV2* io) {
    binaryRoundingLogic(io, tcfp::fp32_subtract, "subtract");
}
void multiplyLogic(TCLogicIOV2* io) {
    binaryRoundingLogic(io, tcfp::fp32_multiply, "multiply");
}
void divideLogic(TCLogicIOV2* io) {
    binaryRoundingLogic(io, tcfp::fp32_divide, "divide");
}
void squareRootLogic(TCLogicIOV2* io) {
    unaryRoundingLogic(io, tcfp::fp32_square_root, "square root");
}
void roundToIntegralLogic(TCLogicIOV2* io) {
    unaryRoundingLogic(io, tcfp::fp32_round_to_integral_exact, "round to integral");
}

/* ---- M3: the bit operations (no flags: they cannot fail) ---------------- */

void negateLogic(TCLogicIOV2* io) {
    if (!io) return;
    censusNote(io->instance_id, io->phase, "negate");
    if (io->phase == TC_LOGIC_RESET) return;
    publishBits(io, tcfp::fp32_negate(inputBits(io, 0)));
}

void absoluteLogic(TCLogicIOV2* io) {
    if (!io) return;
    censusNote(io->instance_id, io->phase, "absolute");
    if (io->phase == TC_LOGIC_RESET) return;
    publishBits(io, tcfp::fp32_absolute(inputBits(io, 0)));
}

/* ---- M3: relation and classification ----------------------------------- */

void compareLogic(TCLogicIOV2* io) {
    if (!io) return;
    censusNote(io->instance_id, io->phase, "compare");
    if (io->phase == TC_LOGIC_RESET) return;
    /* The relation is quiet (plan 2.5): exactly one of lt/eq/gt is set for
       numbers and none of them for a NaN input, which is how the player reads
       "unordered" off the three bits.  Only a signaling NaN raises NV, and that
       is what the Flags output carries. */
    const tcfp::FP32Relation relation =
        tcfp::fp32_compare(inputBits(io, 0), inputBits(io, 1));
    publishBits(io, relation.lt ? 1u : 0u, 0);
    publishBits(io, relation.eq ? 1u : 0u, 1);
    publishBits(io, relation.gt ? 1u : 0u, 2);
    publishBits(io, relation.flags, 3);
}

void classifyLogic(TCLogicIOV2* io) {
    if (!io) return;
    censusNote(io->instance_id, io->phase, "classify");
    if (io->phase == TC_LOGIC_RESET) return;
    publishBits(io, tcfp::fp32_classify(inputBits(io, 0)));
}

/* ---- M4: the extended operations --------------------------------------- */

void fusedMultiplyAddLogic(TCLogicIOV2* io) {
    if (!io) return;
    censusNote(io->instance_id, io->phase, "fused multiply add");
    if (io->phase == TC_LOGIC_RESET) return;
    const OpsConfig config = opsConfigOf(io);
    publishResult(io, tcfp::fp32_fused_multiply_add(inputBits(io, 0), inputBits(io, 1),
                                                    inputBits(io, 2),
                                                    roundingOf(config.rounding)));
}

void remainderLogic(TCLogicIOV2* io) {
    if (!io) return;
    censusNote(io->instance_id, io->phase, "remainder");
    if (io->phase == TC_LOGIC_RESET) return;
    /* IEEE remainder fixes the quotient to nearest-ties-to-even whatever the
       instance's stored mode is, so there is no rounding badge on this type
       (plan 6.3) - the configuration keeps the field only so every catalogue
       blob has the same shape. */
    publishResult(io, tcfp::fp32_remainder(inputBits(io, 0), inputBits(io, 1)));
}

void minimumLogic(TCLogicIOV2* io) {
    if (!io) return;
    censusNote(io->instance_id, io->phase, "minimum number");
    if (io->phase == TC_LOGIC_RESET) return;
    publishResult(io, tcfp::fp32_minimum_number(inputBits(io, 0), inputBits(io, 1)));
}

void maximumLogic(TCLogicIOV2* io) {
    if (!io) return;
    censusNote(io->instance_id, io->phase, "maximum number");
    if (io->phase == TC_LOGIC_RESET) return;
    publishResult(io, tcfp::fp32_maximum_number(inputBits(io, 0), inputBits(io, 1)));
}

void i32ToFp32Logic(TCLogicIOV2* io) {
    if (!io) return;
    censusNote(io->instance_id, io->phase, "i32 to fp32");
    if (io->phase == TC_LOGIC_RESET) return;
    const OpsConfig config = opsConfigOf(io);
    publishResult(io, tcfp::fp32_from_i32(static_cast<int32_t>(inputBits(io, 0)),
                                          roundingOf(config.rounding)));
}

void u32ToFp32Logic(TCLogicIOV2* io) {
    if (!io) return;
    censusNote(io->instance_id, io->phase, "u32 to fp32");
    if (io->phase == TC_LOGIC_RESET) return;
    const OpsConfig config = opsConfigOf(io);
    publishResult(io, tcfp::fp32_from_u32(inputBits(io, 0), roundingOf(config.rounding)));
}

void fp32ToI32Logic(TCLogicIOV2* io) {
    if (!io) return;
    censusNote(io->instance_id, io->phase, "fp32 to i32");
    if (io->phase == TC_LOGIC_RESET) return;
    const OpsConfig config = opsConfigOf(io);
    publishInt(io, tcfp::fp32_to_i32(inputBits(io, 0), roundingOf(config.rounding)));
}

void fp32ToU32Logic(TCLogicIOV2* io) {
    if (!io) return;
    censusNote(io->instance_id, io->phase, "fp32 to u32");
    if (io->phase == TC_LOGIC_RESET) return;
    const OpsConfig config = opsConfigOf(io);
    publishInt(io, tcfp::fp32_to_u32(inputBits(io, 0), roundingOf(config.rounding)));
}

void splitBitsLogic(TCLogicIOV2* io) {
    if (!io) return;
    censusNote(io->instance_id, io->phase, "split bits");
    if (io->phase == TC_LOGIC_RESET) return;
    /* Sign, exponent and significand are published as they are stored: the
       exponent is the biased 8-bit field and the significand is the 23-bit
       fraction, so Make Bits rebuilds the pattern bit for bit (plan 6.3). */
    const uint32_t bits = inputBits(io, 0);
    publishBits(io, (bits >> 31) & 1u, 0);
    publishBits(io, (bits >> 23) & 0xFFu, 1);
    publishBits(io, bits & 0x7FFFFFu, 2);
}

void makeBitsLogic(TCLogicIOV2* io) {
    if (!io) return;
    censusNote(io->instance_id, io->phase, "make bits");
    if (io->phase == TC_LOGIC_RESET) return;
    const uint32_t sign = inputBits(io, 0) & 1u;
    const uint32_t exponent = inputBits(io, 1) & 0xFFu;
    const uint32_t significand = inputBits(io, 2) & 0x7FFFFFu;
    publishBits(io, (sign << 31) | (exponent << 23) | significand);
}

const CatalogueType kCatalogue[] = {
    {{kSubtractId, "local.float-ops/fp32-subtract", "FP32 Subtract",
      "A − B，按元件自己的舍入模式（默认 RNE），输出 R[32] 与本次运算的 Flags[5]。",
      "SUB", "−", true, nullptr},
     kInAB, 2, kOutRFlags, 2, &subtractLogic},
    {{kMultiplyId, "local.float-ops/fp32-multiply", "FP32 Multiply",
      "A × B，按元件自己的舍入模式（默认 RNE），输出 R[32] 与本次运算的 Flags[5]。",
      "MUL", "×", true, nullptr},
     kInAB, 2, kOutRFlags, 2, &multiplyLogic},
    {{kDivideId, "local.float-ops/fp32-divide", "FP32 Divide",
      "A ÷ B，按元件自己的舍入模式（默认 RNE）；除数为零置 DZ，输出 R[32] 与 Flags[5]。",
      "DIV", "÷", true, nullptr},
     kInAB, 2, kOutRFlags, 2, &divideLogic},
    {{kSquareRootId, "local.float-ops/fp32-square-root", "FP32 Square Root",
      "√A，按元件自己的舍入模式（默认 RNE）；负数置 NV 并返回 canonical NaN。",
      "SQRT", "√", true, nullptr},
     kInA, 1, kOutRFlags, 2, &squareRootLogic},
    {{kNegateId, "local.float-ops/fp32-negate", "FP32 Negate",
      "翻转符号位，其余位原样保留（payload 不丢，不置任何 flag）。",
      "NEG", "−x", false, "位操作：只翻符号位，NaN 的 payload 原样保留，永远不会置 Flags。"},
     kInA, 1, kOutR, 1, &negateLogic},
    {{kAbsoluteId, "local.float-ops/fp32-absolute", "FP32 Absolute",
      "清掉符号位，其余位原样保留（payload 不丢，不置任何 flag）。",
      "ABS", "|x|", false, "位操作：只清符号位，NaN 的 payload 原样保留，永远不会置 Flags。"},
     kInA, 1, kOutR, 1, &absoluteLogic},
    {{kCompareId, "local.float-ops/fp32-compare", "FP32 Compare",
      "安静比较：输出 LT/EQ/GT 与 Flags[5]；有 NaN 时三个关系位全 0（无序），只有 sNaN 置 NV。",
      "CMP", "CMP", false,
      "输出 LT/EQ/GT（各 1 位）与 Flags[5]；无序时三位全 0，只有 sNaN 置 NV。"},
     kInAB, 2, kOutRelation, 4, &compareLogic},
    {{kClassifyId, "local.float-ops/fp32-classify", "FP32 Classify",
      "输出 Class[10]，位序 bit0…bit9 = -inf, -正规, -次正规, -0, +0, +次正规, +正规, +inf, sNaN, qNaN。",
      "CLS", "CLS", false,
      "Class[10] 位序 bit0…bit9 = -inf, -norm, -sub, -0, +0, +sub, +norm, +inf, sNaN, qNaN。"},
     kInA, 1, kOutClass, 1, &classifyLogic},
    {{kFusedMultiplyAddId, "local.float-ops/fp32-fma", "FP32 Fused Multiply Add",
      "A × B + C，只舍入一次（默认 RNE），输出 R[32] 与 Flags[5]。",
      "FMA", "a×b+c", true, nullptr},
     kInABC, 3, kOutRFlags, 2, &fusedMultiplyAddLogic},
    {{kRemainderId, "local.float-ops/fp32-remainder", "FP32 Remainder",
      "IEEE remainder（不是 fmod）：商的整数部分固定按最近偶数取整，不受元件舍入模式影响。",
      "REM", "REM", false,
      "IEEE remainder：商的整数按最近偶数取整，和元件舍入模式无关，所以本体上不显示舍入框。"},
     kInAB, 2, kOutRFlags, 2, &remainderLogic},
    {{kRoundToIntegralId, "local.float-ops/fp32-round-to-integral", "FP32 Round To Integral",
      "按元件自己的舍入模式取整到整数值；结果与原值不同就置 NX。",
      "RND", "RND", true, nullptr},
     kInA, 1, kOutRFlags, 2, &roundToIntegralLogic},
    {{kMinimumId, "local.float-ops/fp32-minimum", "FP32 MinimumNumber",
      "IEEE 754-2019 minimumNumber：只有一个 NaN 时取另一个；两个都是 NaN 得 canonical NaN，sNaN 置 NV。",
      "MIN", "MIN", false,
      "2019 minimumNumber：一个 NaN 时取另一个操作数，sNaN 置 NV，不做 minNum 的那种悄悄取数。"},
     kInAB, 2, kOutRFlags, 2, &minimumLogic},
    {{kMaximumId, "local.float-ops/fp32-maximum", "FP32 MaximumNumber",
      "IEEE 754-2019 maximumNumber：只有一个 NaN 时取另一个；两个都是 NaN 得 canonical NaN，sNaN 置 NV。",
      "MAX", "MAX", false,
      "2019 maximumNumber：一个 NaN 时取另一个操作数，sNaN 置 NV，不做 maxNum 的那种悄悄取数。"},
     kInAB, 2, kOutRFlags, 2, &maximumLogic},
    {{kI32ToFp32Id, "local.float-ops/i32-to-fp32", "I32 To FP32",
      "32 位有符号整数转 FP32，按元件自己的舍入模式；不能精确表示时置 NX。",
      "I2F", "I→F", true,
      "输入按二进制补码解释；不能精确表示时置 NX，永远不会置 NV。"},
     kInI32, 1, kOutRFlags, 2, &i32ToFp32Logic},
    {{kU32ToFp32Id, "local.float-ops/u32-to-fp32", "U32 To FP32",
      "32 位无符号整数转 FP32，按元件自己的舍入模式；不能精确表示时置 NX。",
      "U2F", "U→F", true,
      "输入按无符号解释；不能精确表示时置 NX，永远不会置 NV。"},
     kInU32, 1, kOutRFlags, 2, &u32ToFp32Logic},
    {{kFp32ToI32Id, "local.float-ops/fp32-to-i32", "FP32 To I32",
      "FP32 转 32 位有符号整数，按元件自己的舍入模式；NaN、无穷或越界置 NV 并饱和。",
      "F2I", "F→I", true,
      "越界/NaN/无穷置 NV 并饱和到 INT32_MAX / INT32_MIN；-0 转 0 不置 NV。"},
     kInA, 1, kOutI32Flags, 2, &fp32ToI32Logic},
    {{kFp32ToU32Id, "local.float-ops/fp32-to-u32", "FP32 To U32",
      "FP32 转 32 位无符号整数，按元件自己的舍入模式；NaN、无穷或越界置 NV 并饱和。",
      "F2U", "F→U", true,
      "越界/NaN/无穷置 NV 并饱和到 UINT32_MAX / 0；负的非零值按越界处理，-0 转 0 不置 NV。"},
     kInA, 1, kOutU32Flags, 2, &fp32ToU32Logic},
    {{kSplitBitsId, "local.float-ops/fp32-split-bits", "FP32 Split Bits",
      "把 32 位模式拆成 Sign[1]、Exponent[8]（带偏移的位段）与 Significand[23]。",
      "SPLIT", "SPLIT", false,
      "输出 Sign[1] / Exponent[8] / Significand[23]，都是原始位段，不做任何解释或规范化。"},
     kInA, 1, kOutSplit, 3, &splitBitsLogic},
    {{kMakeBitsId, "local.float-ops/fp32-make-bits", "FP32 Make Bits",
      "把 Sign[1]、Exponent[8]、Significand[23] 拼回 32 位模式（Split Bits 的逆）。",
      "MAKE", "MAKE", false,
      "输入 Sign[1] / Exponent[8] / Significand[23]，直接拼成 32 位模式，不做数值检查。"},
     kInSign, 3, kOutR, 1, &makeBitsLogic},
};

const CatalogueType* catalogueEntry(uint64_t id) {
    for (const CatalogueType& entry : kCatalogue)
        if (entry.info.id == id) return &entry;
    return nullptr;
}

/* ---- the component column's picture (tc.component.render V5) -------------

   The game draws a custom prototype's picture from a texture it asks its
   renderer for every frame ("?snapshot_cc/com_custom_<decimal id>.png",
   docs/research/component-icons.md), and it renders that picture itself: a PNG
   dropped at the requested path is read but not shown.  V5 hands the loader a
   PNG of the Mod's own to answer that request with, and the pictures shipped
   here are made by this Mod's build from *this* drawing code
   (examples/float-ops/icons.cpp runs renderCallback against a rasterising draw
   table), so the part in the component column and in the drawer's preview looks
   the way the part looks on the board.  The placement ghost is drawn by the
   ordinary callback through render V6 instead of this texture.

   The files are deployed with the package (files/asset/float-ops-icons/
   <decimal id>.png), so the path is the game root's asset directory: this
   plugin's own data directory is <game>/tc-modloader-data/plugin-data/<id>, the
   same three levels up the clock example walks.  Every failure - an older
   loader without V5, a package whose files were not deployed, a missing file -
   leaves the game's own picture in place, and the log line says which. */
void registerPictures() {
    if (!host || !host->data_directory_utf8) return;
    std::string directory =
        std::string(host->data_directory_utf8) + "/../../../asset/float-ops-icons";
    /* The true-game case has to tell "the picture is ours" from "the game drew
       its own": it points this at a directory of deliberately garish PNGs (the
       same trick tests/icon-playtest.ps1 uses for the game's own cache path), and
       turns the whole registration off with "0" for the control run.  Unset means
       the pictures the package ships. */
    if (const char* override_path = std::getenv("TC_FLOATOPS_PICTURES")) {
        if (override_path[0] == '0' && override_path[1] == '\0') {
            note("float-ops V5: picture registration disabled by TC_FLOATOPS_PICTURES");
            return;
        }
        if (*override_path) directory = override_path;
    }
    tc::component_render::ApiV5 pictures{};
    if (!tc::component_render::tableV5(host, &pictures)) {
        note("float-ops V5: this loader has no tc.component.render V5; the component "
             "column keeps the game's own picture");
        return;
    }
    int registered = 0, missing = 0;
    auto apply = [&](uint64_t id) {
        char path[1024] = {};
        std::snprintf(path, sizeof(path), "%s/%llu.png", directory.c_str(),
                      static_cast<unsigned long long>(id));
        std::ifstream probe(path, std::ios::binary);
        if (!probe) {
            ++missing;
            return;
        }
        probe.close();
        if (tc::component_render::setPicture(pictures, id, path) == TC_COMPONENT_RENDER_OK)
            ++registered;
        else
            ++missing;
    };
    apply(kConstantId);
    apply(kAddId);
    apply(kDisplayId);
    for (const CatalogueType& entry : kCatalogue) apply(entry.info.id);
    char line[320] = {};
    std::snprintf(line, sizeof(line),
                  "float-ops V5: %d of %d component pictures registered from %s",
                  registered, registered + missing, directory.c_str());
    note(line);
}

/* The pin rows a type uses, over both sides. */
void cataloguePinSpan(const CatalogueType& entry, int* minRow, int* maxRow) {
    bool any = false;
    for (uint32_t index = 0; index < entry.input_count; ++index) {
        const int row = inputPinRow(index, entry.input_count);
        if (!any) { *minRow = *maxRow = row; any = true; }
        *minRow = std::min(*minRow, row);
        *maxRow = std::max(*maxRow, row);
    }
    for (uint32_t index = 0; index < entry.output_count; ++index) {
        const int row = outputPinRow(index);
        if (!any) { *minRow = *maxRow = row; any = true; }
        *minRow = std::min(*minRow, row);
        *maxRow = std::max(*maxRow, row);
    }
}

BodyRect catalogueBody(const CatalogueType& entry) {
    int minRow = 0, maxRow = 0;
    cataloguePinSpan(entry, &minRow, &maxRow);
    return bodyRectForPins(minRow, maxRow);
}

/* One catalogue type, drawn exactly like the three M2 components: the stock
   face, the 32 badge, the name in the top right, the symbol in the middle, the
   pins on the 3.0 lane, and - only for the types whose arithmetic rounds - the
   rounding badge next to the width box.  The rounding *choice* is a row in the
   game's own drawer (components_ui.cpp), never a popup on the board. */
void drawCatalogueType(const TCComponentRenderFrameV1& frame, const CatalogueType& entry,
                       const OpsConfig& config, InstanceBoxes* out, char* shownText,
                       size_t textCapacity, char* shownName, size_t nameCapacity) {
    int minRow = 0, maxRow = 0;
    cataloguePinSpan(entry, &minRow, &maxRow);
    const BodyRect body = bodyRectForPins(minRow, maxRow);
    out->body = hitBox(frame, body.x0, body.y0, body.x1, body.y1);
    out->width = hitBox(frame, widthBoxX0(body), widthBoxY0(body),
                        widthBoxX0(body) + kBoxWidth, widthBoxY0(body) + kBoxHeight);
    out->rounding = entry.info.rounding
                        ? hitBox(frame, widthBoxX0(body) + kBoxWidth + 0.14f,
                                 widthBoxY0(body),
                                 widthBoxX0(body) + kBoxWidth + 0.14f + 1.24f,
                                 widthBoxY0(body) + kBoxHeight)
                        : out->width;
    drawChrome(frame, body, entry.info.board_name, config.label, out->body.hovered);
    if (entry.info.rounding)
        drawRoundingBox(frame, body, roundingOf(config.rounding), out->rounding.hovered,
                        false);
    drawTextCells(frame, 0.f, body.centerY, kValueCapCells, TextAlign::centre, true,
                  bodyText(), entry.info.symbol);
    for (uint32_t index = 0; index < entry.input_count; ++index)
        drawPin(frame, -kPinLaneX, static_cast<float>(inputPinRow(index, entry.input_count)),
                body.x0);
    for (uint32_t index = 0; index < entry.output_count; ++index)
        drawPin(frame, kPinLaneX, static_cast<float>(outputPinRow(index)), body.x1);
    std::snprintf(shownText, textCapacity, "%s", entry.info.symbol);
    std::snprintf(shownName, nameCapacity, "%s",
                  config.label[0] ? config.label : entry.info.board_name);
}

/* ---- render callback ---------------------------------------------------- */

void renderCallback(void*, const TCComponentRenderFrameV1* frame) {
    if (!frame || !frame->draw) return;
    InstanceBoxes entry{};
    entry.custom_id = frame->custom_id;
    /* Nothing on the body opens anything: the editor is the game's own component
       drawer (components_ui.cpp), so the boxes below exist for the hover
       highlight and for the drawer to tell this Mod's types apart. */
    /* What the body prints, for the layout line below: the render case reads it
       back to know whether the value it measures is the full-size one or a
       value that had to be shrunk to fit the body. */
    char shownText[48] = {};
    /* The name the body prints: the instance's own label, or the type's name
       when it has none (the stock rule). */
    char shownName[32] = {};
    switch (frame->custom_id) {
        case kConstantId: {
            const ConstantConfig config = decodeOr<ConstantConfig>(
                frame->config, frame->config_size, frame->config_schema,
                constantDefault());
            const BodyRect body = bodyRect(kBodyHalfHeight);
            entry.body = hitBox(*frame, body.x0, body.y0, body.x1, body.y1);
            entry.width = hitBox(*frame, widthBoxX0(body), widthBoxY0(body),
                                 widthBoxX0(body) + kBoxWidth,
                                 widthBoxY0(body) + kBoxHeight);
            entry.rounding = entry.width;
            drawChrome(*frame, body, "CONST", config.label, entry.body.hovered);
            drawPin(*frame, kPinLaneX, kPinRowFirst, body.x1);
            /* The value is printed in the middle of the body, in the same place
               and at the same size the stock Constant prints its own: the
               decimal on its own, because a constant's value is the one thing
               the player reads off the board.  A value that would not fit the
               body is shrunk until it does. */
            char shortest[40] = {};
            tcfp::format_shortest(config.bits, shortest, sizeof(shortest));
            std::snprintf(shownText, sizeof(shownText), "%s", shortest);
            std::snprintf(shownName, sizeof(shownName), "%s",
                          config.label[0] ? config.label : "CONST");
            drawFittedValue(*frame, 0.f, 0.f, kValueCapCells, kValueMaxCells,
                            bodyText(), shortest);
            break;
        }
        case kAddId: {
            const AddConfig config = decodeOr<AddConfig>(
                frame->config, frame->config_size, frame->config_schema, addDefault());
            const BodyRect body = bodyRect(kBodyHalfHeight);
            entry.body = hitBox(*frame, body.x0, body.y0, body.x1, body.y1);
            const float roundingX0 = widthBoxX0(body) + kBoxWidth + 0.14f;
            entry.width = hitBox(*frame, widthBoxX0(body), widthBoxY0(body),
                                 widthBoxX0(body) + kBoxWidth,
                                 widthBoxY0(body) + kBoxHeight);
            entry.rounding = hitBox(*frame, roundingX0, widthBoxY0(body),
                                    roundingX0 + 1.24f, widthBoxY0(body) + kBoxHeight);
            drawChrome(*frame, body, "ADD", config.label, entry.body.hovered);
            /* The rounding box shows what the instance is set to; the choice is
               made in the drawer's own row (components_ui.cpp). */
            drawRoundingBox(*frame, body, roundingOf(config.rounding),
                            entry.rounding.hovered, false);
            /* The operator sits in the middle of the body, where the stock parts
               print their value (plan 7.2). */
            drawTextCells(*frame, 0.f, 0.f, kValueCapCells, TextAlign::centre, true,
                          bodyText(), "+");
            std::snprintf(shownText, sizeof(shownText), "+");
            std::snprintf(shownName, sizeof(shownName), "%s",
                          config.label[0] ? config.label : "ADD");
            /* Pin lanes as the loader reports them for a two-pin side:
               in0=(-3,0) in1=(-3,1), out0=(3,0) out1=(3,1). */
            drawPin(*frame, -kPinLaneX, kPinRowFirst, body.x0);
            drawPin(*frame, -kPinLaneX, kPinRowSecond, body.x0);
            drawPin(*frame, kPinLaneX, kPinRowFirst, body.x1);
            drawPin(*frame, kPinLaneX, kPinRowSecond, body.x1);
            break;
        }
        case kDisplayId: {
            const DisplayConfig config = decodeOr<DisplayConfig>(
                frame->config, frame->config_size, frame->config_schema,
                displayDefault());
            const BodyRect body = bodyRect(kBodyHalfHeight);
            entry.body = hitBox(*frame, body.x0, body.y0, body.x1, body.y1);
            entry.width = hitBox(*frame, widthBoxX0(body), widthBoxY0(body),
                                 widthBoxX0(body) + kBoxWidth,
                                 widthBoxY0(body) + kBoxHeight);
            entry.rounding = entry.width;
            drawChrome(*frame, body, "DISP", config.label, entry.body.hovered);
            drawPin(*frame, -kPinLaneX, kPinRowFirst, body.x0);
            /* The value is this component's whole point, so it is split over the
               value rows: the decimal on the stock value row, the bit pattern on
               a smaller row under it, never on top of the label.  A mode that
               shows only one of them uses the value row alone. */
            {
                uint32_t shown = 0;
                {
                    std::lock_guard<std::mutex> lock(valueMutex);
                    const auto found = values.find(frame->instance_id);
                    if (found != values.end()) shown = found->second;
                }
                char decimal[40] = {};
                char hex[32] = {};
                tcfp::format_shortest(shown, decimal, sizeof(decimal));
                tcfp::format_hex(shown, hex, sizeof(hex));
                std::snprintf(shownText, sizeof(shownText), "%s", decimal);
                std::snprintf(shownName, sizeof(shownName), "%s",
                              config.label[0] ? config.label : "DISP");
                const tcfp::DisplayMode mode = displayModeOf(config.mode);
                const bool showDecimal = mode == tcfp::DisplayMode::decimal_and_hex ||
                                         mode == tcfp::DisplayMode::shortest ||
                                         mode == tcfp::DisplayMode::scientific;
                const bool showHex = mode == tcfp::DisplayMode::decimal_and_hex ||
                                     mode == tcfp::DisplayMode::hex_only;
                /* One row when the mode shows a single form, two when it shows
                   both; the row that is alone stays on the centre, which is
                   where the stock parts print their value. */
                const bool both = showDecimal && showHex;
                if (showDecimal)
                    drawFittedValue(*frame, 0.f, both ? kDisplayValueRow : 0.f,
                                    kValueCapCells, kValueMaxCells, bodyText(), decimal);
                if (showHex)
                    drawFittedValue(*frame, 0.f, both ? kDisplayHexRow : 0.f,
                                    both ? kDisplayHexCapCells : kValueCapCells,
                                    kValueMaxCells, boxText(), hex);
            }
            break;
        }
        default: {
            /* The M3/M4 catalogue: same face, same badge, same pin lane as the
               three types above, drawn from the one table. */
            const CatalogueType* catalogue = catalogueEntry(frame->custom_id);
            if (!catalogue) return;
            const OpsConfig config = decodeOr<OpsConfig>(
                frame->config, frame->config_size, frame->config_schema, opsDefault());
            drawCatalogueType(*frame, *catalogue, config, &entry, shownText,
                              sizeof(shownText), shownName, sizeof(shownName));
            break;
        }
    }
    /* One bounded line per type: the frame's scale, where the body landed on
       screen and where the pins are.  The true-game render case reads these
       numbers to measure the picture it captured instead of guessing which
       pixels belong to which component; with TC_FLOATOPS_LAYOUT set the same
       line is written every frame, which is how that case matches a picture to
       the frame it was captured from (the camera can move between the first
       frame and the capture). */
    {
        static uint64_t logged[3] = {};
        static int loggedCount = 0;
        static int traced = -1;
        if (traced < 0) {
            const char* value = std::getenv("TC_FLOATOPS_LAYOUT");
            traced = (value && *value && value[0] != '0') ? 1 : 0;
        }
        bool known = false;
        for (int i = 0; i < loggedCount; ++i)
            if (logged[i] == frame->custom_id) known = true;
        const bool first = !known && loggedCount < 3;
        if (first || traced) {
            if (first) logged[loggedCount++] = frame->custom_id;
            float pinX = 0.f, pinY = 0.f;
            tc::component_render::localToScreen(*frame, kPinLaneX, kPinRowFirst, &pinX, &pinY);
            char line[288] = {};
            std::snprintf(line, sizeof(line),
                          "float-ops render: type=0x%llx unit=%.2f px/cell "
                          "body=%.0f,%.0f..%.0f,%.0f pin0=%.0f,%.0f value=\"%s\" name=\"%s\"",
                          static_cast<unsigned long long>(frame->custom_id),
                          static_cast<double>(cellPixels(*frame)),
                          static_cast<double>(entry.body.min_x),
                          static_cast<double>(entry.body.min_y),
                          static_cast<double>(entry.body.max_x),
                          static_cast<double>(entry.body.max_y),
                          static_cast<double>(pinX), static_cast<double>(pinY),
                          shownText, shownName);
            note(line);
        }
    }
    /* A placement preview deliberately has no live instance.  It uses this same
       callback for visual parity, but must not enter the editor/hit-test cache
       under the sentinel id 0. */
    if (!frame->instance_id) return;
    std::lock_guard<std::mutex> lock(hitMutex);
    entry.frame = frameCounter;
    boxes[frame->instance_id] = entry;
    /* The body of every instance on screen, for the true-game cases: the
       player's way into a component's drawer is a click on the body, so a
       driver that wants the same drawer has to know where the bodies are.
       Rewritten once per frame into one small file - the layout the case needs
       follows the camera, and a stale rectangle would click the board instead
       of the part. */
    static const bool writeBodies = [] {
        const char* value = std::getenv("TC_FLOATOPS_BODIES");
        return value && value[0] && value[0] != '0';
    }();
    if (writeBodies && host && host->data_directory_utf8) {
        static uint64_t writtenFrame = ~UINT64_C(0);
        const int tick = frameCounter;
        if (writtenFrame != static_cast<uint64_t>(tick)) {
            writtenFrame = static_cast<uint64_t>(tick);
            char path[512] = {};
            std::snprintf(path, sizeof(path), "%s\\bodies.txt",
                          host->data_directory_utf8);
            std::FILE* file = std::fopen(path, "wb");
            if (file) {
                for (const auto& item : boxes) {
                    if (!item.second.body.valid) continue;
                    std::fprintf(file, "body 0x%llx %.2f %.2f %.2f %.2f\n",
                                 static_cast<unsigned long long>(item.first),
                                 item.second.body.min_x, item.second.body.min_y,
                                 item.second.body.max_x, item.second.body.max_y);
                }
                std::fclose(file);
            }
        }
    }
}

}  // namespace internals

/* ---- registration -------------------------------------------------------- */

bool initialize(const TCHost* host, TCPlugin* plugin) {
    using namespace internals;
    if (!host || !plugin) return false;
    internals::host = host;

    /* The name every type registers under.  The prefix is what puts the float
       types into their own folder in the game's own component column: the
       palette code splits a custom prototype's name on "/" and walks the parts
       as parent categories, so "浮点/FP32 Add" is a component called
       "FP32 Add" inside a "浮点" folder, built by the game itself
       (docs/research/palette-categories.md has the measurement).  The prefix is
       in the name on purpose - it is the only channel the pinned build offers -
       and TC_FLOATOPS_PALETTE overrides it so a test can compare the folded and
       flat menus with one binary. */
    std::string namePrefix = "浮点/";
    if (const char* prefix = std::getenv("TC_FLOATOPS_PALETTE")) namePrefix = prefix;
    auto registeredName = [&](const char* base) {
        static std::vector<std::string> keep;
        keep.push_back(namePrefix + base);
        return keep.back().c_str();
    };

    if (!tc::component_types::table(host, &types)) {
        note("float-ops M2: this loader has no tc.component.types");
        return false;
    }
    renderReady = tc::component_render::tableV2(host, &render);
    placementPreviewReady = tc::component_render::tableV6(host, &renderV6);
    const bool geometryReady = tc::component_geometry::table(host, &geometry);
    /* V3 carries the cell form of the footprint, which is what a face that is
       not centred on the component's own cell needs (the multi-pin types). */
    const bool geometryCellsReady = tc::component_geometry::tableV3(host, &geometryV3);
    const bool storageReady = tc::component_storage::tableV2(host, &storage);
    if (storageReady) std::memcpy(&storageV1, &storage, sizeof(storageV1));
    const bool instancesReady = tc::component_instances::table(host, &instances);
    commandsReady = tc::commandService(host, &commands) == TC_SERVICE_OK &&
                    commands.version == TC_COMMAND_API_VERSION_1 && commands.submit &&
                    commands.get_status;
    uiReady = tc::ui::load(host);
    if (host->engine_proc) {
        mouseClicked = reinterpret_cast<bool (*)(int, bool)>(
            host->engine_proc(host->context, "igIsMouseClicked_Bool"));
        mouseReleased = reinterpret_cast<bool (*)(int)>(
            host->engine_proc(host->context, "igIsMouseReleased_Nil"));
        void* mousePos = host->engine_proc(host->context, "igGetMousePos");
        std::memcpy(&getMousePos, &mousePos, sizeof(getMousePos));
    }

    static const TCComponentPinV2 kConstantOutputs[] = {{"r", "R", 32, 0}};
    static const TCComponentPinV2 kAddInputs[] = {{"a", "A", 32, 0}, {"b", "B", 32, 0}};
    static const TCComponentPinV2 kAddOutputs[] = {{"r", "R", 32, 0},
                                                   {"f", "Flags", 5, 0}};
    static const TCComponentPinV2 kDisplayInputs[] = {{"a", "A", 32, 0}};

    const ConstantConfig constant_config = constantDefault();
    const AddConfig add_config = addDefault();
    const DisplayConfig display_config = displayDefault();

    struct Definition {
        uint64_t id;
        const char* type_id;
        const char* name;
        const char* description;
        const TCComponentPinV2* inputs;
        uint32_t input_count;
        const TCComponentPinV2* outputs;
        uint32_t output_count;
        TCLogicCallbackV2 callback;
        const void* config;
        uint32_t config_size;
        uint64_t gate_cost;
        uint64_t delay;
    };
    /* The cost is the honest "unknown" for a sandbox package (plan 9): zero
       declared gates and zero declared cycles, because the component is
       combinational and this release does not claim a scoring model. */
    const Definition definitions[] = {
        {kConstantId, "local.float-ops/fp32-constant", "FP32 Constant",
         "输出一个 FP32 位模式；点本体可输入十进制、科学计数法或 bits:0xXXXXXXXX。",
         nullptr, 0, kConstantOutputs, 1, &constantLogic, &constant_config,
         sizeof(ConstantConfig), 0, 0},
        {kAddId, "local.float-ops/fp32-add", "FP32 Add",
         "A + B，按元件自己的舍入模式（默认 RNE），输出 R[32] 与本次运算的 Flags[5]。",
         kAddInputs, 2, kAddOutputs, 2, &addLogic, &add_config, sizeof(AddConfig), 0, 0},
        {kDisplayId, "local.float-ops/fp32-display", "FP32 Display",
         "把 32 位输入显示成人读文本；显示方式按元件保存，暂停时也刷新。",
         kDisplayInputs, 1, nullptr, 0, &displayLogic, &display_config,
         sizeof(DisplayConfig), 0, 0},
    };

    for (const Definition& definition : definitions) {
        TCComponentTypeDefinitionV2 registered{};
        registered.size = sizeof(registered);
        registered.version = TC_COMPONENT_TYPES_VERSION_2;
        registered.custom_id = definition.id;
        registered.type_id = definition.type_id;
        registered.name = registeredName(definition.name);
        registered.description = definition.description;
        registered.inputs = definition.inputs;
        registered.outputs = definition.outputs;
        registered.input_count = definition.input_count;
        registered.output_count = definition.output_count;
        registered.state_words = 0;
        registered.gate_cost = definition.gate_cost;
        registered.delay = definition.delay;
        registered.callback = definition.callback;
        registered.lifecycle =
            definition.id == kConstantId ? &kConstantLifecycle : nullptr;
        registered.config_schema = kConfigSchema;
        registered.config_size = definition.config_size;
        registered.default_config = definition.config;
        registered.config_migration_version = TC_COMPONENT_CONFIG_MIGRATION_VERSION_1;
        registered.migrate_config = &migrateLegacyConfig;
        registered.migration_user =
            reinterpret_cast<void*>(static_cast<uintptr_t>(definition.id));
        /* The pins go on the 3.0 lane, where the stock Constant and Static Value
           put theirs, so the body can be the stock 4.92 x 2.93 cells with the
           pins outside it instead of underneath it (see components_internal.hpp
           and sdk/tc_service_api.h).  A loader older than the pin-lane cut
           ignores the field and keeps the pins at 2.0, which would put them
           back inside this body; the render callback notes that case below. */
        registered.pin_lane = kPinLaneX;
        const int status = tc::component_types::registerDefinition(types, &registered);
        if (status != TC_COMPONENT_TYPES_OK) {
            char detail[192] = {};
            std::snprintf(detail, sizeof(detail),
                          "float-ops M2: registering %s failed: %s (%d)",
                          definition.type_id, tc::component_types::errorText(status),
                          status);
            note(detail);
            return false;
        }
        if (renderReady)
            (void)tc::component_render::setDrawCallback(render, definition.id,
                                                       &renderCallback);
        /* The M0 probes keep the game's default drawing; the M2 components draw
           themselves, so the stock body would be a second silhouette behind
           them. */
        if (renderReady)
            (void)tc::component_render::setDefaultDrawing(render, definition.id, false);
        if (placementPreviewReady)
            (void)tc::component_render::setPlacementPreview(renderV6, definition.id, true);
        /* The three M2 types keep the stock face centred on their own cell, so
           either form of the declaration gives the same five-by-three box. */
        if (geometryCellsReady) {
            (void)tc::component_geometry::setFootprintCells(geometryV3, definition.id, -2, -1,
                                                            5, 3);
        } else if (geometryReady) {
            (void)tc::component_geometry::setFootprint(geometry, definition.id,
                                                       kFootprintHalfWidth,
                                                       kFootprintHalfHeight);
        }
    }

    /* The M3/M4 catalogue (plan 6.2 and 6.3): the same registration, drawing and
       configuration path as the three types above, one row of the table each.
       The footprint follows the face, so a taller multi-pin type is draggable
       over the whole body. */
    const OpsConfig ops_config = opsDefault();
    for (const CatalogueType& entry : kCatalogue) {
        const BodyRect body = catalogueBody(entry);
        TCComponentTypeDefinitionV2 registered{};
        registered.size = sizeof(registered);
        registered.version = TC_COMPONENT_TYPES_VERSION_2;
        registered.custom_id = entry.info.id;
        registered.type_id = entry.info.type_id;
        registered.name = registeredName(entry.info.name);
        registered.description = entry.info.description;
        registered.inputs = entry.inputs;
        registered.outputs = entry.outputs;
        registered.input_count = entry.input_count;
        registered.output_count = entry.output_count;
        registered.state_words = 0;
        registered.gate_cost = 0;
        registered.delay = 0;
        registered.callback = entry.callback;
        registered.config_schema = kConfigSchema;
        registered.config_size = sizeof(OpsConfig);
        registered.default_config = &ops_config;
        registered.config_migration_version = TC_COMPONENT_CONFIG_MIGRATION_VERSION_1;
        registered.migrate_config = &migrateLegacyConfig;
        registered.migration_user =
            reinterpret_cast<void*>(static_cast<uintptr_t>(entry.info.id));
        registered.pin_lane = kPinLaneX;
        const int status = tc::component_types::registerDefinition(types, &registered);
        if (status != TC_COMPONENT_TYPES_OK) {
            char line[208] = {};
            std::snprintf(line, sizeof(line), "float-ops M3: registering %s failed: %s (%d)",
                          entry.info.type_id, tc::component_types::errorText(status), status);
            note(line);
            return false;
        }
        if (renderReady) {
            (void)tc::component_render::setDrawCallback(render, entry.info.id, &renderCallback);
            (void)tc::component_render::setDefaultDrawing(render, entry.info.id, false);
        }
        if (placementPreviewReady)
            (void)tc::component_render::setPlacementPreview(renderV6, entry.info.id, true);
        if (geometryReady) {
            /* The declared box is the union of the face's cells and the pins'
               cells, stated in whole cells so the host stores exactly what was
               asked for.  A centred box cannot express a multi-pin face (its
               pins start at row 0, so the face sits below the component's own
               cell): the old half-extent form there either wasted a row above
               the face or missed its lower edge, and the measured cost was a
               visible band between parts plus a drag area that hung in empty
               space (tests/float-pitch-probe.cpp). */
            int minRow = 0, maxRow = 0;
            cataloguePinSpan(entry, &minRow, &maxRow);
            const FootprintCells cells = footprintCellsFor(body, minRow, maxRow);
            if (geometryCellsReady) {
                (void)tc::component_geometry::setFootprintCells(
                    geometryV3, entry.info.id, cells.x, cells.y, cells.width, cells.height);
            } else {
                const float halfWidth = 0.5f * static_cast<float>(cells.width);
                const float halfHeight = 0.5f * static_cast<float>(cells.height);
                (void)tc::component_geometry::setFootprint(
                    geometry, entry.info.id, halfWidth > kFootprintHalfWidth
                                                   ? halfWidth
                                                   : kFootprintHalfWidth,
                    halfHeight > kFootprintHalfHeight ? halfHeight : kFootprintHalfHeight);
            }
        }
    }

    if (plugin->size >= offsetof(TCPlugin, on_frame) + sizeof(plugin->on_frame))
        plugin->on_frame = &frameCallback;

    /* The editors are rows inside the game's own component drawer, not windows
       of this Mod's own: selection is what opens them, exactly like the stock
       parts' label and value fields. */
    if (uiReady) registerEditors(host);

    char detail[400] = {};
    std::snprintf(detail, sizeof(detail),
                  "float-ops M2: constant/add/display registered; render=%s geometry=%s "
                  "storage=%s instances=%s save=%s ui=%s mouse=%s",
                  renderReady ? "ok" : "missing", geometryReady ? "ok" : "missing",
                  storageReady ? "ok" : "missing", instancesReady ? "ok" : "missing",
                  commandsReady ? "ok" : "missing", uiReady ? "ok" : "missing",
                  (mouseClicked && getMousePos) ? "ok" : "missing");
    note(detail);
    char catalogueLine[192] = {};
    std::snprintf(catalogueLine, sizeof(catalogueLine),
                  "float-ops M3/M4: %zu more types registered (subtract, multiply, divide, "
                  "square root, negate, absolute, compare, classify, fma, remainder, round, "
                  "min, max, i32/u32 both ways, split/make bits)",
                  sizeof(kCatalogue) / sizeof(kCatalogue[0]));
    note(catalogueLine);
    registerPictures();
    /* Only tc.component.types is required: without it there are no components
       at all.  The rest are what the editors need, and a loader that lacks them
       still gets a board that computes - it just cannot be edited in place, so
       the log above says exactly which piece is missing instead of refusing to
       load (plan 4.1). */
    return true;
}

int liveInstanceCount() {
    std::lock_guard<std::mutex> lock(internals::valueMutex);
    return static_cast<int>(internals::values.size());
}

size_t displayText(uint64_t instance, char* out, size_t capacity) {
    std::lock_guard<std::mutex> lock(internals::valueMutex);
    const auto found = internals::texts.find(instance);
    if (found == internals::texts.end() || !capacity) {
        if (capacity) out[0] = '\0';
        return 0;
    }
    std::snprintf(out, capacity, "%s", found->second.c_str());
    return std::strlen(out);
}

/* The catalogue as the rest of the Mod sees it (the drawer rows ask by id, so
   the panel never repeats the list of types). */
size_t catalogueCount() {
    return sizeof(internals::kCatalogue) / sizeof(internals::kCatalogue[0]);
}

const CatalogueInfo* catalogueAt(size_t index) {
    if (index >= catalogueCount()) return nullptr;
    return &internals::kCatalogue[index].info;
}

const CatalogueInfo* catalogueInfo(uint64_t custom_id) {
    const internals::CatalogueType* entry = internals::catalogueEntry(custom_id);
    return entry ? &entry->info : nullptr;
}

}  // namespace floatops
