#include "tc_verilog_project.hpp"
#include "tc_component_runtime.h"

#include "../vendor/json.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace tc {
namespace verilog {
namespace {

using Json = nlohmann::json;
constexpr size_t kMaxManifestBytes = 1024u * 1024u;
constexpr size_t kMaxComponents = 256;
constexpr size_t kMaxSourceRoots = 32;

struct ProjectError : std::runtime_error {
    ProjectStage stage;
    size_t index;
    std::string id;
    std::filesystem::path file;
    uint32_t line, column;
    ProjectError(ProjectStage stage_, size_t index_, std::string id_, std::filesystem::path file_,
                 uint32_t line_, uint32_t column_, const std::string& message)
        : std::runtime_error(message), stage(stage_), index(index_), id(std::move(id_)),
          file(std::move(file_)), line(line_), column(column_) {}
};

std::string readText(const std::filesystem::path& path, size_t limit, ProjectStage stage,
                     size_t index = static_cast<size_t>(-1), const std::string& id = {}) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw ProjectError(stage, index, id, path, 0, 0, "cannot open file");
    input.seekg(0, std::ios::end);
    const std::streamoff length = input.tellg();
    if (length < 0 || static_cast<uint64_t>(length) > limit)
        throw ProjectError(stage, index, id, path, 0, 0, "file exceeds configured size limit");
    input.seekg(0, std::ios::beg);
    std::string text(static_cast<size_t>(length), '\0');
    if (length && !input.read(&text[0], length))
        throw ProjectError(stage, index, id, path, 0, 0, "cannot read complete file");
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xef &&
        static_cast<unsigned char>(text[1]) == 0xbb && static_cast<unsigned char>(text[2]) == 0xbf)
        text.erase(0, 3);
    return text;
}

std::pair<uint32_t, uint32_t> lineColumn(const std::string& text, size_t byte) {
    if (!byte) return {0, 0};
    const size_t stop = std::min(byte - 1, text.size());
    uint32_t line = 1, column = 1;
    for (size_t i = 0; i < stop; ++i) {
        if (text[i] == '\n') { ++line; column = 1; }
        else ++column;
    }
    return {line, column};
}

void requireObject(const Json& value, const std::filesystem::path& file, size_t index,
                   const std::string& id, const char* what) {
    if (!value.is_object())
        throw ProjectError(ProjectStage::Manifest, index, id, file, 0, 0,
                           std::string(what) + " must be an object");
}

void rejectUnknown(const Json& value, const std::set<std::string>& allowed,
                   const std::filesystem::path& file, size_t index, const std::string& id) {
    for (auto it = value.begin(); it != value.end(); ++it)
        if (!allowed.count(it.key()))
            throw ProjectError(ProjectStage::Manifest, index, id, file, 0, 0,
                               "unknown field '" + it.key() + "'");
}

std::string stringField(const Json& object, const char* key, bool required,
                        const std::filesystem::path& file, size_t index, const std::string& id) {
    const auto it = object.find(key);
    if (it == object.end()) {
        if (required) throw ProjectError(ProjectStage::Manifest, index, id, file, 0, 0,
                                         std::string("missing field '") + key + "'");
        return {};
    }
    if (!it->is_string()) throw ProjectError(ProjectStage::Manifest, index, id, file, 0, 0,
                                             std::string("field '") + key + "' must be a string");
    return it->get<std::string>();
}

uint64_t unsignedField(const Json& object, const char* key, uint64_t fallback, bool required,
                       const std::filesystem::path& file, size_t index, const std::string& id) {
    const auto it = object.find(key);
    if (it == object.end()) {
        if (required) throw ProjectError(ProjectStage::Manifest, index, id, file, 0, 0,
                                         std::string("missing field '") + key + "'");
        return fallback;
    }
    if (it->is_number_unsigned()) return it->get<uint64_t>();
    if (it->is_number_integer()) {
        const int64_t value = it->get<int64_t>();
        if (value >= 0) return static_cast<uint64_t>(value);
    }
    if (it->is_string()) {
        const std::string text = it->get<std::string>();
        if (!text.empty() && text[0] != '-') {
            errno = 0;
            char* end = nullptr;
            const int base = text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X') ? 16 : 10;
            const unsigned long long value = std::strtoull(text.c_str(), &end, base);
            if (end && end != text.c_str() && *end == '\0' && errno != ERANGE)
                return static_cast<uint64_t>(value);
        }
    }
    throw ProjectError(ProjectStage::Manifest, index, id, file, 0, 0,
                       std::string("field '") + key + "' must be an unsigned integer or integer string");
}

bool safeSourceName(const std::string& name) {
    if (name.empty() || name.size() > 240 || name.find('\\') != std::string::npos ||
        name.find(':') != std::string::npos || name.front() == '/') return false;
    const std::filesystem::path path = std::filesystem::u8path(name);
    for (const auto& part : path)
        if (part == "." || part == ".." || part.empty()) return false;
    std::string extension = path.extension().u8string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension == ".v";
}

bool safeRelativeDirectory(const std::string& name) {
    if (name == ".") return true;
    if (name.empty() || name.size() > 200 || name.find('\\') != std::string::npos ||
        name.find(':') != std::string::npos || name.front() == '/') return false;
    const std::filesystem::path path = std::filesystem::u8path(name);
    for (const auto& part : path)
        if (part == "." || part == ".." || part.empty()) return false;
    return true;
}

bool safeProjectKey(const std::string& value) {
    if (value.empty() || value.size() > 96) return false;
    for (unsigned char c : value) {
        const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                          (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
        if (!safe) return false;
    }
    return true;
}

bool safeVerilogIdentifier(const std::string& value) {
    if (value.empty() || value.size() > 240) return false;
    const unsigned char first = static_cast<unsigned char>(value.front());
    if (!((first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z') ||
          first == '_')) return false;
    for (unsigned char c : value)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '$')) return false;
    return true;
}

bool sourceInsideRoot(const std::filesystem::path& source,
                      const std::filesystem::path& root) {
    if (root == ".") return true;
    std::string sourceName = source.generic_u8string();
    std::string rootName = root.generic_u8string();
    std::transform(sourceName.begin(), sourceName.end(), sourceName.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::transform(rootName.begin(), rootName.end(), rootName.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (!rootName.empty() && rootName.back() != '/') rootName.push_back('/');
    return sourceName.size() > rootName.size() && sourceName.compare(0, rootName.size(), rootName) == 0;
}

bool booleanField(const Json& object, const char* key, bool fallback,
                  const std::filesystem::path& file, size_t index, const std::string& id) {
    const auto it = object.find(key);
    if (it == object.end()) return fallback;
    if (!it->is_boolean())
        throw ProjectError(ProjectStage::Manifest, index, id, file, 0, 0,
                           std::string("field '") + key + "' must be boolean");
    return it->get<bool>();
}

uint32_t boundedUint32Field(const Json& object, const char* key, uint32_t fallback,
                            uint32_t minimum, uint32_t maximum,
                            const std::filesystem::path& file) {
    const auto it = object.find(key);
    if (it == object.end()) return fallback;
    if (!it->is_number_unsigned())
        throw ProjectError(ProjectStage::Manifest, static_cast<size_t>(-1), {}, file, 0, 0,
                           std::string("build field '") + key + "' must be an unsigned integer");
    const uint64_t value = it->get<uint64_t>();
    if (value < minimum || value > maximum)
        throw ProjectError(ProjectStage::Manifest, static_cast<size_t>(-1), {}, file, 0, 0,
                           std::string("build field '") + key + "' is outside the supported range");
    return static_cast<uint32_t>(value);
}

std::string componentIdentity(const ComponentMetadata& metadata, size_t index) {
    if (!metadata.type_id.empty()) return metadata.type_id;
    if (!metadata.name.empty()) return metadata.name;
    std::ostringstream out; out << "component[" << index << ']'; return out.str();
}

/* FNV-1a: a stable seed for the custom id of a file the editor created, so a
   given name maps to the same id across sessions until it collides. */
uint64_t stableHash(const std::string& text) {
    uint64_t hash = 1469598103934665603ULL;
    for (unsigned char byte : text) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::string lowerAscii(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

/* The manifest spelling of a path: '/' separators, never a backslash. */
std::string relativeName(const std::filesystem::path& path) {
    return path.generic_u8string();
}

/* "rtl/ram16x8.v" -> "rtl/ram16x8": a per-file identity that stays unique
   inside one project even when two files share a base name. */
std::string identityStem(const std::filesystem::path& path) {
    std::string name = relativeName(path);
    const std::string extension = lowerAscii(path.extension().u8string());
    if (extension == ".v" && name.size() > extension.size())
        name.resize(name.size() - extension.size());
    return name;
}

void pushSaveDiagnostic(std::vector<ProjectDiagnostic>* diagnostics, size_t index,
                        const std::string& id, const std::filesystem::path& file,
                        const std::string& message) {
    if (diagnostics)
        diagnostics->push_back({ProjectStage::Save, index, id, file, 0, 0, message});
}

}  // namespace

struct Project::Impl {
    struct Item {
        std::filesystem::path relativeSource;
        std::filesystem::path sourcePath;
        std::string source;
        std::unique_ptr<Component> component;
        ProjectComponentInfo info;
        /* Display name written to the manifest.  Empty means "use the
           component's own name"; a rename fills it in, because the game keys a
           component by the name in the manifest and would otherwise keep
           showing the name the file was created with. */
        std::string nameOverride;
    };
    std::filesystem::path manifest;
    std::string manifestText;
    ProjectMetadata metadata;
    ProjectBuildSettings build;
    std::vector<Item> items;
    /* Components removed by the file manager during this session.  The host
       keeps pointers into a registered Component (its Program is the callback
       user pointer, its strings and pin array went into the definition), so a
       removed file must not destroy the object it registered.  The entry is
       gone from the manifest; the object lives until the process exits. */
    std::vector<Item> retired;
    /* A replaced definition is no longer referenced by an active item, but
       its strings/program may still be on an in-flight host stack. */
    std::vector<std::unique_ptr<Component>> retiredComponents;
    std::string typePrefix = "tc-verilog";

    CompileOptions effectiveOptions(const CompileOptions& requested) const {
        CompileOptions result;
        result.max_source_bytes = std::min(requested.max_source_bytes, build.limits.max_source_bytes);
        result.max_tokens = std::min(requested.max_tokens, build.limits.max_tokens);
        result.max_state_words = std::min(requested.max_state_words, build.limits.max_state_words);
        result.max_expression_depth =
            std::min(requested.max_expression_depth, build.limits.max_expression_depth);
        return result;
    }

    /* One manifest entry, in the order the documentation shows.  Optional
       fields are omitted when they hold their default, which keeps a rewritten
       manifest close to a hand-written one. */
    nlohmann::ordered_json entryFor(const Item& item) const {
        const ComponentMetadata& metadata = item.component->metadata();
        nlohmann::ordered_json entry;
        if (this->metadata.format >= 2) {
            entry["key"] = item.info.key;
            if (!item.info.top_module.empty()) entry["top_module"] = item.info.top_module;
            if (!item.info.group.empty()) entry["group"] = item.info.group;
            if (!item.info.enabled) entry["enabled"] = false;
        }
        entry["source"] = relativeName(item.relativeSource);
        std::ostringstream id;
        id << "0x" << std::hex << metadata.custom_id;
        entry["custom_id"] = id.str();
        if (!metadata.type_id.empty()) entry["type_id"] = metadata.type_id;
        const std::string& name = item.nameOverride.empty() ? metadata.name : item.nameOverride;
        if (!name.empty()) entry["name"] = name;
        if (!metadata.description.empty()) entry["description"] = metadata.description;
        if (!metadata.shape_svg.empty()) entry["shape_svg"] = metadata.shape_svg;
        if (metadata.gate_cost) entry["gate_cost"] = metadata.gate_cost;
        if (metadata.delay) entry["delay"] = metadata.delay;
        if (metadata.pin_lane > 0.f) entry["pin_lane"] = metadata.pin_lane;
        if (metadata.flags & TC_COMPONENT_TYPE_SHOW_DESCRIPTION) entry["show_description"] = true;
        return entry;
    }

    /* The whole manifest as text, skipping one entry and optionally appending
       one that is not in `items` yet.  A mutation builds this before it writes
       anything, so a later failure has nothing to undo in memory. */
    std::string manifestTextExcluding(size_t skip, const Item* extra) const {
        nlohmann::ordered_json root;
        root["format"] = metadata.format >= 2 ? 2 : 1;
        if (metadata.format >= 2) {
            nlohmann::ordered_json project;
            project["id"] = metadata.id;
            project["name"] = metadata.name;
            if (!metadata.description.empty()) project["description"] = metadata.description;
            nlohmann::ordered_json roots = nlohmann::ordered_json::array();
            for (const auto& sourceRoot : metadata.source_roots)
                roots.push_back(relativeName(sourceRoot));
            project["source_roots"] = std::move(roots);
            project["default_component"] = metadata.default_component;
            root["project"] = std::move(project);

            nlohmann::ordered_json buildObject;
            buildObject["max_source_bytes"] = build.limits.max_source_bytes;
            buildObject["max_tokens"] = build.limits.max_tokens;
            buildObject["max_state_words"] = build.limits.max_state_words;
            buildObject["max_expression_depth"] = build.limits.max_expression_depth;
            root["build"] = std::move(buildObject);
        }
        nlohmann::ordered_json components = nlohmann::ordered_json::array();
        for (size_t index = 0; index < items.size(); ++index) {
            if (index == skip) continue;
            components.push_back(entryFor(items[index]));
        }
        if (extra) components.push_back(entryFor(*extra));
        root["components"] = std::move(components);
        return root.dump(2) + "\n";
    }

    bool nameTaken(const std::filesystem::path& relative, size_t ignore) const {
        const std::string wanted = lowerAscii(relativeName(relative));
        for (size_t index = 0; index < items.size(); ++index) {
            if (index == ignore) continue;
            if (lowerAscii(relativeName(items[index].relativeSource)) == wanted) return true;
        }
        return false;
    }

    bool keyTaken(const std::string& key, size_t ignore) const {
        const std::string wanted = lowerAscii(key);
        for (size_t index = 0; index < items.size(); ++index) {
            if (index == ignore) continue;
            if (lowerAscii(items[index].info.key) == wanted) return true;
        }
        return false;
    }

    bool sourceAllowed(const std::filesystem::path& relative) const {
        if (metadata.format < 2) return true;
        for (const auto& sourceRoot : metadata.source_roots)
            if (sourceInsideRoot(relative, sourceRoot)) return true;
        return false;
    }

    std::string freshKey(const std::filesystem::path& relative) const {
        std::string base = identityStem(relative);
        std::replace(base.begin(), base.end(), '/', '.');
        for (char& c : base)
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-' && c != '.')
                c = '-';
        if (!safeProjectKey(base)) base = "component";
        std::string candidate = base;
        for (size_t suffix = 2; keyTaken(candidate, static_cast<size_t>(-1)); ++suffix)
            candidate = base + "-" + std::to_string(suffix);
        return candidate;
    }

    std::filesystem::path resolve(const std::filesystem::path& relative) const {
        return (manifest.parent_path() / relative).lexically_normal();
    }

    std::set<uint64_t> customIds() const {
        std::set<uint64_t> ids;
        for (const Item& item : items) ids.insert(item.component->metadata().custom_id);
        return ids;
    }

    uint64_t freshCustomId(const std::string& seed) const {
        const std::set<uint64_t> taken = customIds();
        uint64_t id = stableHash(seed);
        if (!id) id = 1;
        while (taken.count(id)) {
            id = id * 6364136223846793005ULL + 1442695040888963407ULL;
            if (!id) id = 1;
        }
        return id;
    }

    /* Shared body of createSource/duplicateSource.  `inherited` carries the
       metadata of the file being copied (null for a brand-new file). */
    bool insertNewFile(const std::filesystem::path& relative, const std::string& source,
                       const ComponentMetadata* inherited,
                       std::vector<ProjectDiagnostic>* diagnostics,
                       const CompileOptions& options) {
        const std::string name = relativeName(relative);
        if (!safeSourceName(name)) {
            pushSaveDiagnostic(diagnostics, static_cast<size_t>(-1), {}, relative,
                               "name must be a safe relative .v path using '/' separators");
            return false;
        }
        if (items.size() >= kMaxComponents) {
            pushSaveDiagnostic(diagnostics, static_cast<size_t>(-1), {}, relative,
                               "a project can hold at most 256 components");
            return false;
        }
        if (nameTaken(relative, static_cast<size_t>(-1))) {
            pushSaveDiagnostic(diagnostics, static_cast<size_t>(-1), {}, relative,
                               "that file name is already in the project");
            return false;
        }
        if (!sourceAllowed(relative)) {
            pushSaveDiagnostic(diagnostics, static_cast<size_t>(-1), {}, relative,
                               "source is outside the project's source_roots");
            return false;
        }
        const std::filesystem::path target = resolve(relative);
        std::error_code statusError;
        if (std::filesystem::exists(target, statusError) && !statusError) {
            pushSaveDiagnostic(diagnostics, static_cast<size_t>(-1), {}, target,
                               "a file with that name already exists");
            return false;
        }

        ComponentMetadata metadata;
        if (inherited) metadata = *inherited;
        metadata.custom_id = freshCustomId(identityStem(relative));
        metadata.name = relative.stem().u8string();
        /* A brand-new component gets the game's unit delay, like the bundled
           example; a copy keeps the settings of the file it came from. */
        if (!inherited) metadata.delay = 1;
        if (!metadata.name.empty() && !typePrefix.empty())
            metadata.type_id = typePrefix + "/" + identityStem(relative);

        std::unique_ptr<Component> compiled(new Component);
        std::vector<Diagnostic> compileDiagnostics;
        if (!compiled->compile(source, metadata, &compileDiagnostics, effectiveOptions(options))) {
            const Diagnostic detail = compileDiagnostics.empty()
                                          ? Diagnostic{0, 0, "compile failed"}
                                          : compileDiagnostics.front();
            if (diagnostics) diagnostics->push_back({ProjectStage::Compile,
                static_cast<size_t>(-1), metadata.type_id, target,
                detail.line, detail.column, detail.message});
            return false;
        }

        Item candidate;
        candidate.relativeSource = relative;
        candidate.sourcePath = target;
        candidate.source = source;
        candidate.component = std::move(compiled);
        candidate.info.key = freshKey(relative);
        candidate.info.source = relative;
        candidate.info.top_module = candidate.component->program().moduleName();
        candidate.info.group = relative.parent_path().generic_u8string();
        const std::string newManifest = manifestTextExcluding(static_cast<size_t>(-1), &candidate);

        std::string error;
        std::error_code directoryError;
        std::filesystem::create_directories(target.parent_path(), directoryError);
        if (!writeTextFileAtomic(target, source, &error)) {
            pushSaveDiagnostic(diagnostics, static_cast<size_t>(-1), metadata.type_id, target,
                               error.empty() ? "cannot write source file" : error);
            return false;
        }
        if (!writeTextFileAtomic(manifest, newManifest, &error)) {
            std::error_code ignored;
            std::filesystem::remove(target, ignored);
            pushSaveDiagnostic(diagnostics, static_cast<size_t>(-1), {}, manifest,
                               "cannot rewrite manifest: " + error);
            return false;
        }
        items.push_back(std::move(candidate));
        manifestText = newManifest;
        return true;
    }
};

const char* projectStageName(ProjectStage stage) noexcept {
    switch (stage) {
        case ProjectStage::Manifest: return "manifest";
        case ProjectStage::Source: return "source";
        case ProjectStage::Compile: return "compile";
        case ProjectStage::Register: return "register";
        case ProjectStage::Save: return "save";
    }
    return "unknown";
}

std::string formatDiagnostic(const ProjectDiagnostic& diagnostic) {
    std::ostringstream out;
    out << projectStageName(diagnostic.stage) << ": ";
    if (!diagnostic.file.empty()) out << diagnostic.file.u8string();
    if (diagnostic.line) out << ':' << diagnostic.line << ':' << diagnostic.column;
    if (!diagnostic.component_id.empty()) out << " [" << diagnostic.component_id << ']';
    if (!diagnostic.file.empty() || !diagnostic.component_id.empty()) out << ": ";
    out << diagnostic.message;
    return out.str();
}

std::filesystem::path moduleDirectory(std::string* error) noexcept {
    if (error) error->clear();
    try {
        HMODULE module = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCWSTR>(&moduleDirectory), &module)) {
            if (error) *error = "GetModuleHandleExW failed";
            return {};
        }
        std::wstring buffer(1024, L'\0');
        for (;;) {
            const DWORD written = GetModuleFileNameW(module, &buffer[0], static_cast<DWORD>(buffer.size()));
            if (!written) { if (error) *error = "GetModuleFileNameW failed"; return {}; }
            if (written < buffer.size() - 1) {
                buffer.resize(written);
                return std::filesystem::path(buffer).parent_path();
            }
            if (buffer.size() >= 32768) { if (error) *error = "module path is too long"; return {}; }
            buffer.resize(buffer.size() * 2);
        }
    } catch (const std::exception& exception) {
        if (error) *error = exception.what();
        return {};
    } catch (...) {
        if (error) *error = "unknown module path error";
        return {};
    }
}

bool writeTextFileAtomic(const std::filesystem::path& path, const std::string& text,
                         std::string* error) noexcept {
    if (error) error->clear();
    std::filesystem::path temporary = path; temporary += L".tc-tmp";
    try {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("cannot open temporary file");
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        output.flush();
        if (!output) throw std::runtime_error("cannot write complete temporary file");
        output.close();
        if (!MoveFileExW(temporary.c_str(), path.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("cannot atomically replace target file");
        return true;
    } catch (const std::exception& exception) {
        std::error_code ignored; std::filesystem::remove(temporary, ignored);
        if (error) *error = exception.what();
        return false;
    } catch (...) {
        std::error_code ignored; std::filesystem::remove(temporary, ignored);
        if (error) *error = "unknown file write error";
        return false;
    }
}

Project::Project() = default;
Project::~Project() = default;
Project::Project(Project&&) noexcept = default;
Project& Project::operator=(Project&&) noexcept = default;

bool Project::load(const std::filesystem::path& manifest,
                   std::vector<ProjectDiagnostic>* diagnostics, const CompileOptions& options) {
    if (diagnostics) diagnostics->clear();
    try {
        const std::string manifestText = readText(manifest, kMaxManifestBytes, ProjectStage::Manifest);
        Json root;
        try { root = Json::parse(manifestText); }
        catch (const Json::parse_error& error) {
            const auto where = lineColumn(manifestText, error.byte);
            throw ProjectError(ProjectStage::Manifest, static_cast<size_t>(-1), {}, manifest,
                               where.first, where.second, error.what());
        }
        requireObject(root, manifest, static_cast<size_t>(-1), {}, "manifest root");
        if (!root.contains("format") || !root["format"].is_number_unsigned())
            throw ProjectError(ProjectStage::Manifest, static_cast<size_t>(-1), {}, manifest, 0, 0,
                               "format must be an unsigned integer");
        const uint64_t format = root["format"].get<uint64_t>();
        if (format != 1 && format != 2)
            throw ProjectError(ProjectStage::Manifest, static_cast<size_t>(-1), {}, manifest, 0, 0,
                               "format must be the unsigned integer 1 or 2");
        rejectUnknown(root, format == 1 ? std::set<std::string>{"format", "components"}
                                        : std::set<std::string>{"format", "project", "build", "components"},
                      manifest, static_cast<size_t>(-1), {});
        if (!root.contains("components") || !root["components"].is_array())
            throw ProjectError(ProjectStage::Manifest, static_cast<size_t>(-1), {}, manifest, 0, 0,
                               "components must be an array");
        if (root["components"].empty() || root["components"].size() > kMaxComponents)
            throw ProjectError(ProjectStage::Manifest, static_cast<size_t>(-1), {}, manifest, 0, 0,
                               "components must contain 1..256 entries");

        std::unique_ptr<Impl> candidate(new Impl);
        candidate->manifest = std::filesystem::absolute(manifest).lexically_normal();
        candidate->manifestText = manifestText;
        candidate->metadata.format = static_cast<uint32_t>(format);
        candidate->build.limits = options;
        const std::filesystem::path directory = candidate->manifest.parent_path();

        if (format == 1) {
            candidate->metadata.id = candidate->manifest.stem().u8string();
            candidate->metadata.name = candidate->metadata.id;
            candidate->metadata.source_roots.push_back(std::filesystem::path("."));
        } else {
            if (!root.contains("project"))
                throw ProjectError(ProjectStage::Manifest, static_cast<size_t>(-1), {}, manifest,
                                   0, 0, "format 2 requires a project object");
            const Json& project = root["project"];
            requireObject(project, manifest, static_cast<size_t>(-1), {}, "project");
            rejectUnknown(project, {"id", "name", "description", "source_roots",
                                    "default_component"}, manifest,
                          static_cast<size_t>(-1), {});
            candidate->metadata.id = stringField(project, "id", true, manifest,
                                                  static_cast<size_t>(-1), {});
            candidate->metadata.name = stringField(project, "name", true, manifest,
                                                    static_cast<size_t>(-1), {});
            candidate->metadata.description = stringField(project, "description", false, manifest,
                                                           static_cast<size_t>(-1), {});
            candidate->metadata.default_component = stringField(
                project, "default_component", true, manifest, static_cast<size_t>(-1), {});
            if (!safeProjectKey(candidate->metadata.id))
                throw ProjectError(ProjectStage::Manifest, static_cast<size_t>(-1), {}, manifest,
                                   0, 0, "project id must contain only letters, digits, '.', '_' or '-'");
            if (candidate->metadata.name.empty() || candidate->metadata.name.size() > 160)
                throw ProjectError(ProjectStage::Manifest, static_cast<size_t>(-1), {}, manifest,
                                   0, 0, "project name must contain 1..160 bytes");
            if (candidate->metadata.description.size() > 2048)
                throw ProjectError(ProjectStage::Manifest, static_cast<size_t>(-1), {}, manifest,
                                   0, 0, "project description exceeds 2048 bytes");
            if (!safeProjectKey(candidate->metadata.default_component))
                throw ProjectError(ProjectStage::Manifest, static_cast<size_t>(-1), {}, manifest,
                                   0, 0, "default_component is not a valid component key");
            if (!project.contains("source_roots") || !project["source_roots"].is_array() ||
                project["source_roots"].empty() ||
                project["source_roots"].size() > kMaxSourceRoots)
                throw ProjectError(ProjectStage::Manifest, static_cast<size_t>(-1), {}, manifest,
                                   0, 0, "source_roots must contain 1..32 relative directories");
            std::set<std::string> roots;
            for (const Json& sourceRoot : project["source_roots"]) {
                if (!sourceRoot.is_string())
                    throw ProjectError(ProjectStage::Manifest, static_cast<size_t>(-1), {}, manifest,
                                       0, 0, "each source_root must be a string");
                const std::string value = sourceRoot.get<std::string>();
                if (!safeRelativeDirectory(value))
                    throw ProjectError(ProjectStage::Manifest, static_cast<size_t>(-1), {}, manifest,
                                       0, 0, "source_root must be '.' or a safe relative directory");
                if (!roots.insert(lowerAscii(value)).second)
                    throw ProjectError(ProjectStage::Manifest, static_cast<size_t>(-1), {}, manifest,
                                       0, 0, "duplicate source_root in project");
                candidate->metadata.source_roots.push_back(std::filesystem::u8path(value));
            }

            if (root.contains("build")) {
                const Json& build = root["build"];
                requireObject(build, manifest, static_cast<size_t>(-1), {}, "build");
                rejectUnknown(build, {"max_source_bytes", "max_tokens", "max_state_words",
                                      "max_expression_depth"}, manifest,
                              static_cast<size_t>(-1), {});
                candidate->build.limits.max_source_bytes = boundedUint32Field(
                    build, "max_source_bytes", options.max_source_bytes, 1,
                    options.max_source_bytes, manifest);
                candidate->build.limits.max_tokens = boundedUint32Field(
                    build, "max_tokens", options.max_tokens, 1, options.max_tokens, manifest);
                candidate->build.limits.max_state_words = boundedUint32Field(
                    build, "max_state_words", options.max_state_words, 1,
                    options.max_state_words, manifest);
                candidate->build.limits.max_expression_depth = boundedUint32Field(
                    build, "max_expression_depth", options.max_expression_depth, 1,
                    options.max_expression_depth, manifest);
            }
        }

        std::set<uint64_t> ids;
        std::set<std::string> keys;
        for (size_t index = 0; index < root["components"].size(); ++index) {
            const Json& item = root["components"][index];
            requireObject(item, manifest, index, {}, "component entry");
            rejectUnknown(item, format == 1
                                      ? std::set<std::string>{"source", "custom_id", "type_id",
                                                              "name", "description", "shape_svg",
                                                              "gate_cost", "delay", "pin_lane",
                                                              "show_description"}
                                      : std::set<std::string>{"key", "source", "top_module", "group",
                                                              "enabled", "custom_id", "type_id",
                                                              "name", "description", "shape_svg",
                                                              "gate_cost", "delay", "pin_lane",
                                                              "show_description"},
                          manifest, index, {});
            ComponentMetadata metadata;
            metadata.type_id = stringField(item, "type_id", false, manifest, index, {});
            metadata.name = stringField(item, "name", false, manifest, index, metadata.type_id);
            const std::string id = componentIdentity(metadata, index);
            metadata.description = stringField(item, "description", false, manifest, index, id);
            metadata.shape_svg = stringField(item, "shape_svg", false, manifest, index, id);
            metadata.custom_id = unsignedField(item, "custom_id", 0, true, manifest, index, id);
            metadata.gate_cost = unsignedField(item, "gate_cost", 0, false, manifest, index, id);
            metadata.delay = unsignedField(item, "delay", 0, false, manifest, index, id);
            if (!metadata.custom_id) throw ProjectError(ProjectStage::Manifest, index, id, manifest, 0, 0,
                                                        "custom_id must be nonzero");
            if (!ids.insert(metadata.custom_id).second)
                throw ProjectError(ProjectStage::Manifest, index, id, manifest, 0, 0,
                                   "duplicate custom_id in project");
            if (item.contains("pin_lane")) {
                if (!item["pin_lane"].is_number())
                    throw ProjectError(ProjectStage::Manifest, index, id, manifest, 0, 0,
                                       "pin_lane must be a number");
                const double lane = item["pin_lane"].get<double>();
                if (!std::isfinite(lane) || lane < 0 || lane > 32)
                    throw ProjectError(ProjectStage::Manifest, index, id, manifest, 0, 0,
                                       "pin_lane must be finite and between 0 and 32");
                metadata.pin_lane = static_cast<float>(lane);
            }
            if (item.contains("show_description")) {
                if (!item["show_description"].is_boolean())
                    throw ProjectError(ProjectStage::Manifest, index, id, manifest, 0, 0,
                                       "show_description must be boolean");
                if (item["show_description"].get<bool>()) metadata.flags |= TC_COMPONENT_TYPE_SHOW_DESCRIPTION;
            }
            const std::string sourceName = stringField(item, "source", true, manifest, index, id);
            if (!safeSourceName(sourceName))
                throw ProjectError(ProjectStage::Manifest, index, id, manifest, 0, 0,
                                   "source must be a safe relative .v path using '/' separators");
            Impl::Item loaded;
            loaded.relativeSource = std::filesystem::u8path(sourceName);
            loaded.info.source = loaded.relativeSource;
            loaded.info.group = loaded.relativeSource.parent_path().generic_u8string();
            if (format == 2) {
                loaded.info.key = stringField(item, "key", true, manifest, index, id);
                loaded.info.top_module = stringField(item, "top_module", false, manifest, index, id);
                loaded.info.group = stringField(item, "group", false, manifest, index, id);
                loaded.info.enabled = booleanField(item, "enabled", true, manifest, index, id);
                if (!safeProjectKey(loaded.info.key))
                    throw ProjectError(ProjectStage::Manifest, index, id, manifest, 0, 0,
                                       "component key must contain only letters, digits, '.', '_' or '-'");
                if (!keys.insert(lowerAscii(loaded.info.key)).second)
                    throw ProjectError(ProjectStage::Manifest, index, id, manifest, 0, 0,
                                       "duplicate component key in project");
                if (!loaded.info.top_module.empty() && !safeVerilogIdentifier(loaded.info.top_module))
                    throw ProjectError(ProjectStage::Manifest, index, id, manifest, 0, 0,
                                       "top_module is not a valid Verilog identifier");
                if (loaded.info.group.size() > 160 || loaded.info.group.find('\n') != std::string::npos ||
                    loaded.info.group.find('\r') != std::string::npos)
                    throw ProjectError(ProjectStage::Manifest, index, id, manifest, 0, 0,
                                       "group must be a single line of at most 160 bytes");
                bool inside = false;
                for (const auto& sourceRoot : candidate->metadata.source_roots)
                    if (sourceInsideRoot(loaded.relativeSource, sourceRoot)) { inside = true; break; }
                if (!inside)
                    throw ProjectError(ProjectStage::Manifest, index, id, manifest, 0, 0,
                                       "source is outside the project's source_roots");
            } else {
                loaded.info.key = identityStem(loaded.relativeSource);
                std::replace(loaded.info.key.begin(), loaded.info.key.end(), '/', '.');
                keys.insert(lowerAscii(loaded.info.key));
            }
            loaded.sourcePath = (directory / loaded.relativeSource).lexically_normal();
            loaded.source = readText(loaded.sourcePath, candidate->build.limits.max_source_bytes,
                                     ProjectStage::Source, index, id);
            loaded.component.reset(new Component);
            std::vector<Diagnostic> compileDiagnostics;
            if (!loaded.component->compile(loaded.source, metadata, &compileDiagnostics,
                                           candidate->build.limits)) {
                /* A source that does not compile no longer aborts the whole
                   load: the entry is kept (with its text and its diagnostic) so
                   the in-game editor can still open it and the user can fix it
                   there.  The component itself stays invalid, so it is skipped
                   at registration and the next start retries. */
                const Diagnostic detail = compileDiagnostics.empty()
                                              ? Diagnostic{0, 0, "compile failed"}
                                              : compileDiagnostics.front();
                if (diagnostics) {
                    diagnostics->push_back({ProjectStage::Compile, index, id, loaded.sourcePath,
                                            detail.line, detail.column, detail.message});
                }
            } else {
                const std::string compiledTop = loaded.component->program().moduleName();
                if (!loaded.info.top_module.empty() && loaded.info.top_module != compiledTop)
                    throw ProjectError(ProjectStage::Manifest, index, id, manifest, 0, 0,
                                       "top_module '" + loaded.info.top_module +
                                           "' does not match compiled top module '" + compiledTop + "'");
                loaded.info.top_module = compiledTop;
            }
            candidate->items.push_back(std::move(loaded));
        }
        if (format == 1) {
            candidate->metadata.default_component = candidate->items.front().info.key;
        } else {
            bool foundDefault = false;
            for (const Impl::Item& item : candidate->items) {
                if (lowerAscii(item.info.key) !=
                    lowerAscii(candidate->metadata.default_component)) continue;
                if (!item.info.enabled)
                    throw ProjectError(ProjectStage::Manifest, static_cast<size_t>(-1), {}, manifest,
                                       0, 0, "default_component must reference an enabled component");
                foundDefault = true;
                break;
            }
            if (!foundDefault)
                throw ProjectError(ProjectStage::Manifest, static_cast<size_t>(-1), {}, manifest,
                                   0, 0, "default_component does not reference a component key");
        }
        impl_ = std::move(candidate);
        return true;
    } catch (const ProjectError& error) {
        if (diagnostics) diagnostics->push_back({error.stage, error.index, error.id, error.file,
                                                 error.line, error.column, error.what()});
    } catch (const std::exception& error) {
        if (diagnostics) diagnostics->push_back({ProjectStage::Manifest, static_cast<size_t>(-1), {}, manifest,
                                                 0, 0, error.what()});
    } catch (...) {
        if (diagnostics) diagnostics->push_back({ProjectStage::Manifest, static_cast<size_t>(-1), {}, manifest,
                                                 0, 0, "unknown project load error"});
    }
    return false;
}

bool Project::loadAdjacent(const std::filesystem::path& manifestName,
                           std::vector<ProjectDiagnostic>* diagnostics, const CompileOptions& options) {
    std::string error;
    const std::filesystem::path directory = moduleDirectory(&error);
    if (directory.empty()) {
        if (diagnostics) {
            diagnostics->clear();
            diagnostics->push_back({ProjectStage::Manifest, static_cast<size_t>(-1), {}, manifestName,
                                    0, 0, error.empty() ? "cannot locate module directory" : error});
        }
        return false;
    }
    return load(directory / manifestName, diagnostics, options);
}

bool Project::createNew(const std::filesystem::path& directory,
                        const ProjectCreateOptions& create,
                        std::vector<ProjectDiagnostic>* diagnostics) {
    if (diagnostics) diagnostics->clear();
    const std::filesystem::path targetDirectory =
        directory.empty() ? directory : std::filesystem::absolute(directory).lexically_normal();
    const std::filesystem::path manifest = targetDirectory / "tc-verilog.json";
    const std::filesystem::path relativeSource =
        create.source_root / std::filesystem::u8path(create.top_module + ".v");
    const std::filesystem::path source = targetDirectory / relativeSource;
    try {
        if (targetDirectory.empty())
            throw ProjectError(ProjectStage::Save, static_cast<size_t>(-1), {}, directory, 0, 0,
                               "project directory is empty");
        if (!safeProjectKey(create.id))
            throw ProjectError(ProjectStage::Save, static_cast<size_t>(-1), create.id, manifest,
                               0, 0, "project id must contain only letters, digits, '.', '_' or '-'");
        if (create.name.empty() || create.name.size() > 160)
            throw ProjectError(ProjectStage::Save, static_cast<size_t>(-1), create.id, manifest,
                               0, 0, "project name must contain 1..160 bytes");
        if (create.description.size() > 2048)
            throw ProjectError(ProjectStage::Save, static_cast<size_t>(-1), create.id, manifest,
                               0, 0, "project description exceeds 2048 bytes");
        const std::string sourceRoot = relativeName(create.source_root);
        if (!safeRelativeDirectory(sourceRoot))
            throw ProjectError(ProjectStage::Save, static_cast<size_t>(-1), create.id, manifest,
                               0, 0, "source_root must be '.' or a safe relative directory");
        if (!safeProjectKey(create.component_key))
            throw ProjectError(ProjectStage::Save, 0, create.component_key, manifest, 0, 0,
                               "component key must contain only letters, digits, '.', '_' or '-'");
        if (!safeVerilogIdentifier(create.top_module))
            throw ProjectError(ProjectStage::Save, 0, create.component_key, source, 0, 0,
                               "top_module is not a valid Verilog identifier");
        if (!safeSourceName(relativeName(relativeSource)))
            throw ProjectError(ProjectStage::Save, 0, create.component_key, source, 0, 0,
                               "initial source path is not a safe relative .v path");
        if (create.initial_source.empty())
            throw ProjectError(ProjectStage::Save, 0, create.component_key, source, 0, 0,
                               "initial source is empty");
        const CompileOptions ceiling;
        if (create.limits.max_source_bytes > ceiling.max_source_bytes ||
            create.limits.max_tokens > ceiling.max_tokens ||
            create.limits.max_state_words > ceiling.max_state_words ||
            create.limits.max_expression_depth > ceiling.max_expression_depth)
            throw ProjectError(ProjectStage::Save, static_cast<size_t>(-1), create.id, manifest,
                               0, 0, "project build limits exceed the host defaults");

        std::error_code statusError;
        if (std::filesystem::exists(manifest, statusError) && !statusError)
            throw ProjectError(ProjectStage::Save, static_cast<size_t>(-1), create.id, manifest,
                               0, 0, "a project manifest already exists in this directory");
        if (statusError) throw std::runtime_error("cannot inspect project manifest destination");
        if (std::filesystem::exists(source, statusError) && !statusError)
            throw ProjectError(ProjectStage::Save, 0, create.component_key, source, 0, 0,
                               "the initial source file already exists");
        if (statusError) throw std::runtime_error("cannot inspect initial source destination");

        ComponentMetadata componentMetadata;
        componentMetadata.custom_id = stableHash(create.id + ":" + create.component_key);
        if (!componentMetadata.custom_id) componentMetadata.custom_id = 1;
        componentMetadata.type_id = create.type_prefix.empty()
                                        ? std::string()
                                        : create.type_prefix + "/" + create.component_key;
        componentMetadata.name = create.top_module;
        componentMetadata.delay = 1;
        Component compiled;
        std::vector<Diagnostic> compileDiagnostics;
        if (!compiled.compile(create.initial_source, componentMetadata, &compileDiagnostics,
                              create.limits)) {
            const Diagnostic detail = compileDiagnostics.empty()
                                          ? Diagnostic{0, 0, "compile failed"}
                                          : compileDiagnostics.front();
            throw ProjectError(ProjectStage::Compile, 0, create.component_key, source,
                               detail.line, detail.column, detail.message);
        }
        if (compiled.program().moduleName() != create.top_module)
            throw ProjectError(ProjectStage::Compile, 0, create.component_key, source, 0, 0,
                               "compiled top module does not match top_module");

        nlohmann::ordered_json root;
        root["format"] = 2;
        nlohmann::ordered_json projectObject;
        projectObject["id"] = create.id;
        projectObject["name"] = create.name;
        if (!create.description.empty()) projectObject["description"] = create.description;
        projectObject["source_roots"] = nlohmann::ordered_json::array({sourceRoot});
        projectObject["default_component"] = create.component_key;
        root["project"] = std::move(projectObject);
        root["build"] = {{"max_source_bytes", create.limits.max_source_bytes},
                         {"max_tokens", create.limits.max_tokens},
                         {"max_state_words", create.limits.max_state_words},
                         {"max_expression_depth", create.limits.max_expression_depth}};
        std::ostringstream customId;
        customId << "0x" << std::hex << componentMetadata.custom_id;
        nlohmann::ordered_json component;
        component["key"] = create.component_key;
        component["source"] = relativeName(relativeSource);
        component["top_module"] = create.top_module;
        component["custom_id"] = customId.str();
        if (!componentMetadata.type_id.empty()) component["type_id"] = componentMetadata.type_id;
        component["name"] = componentMetadata.name;
        component["delay"] = componentMetadata.delay;
        root["components"] = nlohmann::ordered_json::array({std::move(component)});

        const bool directoryExisted = std::filesystem::exists(targetDirectory);
        const bool sourceDirectoryExisted = std::filesystem::exists(source.parent_path());
        std::filesystem::create_directories(source.parent_path());
        std::string writeError;
        if (!writeTextFileAtomic(source, create.initial_source, &writeError))
            throw ProjectError(ProjectStage::Save, 0, create.component_key, source, 0, 0,
                               writeError.empty() ? "cannot write initial source" : writeError);
        if (!writeTextFileAtomic(manifest, root.dump(2) + "\n", &writeError)) {
            std::error_code ignored;
            std::filesystem::remove(source, ignored);
            if (!sourceDirectoryExisted) std::filesystem::remove(source.parent_path(), ignored);
            if (!directoryExisted) std::filesystem::remove(targetDirectory, ignored);
            throw ProjectError(ProjectStage::Save, static_cast<size_t>(-1), create.id, manifest,
                               0, 0, writeError.empty() ? "cannot write project manifest" : writeError);
        }

        Project loaded;
        std::vector<ProjectDiagnostic> loadDiagnostics;
        if (!loaded.load(manifest, &loadDiagnostics)) {
            std::error_code ignored;
            std::filesystem::remove(manifest, ignored);
            std::filesystem::remove(source, ignored);
            if (!sourceDirectoryExisted) std::filesystem::remove(source.parent_path(), ignored);
            if (!directoryExisted) std::filesystem::remove(targetDirectory, ignored);
            if (!loadDiagnostics.empty()) {
                if (diagnostics) *diagnostics = std::move(loadDiagnostics);
                return false;
            }
            throw std::runtime_error("created project could not be loaded");
        }
        impl_ = std::move(loaded.impl_);
        return true;
    } catch (const ProjectError& error) {
        if (diagnostics) diagnostics->push_back({error.stage, error.index, error.id, error.file,
                                                 error.line, error.column, error.what()});
    } catch (const std::exception& error) {
        if (diagnostics) diagnostics->push_back({ProjectStage::Save, static_cast<size_t>(-1),
                                                 create.id, manifest, 0, 0, error.what()});
    } catch (...) {
        if (diagnostics) diagnostics->push_back({ProjectStage::Save, static_cast<size_t>(-1),
                                                 create.id, manifest, 0, 0,
                                                 "unknown project creation error"});
    }
    return false;
}

bool Project::bootstrapWorkspace(const std::filesystem::path& directory,
                                 std::vector<ProjectDiagnostic>* diagnostics) const noexcept {
    if (diagnostics) diagnostics->clear();
    if (!impl_ || directory.empty()) {
        if (diagnostics) diagnostics->push_back({ProjectStage::Save, static_cast<size_t>(-1), {},
                                                 directory, 0, 0, "project or workspace directory is invalid"});
        return false;
    }
    try {
        const std::filesystem::path targetManifest = workspaceManifestPath(directory);
        std::error_code statusError;
        if (std::filesystem::exists(targetManifest, statusError)) return true;
        if (statusError) throw std::runtime_error("cannot inspect workspace manifest");
        std::filesystem::create_directories(directory);
        for (size_t index = 0; index < impl_->items.size(); ++index) {
            const Impl::Item& item = impl_->items[index];
            const std::filesystem::path target = directory / item.relativeSource;
            std::filesystem::create_directories(target.parent_path());
            std::error_code sourceError;
            if (std::filesystem::exists(target, sourceError)) continue;
            if (sourceError) throw ProjectError(ProjectStage::Save, index,
                componentIdentity(item.component->metadata(), index), target, 0, 0,
                "cannot inspect workspace source");
            std::string error;
            if (!writeTextFileAtomic(target, item.source, &error))
                throw ProjectError(ProjectStage::Save, index,
                    componentIdentity(item.component->metadata(), index), target, 0, 0, error);
        }
        std::string error;
        if (!writeTextFileAtomic(targetManifest, impl_->manifestText, &error))
            throw ProjectError(ProjectStage::Save, static_cast<size_t>(-1), {}, targetManifest, 0, 0, error);
        return true;
    } catch (const ProjectError& error) {
        if (diagnostics) diagnostics->push_back({error.stage, error.index, error.id, error.file,
                                                 error.line, error.column, error.what()});
    } catch (const std::exception& error) {
        if (diagnostics) diagnostics->push_back({ProjectStage::Save, static_cast<size_t>(-1), {},
                                                 directory, 0, 0, error.what()});
    } catch (...) {
        if (diagnostics) diagnostics->push_back({ProjectStage::Save, static_cast<size_t>(-1), {},
                                                 directory, 0, 0, "unknown workspace error"});
    }
    return false;
}

int Project::registerAll(const TCHost* host, std::vector<ProjectDiagnostic>* diagnostics) noexcept {
    if (diagnostics) diagnostics->clear();
    if (!impl_) return TC_COMPONENT_TYPES_ERR_ARGUMENT;
    try {
        int firstError = TC_COMPONENT_TYPES_OK;
        for (size_t index = 0; index < impl_->items.size(); ++index) {
            if (!impl_->items[index].info.enabled) continue;
            Component& component = *impl_->items[index].component;
            const int status = component.registerWith(host);
            if (status == TC_COMPONENT_TYPES_OK) continue;
            /* One entry that the host refuses (a duplicate id, a broken source,
               a shape the bridge cannot carry) must not stop the others: the
               editor page has to keep loading so the problem can be fixed in
               place, and the caller gets the first failure back. */
            if (diagnostics) diagnostics->push_back({ProjectStage::Register, index,
                componentIdentity(component.metadata(), index), impl_->manifest, 0, 0,
                tc::component_types::errorText(status)});
            if (firstError == TC_COMPONENT_TYPES_OK) firstError = status;
        }
        return firstError;
    } catch (...) {
        if (diagnostics) diagnostics->push_back({ProjectStage::Register, static_cast<size_t>(-1), {},
                                                 impl_->manifest, 0, 0, "unknown registration error"});
        return TC_COMPONENT_TYPES_ERR_UNAVAILABLE;
    }
}

bool Project::validateSource(size_t index, const std::string& source,
                             std::vector<ProjectDiagnostic>* diagnostics,
                             const CompileOptions& options) const noexcept {
    if (diagnostics) diagnostics->clear();
    if (!impl_ || index >= impl_->items.size()) {
        if (diagnostics) diagnostics->push_back({ProjectStage::Compile, index, {}, {}, 0, 0,
                                                 "source index is outside the project"});
        return false;
    }
    const Impl::Item& item = impl_->items[index];
    const std::string id = componentIdentity(item.component->metadata(), index);
    try {
        Component candidate;
        std::vector<Diagnostic> compileDiagnostics;
        if (!candidate.compile(source, item.component->metadata(), &compileDiagnostics,
                               impl_->effectiveOptions(options))) {
            const Diagnostic detail = compileDiagnostics.empty()
                                          ? Diagnostic{0, 0, "compile failed"}
                                          : compileDiagnostics.front();
            if (diagnostics) diagnostics->push_back({ProjectStage::Compile, index, id,
                item.sourcePath, detail.line, detail.column, detail.message});
            return false;
        }
        if (!item.info.top_module.empty() &&
            candidate.program().moduleName() != item.info.top_module) {
            if (diagnostics) diagnostics->push_back({ProjectStage::Compile, index, id,
                item.sourcePath, 0, 0, "compiled top module '" + candidate.program().moduleName() +
                "' does not match project top_module '" + item.info.top_module + "'"});
            return false;
        }
        return true;
    } catch (const std::exception& error) {
        if (diagnostics) diagnostics->push_back({ProjectStage::Compile, index, id,
                                                 item.sourcePath, 0, 0, error.what()});
    } catch (...) {
        if (diagnostics) diagnostics->push_back({ProjectStage::Compile, index, id,
                                                 item.sourcePath, 0, 0,
                                                 "unknown source validation error"});
    }
    return false;
}

bool Project::validateAndSaveSource(size_t index, const std::string& source,
                                    std::vector<ProjectDiagnostic>* diagnostics,
                                    const CompileOptions& options) const noexcept {
    if (!validateSource(index, source, diagnostics, options)) return false;
    const Impl::Item& item = impl_->items[index];
    std::string error;
    if (writeTextFileAtomic(item.sourcePath, source, &error)) return true;
    if (diagnostics) diagnostics->push_back({ProjectStage::Save, index,
        componentIdentity(item.component->metadata(), index), item.sourcePath, 0, 0,
        error.empty() ? "cannot save source" : error});
    return false;
}

void Project::setTypePrefix(std::string prefix) {
    if (impl_) impl_->typePrefix = std::move(prefix);
}

bool Project::saveAndReloadSource(size_t index, const std::string& source, bool* reloaded,
                                  std::vector<ProjectDiagnostic>* diagnostics,
                                  const CompileOptions& options) const noexcept {
    if (reloaded) *reloaded = false;
    if (diagnostics) diagnostics->clear();
    if (!impl_ || index >= impl_->items.size()) {
        if (diagnostics) diagnostics->push_back({ProjectStage::Compile, index, {}, {}, 0, 0,
                                                  "source index is outside the project"});
        return false;
    }
    const Impl::Item& item = impl_->items[index];
    const std::string id = componentIdentity(item.component->metadata(), index);
    /* A hot reload is only possible while the shape stands still; when it does
       not, the project's ordinary gate tells a compile error from a legitimate
       change of ports, and a change of ports still has to be saved. */
    if (!validateSource(index, source, diagnostics, options)) return false;
    std::vector<Diagnostic> hotDiagnostics;
    const CompileOptions effective = impl_->effectiveOptions(options);
    const bool hot = item.component->checkHotSwap(source, &hotDiagnostics, effective);
    std::string error;
    if (!writeTextFileAtomic(item.sourcePath, source, &error)) {
        if (diagnostics) diagnostics->push_back({ProjectStage::Save, index, id, item.sourcePath,
                                                 0, 0, error.empty() ? "cannot save source" : error});
        return false;
    }
    if (hot) {
        std::vector<Diagnostic> applied;
        if (item.component->hotSwap(source, &applied, effective) && reloaded) *reloaded = true;
    }
    impl_->items[index].source = source;
    return true;
}

const std::string& Project::typePrefix() const noexcept {
    static const std::string empty;
    return impl_ ? impl_->typePrefix : empty;
}

int Project::registerLive(size_t index, const TCHost* host) noexcept {
    if (!impl_ || index >= impl_->items.size() || !impl_->items[index].info.enabled)
        return TC_COMPONENT_TYPES_ERR_ARGUMENT;
    try {
        Component& component = *impl_->items[index].component;
        if (component.registered()) return TC_COMPONENT_TYPES_OK;
        return component.registerLive(host);
    } catch (...) {
        return TC_COMPONENT_TYPES_ERR_GAME;
    }
}

int Project::reloadLive(size_t index, const TCHost* host,
                        std::vector<ProjectDiagnostic>* diagnostics,
                        const CompileOptions& options) noexcept {
    if (diagnostics) diagnostics->clear();
    if (!impl_ || index >= impl_->items.size() || !host || !impl_->items[index].info.enabled)
        return TC_COMPONENT_TYPES_ERR_ARGUMENT;
    Impl::Item& item = impl_->items[index];
    const std::string id = componentIdentity(item.component->metadata(), index);
    try {
        ComponentMetadata metadata = item.component->metadata();
        if (!item.nameOverride.empty()) metadata.name = item.nameOverride;
        std::unique_ptr<Component> candidate(new Component);
        std::vector<Diagnostic> compileDiagnostics;
        if (!candidate->compile(item.source, metadata, &compileDiagnostics,
                                impl_->effectiveOptions(options))) {
            const Diagnostic detail = compileDiagnostics.empty()
                                          ? Diagnostic{0, 0, "compile failed"}
                                          : compileDiagnostics.front();
            if (diagnostics) diagnostics->push_back({ProjectStage::Compile, index, id,
                item.sourcePath, detail.line, detail.column, detail.message});
            return TC_COMPONENT_TYPES_ERR_ARGUMENT;
        }
        const int status = item.component->registered()
                               ? item.component->replaceLiveWith(host, candidate.get())
                               : candidate->registerLive(host);
        if (status != TC_COMPONENT_TYPES_OK) {
            if (diagnostics) diagnostics->push_back({ProjectStage::Register, index, id,
                item.sourcePath, 0, 0, component_runtime::errorText(status)});
            return status;
        }
        impl_->retiredComponents.push_back(std::move(item.component));
        item.component = std::move(candidate);
        return TC_COMPONENT_TYPES_OK;
    } catch (const std::exception& error) {
        if (diagnostics) diagnostics->push_back({ProjectStage::Register, index, id,
                                                 item.sourcePath, 0, 0, error.what()});
    } catch (...) {
        if (diagnostics) diagnostics->push_back({ProjectStage::Register, index, id,
            item.sourcePath, 0, 0, "unknown runtime replacement error"});
    }
    return TC_COMPONENT_TYPES_ERR_GAME;
}

int Project::unregisterLive(uint64_t custom_id, const TCHost* host) noexcept {
    if (!impl_ || !custom_id || !host) return TC_COMPONENT_TYPES_ERR_ARGUMENT;
    try {
        for (Impl::Item& item : impl_->items)
            if (item.component->metadata().custom_id == custom_id)
                return item.component->unregisterLive(host);
        for (auto it = impl_->retired.rbegin(); it != impl_->retired.rend(); ++it)
            if (it->component->metadata().custom_id == custom_id)
                return it->component->unregisterLive(host);
        for (auto it = impl_->retiredComponents.rbegin(); it != impl_->retiredComponents.rend(); ++it)
            if ((*it)->metadata().custom_id == custom_id)
                return (*it)->unregisterLive(host);
    } catch (...) {
        return TC_COMPONENT_TYPES_ERR_GAME;
    }
    return TC_COMPONENT_TYPES_ERR_ARGUMENT;
}

bool Project::createSource(const std::filesystem::path& relative_name,
                           const std::string& source,
                           std::vector<ProjectDiagnostic>* diagnostics,
                           const CompileOptions& options) {
    if (diagnostics) diagnostics->clear();
    if (!impl_) {
        pushSaveDiagnostic(diagnostics, static_cast<size_t>(-1), {}, relative_name,
                           "project is not loaded");
        return false;
    }
    return impl_->insertNewFile(relative_name, source, nullptr, diagnostics, options);
}

bool Project::duplicateSource(size_t index, const std::filesystem::path& relative_name,
                              std::vector<ProjectDiagnostic>* diagnostics,
                              const CompileOptions& options) {
    if (diagnostics) diagnostics->clear();
    if (!impl_ || index >= impl_->items.size()) {
        pushSaveDiagnostic(diagnostics, index, {}, relative_name,
                           "source index is outside the project");
        return false;
    }
    const Impl::Item& item = impl_->items[index];
    return impl_->insertNewFile(relative_name, item.source,
                                &item.component->metadata(), diagnostics, options);
}

bool Project::renameSource(size_t index, const std::filesystem::path& relative_name,
                           std::vector<ProjectDiagnostic>* diagnostics) {
    if (diagnostics) diagnostics->clear();
    if (!impl_ || index >= impl_->items.size()) {
        pushSaveDiagnostic(diagnostics, index, {}, relative_name,
                           "source index is outside the project");
        return false;
    }
    Impl::Item& item = impl_->items[index];
    const std::string id = componentIdentity(item.component->metadata(), index);
    const std::string name = relativeName(relative_name);
    if (!safeSourceName(name)) {
        pushSaveDiagnostic(diagnostics, index, id, relative_name,
                           "name must be a safe relative .v path using '/' separators");
        return false;
    }
    if (lowerAscii(relativeName(item.relativeSource)) == lowerAscii(name)) {
        pushSaveDiagnostic(diagnostics, index, id, item.sourcePath,
                           "the file already has that name");
        return false;
    }
    if (impl_->nameTaken(relative_name, index)) {
        pushSaveDiagnostic(diagnostics, index, id, relative_name,
                           "that file name is already in the project");
        return false;
    }
    if (!impl_->sourceAllowed(relative_name)) {
        pushSaveDiagnostic(diagnostics, index, id, relative_name,
                           "source is outside the project's source_roots");
        return false;
    }
    const std::filesystem::path target = impl_->resolve(relative_name);
    std::error_code statusError;
    if (std::filesystem::exists(target, statusError) && !statusError) {
        pushSaveDiagnostic(diagnostics, index, id, target,
                           "a file with that name already exists");
        return false;
    }

    /* Build the new manifest with the entry's source and display name swapped,
       restoring the in-memory values if a later write fails. */
    const std::filesystem::path previousRelative = item.relativeSource;
    const std::string previousName = item.nameOverride;
    const ProjectComponentInfo previousInfo = item.info;
    const std::string renamedName = relative_name.stem().u8string();
    item.relativeSource = relative_name;
    item.nameOverride = renamedName;
    item.info.source = relative_name;
    const std::string previousDerivedGroup = previousRelative.parent_path().generic_u8string();
    if (item.info.group == previousDerivedGroup)
        item.info.group = relative_name.parent_path().generic_u8string();
    const std::string newManifest = impl_->manifestTextExcluding(static_cast<size_t>(-1), nullptr);
    item.relativeSource = previousRelative;
    item.nameOverride = previousName;
    item.info = previousInfo;

    std::string error;
    std::error_code directoryError;
    std::filesystem::create_directories(target.parent_path(), directoryError);
    if (!writeTextFileAtomic(target, item.source, &error)) {
        pushSaveDiagnostic(diagnostics, index, id, target,
                           error.empty() ? "cannot write renamed source" : error);
        return false;
    }
    if (!writeTextFileAtomic(impl_->manifest, newManifest, &error)) {
        std::error_code ignored;
        std::filesystem::remove(target, ignored);
        pushSaveDiagnostic(diagnostics, index, id, impl_->manifest,
                           "cannot rewrite manifest: " + error);
        return false;
    }
    const std::filesystem::path previousPath = item.sourcePath;
    item.relativeSource = relative_name;
    item.sourcePath = target;
    /* The custom id and type id stay put, so circuits that already use the
       component keep resolving it. */
    item.nameOverride = renamedName;
    item.info.source = relative_name;
    if (previousInfo.group == previousDerivedGroup)
        item.info.group = relative_name.parent_path().generic_u8string();
    impl_->manifestText = newManifest;
    std::error_code ignored;
    std::filesystem::remove(previousPath, ignored);
    return true;
}

bool Project::removeSource(size_t index, std::vector<ProjectDiagnostic>* diagnostics) {
    if (diagnostics) diagnostics->clear();
    if (!impl_ || index >= impl_->items.size()) {
        pushSaveDiagnostic(diagnostics, index, {}, {}, "source index is outside the project");
        return false;
    }
    if (impl_->items.size() <= 1) {
        pushSaveDiagnostic(diagnostics, index,
            componentIdentity(impl_->items[index].component->metadata(), index),
            impl_->items[index].sourcePath,
            "a project must keep at least one component file");
        return false;
    }
    const std::string previousDefault = impl_->metadata.default_component;
    if (impl_->metadata.format >= 2 &&
        lowerAscii(impl_->items[index].info.key) == lowerAscii(previousDefault)) {
        impl_->metadata.default_component.clear();
        for (size_t candidate = 0; candidate < impl_->items.size(); ++candidate) {
            if (candidate != index && impl_->items[candidate].info.enabled) {
                impl_->metadata.default_component = impl_->items[candidate].info.key;
                break;
            }
        }
        if (impl_->metadata.default_component.empty()) {
            impl_->metadata.default_component = previousDefault;
            pushSaveDiagnostic(diagnostics, index, impl_->items[index].info.key,
                               impl_->items[index].sourcePath,
                               "the default component cannot be removed while every other component is disabled");
            return false;
        }
    }
    const std::string newManifest = impl_->manifestTextExcluding(index, nullptr);
    std::string error;
    if (!writeTextFileAtomic(impl_->manifest, newManifest, &error)) {
        impl_->metadata.default_component = previousDefault;
        pushSaveDiagnostic(diagnostics, index, {}, impl_->manifest,
                           "cannot rewrite manifest: " + error);
        return false;
    }
    const std::filesystem::path removed = impl_->items[index].sourcePath;
    impl_->retired.push_back(std::move(impl_->items[index]));
    impl_->items.erase(impl_->items.begin() + static_cast<std::ptrdiff_t>(index));
    impl_->manifestText = newManifest;
    /* The manifest no longer references this file, so a removal failure only
       leaves an orphan behind; it cannot break the next load. */
    std::error_code ignored;
    std::filesystem::remove(removed, ignored);
    return true;
}

bool Project::valid() const noexcept { return impl_ && !impl_->items.empty(); }
size_t Project::size() const noexcept { return impl_ ? impl_->items.size() : 0; }
const std::filesystem::path& Project::manifestPath() const noexcept {
    static const std::filesystem::path empty; return impl_ ? impl_->manifest : empty;
}
std::filesystem::path Project::workspaceManifestPath(const std::filesystem::path& directory) const {
    if (!impl_) return {};
    return directory / impl_->manifest.filename();
}
const std::filesystem::path& Project::sourcePath(size_t index) const {
    if (!impl_ || index >= impl_->items.size()) throw std::out_of_range("TC-Verilog source index");
    return impl_->items[index].sourcePath;
}
const std::string& Project::sourceText(size_t index) const {
    if (!impl_ || index >= impl_->items.size()) throw std::out_of_range("TC-Verilog source index");
    return impl_->items[index].source;
}
const Component* Project::component(size_t index) const noexcept {
    return impl_ && index < impl_->items.size() ? impl_->items[index].component.get() : nullptr;
}
const ProjectMetadata& Project::metadata() const noexcept {
    static const ProjectMetadata empty;
    return impl_ ? impl_->metadata : empty;
}
const ProjectBuildSettings& Project::buildSettings() const noexcept {
    static const ProjectBuildSettings empty;
    return impl_ ? impl_->build : empty;
}
const ProjectComponentInfo& Project::componentInfo(size_t index) const {
    if (!impl_ || index >= impl_->items.size())
        throw std::out_of_range("TC-Verilog component info index");
    return impl_->items[index].info;
}
size_t Project::findComponent(const std::string& key) const noexcept {
    if (!impl_) return static_cast<size_t>(-1);
    const std::string wanted = lowerAscii(key);
    for (size_t index = 0; index < impl_->items.size(); ++index)
        if (lowerAscii(impl_->items[index].info.key) == wanted) return index;
    return static_cast<size_t>(-1);
}

}  // namespace verilog
}  // namespace tc
