/* tc.component.instances V2: the id the game addresses a component by.

   Two halves are pinned here, both offline:

   1. src/component_container.hpp - the rule the loader's compile walk applies to
      the flattened board sequence.  A custom component placed directly on the
      board maps to itself; one that lives inside another custom component's
      internal circuit maps to its container; records that are not custom
      instances do not participate, and a missing container field never comes
      back as 0.
   2. sdk/tc_component_instances.h - the wrapper a Mod uses: the V2 table check,
      the argument/availability/stale answers, and the field-by-field V1 view
      that lets one ApiV2 value feed the old helpers.

   Built and run by build.ps1 ("Component instances test"). */

#include "../sdk/tc_component_instances.h"
#include "../src/component_container.hpp"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

int failures = 0;

void expect(bool value, const char* what) {
    if (value) return;
    std::printf("FAIL %s\n", what);
    std::fflush(stdout);
    ++failures;
}

using tc::logic::ComponentNode;

constexpr uint64_t kTopA = 0x1111;
constexpr uint64_t kTopB = 0x2222;
constexpr uint64_t kNested = 0x3333;
constexpr uint64_t kDeep = 0x4444;

void test_container_map() {
    const std::vector<ComponentNode> nodes{
        {0x03, 0x9999, kTopA},          /* a gate inside a custom component */
        {0x4e, kTopA, kTopA},           /* board-level: its own id */
        {0x4e, kNested, kTopA},         /* inside kTopA */
        {0x4e, kTopB, 0},               /* a record with no container field */
        {0x2e, 0x8888, kTopB},          /* a constant inside a custom component */
        {0x4e, kDeep, kNested},         /* two levels down */
        {0x4e, 0, kTopA},               /* a record with no id is not an instance */
    };
    const auto owners = tc::logic::containerMap(nodes);
    expect(owners.size() == 4, "only custom records with an id become entries");
    expect(owners.at(kTopA) == kTopA, "a board-level component is its own owner");
    expect(owners.at(kNested) == kTopA, "a nested component names its container");
    expect(owners.at(kDeep) == kNested, "a deeper container is reported as it is recorded");
    expect(owners.at(kTopB) == kTopB, "a zero container field never surfaces as 0");
    expect(owners.find(0x9999) == owners.end(), "a gate is not an instance");
    expect(owners.find(0) == owners.end(), "an id-less record is skipped");
    expect(tc::logic::containerMap({}).empty(), "an empty board has no owners");
    /* A board-level component and a nested one can share an id space: the map is
       keyed by the instance, so the last record for an id wins. */
    const std::vector<ComponentNode> replayed{{0x4e, kNested, kNested},
                                              {0x4e, kNested, kTopA}};
    expect(tc::logic::containerMap(replayed).at(kNested) == kTopA,
           "a later record for the same instance replaces the earlier one");
}

/* A stand-in host table: the wrapper only ever forwards to these pointers, so
   the contract can be checked without a loader. */
struct Fake {
    static int enumerateFake(void*, uint64_t, TCComponentInstanceHandle*, uint32_t, uint32_t*,
                             uint32_t*) {
        return TC_COMPONENT_INSTANCES_OK;
    }
    static int validateFake(void*, const TCComponentInstanceHandle*) {
        return TC_COMPONENT_INSTANCES_OK;
    }
    static int infoFake(void*, const TCComponentInstanceHandle*, TCComponentInstanceInfoV1*) {
        return TC_COMPONENT_INSTANCES_OK;
    }
    static int stateFake(void*, const TCComponentInstanceHandle*, uint64_t*, uint32_t,
                         uint32_t*) {
        return TC_COMPONENT_INSTANCES_OK;
    }
    static int resetFake(void*, const TCComponentInstanceHandle*) {
        return TC_COMPONENT_INSTANCES_OK;
    }
};

uint64_t gFakeAnswer = kTopA;
int gFakeStatus = TC_COMPONENT_INSTANCES_OK;

int topLevelFake(void*, const TCComponentInstanceHandle* handle, uint64_t* out) {
    if (!out) return TC_COMPONENT_INSTANCES_ERR_ARGUMENT;
    *out = handle && handle->instance_id == kNested ? gFakeAnswer : handle->instance_id;
    return gFakeStatus;
}

TCComponentInstancesApiV2 makeApi() {
    TCComponentInstancesApiV2 api{};
    api.size = sizeof(api);
    api.version = TC_COMPONENT_INSTANCES_API_VERSION_2;
    api.context = reinterpret_cast<void*>(0x1);
    api.enumerate = &Fake::enumerateFake;
    api.validate = &Fake::validateFake;
    api.info = &Fake::infoFake;
    api.state = &Fake::stateFake;
    api.reset = &Fake::resetFake;
    api.top_level_id = &topLevelFake;
    return api;
}

void test_wrapper() {
    const auto api = makeApi();
    const auto handle = tc::component_instances::makeHandle(0x1234, kNested, 7);
    expect(handle.size == sizeof(handle), "makeHandle fills the size");
    expect(handle.version == TC_COMPONENT_INSTANCES_API_VERSION_1,
           "a handle keeps the V1 wording it always had");

    uint64_t top = 0;
    gFakeStatus = TC_COMPONENT_INSTANCES_OK;
    gFakeAnswer = kTopA;
    expect(tc::component_instances::topLevelId(api, handle, &top) == TC_COMPONENT_INSTANCES_OK,
           "the call succeeds while the handle is live");
    expect(top == kTopA, "the container id comes back verbatim");
    expect(tc::component_instances::topLevelId(api, handle, nullptr) ==
               TC_COMPONENT_INSTANCES_ERR_ARGUMENT,
           "a null answer is an argument error");

    /* A board-level component answers with itself. */
    const auto own = tc::component_instances::makeHandle(0x1234, kTopB, 3);
    expect(tc::component_instances::topLevelId(api, own, &top) == TC_COMPONENT_INSTANCES_OK &&
               top == kTopB,
           "a component on the board is its own top level");

    /* A stale handle must not leave a half-filled answer behind. */
    gFakeStatus = TC_COMPONENT_INSTANCES_ERR_STALE;
    top = 0xdead;
    expect(tc::component_instances::topLevelId(api, handle, &top) ==
               TC_COMPONENT_INSTANCES_ERR_STALE,
           "a stale handle is reported as stale");
    expect(top == 0, "a refused call does not publish a stale id");

    /* A loader that only knows V1 has no such call. */
    auto noTop = api;
    noTop.top_level_id = nullptr;
    expect(tc::component_instances::topLevelId(noTop, handle, &top) ==
               TC_COMPONENT_INSTANCES_ERR_UNAVAILABLE,
           "a V1-only table reports the call as unavailable");

    /* The V1 half of a V2 table must keep the same five entry points, so a Mod
       that needs both does not have to query the service twice. */
    const auto view = tc::component_instances::v1(api);
    expect(view.context == api.context, "the V1 view keeps the context");
    expect(view.enumerate == api.enumerate && view.validate == api.validate &&
               view.info == api.info && view.state == api.state && view.reset == api.reset,
           "the V1 view forwards every V1 entry point");
    expect(view.version == TC_COMPONENT_INSTANCES_API_VERSION_1 &&
               view.size == sizeof(TCComponentInstancesApiV1),
           "the V1 view announces the V1 size and version");
}

}  // namespace

int main() {
    test_container_map();
    test_wrapper();
    if (failures) {
        std::printf("FAIL component instances (%d)\n", failures);
        return 1;
    }
    std::printf("PASS component instances: the container rule and the V2 wrapper are pinned\n");
    return 0;
}
