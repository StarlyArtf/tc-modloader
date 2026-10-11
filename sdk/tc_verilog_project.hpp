#ifndef TC_VERILOG_PROJECT_HPP
#define TC_VERILOG_PROJECT_HPP

#include "tc_verilog.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace tc {
namespace verilog {

enum class ProjectStage : uint8_t { Manifest, Source, Compile, Register, Save };

struct ProjectDiagnostic {
    ProjectStage stage = ProjectStage::Manifest;
    size_t component_index = static_cast<size_t>(-1);
    std::string component_id;
    std::filesystem::path file;
    uint32_t line = 0, column = 0;
    std::string message;
};

const char* projectStageName(ProjectStage stage) noexcept;
std::string formatDiagnostic(const ProjectDiagnostic& diagnostic);

/* The directory of the DLL/executable that contains tc_verilog_project.cpp.
   A packaged plugin can therefore load "tc-verilog.json" beside itself without
   knowing the loader's digest-cache path. */
std::filesystem::path moduleDirectory(std::string* error = nullptr) noexcept;

/* UTF-8 text replacement used by source editors.  The temporary file lives
   beside the target and is flushed before an atomic replace. */
bool writeTextFileAtomic(const std::filesystem::path& path, const std::string& text,
                         std::string* error = nullptr) noexcept;

/* Format-2 project identity.  source_roots are manifest-relative directories
   used by the IDE to build its tree and by the loader to keep every component
   source inside an explicitly declared part of the project. */
struct ProjectMetadata {
    uint32_t format = 0;
    std::string id;
    std::string name;
    std::string description;
    std::vector<std::filesystem::path> source_roots;
    std::string default_component;
};

/* Project-local safety limits.  The caller's CompileOptions remain a hard
   ceiling; format 2 may only lower them, never use the manifest to raise a
   host-imposed limit. */
struct ProjectBuildSettings {
    CompileOptions limits;
};

/* Stable project-facing identity for one component target.  `key` is what
   default_component references; it is independent of the game-facing type_id
   and custom_id. */
struct ProjectComponentInfo {
    std::string key;
    std::filesystem::path source;
    std::string top_module;
    std::string group;
    bool enabled = true;
};

/* Inputs for creating a minimal format-2 project.  The first component is
   compiled before any file is written; the manifest is committed last, so an
   interrupted or rejected creation never leaves a loadable half-project. */
struct ProjectCreateOptions {
    std::string id;
    std::string name;
    std::string description;
    std::filesystem::path source_root = "src";
    std::string component_key = "main";
    std::string top_module = "main";
    std::string initial_source;
    std::string type_prefix = "tc-verilog";
    CompileOptions limits;
};

class Project final {
public:
    Project();
    ~Project();
    Project(Project&&) noexcept;
    Project& operator=(Project&&) noexcept;
    Project(const Project&) = delete;
    Project& operator=(const Project&) = delete;

    /* Loads format-1 or format-2 JSON, then reads and compiles every referenced
       .v file.  Failure is transactional: the previous Project remains intact. */
    bool load(const std::filesystem::path& manifest,
              std::vector<ProjectDiagnostic>* diagnostics = nullptr,
              const CompileOptions& options = {});
    bool loadAdjacent(const std::filesystem::path& manifest_name,
                      std::vector<ProjectDiagnostic>* diagnostics = nullptr,
                      const CompileOptions& options = {});

    /* Creates and loads a new format-2 project in `directory`.  Existing
       manifests or source files are never overwritten. */
    bool createNew(const std::filesystem::path& directory,
                   const ProjectCreateOptions& create,
                   std::vector<ProjectDiagnostic>* diagnostics = nullptr);

    /* First-run persistence helper.  If the destination manifest is absent,
       copies every source (without overwriting an existing edit) and writes
       the manifest last as the workspace commit marker. */
    bool bootstrapWorkspace(const std::filesystem::path& directory,
                            std::vector<ProjectDiagnostic>* diagnostics = nullptr) const noexcept;

    /* All components are compiled before the first registration.  A host-side
       failure is reported with the exact manifest entry; plugin load should
       then fail so the loader rolls back registrations from that Mod. */
    int registerAll(const TCHost* host,
                    std::vector<ProjectDiagnostic>* diagnostics = nullptr) noexcept;

    /* Editor operations compile against this entry's metadata without
       mutating the already-registered component.  A save replaces the .v file
       only after validation succeeds; the new definition takes effect on the
       next game start. */
    bool validateSource(size_t index, const std::string& source,
                        std::vector<ProjectDiagnostic>* diagnostics = nullptr,
                        const CompileOptions& options = {}) const noexcept;
    bool validateAndSaveSource(size_t index, const std::string& source,
                               std::vector<ProjectDiagnostic>* diagnostics = nullptr,
                               const CompileOptions& options = {}) const noexcept;

    /* Editor save with hot reload: compile, write, and - when the ports and the
       state layout did not move - put the new program in place so the running
       instances use it on the next cycle.  `reloaded` reports which of the two
       happened; the call fails only when the source does not compile or the
       write fails, and then neither the file nor the live program changed.
       A moved port or state layout is still saved; `reloaded == false` tells an
       editor to queue reloadLive on the next game-thread frame. */
    bool saveAndReloadSource(size_t index, const std::string& source, bool* reloaded,
                             std::vector<ProjectDiagnostic>* diagnostics = nullptr,
                             const CompileOptions& options = {}) const noexcept;

    /* Registers a freshly created entry with the running game (the editor's
       "new component" path, tc.component.runtime).  Answers the loader's
       TC_COMPONENT_TYPES_* code; TC_COMPONENT_TYPES_ERR_UNAVAILABLE means this
       loader has no runtime service, and the type appears on the next start. */
    int registerLive(size_t index, const TCHost* host) noexcept;

    /* Recompile the current saved source and replace its running definition.
       Unlike same-shape hotSwap, this also handles a renamed display name,
       moved ports and a changed state layout.  The old Component stays alive
       for host pointer safety. */
    int reloadLive(size_t index, const TCHost* host,
                   std::vector<ProjectDiagnostic>* diagnostics = nullptr,
                   const CompileOptions& options = {}) noexcept;

    /* Removes a file-manager-retired component from the running game. */
    int unregisterLive(uint64_t custom_id, const TCHost* host) noexcept;

    /* File manager operations.  Each one validates (and, where it changes a
       definition, compiles) before touching the disk, then rewrites the
       manifest atomically, so a failed call leaves the project exactly as it
       was and the next launch loads the same sources.

       relative_name follows the same rule as a manifest `source`: a safe
       relative .v path using '/' separators.  Absolute paths, "..", a
       backslash, a missing .v extension and a name that is already taken are
       refused; the parent directory of a nested name is created.  A new or
       duplicated file takes a fresh custom id and a type id built from
       typePrefix(); a rename keeps the component identity and only moves the
       file.  The last component cannot be removed, because a project must
       keep at least one source. */
    bool createSource(const std::filesystem::path& relative_name, const std::string& source,
                      std::vector<ProjectDiagnostic>* diagnostics = nullptr,
                      const CompileOptions& options = {});
    bool duplicateSource(size_t index, const std::filesystem::path& relative_name,
                         std::vector<ProjectDiagnostic>* diagnostics = nullptr,
                         const CompileOptions& options = {});
    bool renameSource(size_t index, const std::filesystem::path& relative_name,
                      std::vector<ProjectDiagnostic>* diagnostics = nullptr);
    bool removeSource(size_t index, std::vector<ProjectDiagnostic>* diagnostics = nullptr);

    /* Prefix of the type id ("<prefix>/<file>") a created or duplicated file
       gets; an empty prefix omits type_id and lets the host derive one.
       Defaults to "tc-verilog"; plugins normally set it from host->mod_id. */
    void setTypePrefix(std::string prefix);
    const std::string& typePrefix() const noexcept;

    bool valid() const noexcept;
    size_t size() const noexcept;
    const std::filesystem::path& manifestPath() const noexcept;
    std::filesystem::path workspaceManifestPath(const std::filesystem::path& directory) const;
    const std::filesystem::path& sourcePath(size_t index) const;
    const std::string& sourceText(size_t index) const;
    const Component* component(size_t index) const noexcept;
    const ProjectMetadata& metadata() const noexcept;
    const ProjectBuildSettings& buildSettings() const noexcept;
    const ProjectComponentInfo& componentInfo(size_t index) const;
    size_t findComponent(const std::string& key) const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace verilog
}  // namespace tc

#endif  // TC_VERILOG_PROJECT_HPP
