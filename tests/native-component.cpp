#include <map>
#include <set>
#include <functional>
#include <fstream>
#include <cassert>
#include <iostream>
#include "../vendor/minhook/include/MinHook.h"
#include "../src/component_definition.hpp"
#include "../src/native_logic.hpp"
#include "../sdk/tc_native_component.h"
#include "../sdk/tc_component_storage.h"

/* A stand-in for the game's custom-tail table.  It holds the same keyspace the
   host writes, so the storage paths can be driven without the game; the find
   callback answers "is there a record for this instance" and write counts every
   commit, which is how "an unchanged write touches nothing" is asserted. */
struct FakeTail {
    std::map<uint64_t, uint64_t> values;
    uint64_t writes = 0;
    bool failWrite = false;
    static bool find(void* context, uint64_t, uint64_t, void** table) {
        if (!table) return false;
        *table = context;
        return true;
    }
    static bool read(void* context, void*, uint64_t key, uint64_t* value, bool* found) {
        const auto& self = *static_cast<FakeTail*>(context);
        const auto entry = self.values.find(key);
        *found = entry != self.values.end();
        if (*found) *value = entry->second;
        return true;
    }
    static bool write(void* context, void*, uint64_t key, uint64_t value) {
        auto& self = *static_cast<FakeTail*>(context);
        if (self.failWrite) return false;
        ++self.writes;
        self.values[key] = value;
        return true;
    }
};

/* A definition's migration callback, driven by the test: the values it observes
   and the code it answers with are both asserted, because "the record was
   migrated" and "the callback ran" are different claims. */
static int gMigrationResult = TC_COMPONENT_CONFIG_MIGRATE_OK;
static uint32_t gMigrationCalls;
static uint32_t gMigrationFromSchema, gMigrationFromBytes, gMigrationToSchema, gMigrationCapacity;
static std::vector<uint8_t> gMigrationInput;
static int migrationCallback(void* user, uint32_t from_schema, const void* from_data,
                            uint32_t from_bytes, uint32_t to_schema, void* out, uint32_t capacity) {
    ++gMigrationCalls;
    assert(user == reinterpret_cast<void*>(0x1234));
    gMigrationFromSchema = from_schema;
    gMigrationFromBytes = from_bytes;
    gMigrationToSchema = to_schema;
    gMigrationCapacity = capacity;
    gMigrationInput.assign(static_cast<const uint8_t*>(from_data),
                           static_cast<const uint8_t*>(from_data) + from_bytes);
    if (gMigrationResult != TC_COMPONENT_CONFIG_MIGRATE_OK) return gMigrationResult;
    auto* bytes = static_cast<uint8_t*>(out);
    for (uint32_t i = 0; i < capacity; ++i) bytes[i] = static_cast<uint8_t>(0xa0u + i);
    return TC_COMPONENT_CONFIG_MIGRATE_OK;
}

static void noop(TCLogicIO*) {}
static unsigned registrations;
static int registered(void*, const TCNativeComponentDefinition* d) {
    ++registrations;
    assert(tc::component_definition::valid(d));
    return 0;
}
static std::vector<std::string> lines;
static void output(const tc::TCNimString* s, void*) {
    lines.emplace_back(static_cast<const char*>(s->data)+8,s->length);
}
static void emit(const std::string& s) {
    std::vector<char> data(8+s.size());
    memcpy(data.data()+8,s.data(),s.size());
    tc::TCNimString value{s.size(),data.data()};
    tc::logic::line(&value,nullptr);
}
/* Lifecycle notifications for the instance tests: counters plus a phase check,
   so a wrong phase shows up as a negative count instead of passing silently. */
static int gLifecycleCreates;
static int gLifecycleDestroys;
static uint32_t gLifecycleConfigSeen;
/* The configuration-change notification: counted, checked for its phase, and used
   to prove the callback may call the storage services from inside (the host must
   not hold the instance lock while it runs, or this would deadlock). */
static int gConfigChanged;
/* The clone notification: fired for an instance the host duplicated, after its
   normal create, with the copied configuration in view. */
static int gClones;
static uint32_t gCloneConfigSeen;
/* The load and save notifications: the first says the configuration came out of
   the circuit's record, the second marks the moment before the game writes. */
static int gLoads;
static uint32_t gLoadConfigSeen;
static int gSaves;
static uint32_t gSaveConfigSeen;
/* Set by the save-dispatch case: the callback writes this configuration, which is
   the "commit it now, the file being written sees it" story. */
static const uint8_t* lifecycleSaveWrites;
static void lifecycleLoad(TCLogicIOV2* io) {
    if (io->phase != TC_LOGIC_LOAD) gLoads = -100;
    ++gLoads;
    if (io->config && io->config_size) gLoadConfigSeen = io->config[0];
}
static void lifecycleSave(TCLogicIOV2* io) {
    if (io->phase != TC_LOGIC_SAVE) gSaves = -100;
    ++gSaves;
    if (io->config && io->config_size) gSaveConfigSeen = io->config[0];
    if (lifecycleSaveWrites) {
        TCComponentInstanceHandle handle{};
        handle.size = sizeof(handle);
        handle.version = TC_COMPONENT_INSTANCES_API_VERSION_1;
        handle.custom_id = 0;
        uint32_t written = 0, total = 0;
        TCComponentInstanceHandle handles[8]{};
        if (tc::logic::instanceEnumerate(0, handles, 8, &written, &total) == TC_COMPONENT_INSTANCES_OK)
            for (uint32_t i = 0; i < written; ++i)
                if (handles[i].instance_id == io->instance_id) handle = handles[i];
        if (handle.instance_id)
            tc::logic::storageWriteConfig(&handle, 7, lifecycleSaveWrites, 4);
    }
}
static void lifecycleClone(TCLogicIOV2* io) {
    if (io->phase != TC_LOGIC_CLONE) gClones = -100;
    ++gClones;
    if (io->config && io->config_size) gCloneConfigSeen = io->config[0];
}
static uint32_t gConfigChangedSeen;
static int gConfigChangedServiceStatus = -99;
const TCComponentInstanceHandle* gConfigChangedHandle;
static void lifecycleConfigChanged(TCLogicIOV2* io) {
    if (io->phase != TC_LOGIC_CONFIG_CHANGED) gConfigChanged = -100;
    ++gConfigChanged;
    if (io->config && io->config_size) gConfigChangedSeen = io->config[0];
    if (gConfigChangedHandle) {
        TCComponentStorageInfoV1 info{};
        info.size = sizeof(info);
        gConfigChangedServiceStatus = tc::logic::storageInfo(gConfigChangedHandle, &info);
    }
}
static void lifecycleCreate(TCLogicIOV2* io) {
    if (io->phase != TC_LOGIC_CREATE) gLifecycleCreates = -100;
    ++gLifecycleCreates;
    if (io->state && io->state_words) io->state[0] = 0x5a5a;
    if (io->size >= offsetof(TCLogicIOV2, config_schema) + sizeof(io->config_schema) &&
        io->config && io->config_size) gLifecycleConfigSeen = io->config[0];
}
static void lifecycleDestroy(TCLogicIOV2* io) {
    if (io->phase != TC_LOGIC_DESTROY) gLifecycleDestroys = -100;
    ++gLifecycleDestroys;
}

/* The encoder writes a literal-only Snappy stream, so a test can read it back
   and check a scaffold node by node.  The 0-pin scaffolds are the ones whose
   *shape* is the contract: a source has no collector and no dependency chain,
   a sink carries one extra driver whose output goes nowhere. */
namespace reader {
struct Cursor {
    const std::vector<uint8_t>& bytes;
    size_t at = 0;
    uint64_t take(size_t size) {
        assert(at + size <= bytes.size());
        uint64_t value = 0;
        for (size_t i = 0; i < size; ++i) value |= uint64_t(bytes[at + i]) << (8 * i);
        at += size;
        return value;
    }
    void skip(size_t size) { assert(at + size <= bytes.size()); at += size; }
};
inline bool decode(const std::vector<uint8_t>& encoded, std::vector<uint8_t>& raw) {
    /* The component definition save format puts a version byte in front of the
       Snappy stream (the fixture writer does the same). */
    if (encoded.empty() || encoded[0] != 14) return false;
    size_t at = 1;
    uint64_t expected = 0;
    unsigned shift = 0;
    while (at < encoded.size()) {
        const uint8_t byte = encoded[at++];
        expected |= uint64_t(byte & 0x7f) << shift;
        if (!(byte & 0x80)) break;
        shift += 7;
    }
    while (at < encoded.size() && raw.size() < expected) {
        const uint8_t tag = encoded[at++];
        if ((tag & 3) != 0) return false;
        uint64_t length = (tag >> 2) + 1;
        if (length > 60) {
            const size_t extra = size_t(length - 60);
            if (at + extra > encoded.size()) return false;
            length = 0;
            for (size_t i = 0; i < extra; ++i) length |= uint64_t(encoded[at++]) << (8 * i);
            length += 1;
        }
        if (at + length > encoded.size() || raw.size() + length > expected) return false;
        raw.insert(raw.end(), encoded.begin() + at, encoded.begin() + at + length);
        at += size_t(length);
    }
    return raw.size() == expected;
}
/* Node kinds of a v14 definition, in file order. */
inline std::vector<uint16_t> nodeKinds(const std::vector<uint8_t>& raw) {
    Cursor cursor{raw};
    cursor.skip(8 + 4 + 8 + 8);      // id, version, gates, delay
    cursor.skip(1 + 8 + 2 + 2);      // flag, seed, (empty values), name length
    cursor.skip(1 + 2 + 2 + 2);      // bool, subcomponents, settings, name length
    cursor.skip(512);
    const uint64_t count = cursor.take(8);
    std::vector<uint16_t> kinds;
    for (uint64_t i = 0; i < count; ++i) {
        kinds.push_back(uint16_t(cursor.take(2)));
        cursor.skip(2 + 2 + 1 + 8);
        const uint64_t nameLength = cursor.take(2);
        cursor.skip(size_t(nameLength));
        const uint64_t values = cursor.take(2);
        cursor.skip(size_t(values) * 8);
        cursor.skip(8 + 2 + 8);      // data, pin index, word size
        cursor.skip(1 + 8 + 8 + 1 + 1 + 2 + 2);
    }
    return kinds;
}
/* A scaffold node's kind and its position in circuit units (eight units to a
   board cell).  The same walk as nodeKinds, keeping x/y. */
struct Node { uint16_t kind; int16_t x; int16_t y; };
inline std::vector<Node> nodes(const std::vector<uint8_t>& raw) {
    Cursor cursor{raw};
    cursor.skip(8 + 4 + 8 + 8);
    cursor.skip(1 + 8 + 2 + 2);
    cursor.skip(1 + 2 + 2 + 2);
    cursor.skip(512);
    const uint64_t count = cursor.take(8);
    std::vector<Node> result;
    for (uint64_t i = 0; i < count; ++i) {
        Node node{};
        node.kind = uint16_t(cursor.take(2));
        node.x = int16_t(cursor.take(2));
        node.y = int16_t(cursor.take(2));
        cursor.skip(1 + 8);
        const uint64_t nameLength = cursor.take(2);
        cursor.skip(size_t(nameLength));
        const uint64_t values = cursor.take(2);
        cursor.skip(size_t(values) * 8);
        cursor.skip(8 + 2 + 8);
        cursor.skip(1 + 8 + 8 + 1 + 1 + 2 + 2);
        result.push_back(node);
    }
    return result;
}
}  // namespace reader
int main() {
    tc::TCNativeComponent c;
    c.id=0xf000000000000001ULL; c.name="Declarative";
    c.inputs={{"carry",1},{"A",8},{"B",8}};
    c.outputs={{"sum",8},{"carry",1}}; c.callback=noop;
    TCHost host{};host.size=sizeof(host);host.api_version=TC_MOD_API_VERSION;
    host.register_component=registered;
    assert(c.registerWith(&host)==0 && registrations==1);
    host.size=offsetof(TCHost,register_component);
    assert(c.registerWith(&host)==-1 && registrations==1);
    assert(c.registerWith(nullptr)==-1);
    TCComponentPin pins[8];for(auto& p:pins)p={"pin",16};
    TCNativeComponentDefinition d{};d.size=sizeof(d);d.custom_id=c.id;d.name="test";
    d.inputs=pins;d.outputs=pins;d.input_count=8;d.output_count=8;d.callback=noop;
    assert(tc::component_definition::valid(&d));
    const auto bytes=tc::component_definition::encode(d);
    assert(!bytes.empty() && bytes[0]==14);
    std::ofstream file("build/declarative-8x8.data",std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());file.close();
    pins[0].bits=17;assert(!tc::component_definition::valid(&d));pins[0].bits=16;
    for(auto bad:{0u,65u}) {pins[0].bits=bad;assert(!tc::component_definition::valid(&d));}pins[0].bits=16;
    d.input_count=0;d.output_count=0;
    /* No pin at all is the decorative shape (the text-note component): it owns
       a place on the board and a configuration but runs nothing.  Its scaffold
       is the sink's single unconnected driver, so the shape is still bridgeable. */
    assert(tc::component_definition::valid(&d));
    d.output_count=8;
    assert(tc::component_definition::valid(&d));       // a source is allowed
    d.input_count=8;d.output_count=0;
    assert(tc::component_definition::valid(&d));       // a sink is allowed
    d.input_count=9;assert(!tc::component_definition::valid(&d));d.input_count=8;
    d.input_count=0;d.inputs=nullptr;d.output_count=8;
    assert(tc::component_definition::valid(&d));
    d.inputs=pins;d.input_count=8;d.outputs=nullptr;d.output_count=0;
    assert(tc::component_definition::valid(&d));
    d.inputs=pins;d.input_count=8;d.outputs=pins;d.output_count=9;
    assert(!tc::component_definition::valid(&d));d.output_count=8;
    d.custom_id=0;assert(!tc::component_definition::valid(&d));d.custom_id=c.id;
    d.callback=nullptr;assert(!tc::component_definition::valid(&d));d.callback=noop;
    d.gate_cost=UINT64_MAX;assert(!tc::component_definition::valid(&d));d.gate_cost=1;

    // 0-pin scaffolds: a source has no input collector and no dependency chain,
    // a sink keeps one driver node with an unconnected output.
    {
        auto source = d;
        TCComponentPin outPin{"Out", 1};
        source.custom_id = 0xf0000000000000a1ULL;
        source.name = "Source";
        source.inputs = nullptr; source.input_count = 0;
        source.outputs = &outPin; source.output_count = 1;
        assert(tc::component_definition::valid(&source));
        std::vector<uint8_t> raw;
        assert(reader::decode(tc::component_definition::encode(source), raw));
        const auto kinds = reader::nodeKinds(raw);
        assert(kinds.size() == 2 && kinds[0] == 0x12 && kinds[1] == 0x51);

        auto sink = d;
        TCComponentPin inPin{"In", 8};
        sink.custom_id = 0xf0000000000000a2ULL;
        sink.name = "Sink";
        sink.inputs = &inPin; sink.input_count = 1;
        sink.outputs = nullptr; sink.output_count = 0;
        assert(tc::component_definition::valid(&sink));
        raw.clear();
        assert(reader::decode(tc::component_definition::encode(sink), raw));
        const auto sinkKinds = reader::nodeKinds(raw);
        assert(sinkKinds.size() == 3 && sinkKinds[0] == 0x4f && sinkKinds[1] == 0x12 &&
               sinkKinds[2] == 0x12);

        // No pins at all: the encoder writes exactly the sink's single
        // unconnected driver and no pins, which is what the bridge binds for a
        // decorative component (the text note).
        auto decorative = d;
        decorative.custom_id = 0xf0000000000000a3ULL;
        decorative.name = "Decorative";
        decorative.inputs = nullptr; decorative.input_count = 0;
        decorative.outputs = nullptr; decorative.output_count = 0;
        assert(tc::component_definition::valid(&decorative));
        raw.clear();
        assert(reader::decode(tc::component_definition::encode(decorative), raw));
        const auto decorativeKinds = reader::nodeKinds(raw);
        assert(decorativeKinds.size() == 1 && decorativeKinds[0] == 0x12);

        // A three-input sink keeps the same relationship: three collectors, the
    // two-gate dependency chain and the one node the callback runs on.
        auto wide = sink;
        TCComponentPin three[3] = {{"A", 1}, {"B", 8}, {"C", 8}};
        wide.inputs = three; wide.input_count = 3;
        raw.clear();
        assert(reader::decode(tc::component_definition::encode(wide), raw));
        assert(reader::nodeKinds(raw).size() == 3 + 3 + 2 + 1);
    }

    /* Every node the scaffold adds sits inside the rectangle the pins span.
       The game renders a generated component's **inner circuit** for its menu,
       foundry and preview pictures, so a node placed further out shows up as a
       stray block beside the part: the dependency gates used to sit at
       (40 + 12i, 100 + 8i), which a player saw as an extra square six cells to
       the right and thirteen below an FP32 Add.  The node *order* is the
       contract, the positions are not, so this is an invariant the encoder can
       keep - and it is checked for the two shapes the scaffold can produce:
       two pins a side, and the widest V2 shape. */
    {
        auto inside = d;
        inside.custom_id = 0xf0000000000000c1ULL;
        inside.name = "Inside";
        TCComponentPin pair[2] = {{"A", 32}, {"B", 32}};
        inside.inputs = pair; inside.input_count = 2;
        inside.outputs = pair; inside.output_count = 2;
        std::vector<uint8_t> raw;
        assert(reader::decode(tc::component_definition::encode(inside, 8, 3.0f), raw));
        const auto placed = reader::nodes(raw);
        assert(placed.size() == 3 * 2 - 1 + 2 * 2);
        /* The contract's order: pins, collectors, the dependency chain, drivers,
           output pins. */
        const uint16_t expected[] = {0x4f, 0x4f, 0x12, 0x12, 0x17,
                                     0x12, 0x12, 0x51, 0x51};
        assert(placed.size() == sizeof(expected) / sizeof(expected[0]));
        for (size_t i = 0; i < placed.size(); ++i) assert(placed[i].kind == expected[i]);
        for (const auto& node : placed) {
            assert(node.x >= -24 && node.x <= 24);   /* the 3.0 lane */
            assert(node.y >= -4 && node.y <= 8);     /* the pins' own rows */
        }
    }
    {
        auto inside = d;
        inside.custom_id = 0xf0000000000000c2ULL;
        inside.name = "Inside wide";
        static TCComponentPin many[16];
        for (unsigned i = 0; i < 16; ++i) many[i] = {"pin", 1};
        inside.inputs = many; inside.input_count = 16;
        inside.outputs = many; inside.output_count = 2;
        std::vector<uint8_t> raw;
        assert(reader::decode(tc::component_definition::encode(inside, 16), raw));
        const auto placed = reader::nodes(raw);
        assert(placed.size() == 3 * 16 - 1 + 2 * 2);
        for (const auto& node : placed) {
            /* The lane is the default 2.0 here, so the pins span -18..+13 units
               across; sixteen inputs are fifteen rows apart, i.e. -60..+60. */
            assert(node.x >= -18 && node.x <= 13);
            assert(node.y >= -60 && node.y <= 60);
        }
    }

    // V2 shapes: the V1 descriptor is capped at eight pins per direction, while
    // the encoder (and the V2 registration that uses it) goes to sixteen as long
    // as the total input width stays inside the two payload words.
    {
        static TCComponentPin many[16];
        for (unsigned i = 0; i < 16; ++i) many[i] = {"pin", 1};
        auto wide = d;
        wide.custom_id = 0xf0000000000000b1ULL;
        wide.inputs = many; wide.input_count = 9; wide.outputs = many; wide.output_count = 1;
        assert(!tc::component_definition::valid(&wide));        // V1: eight pins max
        assert(tc::component_definition::validShape(&wide, 16));
        std::vector<uint8_t> raw;
        assert(reader::decode(tc::component_definition::encode(wide, 16), raw));
        assert(reader::nodeKinds(raw).size() == 9 + 9 + 8 + 1 + 1);

        wide.input_count = 16;
        assert(tc::component_definition::validShape(&wide, 16));
        raw.clear();
        assert(reader::decode(tc::component_definition::encode(wide, 16), raw));
        assert(reader::nodeKinds(raw).size() == 16 + 16 + 15 + 1 + 1);
        assert(!tc::component_definition::validShape(&wide, 8));

        // The budget is the host's, not the game's: 129 input bits cannot be
        // packed into the two payload words the generated call carries.
        static TCComponentPin budget[2] = {{"a", 64}, {"b", 64}};
        auto over = d;
        over.custom_id = 0xf0000000000000b2ULL;
        over.inputs = budget; over.input_count = 2;
        over.outputs = many; over.output_count = 1;
        assert(tc::component_definition::validShape(&over, 16));   // 128 bits exactly
        budget[1].bits = 65;                                       // out of range
        assert(!tc::component_definition::validShape(&over, 16));
        TCComponentPin three[3] = {{"a", 64}, {"b", 64}, {"c", 1}};
        auto tooWide = over;
        tooWide.inputs = three; tooWide.input_count = 3;
        assert(!tc::component_definition::validShape(&tooWide, 16));  // 129 bits
    }

    // A 64-bit pin crossing payload words used to overrun the payload array.
    // Construct payload independently, one bit at a time, then verify decoding.
    using namespace tc::logic;
    auto definition=std::make_shared<Definition>();
    definition->inputs=3;definition->outputs=1;definition->inputWidths={1,64,63};
    placeInputs(definition->inputWidths,3,definition->inputWord,definition->inputShift);
    definition->api.callback=[](TCLogicIO* io) {
        assert(io->inputs[0]==1);
        assert(io->inputs[1]==0xfedcba9876543210ULL);
        assert(io->inputs[2]==0x7123456789abcdefULL);
        io->outputs[0]=1;
    };
    auto binding=std::make_shared<Binding>();binding->definition=definition;bindings.push_back(binding);
    /* The host sizes a binding's state from the definition (eight words for a
       V1 shape); a hand-made binding has to do the same or `invoke` has
       nowhere to commit the callback's state. */
    binding->state.assign(8,0);
    const uint64_t values[]={1,0xfedcba9876543210ULL,0x7123456789abcdefULL};
    uint64_t packed[2]={};unsigned cursor=0;
    for(unsigned i=0;i<3;++i)for(unsigned b=0;b<definition->inputWidths[i];++b,++cursor)
        if((values[i]>>b)&1)packed[cursor/64]|=uint64_t(1)<<(cursor%64);
    assert(invoke(1,0,packed[0],packed[1],TC_LOGIC_CYCLE)==1);

    // Nontrivial collector emission order must not reorder callback inputs.
    originalLine=output;
    for(bool refresh:{false,true}) {
        Emission e;e.instances[1].generated=true;
        auto& i=e.instances[1];i.inputs=3;i.outputs=1;i.widths={1,64,63};i.outWidths={1};i.operands.resize(3);
        e.nodes[10]={1,0,false,true};e.nodes[11]={1,1,false,true};e.nodes[12]={1,2,false,true};e.nodes[15]={1,0,true,false};
        emission=&e;lines.clear();
        for(auto k:{2,0,1}) {
            emit("// "+std::to_string(10+k));
            emit(refresh?"let value_id"+std::to_string(10+k)+" = ~(load(<U64>, #STATE + "+std::to_string(100+k)+"))":
                "var vid"+std::to_string(10+k)+" = U64 ~(U64 vid"+std::to_string(100+k)+")");
        }
        emit("// 15");emit(refresh?"let value_id15 = ~(load(<U1>, #STATE + 14))":"var vid15 = U1 ~(U1 vid14)");
        assert(e.rewritten==1 && !i.failed);
        assert(lines.back().find(refresh?"tc_logic_peek":"tc_logic_invoke")!=std::string::npos);
        assert(i.operands[0].find("100")!=std::string::npos && i.operands[1].find("101")!=std::string::npos);
        assert(lines.back().find(">> 63")!=std::string::npos);
        emission=nullptr;
    }
    // Wide constants become runtime lookups in both generated phases, allowing
    // a UI edit to change their value without compiling the whole board again.
    for(bool refresh:{false,true}) {
        Emission e;
        e.constants[89]={4805296447517530978ULL,10,64};
        emission=&e;lines.clear();
        emit("// 89 com_constant 64 4805296447517530978");
        emit(refresh?"let value_id316 = ( 10 & 0xffffffffffffffff)":
                     "var vid316 = U64 ( 10 & 0xffffffffffffffff)");
        assert(e.rewritten==1);
        assert(lines.back().find("tc_dynamic_constant")!=std::string::npos);
        assert(lines.back().find("4805296447517530978")!=std::string::npos);
        emission=nullptr;
    }
    setDynamicConstant(7,11);
    assert(getDynamicConstant(7,3)==11 && getDynamicConstant(8,3)==3);

    // Instances: stable handles with generations, enumeration, per-instance
    // reset and release-on-recompile.  The lifecycle callbacks count their
    // notifications, which is what the host promises a V2 definition.
    {
        using namespace tc::logic;
        TCComponentLifecycleV1 lifecycle{};
        lifecycle.size = sizeof(lifecycle);
        lifecycle.version = TC_COMPONENT_LIFECYCLE_VERSION_1;
        lifecycle.on_create = &lifecycleCreate;
        lifecycle.on_destroy = &lifecycleDestroy;
        lifecycle.on_config_changed = &lifecycleConfigChanged;
        lifecycle.on_clone = &lifecycleClone;
        lifecycle.on_load = &lifecycleLoad;
        lifecycle.on_save = &lifecycleSave;

        bindings.clear();
        bindingKeys.clear();
        auto shared = std::make_shared<Definition>();
        shared->api.custom_id = 0xf0000000000000c1ULL;
        shared->api.callback = nullptr;
        shared->useV2 = true;
        shared->callbackV2 = [](TCLogicIOV2* io) {
            if (io->state && io->state_words) io->state[0] += 1;
            if (io->size >= offsetof(TCLogicIOV2, config_schema) + sizeof(io->config_schema) &&
                io->config && io->config_size) gLifecycleConfigSeen = io->config[0];
        };
        shared->stateWords = 2;
        shared->configSchema = 7;
        shared->defaultConfig = {0x11, 0x22, 0x33, 0x44};
        shared->inputs = 9;
        shared->outputs = 1;
        shared->lifecycle = &lifecycle;
        const auto bind = [&](uint64_t instance) {
            auto binding = std::make_shared<Binding>();
            binding->definition = shared;
            binding->instance = instance;
            binding->state.assign(shared->stateWords, 0);
            binding->config = shared->defaultConfig;
            restoreTailConfig(*binding);   /* what circuit() does for a new binding */
            /* ... and this is how circuit() learns about a duplication. */
            binding->clonePending =
                tc::logic::pendingClones.erase(
                    std::make_pair(shared->api.custom_id, instance)) > 0;
            binding->generation = instanceGeneration.fetch_add(1) + 1;
            bindings.push_back(binding);
            const uint64_t token = bindings.size();
            bindingKeys.emplace(std::make_pair(shared->api.custom_id, instance), token);
            {
                std::lock_guard<std::mutex> lock(binding->mutex);
                notifyCreate(*binding);
                if (binding->clonePending) {
                    binding->clonePending = false;
                    notifyClone(*binding);
                } else if (binding->loadedFromRecord) {
                    notifyLoad(*binding);
                }
            }
            return binding;
        };
        auto first = bind(0x1111111111111111ULL);
        auto second = bind(0x2222222222222222ULL);
        assert(gLifecycleCreates == 2);
        assert(gClones == 0);   // an ordinary bind is not a duplication
        assert(gLifecycleConfigSeen == 0x11);   // default config is visible to on_create
        assert(first->state[0] == 0x5a5a && second->state[0] == 0x5a5a);   // on_create wrote state

        TCComponentInstanceHandle handles[4]{};
        uint32_t written = 0, total = 0;
        assert(instanceEnumerate(0, handles, 4, &written, &total) == TC_COMPONENT_INSTANCES_OK);
        assert(written == 2 && total == 2);
        assert(handles[0].generation != handles[1].generation);
        const TCComponentInstanceHandle firstHandle = handles[0];
        const TCComponentInstanceHandle secondHandle = handles[1];
        /* The configuration callback may use the storage services; give it a
           handle to do that with before the first write fires it. */
        gConfigChangedHandle = &firstHandle;
        assert(instanceEnumerate(shared->api.custom_id, handles, 1, &written, &total) ==
               TC_COMPONENT_INSTANCES_ERR_RANGE && written == 1 && total == 2);

        TCComponentInstanceInfoV1 info{};
        info.size = sizeof(info);
        assert(instanceInfo(&handles[0], &info) == TC_COMPONENT_INSTANCES_OK);
        assert(info.state_words == 2 && info.input_count == 9 && info.output_count == 1);
        assert((info.flags & TC_COMPONENT_INSTANCE_HAS_STATE) != 0);

        // M3 storage: configuration and simulation state have separate blobs
        // and separate reset semantics.  Reads may first query the byte count.
        TCComponentStorageInfoV1 storage{};
        storage.size = sizeof(storage);
        assert(storageInfo(&firstHandle, &storage) == TC_COMPONENT_STORAGE_OK);
        assert(storage.version == TC_COMPONENT_STORAGE_INFO_VERSION_1 &&
               storage.config_schema == 7 && storage.config_size == 4 &&
               storage.state_size == 16 && storage.config_revision == 1);
        assert(storage.flags == (TC_COMPONENT_STORAGE_HAS_CONFIG | TC_COMPONENT_STORAGE_HAS_STATE));
        uint32_t configBytes = 0;
        assert(storageReadConfig(&firstHandle, nullptr, 0, &configBytes) == TC_COMPONENT_STORAGE_OK &&
               configBytes == 4);
        uint8_t shortConfig[2]{};
        assert(storageReadConfig(&firstHandle, shortConfig, sizeof(shortConfig), &configBytes) ==
               TC_COMPONENT_STORAGE_ERR_SIZE && configBytes == 4);
        uint8_t config[4]{};
        assert(storageReadConfig(&firstHandle, config, sizeof(config), &configBytes) ==
               TC_COMPONENT_STORAGE_OK && config[0] == 0x11 && config[3] == 0x44);
        const uint8_t replacement[4]{0xaa, 0xbb, 0xcc, 0xdd};
        assert(storageWriteConfig(&firstHandle, 8, replacement, sizeof(replacement)) ==
               TC_COMPONENT_STORAGE_ERR_SCHEMA);
        assert(storageWriteConfig(&firstHandle, 7, replacement, 3) ==
               TC_COMPONENT_STORAGE_ERR_SIZE);
        assert(storageWriteConfig(&firstHandle, 7, replacement, sizeof(replacement)) ==
               TC_COMPONENT_STORAGE_OK);
        /* M2 backfill: the definition hears about the change, with the new bytes
           in view, and may use the storage services from inside the callback. */
        assert(gConfigChanged == 1 && gConfigChangedSeen == 0xaa);
        if (gConfigChangedServiceStatus != TC_COMPONENT_STORAGE_OK) {
            std::cerr << "on_config_changed service status=" << gConfigChangedServiceStatus << "\n";
            assert(false);
        }
        /* Writing the bytes it already holds is not a change. */
        assert(storageWriteConfig(&firstHandle, 7, replacement, sizeof(replacement)) ==
               TC_COMPONENT_STORAGE_OK);
        assert(gConfigChanged == 1);
        gConfigChangedHandle = nullptr;
        storage.size = sizeof(storage);
        assert(storageInfo(&firstHandle, &storage) == TC_COMPONENT_STORAGE_OK &&
               storage.config_revision == 2);
        uint64_t capturedState[2]{}; uint32_t stateBytes = 0;
        assert(storageCaptureState(&firstHandle, capturedState, sizeof(capturedState), &stateBytes) ==
               TC_COMPONENT_STORAGE_OK && stateBytes == sizeof(capturedState) &&
               capturedState[0] == 0x5a5a);

        first->calls = 7;
        assert(instanceReset(&firstHandle) == TC_COMPONENT_INSTANCES_OK);
        assert(first->state[0] == 1 && first->resets == 1);   // reset ran the callback again
        assert(second->state[0] == 0x5a5a && second->resets == 0);   // untouched
        assert(first->config == std::vector<uint8_t>(replacement, replacement + 4));
        assert(gLifecycleConfigSeen == 0xaa);   // the next callback sees the atomic replacement
        assert(storageRestoreState(&firstHandle, capturedState, sizeof(capturedState)) ==
               TC_COMPONENT_STORAGE_OK && first->state[0] == 0x5a5a);

        // A superseded generation is refused rather than resolved.
        auto forged = firstHandle;
        forged.generation += 1;
        assert(instanceReset(&forged) == TC_COMPONENT_INSTANCES_ERR_STALE);
        assert(instanceInfo(&forged, &info) == TC_COMPONENT_INSTANCES_ERR_STALE);
        uint64_t words[4]{}; uint32_t count = 0;
        assert(instanceState(&forged, words, 4, &count) == TC_COMPONENT_INSTANCES_ERR_STALE);
        assert(storageReadConfig(&forged, config, sizeof(config), &configBytes) ==
               TC_COMPONENT_STORAGE_ERR_STALE);

        // A compile that contains only the second instance releases the first.
        std::set<std::pair<uint64_t, uint64_t>> survivors;
        survivors.emplace(shared->api.custom_id, second->instance);
        releaseInstances(&survivors, "test");
        assert(gLifecycleDestroys == 1);
        assert(instanceValidate(&firstHandle) == TC_COMPONENT_INSTANCES_ERR_STALE);
        assert(instanceEnumerate(0, handles, 4, &written, &total) == TC_COMPONENT_INSTANCES_OK);
        assert(written == 1 && handles[0].instance_id == second->instance);
        assert(instanceValidate(&secondHandle) == TC_COMPONENT_INSTANCES_OK);

        // The slot is reused with a fresh generation, so the old handle stays
        // stale instead of naming the new instance that took its place.
        auto third = bind(0x3333333333333333ULL);
        assert(gLifecycleCreates == 3);
        assert(third->generation != first->generation);
        assert(instanceValidate(&firstHandle) == TC_COMPONENT_INSTANCES_ERR_STALE);
        uint64_t stateWords[4]{}; count = 0;
        assert(instanceState(&secondHandle, stateWords, 4, &count) == TC_COMPONENT_INSTANCES_OK &&
               count == 2);

        /* M3 persistence: the configuration travels in the component record's own
           table.  The table below stands in for the game's, so the same code the
           host runs over a live record is driven offline: restore at bind,
           unchanged writes skipping the record, a commit, a commit that fails,
           and a stored record that belongs to another definition. */
        {
            FakeTail table;
            const uint8_t saved[4]{0xde, 0xad, 0xbe, 0xef};
            for (const auto& entry : tc::component_tail::encode(
                     shared->api.custom_id, shared->configSchema, saved, sizeof(saved)))
                table.values[entry.key] = entry.value;
            bindTailAccess({&table, &FakeTail::find, &FakeTail::read, &FakeTail::write});
            auto restored = bind(0x4444444444444444ULL);
            assert(gLifecycleConfigSeen == 0xde);          // on_create saw the stored byte
            assert(restored->config == std::vector<uint8_t>(saved, saved + sizeof(saved)));
            assert(restored->tailBound && restored->tailRecord);

            TCComponentInstanceHandle persisted{};
            TCComponentInstanceHandle others[8]{};
            assert(instanceEnumerate(shared->api.custom_id, others, 8, &written, &total) ==
                   TC_COMPONENT_INSTANCES_OK);
            bool found = false;
            for (uint32_t i = 0; i < written; ++i)
                if (others[i].instance_id == restored->instance) {
                    persisted = others[i];
                    found = true;
                }
            assert(found);
            TCComponentStorageInfoV1 reported{};
            reported.size = sizeof(reported);
            assert(storageInfo(&persisted, &reported) == TC_COMPONENT_STORAGE_OK);
            assert((reported.flags & TC_COMPONENT_STORAGE_HAS_CONFIG) != 0);
            assert((reported.flags & TC_COMPONENT_STORAGE_HAS_PERSISTENCE) != 0);
            assert(reported.config_revision == 1);   // the restore is not a write

            /* Writing the value the record already holds must not touch it. */
            const uint64_t writesBefore = table.writes;
            assert(storageWriteConfig(&persisted, shared->configSchema, saved, sizeof(saved)) ==
                   TC_COMPONENT_STORAGE_OK);
            assert(table.writes == writesBefore && restored->configRevision == 1);

            /* A real change is committed to the record and then to memory, and
               the record decodes back to exactly what was written. */
            const uint8_t replacement[4]{0x11, 0x22, 0x33, 0x44};
            assert(storageWriteConfig(&persisted, shared->configSchema, replacement,
                                      sizeof(replacement)) == TC_COMPONENT_STORAGE_OK);
            assert(table.writes > writesBefore && restored->configRevision == 2);
            assert(restored->config == std::vector<uint8_t>(replacement, replacement + 4));
            uint8_t decoded[4]{};
            assert(tc::component_tail::decode(&FakeTail::read, &table, &table, shared->api.custom_id,
                                              shared->configSchema, sizeof(decoded),
                                              decoded) == tc::component_tail::Status::Ok);
            assert(std::vector<uint8_t>(decoded, decoded + 4) ==
                   std::vector<uint8_t>(replacement, replacement + 4));

            /* An undo step is a configuration change like any other, and reports
               the bytes it restored (the record the block started from). */
            {
                const int before = gConfigChanged;
                assert(tc::logic::undoConfigEdit());
                assert(gConfigChanged == before + 1 && gConfigChangedSeen == 0xde);
                assert(tc::logic::redoConfigEdit());
                assert(gConfigChanged == before + 2 && gConfigChangedSeen == 0x11);
                uint8_t restored[4]{};
                assert(tc::component_tail::decode(&FakeTail::read, &table, &table,
                                                  shared->api.custom_id, shared->configSchema,
                                                  sizeof(restored), restored) ==
                       tc::component_tail::Status::Ok);
                assert(std::vector<uint8_t>(restored, restored + 4) ==
                       std::vector<uint8_t>(replacement, replacement + 4));
            }


            /* A record that cannot be written leaves the configuration alone
               instead of making memory and the circuit file disagree. */
            table.failWrite = true;
            const uint64_t revisionBeforeRefused = restored->configRevision;
            table.failWrite = true;
            assert(storageWriteConfig(&persisted, shared->configSchema, saved, sizeof(saved)) ==
                   TC_COMPONENT_STORAGE_ERR_UNAVAILABLE);
            assert(restored->config == std::vector<uint8_t>(replacement, replacement + 4));
            /* The revision is monotonic, not a fixed count: reverting a step moves
               it as well, so the claim here is that a refused write leaves it where
               it was. */
            assert(revisionBeforeRefused >= 2 && restored->configRevision == revisionBeforeRefused);
            table.failWrite = false;
            /* A duplicated instance: the host marks it, the record already carries
               the source's configuration, and the bind announces it after the
               normal create. */
            {
                FakeTail table;
                const uint8_t copied[4]{0x77, 0x66, 0x55, 0x44};
                for (const auto& entry : tc::component_tail::encode(
                         shared->api.custom_id, shared->configSchema, copied, sizeof(copied)))
                    table.values[entry.key] = entry.value;
                bindTailAccess({&table, &FakeTail::find, &FakeTail::read, &FakeTail::write});
                const uint64_t cloneInstance = 0x9999999999999999ULL;
                /* The host hands the copied bytes to the bind as well: at bind time
                   the record may not be readable yet (a compile works on a
                   flattened sequence). */
                tc::logic::markClone(shared->api.custom_id, cloneInstance,
                                     std::vector<uint8_t>(copied, copied + sizeof(copied)));
                const int clonesBefore = gClones;
                auto clone = bind(cloneInstance);
                assert(gClones == clonesBefore + 1 && gCloneConfigSeen == 0x77);
                assert(gLifecycleConfigSeen == 0x77);   // its create saw the copied bytes
                assert(clone->config == std::vector<uint8_t>(copied, copied + 4));
                assert(!clone->clonePending);
                clearTailAccess();
            }

            /* Load and save.  A record that installs a configuration makes the bind
               say TC_LOGIC_LOAD; a default-only instance says nothing; and the save
               dispatch reaches every live instance before the game writes. */
            {
                FakeTail table;
                const uint8_t stored[4]{0x5a, 0x5b, 0x5c, 0x5d};
                for (const auto& entry : tc::component_tail::encode(
                         shared->api.custom_id, shared->configSchema, stored, sizeof(stored)))
                    table.values[entry.key] = entry.value;
                bindTailAccess({&table, &FakeTail::find, &FakeTail::read, &FakeTail::write});
                const int loadsBefore = gLoads;
                auto loaded = bind(0xaaaaaaaaaaaaaaaaULL);
                assert(gLoads == loadsBefore + 1 && gLoadConfigSeen == 0x5a);
                assert(loaded->config == std::vector<uint8_t>(stored, stored + 4));
                /* A component with no record loads nothing to announce. */
                FakeTail empty;
                bindTailAccess({&empty, &FakeTail::find, &FakeTail::read, &FakeTail::write});
                const int loadsAfterRecord = gLoads;
                bind(0xbbbbbbbbbbbbbbbbULL);
                assert(gLoads == loadsAfterRecord);
                /* The save dispatch reaches the live instances with their own
                   configuration in view, and a write from inside it lands in the
                   record the game is about to write. */
                const int savesBefore = gSaves;
                const uint8_t fromSave[4]{0x6a, 0x6b, 0x6c, 0x6d};
                lifecycleSaveWrites = fromSave;
                tc::logic::notifySave();
                lifecycleSaveWrites = nullptr;
                assert(gSaves > savesBefore && gSaveConfigSeen != 0);
                clearTailAccess();
            }
            /* A record written by another definition is neither applied nor
               overwritten: it is what a missing Mod or an older schema leaves
               behind, and the next loader has to be able to read it. */
            FakeTail foreign;
            const uint8_t other[4]{1, 2, 3, 4};
            for (const auto& entry : tc::component_tail::encode(
                     0x9999999999999999ULL, shared->configSchema, other, sizeof(other)))
                foreign.values[entry.key] = entry.value;
            bindTailAccess({&foreign, &FakeTail::find, &FakeTail::read, &FakeTail::write});
            auto kept = bind(0x5555555555555555ULL);
            assert(kept->config == shared->defaultConfig);   // the default, not the foreign bytes
            assert(kept->tailBound && !kept->tailRecord);
            assert(foreign.writes == 0);
            const uint64_t foreignFormat = foreign.values[tc::component_tail::fieldKey(
                tc::component_tail::kFieldFormat)];
            assert(foreignFormat == tc::component_tail::kRecordFormatVersion);
            /* A grouped edit (tc.component.storage V2): the writes in between are
               ordinary writes, but the whole span is one undo step, and aborting
               one puts the configuration back without touching the stack. */
            {
                const uint8_t first[4]{0x31, 0x32, 0x33, 0x34};
                const uint8_t second[4]{0x41, 0x42, 0x43, 0x44};
                assert(storageWriteConfig(&persisted, shared->configSchema, first, 4) ==
                       TC_COMPONENT_STORAGE_OK);
                const size_t stackBefore = configUndoStack.size();
                const int changesBefore = gConfigChanged;
                assert(storageBeginEdit(&persisted) == TC_COMPONENT_STORAGE_OK);
                assert(storageBeginEdit(&persisted) == TC_COMPONENT_STORAGE_ERR_STATE);
                assert(storageWriteConfig(&persisted, shared->configSchema, second, 4) ==
                       TC_COMPONENT_STORAGE_OK);
                assert(gConfigChanged == changesBefore + 1);   // each write still notifies
                assert(configUndoStack.size() == stackBefore);  // ... but pushes no step
                assert(storageCommitEdit(&persisted) == TC_COMPONENT_STORAGE_OK);
                assert(storageCommitEdit(&persisted) == TC_COMPONENT_STORAGE_ERR_STATE);
                assert(configUndoStack.size() == stackBefore + 1);
                assert(undoConfigEdit());
                assert(restored->config == std::vector<uint8_t>(first, first + 4));
                assert(redoConfigEdit());
                assert(restored->config == std::vector<uint8_t>(second, second + 4));
                /* Abort: the bytes go back and the stack does not grow. */
                assert(storageBeginEdit(&persisted) == TC_COMPONENT_STORAGE_OK);
                assert(storageWriteConfig(&persisted, shared->configSchema, first, 4) ==
                       TC_COMPONENT_STORAGE_OK);
                const size_t beforeAbort = configUndoStack.size();
                assert(storageAbortEdit(&persisted) == TC_COMPONENT_STORAGE_OK);
                assert(storageAbortEdit(&persisted) == TC_COMPONENT_STORAGE_ERR_STATE);
                assert(restored->config == std::vector<uint8_t>(second, second + 4));
                assert(configUndoStack.size() == beforeAbort);
            }
            clearTailAccess();
        }

        /* M3 migration: a configuration saved under an older schema.  The
           definition is asked to convert it; "converted", "kept" and "refused"
           are three different outcomes, and only the first one may replace what
           the circuit carries. */
        {
            using namespace tc::component_tail;
            constexpr uint32_t kLegacySchema = 6;
            const uint8_t legacy[4]{0x0f, 0x1e, 0x2d, 0x3c};
            const auto seed = [&](FakeTail& table) {
                for (const auto& entry : encode(shared->api.custom_id, kLegacySchema, legacy,
                                               sizeof(legacy)))
                    table.values[entry.key] = entry.value;
            };
            const auto recordSchema = [&](const FakeTail& table) {
                return schemaOf(table.values.at(fieldKey(kFieldSchemaLength)));
            };
            shared->migrationVersion = TC_COMPONENT_CONFIG_MIGRATION_VERSION_1;
            shared->migrateConfig = &migrationCallback;
            shared->migrationUser = reinterpret_cast<void*>(0x1234);

            /* Converted: the callback saw the old bytes, the instance runs on
               what it returned, and the record now carries the new schema. */
            {
                FakeTail table;
                seed(table);
                gMigrationResult = TC_COMPONENT_CONFIG_MIGRATE_OK;
                gMigrationCalls = 0;
                bindTailAccess({&table, &FakeTail::find, &FakeTail::read, &FakeTail::write});
                auto migrated = bind(0x6666666666666666ULL);
                assert(gMigrationCalls == 1);
                assert(gMigrationFromSchema == kLegacySchema && gMigrationFromBytes == sizeof(legacy));
                assert(gMigrationInput == std::vector<uint8_t>(legacy, legacy + sizeof(legacy)));
                assert(gMigrationToSchema == shared->configSchema &&
                       gMigrationCapacity == shared->defaultConfig.size());
                const std::vector<uint8_t> expected{0xa0, 0xa1, 0xa2, 0xa3};
                assert(migrated->config == expected);
                assert(gLifecycleConfigSeen == 0xa0);   // on_create saw the migrated value
                assert(migrated->configRevision == 1);  // a migration is a load, not a write
                assert(recordSchema(table) == shared->configSchema);
                uint8_t decoded[4]{};
                assert(decode(&FakeTail::read, &table, &table, shared->api.custom_id,
                              shared->configSchema, sizeof(decoded), decoded) == Status::Ok);
                assert(std::vector<uint8_t>(decoded, decoded + 4) == expected);
            }
            /* Kept and refused: the instance runs on the default and the
               record's bytes are still there for a later, better migration. */
            for (int result : {TC_COMPONENT_CONFIG_MIGRATE_KEEP, TC_COMPONENT_CONFIG_MIGRATE_REJECT,
                               99}) {
                FakeTail table;
                seed(table);
                const auto before = table.values;
                gMigrationResult = result;
                gMigrationCalls = 0;
                bindTailAccess({&table, &FakeTail::find, &FakeTail::read, &FakeTail::write});
                auto kept = bind(0x7777777777777777ULL);
                assert(gMigrationCalls == 1);
                assert(kept->config == shared->defaultConfig);
                assert(recordSchema(table) == kLegacySchema);
                assert(table.values == before && table.writes == 0);
            }
            /* A definition without a migration never calls one, and old bytes
               are still preserved. */
            {
                shared->migrateConfig = nullptr;
                shared->migrationVersion = 0;
                FakeTail table;
                seed(table);
                const auto before = table.values;
                gMigrationCalls = 0;
                bindTailAccess({&table, &FakeTail::find, &FakeTail::read, &FakeTail::write});
                auto plain = bind(0x8888888888888888ULL);
                assert(gMigrationCalls == 0);
                assert(plain->config == shared->defaultConfig);
                assert(table.values == before && table.writes == 0);
            }
            clearTailAccess();
        }
        bindings.clear();
        bindingKeys.clear();
    }
    std::cout<<"PASS declarative API guards, 8x8 definition, invalid shapes, cross-word payloads, collector ordering, emission phases and runtime constants\n";
}
