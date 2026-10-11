#include "../sdk/tc_verilog_project.hpp"

#include <cassert>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

struct Host {
    unsigned registered = 0;
    unsigned failAt = 0;
    std::vector<uint64_t> ids;
    std::vector<std::string> names;
    /* tc.component.runtime: on by default, so a test can also play a loader that
       predates the service (runtime = false) or one that refuses because a board
       is up (runtimeError = TC_COMPONENT_TYPES_ERR_STATE). */
    bool runtime = true;
    int runtimeError = TC_COMPONENT_TYPES_OK;
    std::vector<uint64_t> live;
    std::vector<uint64_t> replaced;
    std::vector<uint64_t> unregistered;

    static int oldEntry(void*, const TCComponentTypeDefinitionV2*) {
        return TC_COMPONENT_TYPES_ERR_UNSUPPORTED;
    }
    static int add(void* context, const TCComponentTypeDefinitionV3* definition) {
        auto& self = *static_cast<Host*>(context);
        ++self.registered;
        if (self.failAt && self.registered == self.failAt) return TC_COMPONENT_TYPES_ERR_GAME;
        self.ids.push_back(definition->custom_id);
        self.names.emplace_back(definition->name ? definition->name : "");
        return TC_COMPONENT_TYPES_OK;
    }
    static int registerLive(void* context, const TCComponentTypeDefinitionV3* definition) {
        auto& self = *static_cast<Host*>(context);
        if (!definition || !definition->custom_id) return TC_COMPONENT_TYPES_ERR_ARGUMENT;
        if (self.runtimeError != TC_COMPONENT_TYPES_OK) return self.runtimeError;
        self.live.push_back(definition->custom_id);
        return TC_COMPONENT_TYPES_OK;
    }
    static int replaceLive(void* context, const TCComponentTypeDefinitionV3* previous,
                           const TCComponentTypeDefinitionV3* replacement) {
        auto& self = *static_cast<Host*>(context);
        if (!previous || !replacement || !previous->custom_id ||
            previous->custom_id != replacement->custom_id)
            return TC_COMPONENT_TYPES_ERR_ARGUMENT;
        if (self.runtimeError != TC_COMPONENT_TYPES_OK) return self.runtimeError;
        self.replaced.push_back(replacement->custom_id);
        self.names.emplace_back(replacement->name ? replacement->name : "");
        return TC_COMPONENT_TYPES_OK;
    }
    static int unregisterLive(void* context, uint64_t customId) {
        auto& self = *static_cast<Host*>(context);
        if (!customId) return TC_COMPONENT_TYPES_ERR_ARGUMENT;
        if (self.runtimeError != TC_COMPONENT_TYPES_OK) return self.runtimeError;
        self.unregistered.push_back(customId);
        return TC_COMPONENT_TYPES_OK;
    }
    static int query(void* context, const char* id, uint32_t version, void* out, uint32_t bytes) {
        auto& self = *static_cast<Host*>(context);
        if (id && std::strcmp(id, TC_SERVICE_COMPONENT_RUNTIME) == 0) {
            if (!self.runtime) return TC_SERVICE_ERR_UNAVAILABLE;
            if (version == TC_COMPONENT_RUNTIME_API_VERSION_2 &&
                bytes >= sizeof(TCComponentRuntimeApiV2)) {
                TCComponentRuntimeApiV2 runtime{};
                runtime.size = sizeof(runtime); runtime.version = TC_COMPONENT_RUNTIME_API_VERSION_2;
                runtime.context = context; runtime.register_type = &registerLive;
                runtime.replace_type = &replaceLive; runtime.unregister_type = &unregisterLive;
                *static_cast<TCComponentRuntimeApiV2*>(out) = runtime;
                return TC_SERVICE_OK;
            }
            if (version == TC_COMPONENT_RUNTIME_API_VERSION_1 &&
                bytes >= sizeof(TCComponentRuntimeApiV1)) {
                TCComponentRuntimeApiV1 runtime{};
                runtime.size = sizeof(runtime); runtime.version = TC_COMPONENT_RUNTIME_API_VERSION_1;
                runtime.context = context; runtime.register_type = &registerLive;
                *static_cast<TCComponentRuntimeApiV1*>(out) = runtime;
                return TC_SERVICE_OK;
            }
            return TC_SERVICE_ERR_UNAVAILABLE;
        }
        if (!id || std::strcmp(id, TC_SERVICE_COMPONENT_TYPES) ||
            version != TC_COMPONENT_TYPES_API_VERSION_2 || bytes < sizeof(TCComponentTypesApiV2))
            return TC_SERVICE_ERR_UNAVAILABLE;
        TCComponentTypesApiV2 api{};
        api.size = sizeof(api); api.version = TC_COMPONENT_TYPES_API_VERSION_2;
        api.context = context; api.register_definition = &oldEntry; api.register_definition_v3 = &add;
        *static_cast<TCComponentTypesApiV2*>(out) = api;
        return TC_SERVICE_OK;
    }
    TCHost table() {
        TCHost host{}; host.size = sizeof(host); host.api_version = TC_MOD_API_VERSION;
        host.context = this; host.query_service = &query; return host;
    }
};

std::string read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

}  // namespace

int main() {
    namespace fs = std::filesystem;
    using namespace tc::verilog;
    const fs::path root = fs::absolute("build/tc-verilog-project-fixture");
    std::error_code ignored;
    fs::remove_all(root, ignored);
    fs::create_directories(root / "rtl");

    std::string error;
    assert(writeTextFileAtomic(root / "rtl/counter.v", R"(
module counter(input clk, input enable, output reg [7:0] q);
always @(posedge clk) if (enable) q <= q + 8'b1;
endmodule
)", &error));
    assert(writeTextFileAtomic(root / "rtl/tri.v", R"(
module tri(input [7:0] data, input oe, output [7:0] bus);
assign bus = oe ? data : 8'bz;
endmodule
)", &error));

    const std::string manifest = R"({
  "format": 1,
  "components": [
    {
      "source": "rtl/counter.v",
      "custom_id": "0x54435650524f4a31",
      "type_id": "test/counter",
      "name": "Manifest Counter",
      "description": "counter from disk",
      "gate_cost": 8,
      "delay": 1,
      "pin_lane": 3.0,
      "show_description": true
    },
    {
      "source": "rtl/tri.v",
      "custom_id": "6071790671995484722",
      "type_id": "test/tri"
    }
  ]
})";
    const fs::path manifestPath = root / "tc-verilog.json";
    assert(writeTextFileAtomic(manifestPath, manifest, &error));
    assert(read(manifestPath) == manifest);

    Project project;
    std::vector<ProjectDiagnostic> diagnostics;
    assert(project.load(manifestPath, &diagnostics));
    assert(project.valid() && project.size() == 2);
    assert(project.manifestPath() == manifestPath.lexically_normal());
    assert(project.sourcePath(0).filename() == "counter.v");
    assert(project.sourceText(1).find("8'bz") != std::string::npos);
    assert(project.component(0)->metadata().name == "Manifest Counter");
    assert(project.component(0)->program().stateWords() == 2);
    assert(project.component(0)->definition()->flags & TC_COMPONENT_TYPE_SHOW_DESCRIPTION);
    assert(project.component(1)->metadata().name == "tri");

    /* The editor gate never writes broken RTL, and a valid source is replaced
       atomically without changing the already-compiled live component. */
    const std::string originalCounter = project.sourceText(0);
    const std::string invalidCounter = "module counter(input clk, output q); broken endmodule\n";
    assert(!project.validateAndSaveSource(0, invalidCounter, &diagnostics));
    assert(diagnostics.size() == 1 && diagnostics[0].stage == ProjectStage::Compile);
    assert(diagnostics[0].line == 1 && read(root / "rtl/counter.v") == originalCounter);
    const std::string editedCounter = R"(module counter(input clk, input enable, output reg [7:0] q);
always @(posedge clk) if (enable) q <= q + 8'b10;
endmodule
)";
    assert(project.validateSource(0, editedCounter, &diagnostics));
    assert(project.validateAndSaveSource(0, editedCounter, &diagnostics));
    assert(diagnostics.empty() && read(root / "rtl/counter.v") == editedCounter);
    assert(project.sourceText(0) == originalCounter);
    assert(writeTextFileAtomic(root / "rtl/counter.v", originalCounter, &error));

    /* First run materialises an editable copy; later runs preserve user edits. */
    const fs::path workspace = root / "workspace";
    assert(project.bootstrapWorkspace(workspace, &diagnostics));
    assert(read(workspace / "tc-verilog.json") == manifest);
    assert(read(workspace / "rtl/counter.v") == project.sourceText(0));
    assert(writeTextFileAtomic(workspace / "rtl/counter.v", "user edit", &error));
    assert(project.bootstrapWorkspace(workspace, &diagnostics));
    assert(read(workspace / "rtl/counter.v") == "user edit");

    /* Format 2 carries the IDE-facing project model while format 1 above stays
       loadable.  Stable keys survive file moves, disabled targets stay in the
       tree without registering, and project limits can only tighten the
       caller's compile limits. */
    {
        const fs::path v2Root = root / "v2";
        fs::create_directories(v2Root / "rtl");
        assert(writeTextFileAtomic(v2Root / "rtl/counter.v", originalCounter, &error));
        assert(writeTextFileAtomic(v2Root / "rtl/tri.v", R"(
module tri(input [7:0] data, input oe, output [7:0] bus);
assign bus = oe ? data : 8'bz;
endmodule
)", &error));
        const std::string v2Manifest = R"({
  "format": 2,
  "project": {
    "id": "cpu-core",
    "name": "CPU Core",
    "description": "Format-2 project fixture",
    "source_roots": ["rtl"],
    "default_component": "counter"
  },
  "build": {
    "max_source_bytes": 131072,
    "max_tokens": 32768,
    "max_state_words": 4096,
    "max_expression_depth": 64
  },
  "components": [
    {
      "key": "counter",
      "source": "rtl/counter.v",
      "top_module": "counter",
      "group": "Core",
      "custom_id": "0x5443565032303031",
      "type_id": "test-v2/counter",
      "name": "Counter"
    },
    {
      "key": "tri-state",
      "source": "rtl/tri.v",
      "top_module": "tri",
      "group": "I/O",
      "enabled": false,
      "custom_id": "0x5443565032303032",
      "type_id": "test-v2/tri"
    }
  ]
})";
        const fs::path v2Path = v2Root / "tc-verilog.json";
        assert(writeTextFileAtomic(v2Path, v2Manifest, &error));

        Project v2;
        std::vector<ProjectDiagnostic> v2Diagnostics;
        assert(v2.load(v2Path, &v2Diagnostics));
        assert(v2.metadata().format == 2 && v2.metadata().id == "cpu-core");
        assert(v2.metadata().name == "CPU Core");
        assert(v2.metadata().source_roots.size() == 1 &&
               v2.metadata().source_roots[0] == fs::path("rtl"));
        assert(v2.metadata().default_component == "counter");
        assert(v2.buildSettings().limits.max_source_bytes == 131072);
        assert(v2.buildSettings().limits.max_tokens == 32768);
        assert(v2.buildSettings().limits.max_state_words == 4096);
        assert(v2.buildSettings().limits.max_expression_depth == 64);
        assert(v2.findComponent("COUNTER") == 0);
        assert(v2.findComponent("missing") == static_cast<size_t>(-1));
        assert(v2.componentInfo(0).top_module == "counter");
        assert(v2.componentInfo(0).group == "Core");
        assert(!v2.componentInfo(1).enabled);

        Host v2HostState;
        TCHost v2Host = v2HostState.table();
        assert(v2.registerAll(&v2Host, &v2Diagnostics) == TC_COMPONENT_TYPES_OK);
        assert(v2HostState.registered == 1 && v2HostState.names[0] == "Counter");
        assert(v2.registerLive(1, &v2Host) == TC_COMPONENT_TYPES_ERR_ARGUMENT);

        std::string renamedTop = originalCounter;
        const size_t moduleName = renamedTop.find("module counter");
        assert(moduleName != std::string::npos);
        renamedTop.replace(moduleName, std::strlen("module counter"), "module renamed");
        assert(!v2.validateSource(0, renamedTop, &v2Diagnostics));
        assert(!v2Diagnostics.empty() &&
               v2Diagnostics[0].message.find("top_module") != std::string::npos);

        const std::string extraSource = R"(module extra(input a, output y);
assign y = a;
endmodule
)";
        assert(!v2.createSource("outside.v", extraSource, &v2Diagnostics));
        assert(v2Diagnostics[0].message.find("source_roots") != std::string::npos);
        v2.setTypePrefix("test-v2");
        assert(v2.createSource("rtl/extra.v", extraSource, &v2Diagnostics));
        const size_t extraIndex = v2.size() - 1;
        const std::string extraKey = v2.componentInfo(extraIndex).key;
        assert(v2.renameSource(extraIndex, "rtl/util/extra_renamed.v", &v2Diagnostics));
        assert(v2.componentInfo(extraIndex).key == extraKey);
        assert(v2.componentInfo(extraIndex).source == fs::path("rtl/util/extra_renamed.v"));

        /* Removing the default chooses the next enabled target and persists
           that choice.  It cannot choose the disabled target as a fallback. */
        assert(v2.removeSource(0, &v2Diagnostics));
        assert(v2.metadata().default_component == extraKey);
        Project v2Reloaded;
        assert(v2Reloaded.load(v2Path, &v2Diagnostics));
        assert(v2Reloaded.metadata().format == 2);
        assert(v2Reloaded.metadata().default_component == extraKey);
        assert(v2Reloaded.findComponent(extraKey) != static_cast<size_t>(-1));
        assert(!v2Reloaded.removeSource(v2Reloaded.findComponent(extraKey), &v2Diagnostics));
        assert(v2Diagnostics[0].message.find("default component") != std::string::npos);

        Project invalidV2;
        assert(writeTextFileAtomic(v2Root / "rtl/counter.v", originalCounter, &error));
        std::string badTop = v2Manifest;
        const size_t topValue = badTop.find("\"top_module\": \"counter\"");
        assert(topValue != std::string::npos);
        badTop.replace(topValue, std::strlen("\"top_module\": \"counter\""),
                       "\"top_module\": \"not_counter\"");
        const fs::path badV2Path = v2Root / "bad-top.json";
        assert(writeTextFileAtomic(badV2Path, badTop, &error));
        assert(!invalidV2.load(badV2Path, &v2Diagnostics));
        assert(v2Diagnostics[0].message.find("does not match") != std::string::npos);

        std::string excessiveBuild = v2Manifest;
        const size_t sourceLimit = excessiveBuild.find("131072");
        assert(sourceLimit != std::string::npos);
        excessiveBuild.replace(sourceLimit, std::strlen("131072"), "999999");
        const fs::path excessivePath = v2Root / "bad-build.json";
        assert(writeTextFileAtomic(excessivePath, excessiveBuild, &error));
        assert(!invalidV2.load(excessivePath, &v2Diagnostics));
        assert(v2Diagnostics[0].message.find("max_source_bytes") != std::string::npos);
    }

    /* New-project creation validates and compiles before committing a format-2
       manifest.  Reusing the same destination refuses to overwrite it and
       leaves the already-loaded Project intact. */
    {
        ProjectCreateOptions create;
        create.id = "created-core";
        create.name = "Created Core";
        create.description = "Created by the project API";
        create.source_root = "src";
        create.component_key = "core";
        create.top_module = "core";
        create.type_prefix = "test-created";
        create.initial_source = R"(module core(input wire a, output wire y);
assign y = a;
endmodule
)";
        const fs::path createdDirectory = root / "created-project";
        Project created;
        std::vector<ProjectDiagnostic> createDiagnostics;
        assert(created.createNew(createdDirectory, create, &createDiagnostics));
        assert(createDiagnostics.empty() && created.valid() && created.size() == 1);
        assert(created.metadata().format == 2 && created.metadata().id == "created-core");
        assert(created.metadata().default_component == "core");
        assert(created.componentInfo(0).source == fs::path("src/core.v"));
        assert(created.componentInfo(0).top_module == "core");
        assert(created.component(0)->metadata().type_id == "test-created/core");
        assert(fs::exists(createdDirectory / "tc-verilog.json"));
        assert(read(createdDirectory / "src/core.v") == create.initial_source);

        const std::string committedManifest = read(createdDirectory / "tc-verilog.json");
        assert(!created.createNew(createdDirectory, create, &createDiagnostics));
        assert(!createDiagnostics.empty() && created.metadata().id == "created-core");
        assert(read(createdDirectory / "tc-verilog.json") == committedManifest);

        ProjectCreateOptions invalid = create;
        invalid.id = "invalid-core";
        invalid.top_module = "9bad";
        const fs::path invalidDirectory = root / "invalid-created-project";
        Project invalidCreated;
        assert(!invalidCreated.createNew(invalidDirectory, invalid, &createDiagnostics));
        assert(!fs::exists(invalidDirectory / "tc-verilog.json"));
    }

    /* Bad reload does not discard the two programs already loaded. */
    assert(writeTextFileAtomic(manifestPath, "{\"format\":1,\"components\":[", &error));
    assert(!project.load(manifestPath, &diagnostics));
    assert(project.size() == 2 && diagnostics.size() == 1);
    assert(diagnostics[0].stage == ProjectStage::Manifest && diagnostics[0].line == 1);
    assert(formatDiagnostic(diagnostics[0]).find("manifest:") == 0);
    assert(writeTextFileAtomic(manifestPath, manifest, &error));

    Host hostState;
    TCHost host = hostState.table();
    assert(project.registerAll(&host, &diagnostics) == TC_COMPONENT_TYPES_OK);
    assert(hostState.registered == 2 && hostState.ids.size() == 2);
    assert(hostState.ids[0] == 0x54435650524f4a31ULL);
    assert(hostState.names[0] == "Manifest Counter" && hostState.names[1] == "tri");

    /* The light hot-swap path writes same-shape edits straight into the program
       the host holds.  A moved port is saved for the full runtime-V2 replacement
       exercised below. */
    {
        const std::string faster = R"(module counter(input clk, input enable, output reg [7:0] q);
always @(posedge clk) if (enable) q <= q + 8'b11;
endmodule
)";
        bool reloaded = false;
        assert(project.saveAndReloadSource(0, faster, &reloaded, &diagnostics));
        assert(reloaded && diagnostics.empty());
        assert(read(root / "rtl/counter.v") == faster);
        const std::string wider = R"(module counter(input clk, input enable, input rst, output reg [7:0] q);
always @(posedge clk) if (enable) q <= q + 8'b11;
endmodule
)";
        assert(project.saveAndReloadSource(0, wider, &reloaded, &diagnostics));
        assert(!reloaded && diagnostics.empty());
        assert(read(root / "rtl/counter.v") == wider);
        assert(!project.saveAndReloadSource(0, "module counter(input clk, output q); broken endmodule\n",
                                            &reloaded, &diagnostics));
        assert(!diagnostics.empty() && read(root / "rtl/counter.v") == wider);
        assert(project.saveAndReloadSource(0, originalCounter, &reloaded, &diagnostics));
        assert(read(root / "rtl/counter.v") == originalCounter);
    }

    /* Path traversal and unknown keys are refused before any source is read. */
    Project unsafe;
    const std::string badPath = R"({"format":1,"components":[{"source":"../x.v","custom_id":1}]})";
    assert(writeTextFileAtomic(manifestPath, badPath, &error));
    assert(!unsafe.load(manifestPath, &diagnostics));
    assert(diagnostics[0].message.find("safe relative") != std::string::npos);
    const std::string typo = R"({"format":1,"components":[{"source":"rtl/tri.v","custom_id":1,"cost":2}]})";
    assert(writeTextFileAtomic(manifestPath, typo, &error));
    assert(!unsafe.load(manifestPath, &diagnostics));
    assert(diagnostics[0].message.find("unknown field 'cost'") != std::string::npos);

    /* Registration diagnostics retain the entry identity and index. */
    assert(writeTextFileAtomic(manifestPath, manifest, &error));
    Project rejected;
    assert(rejected.load(manifestPath, &diagnostics));
    Host rejecting; rejecting.failAt = 2; TCHost rejectingHost = rejecting.table();
    assert(rejected.registerAll(&rejectingHost, &diagnostics) == TC_COMPONENT_TYPES_ERR_GAME);
    assert(diagnostics.size() == 1 && diagnostics[0].stage == ProjectStage::Register);
    assert(diagnostics[0].component_index == 1 && diagnostics[0].component_id == "test/tri");

    /* File manager: create, duplicate, rename and remove rewrite the manifest
       atomically and leave a project the next launch can load. */
    assert(writeTextFileAtomic(manifestPath, manifest, &error));
    Project managed;
    std::vector<ProjectDiagnostic> managedDiagnostics;
    assert(managed.load(manifestPath, &managedDiagnostics));
    managed.setTypePrefix("test");
    assert(managed.typePrefix() == "test");

    const fs::path createdRelative = fs::path("rtl") / "blink.v";
    const std::string blinkSource = R"(module blink(input clk, output reg led);
always @(posedge clk) led <= ~led;
endmodule
)";
    assert(managed.createSource(createdRelative, blinkSource, &managedDiagnostics));
    assert(managedDiagnostics.empty() && managed.size() == 3);
    assert(managed.sourcePath(2).filename() == "blink.v");
    assert(read(managed.sourcePath(2)) == blinkSource);
    assert(managed.component(2)->metadata().type_id == "test/rtl/blink");
    assert(managed.component(2)->metadata().name == "blink");
    assert(managed.component(2)->metadata().custom_id != 0);
    assert(managed.component(2)->metadata().custom_id != managed.component(0)->metadata().custom_id);

    Project reloaded;
    assert(reloaded.load(manifestPath, &managedDiagnostics));
    assert(reloaded.size() == 3 && reloaded.sourcePath(2).filename() == "blink.v");
    assert(reloaded.component(2)->program().moduleName() == "blink");

    assert(managed.duplicateSource(0, fs::path("rtl") / "counter_copy.v", &managedDiagnostics));
    assert(managedDiagnostics.empty() && managed.size() == 4);
    assert(managed.sourcePath(3).filename() == "counter_copy.v");
    assert(read(managed.sourcePath(3)) == managed.sourceText(0));
    assert(managed.component(3)->metadata().description == "counter from disk");
    assert(managed.component(3)->metadata().type_id == "test/rtl/counter_copy");
    assert(managed.component(3)->metadata().custom_id != managed.component(0)->metadata().custom_id);

    /* A rename moves the file and the manifest source but keeps the identity,
       so circuits that already reference the component keep working. */
    const uint64_t renamedId = managed.component(0)->metadata().custom_id;
    assert(managed.renameSource(0, fs::path("rtl") / "counter_renamed.v", &managedDiagnostics));
    assert(managedDiagnostics.empty() && managed.size() == 4);
    assert(managed.sourcePath(0).filename() == "counter_renamed.v");
    assert(!fs::exists(root / "rtl/counter.v"));
    assert(read(managed.sourcePath(0)) == originalCounter);
    assert(managed.component(0)->metadata().custom_id == renamedId);
    assert(read(manifestPath).find("rtl/counter_renamed.v") != std::string::npos);
    /* The running definition is frozen, but the manifest must carry the new
       name so the game stops showing the name the file was created with. */
    assert(managed.component(0)->metadata().name == "Manifest Counter");
    {
        Project afterRename;
        assert(afterRename.load(manifestPath, &managedDiagnostics));
        assert(afterRename.component(0)->metadata().name == "counter_renamed");
        assert(afterRename.component(0)->metadata().custom_id == renamedId);
    }

    /* The host keeps pointers into a registered component, so removing a file
       must keep that object alive for the rest of the session; the pointer
       taken before the removal has to stay readable. */
    const Component* retired = managed.component(3);
    const uint64_t retiredId = retired->metadata().custom_id;
    const std::string retiredModule = retired->program().moduleName();
    assert(managed.removeSource(3, &managedDiagnostics));
    assert(managedDiagnostics.empty() && managed.size() == 3);
    assert(!fs::exists(root / "rtl/counter_copy.v"));
    assert(read(manifestPath).find("counter_copy.v") == std::string::npos);
    assert(retired->metadata().custom_id == retiredId);
    assert(retired->program().moduleName() == retiredModule);

    /* Refusals leave the project and its files exactly as they were. */
    assert(!managed.createSource(createdRelative, blinkSource, &managedDiagnostics));
    assert(managed.size() == 3 && !managedDiagnostics.empty());
    assert(!managed.createSource(fs::path("..") / "escape.v", blinkSource, &managedDiagnostics));
    assert(managed.size() == 3 && !managedDiagnostics.empty());
    assert(!managed.renameSource(1, fs::path("rtl") / "blink.v", &managedDiagnostics));
    assert(managed.size() == 3 && !managedDiagnostics.empty());
    while (managed.size() > 1)
        assert(managed.removeSource(managed.size() - 1, &managedDiagnostics));
    assert(!managed.removeSource(0, &managedDiagnostics));
    assert(managed.size() == 1 && !managedDiagnostics.empty());

    /* Runtime registration: a file created while the game runs can go live
       instead of waiting for the next start. */
    {
        const std::string liveSource = R"(module live(input wire a, output wire y);
assign y = a;
endmodule
)";
        Host liveHost;
        TCHost live = liveHost.table();
        Project started;
        std::vector<ProjectDiagnostic> liveDiagnostics;
        assert(started.load(manifestPath, &liveDiagnostics));
        started.setTypePrefix("test");
        assert(started.createSource(fs::path("rtl") / "live.v", liveSource, &liveDiagnostics));
        const size_t index = started.size() - 1;
        assert(!started.component(index)->registered());
        assert(started.registerLive(index, &live) == TC_COMPONENT_TYPES_OK);
        assert(started.component(index)->registered());
        assert(liveHost.live.size() == 1);
        assert(liveHost.live.back() == started.component(index)->metadata().custom_id);
        /* a second call reports the same success without registering twice */
        assert(started.registerLive(index, &live) == TC_COMPONENT_TYPES_OK);
        assert(liveHost.live.size() == 1);
        /* and now that the type is live, saving it hot reloads it */
        bool reloaded = false;
        assert(started.saveAndReloadSource(index, liveSource, &reloaded, &liveDiagnostics));
        assert(reloaded);
        /* A shape edit, rename and removal all mutate the same running id and
           therefore the component-menu data without restarting the process. */
        const uint64_t liveId = started.component(index)->metadata().custom_id;
        const std::string reshaped = R"(module live(input wire a, input wire b, output wire y);
assign y = a & b;
endmodule
)";
        assert(started.saveAndReloadSource(index, reshaped, &reloaded, &liveDiagnostics));
        assert(!reloaded);
        assert(started.reloadLive(index, &live, &liveDiagnostics) == TC_COMPONENT_TYPES_OK);
        assert(liveHost.replaced.size() == 1 && liveHost.replaced.back() == liveId);
        assert(started.component(index)->registered());
        assert(started.component(index)->program().inputCount() == 2);
        assert(started.renameSource(index, fs::path("rtl") / "live_renamed.v", &liveDiagnostics));
        assert(started.reloadLive(index, &live, &liveDiagnostics) == TC_COMPONENT_TYPES_OK);
        assert(liveHost.replaced.size() == 2);
        assert(liveHost.names.back() == "live_renamed");
        assert(started.removeSource(index, &liveDiagnostics));
        assert(started.unregisterLive(liveId, &live) == TC_COMPONENT_TYPES_OK);
        assert(liveHost.unregistered.size() == 1 && liveHost.unregistered.back() == liveId);
        /* a loader without the runtime service keeps the restart behaviour */
        Host oldLoader; oldLoader.runtime = false;
        TCHost oldTable = oldLoader.table();
        Project waiting;
        assert(waiting.load(manifestPath, &liveDiagnostics));
        assert(waiting.registerLive(0, &oldTable) == TC_COMPONENT_TYPES_ERR_UNAVAILABLE);
        /* and a refusal (a board is up) is passed through unchanged */
        Host busy; busy.runtimeError = TC_COMPONENT_TYPES_ERR_STATE;
        TCHost busyTable = busy.table();
        assert(waiting.registerLive(0, &busyTable) == TC_COMPONENT_TYPES_ERR_STATE);
    }

    assert(!moduleDirectory(&error).empty());
    fs::remove_all(root, ignored);
    std::cout << "TC-Verilog project tests passed\n";
    return 0;
}
