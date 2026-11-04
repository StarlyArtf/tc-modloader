#include "../../sdk/tc_verilog_project.hpp"
#include "../../sdk/tc_code_editor.h"
#include "../../sdk/tc_component_runtime.h"
#include "../../sdk/tc_game_ui.h"
#include "../../sdk/tc_ui.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace {

tc::verilog::Project project;
const TCHost* pluginHost = nullptr;
std::filesystem::path projectSearchRoot;
std::string projectTypePrefix = "tc-verilog-ram";

constexpr size_t kNoSource = static_cast<size_t>(-1);

struct FileTreeFile {
    std::filesystem::path relative;
    std::string name;
    size_t sourceIndex = kNoSource;
};

struct FileTreeNode {
    std::filesystem::path relative;
    std::string name;
    std::vector<FileTreeNode> directories;
    std::vector<FileTreeFile> files;
};

std::vector<FileTreeNode> fileTreeRoots;
std::set<std::string> expandedDirectories;
void rebuildFileTree();

struct ProjectChoice {
    std::filesystem::path manifest;
    std::string id;
    std::string name;
    std::string description;
    std::vector<std::filesystem::path> sourceRoots;
    std::string defaultComponent;
    tc::verilog::CompileOptions limits;
    size_t componentCount = 0;
    bool loadable = false;
    std::string issue;
};

struct ProjectBrowserState {
    enum class Page { Browse, Create } page = Page::Browse;
    bool visible = false;
    std::vector<ProjectChoice> choices;
    size_t selected = 0;
    char filter[96] = {0};
    char manifestField[512] = {0};
    std::filesystem::path pendingManifest;
    bool pendingCreate = false;
    tc::verilog::ProjectCreateOptions createOptions;
    std::filesystem::path createDirectory;
    char createName[161] = {0};
    char createId[97] = {0};
    char createDescription[513] = {0};
    char createDirectoryField[512] = {0};
    char createSourceRoot[129] = {0};
    char createTopModule[129] = {0};
    std::string message;
    bool messageIsError = false;
};

ProjectBrowserState projectBrowser;

/* A file the editor just created is registered with the running game from the
   next frame: the registration mutates the game's component model, so it must
   not happen inside the page's own draw call.  The id is kept beside the index
   so that a later edit of the list cannot register the wrong entry. */
struct PendingLive {
    enum class Action { Register, Reload, Unregister } action = Action::Register;
    size_t index = 0;
    uint64_t customId = 0;
};
std::vector<PendingLive> pendingLive;
/* Development aid (TC_VERILOG_LIVE_TEST): created a few seconds after boot, so
   a test reproduces "the player makes a component while the game runs". */
std::string liveTestName;
int frameCount = 0;
/* The page focuses its source field when it opens, which is also what turns the
   editor's own coloured characters on: the widget paints the whole buffer only
   after the input callback has proved that this build's callback structure is
   the one it expects, and a callback only runs while the field is active.
   TC_VERILOG_EDITOR_FOCUS=1 keeps that focus on for a whole session, so a
   playtest can capture the state even after the opening frames have passed. */
bool editorAutoFocus = false;
int editorFocusFrames = 0;

struct EditorState {
    enum class Notice { Neutral, Success, Warning, Error };
    enum class FileOp { None, Create, Duplicate, Rename, Remove };
    static constexpr size_t kCapacity = 256u * 1024u + 1u;
    static constexpr size_t kNameCapacity = 192u;
    std::vector<std::string> drafts;
    std::vector<std::string> saved;
    std::vector<char> buffer;
    std::vector<tc::verilog::ProjectDiagnostic> diagnostics;
    std::string status = "源码已从持久工作区载入。";
    Notice notice = Notice::Neutral;
    size_t selected = 0;
    FileOp operation = FileOp::None;
    size_t operationTarget = 0;
    char nameField[kNameCapacity] = {0};
    std::string operationMessage;
    bool focusName = false;
    enum class BottomTool { Problems, Build } bottomTool = BottomTool::Problems;
    char fileFilter[96] = {0};
    /* Chrome for the source view: line numbers, colouring and the Enter
       behaviour live in tc::ui::codeEditor. */
    tc::ui::CodeEditorState sourceEditor;

    void initialise(size_t select = 0) {
        drafts.clear();
        saved.clear();
        for (size_t index = 0; index < project.size(); ++index) {
            drafts.push_back(project.sourceText(index));
            saved.push_back(project.sourceText(index));
        }
        buffer.assign(kCapacity, 0);
        diagnostics.clear();
        operation = FileOp::None;
        operationMessage.clear();
        focusName = false;
        selected = drafts.empty() ? 0 : std::min(select, drafts.size() - 1);
        loadBuffer();
        rebuildFileTree();
    }

    void captureBuffer() {
        if (selected < drafts.size() && !buffer.empty()) drafts[selected] = buffer.data();
    }

    void loadBuffer() {
        if (selected >= drafts.size() || buffer.empty()) return;
        std::fill(buffer.begin(), buffer.end(), 0);
        const size_t count = std::min(drafts[selected].size(), buffer.size() - 1);
        std::memcpy(buffer.data(), drafts[selected].data(), count);
    }

    bool dirty(size_t index) const {
        return index < drafts.size() && drafts[index] != saved[index];
    }

    bool anyDirty() const {
        for (size_t index = 0; index < drafts.size(); ++index)
            if (dirty(index)) return true;
        return false;
    }
};

EditorState editor;

constexpr tc::ui::Vec4 kSuccess{0.45f, 0.78f, 0.52f, 1.f};
constexpr tc::ui::Vec4 kWarning{0.93f, 0.73f, 0.20f, 1.f};
constexpr tc::ui::Vec4 kError{1.00f, 0.42f, 0.42f, 1.f};
constexpr tc::ui::Vec4 kIdeText{0.85f, 0.87f, 0.90f, 1.f};
constexpr tc::ui::Vec4 kIdeMuted{0.55f, 0.58f, 0.63f, 1.f};
constexpr tc::ui::Vec4 kIdeAccent{0.29f, 0.65f, 0.96f, 1.f};
constexpr tc::ui::Vec4 kIdeWindow{0.118f, 0.122f, 0.133f, 1.f};
constexpr tc::ui::Vec4 kIdePanel{0.145f, 0.153f, 0.169f, 1.f};
constexpr tc::ui::Vec4 kIdeEditor{0.106f, 0.114f, 0.129f, 1.f};
constexpr tc::ui::Vec4 kIdeBorder{0.204f, 0.212f, 0.235f, 1.f};
constexpr tc::ui::Vec4 kIdeHover{0.196f, 0.227f, 0.271f, 1.f};
constexpr tc::ui::Vec4 kIdeSelected{0.145f, 0.278f, 0.420f, 1.f};

/* A compact, square-edged workbench palette.  The page host owns the outer
   window; these colours are scoped to the editor's children and controls. */
class IdeStyle {
public:
    IdeStyle() {
        auto& style = tc::game_ui::gameTable();
        if (!style.pushStyleColor || !style.popStyleColor) return;
        const auto push = [&](int slot, tc::ui::Vec4 colour) {
            style.pushStyleColor(slot, colour);
            ++count_;
        };
        push(0, kIdeText);       // Text
        push(1, kIdeMuted);      // TextDisabled
        push(2, kIdeWindow);     // WindowBg
        push(3, kIdePanel);      // ChildBg
        push(5, kIdeBorder);     // Border
        push(7, kIdeEditor);     // FrameBg
        push(8, kIdeHover);      // FrameBgHovered
        push(9, kIdeSelected);   // FrameBgActive
        push(21, kIdePanel);     // Button
        push(22, kIdeHover);     // ButtonHovered
        push(23, kIdeSelected);  // ButtonActive
        push(24, kIdePanel);     // Header
        push(25, kIdeHover);     // HeaderHovered
        push(26, kIdeSelected);  // HeaderActive
        push(27, kIdeBorder);    // Separator
    }
    ~IdeStyle() {
        auto& style = tc::game_ui::gameTable();
        if (count_ && style.popStyleColor) style.popStyleColor(count_);
    }
    IdeStyle(const IdeStyle&) = delete;
    IdeStyle& operator=(const IdeStyle&) = delete;

private:
    int count_ = 0;
};

std::string fileNameOf(size_t index) {
    if (index >= project.size()) return {};
    return project.sourcePath(index).filename().u8string();
}

std::string fileStemOf(size_t index) {
    if (index >= project.size()) return {};
    return project.sourcePath(index).stem().u8string();
}

/* The path as the manifest spells it ("rtl/ram16x8.v"), so the name field
   shows the same relative form a project file uses. */
std::filesystem::path relativeSourcePath(size_t index) {
    if (index >= project.size()) return {};
    const std::filesystem::path relative =
        project.sourcePath(index).lexically_relative(project.manifestPath().parent_path());
    return relative.empty() ? project.sourcePath(index).filename() : relative;
}

std::string lowerPath(std::filesystem::path path) {
    std::string value = path.lexically_normal().generic_u8string();
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string compactPath(const std::filesystem::path& path, size_t maxBytes = 72) {
    const std::string value = path.u8string();
    if (value.size() <= maxBytes || maxBytes < 8) return value;
    return "…" + value.substr(value.size() - (maxBytes - 3));
}

std::string compactLabel(const std::string& value, size_t maxBytes = 20) {
    if (value.size() <= maxBytes || maxBytes < 6) return value;
    return value.substr(0, maxBytes - 3) + "…";
}

size_t sourceIndexFor(const std::filesystem::path& relative) {
    const std::string wanted = lowerPath(relative);
    for (size_t index = 0; index < project.size(); ++index)
        if (lowerPath(project.componentInfo(index).source) == wanted) return index;
    return kNoSource;
}

void collectDirectory(FileTreeNode* node, const std::filesystem::path& absolute,
                      size_t depth, size_t* budget) {
    if (!node || !budget || !*budget || depth > 24) return;
    std::error_code error;
    std::filesystem::directory_iterator iterator(absolute, error), end;
    if (error) return;
    for (; iterator != end && *budget; iterator.increment(error)) {
        if (error) break;
        --*budget;
        const std::filesystem::directory_entry& entry = *iterator;
        const std::string name = entry.path().filename().u8string();
        if (name.empty() || name[0] == '.') continue;
        std::error_code typeError;
        if (entry.is_symlink(typeError)) continue;
        if (entry.is_directory(typeError)) {
            FileTreeNode child;
            child.name = name;
            child.relative = node->relative / entry.path().filename();
            collectDirectory(&child, entry.path(), depth + 1, budget);
            node->directories.push_back(std::move(child));
            continue;
        }
        if (!entry.is_regular_file(typeError)) continue;
        std::string extension = entry.path().extension().u8string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (extension != ".v") continue;
        FileTreeFile file;
        file.name = name;
        file.relative = node->relative / entry.path().filename();
        file.sourceIndex = sourceIndexFor(file.relative);
        node->files.push_back(std::move(file));
    }
    const auto byName = [](const auto& left, const auto& right) {
        std::string a = left.name, b = right.name;
        std::transform(a.begin(), a.end(), a.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::transform(b.begin(), b.end(), b.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return a < b;
    };
    std::sort(node->directories.begin(), node->directories.end(), byName);
    std::sort(node->files.begin(), node->files.end(), byName);
}

void rememberExpandedDirectories(const FileTreeNode& node) {
    expandedDirectories.insert(lowerPath(node.relative));
    for (const auto& child : node.directories) rememberExpandedDirectories(child);
}

void rebuildFileTree() {
    fileTreeRoots.clear();
    if (!project.valid()) return;
    size_t budget = 2048;
    const std::filesystem::path projectDirectory = project.manifestPath().parent_path();
    std::vector<std::filesystem::path> roots = project.metadata().source_roots;
    if (roots.empty()) roots.push_back(".");
    for (const auto& relativeRoot : roots) {
        FileTreeNode root;
        root.relative = relativeRoot;
        root.name = relativeRoot == std::filesystem::path(".")
                        ? std::string("项目根目录")
                        : relativeRoot.generic_u8string();
        collectDirectory(&root, (projectDirectory / relativeRoot).lexically_normal(), 0, &budget);
        rememberExpandedDirectories(root);
        fileTreeRoots.push_back(std::move(root));
        if (!budget) break;
    }
}

void setNameField(const std::string& value) {
    const size_t count = std::min(value.size(), EditorState::kNameCapacity - 1);
    std::memcpy(editor.nameField, value.data(), count);
    editor.nameField[count] = '\0';
}

/* A Verilog identifier the new file template can declare.  Anything the
   compiler would refuse (Chinese, spaces, a leading digit) is mapped to an
   ASCII fallback so "new file" never starts life uncompilable. */
std::string moduleNameFor(const std::string& filename) {
    const std::string stem = std::filesystem::path(filename).stem().u8string();
    std::string out;
    out.reserve(stem.size() + 1);
    for (unsigned char byte : stem) {
        const bool safe = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
                          (byte >= '0' && byte <= '9') || byte == '_';
        out.push_back(safe ? static_cast<char>(byte) : '_');
    }
    if (out.empty()) out = "new_component";
    if (out.front() >= '0' && out.front() <= '9') out.insert(out.begin(), 'm');
    return out;
}

std::string templateSource(const std::string& filename) {
    const std::string name = moduleNameFor(filename);
    return "module " + name +
           "(\n"
           "    input wire a,\n"
           "    output wire y\n"
           ");\n"
           "assign y = a;\n"
           "endmodule\n";
}

/* The name field, normalised to a manifest-style relative .v path. */
std::filesystem::path requestedRelativePath() {
    std::string name(editor.nameField);
    while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) name.pop_back();
    size_t start = 0;
    while (start < name.size() && (name[start] == ' ' || name[start] == '\t')) ++start;
    name.erase(0, start);
    std::replace(name.begin(), name.end(), '\\', '/');
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower.size() < 2 || lower.compare(lower.size() - 2, 2, ".v") != 0) name += ".v";
    return std::filesystem::u8path(name);
}

void colouredText(tc::ui::Vec4 colour, const std::string& text) {
    auto& style = tc::game_ui::gameTable();
    if (style.pushStyleColor && style.popStyleColor) {
        style.pushStyleColor(tc::game_ui::Color_Text, colour);
        tc::ui::text(text);
        style.popStyleColor(1);
    } else {
        tc::ui::text(text);
    }
}

void drawNotice() {
    if (editor.status.empty()) return;
    switch (editor.notice) {
        case EditorState::Notice::Success: colouredText(kSuccess, editor.status); break;
        case EditorState::Notice::Warning: colouredText(kWarning, editor.status); break;
        case EditorState::Notice::Error: colouredText(kError, editor.status); break;
        case EditorState::Notice::Neutral: tc::ui::textDisabled(editor.status); break;
    }
}

bool primaryButton(const char* label, tc::ui::Vec2 size, bool disabled) {
    if (tc::game_ui::ready())
        return tc::game_ui::framed_button(label, size, disabled);
    tc::ui::Disabled scope(disabled);
    return tc::ui::button(label, size) && !disabled;
}

/* Compiles the selected source, reloads the runtime component when the layout
   is unchanged and queues a replacement when it is not.  Shared by the
   "保存源码" button and by Ctrl+S in the editor. */
bool saveSelectedSource() {
    const size_t selected = editor.selected;
    if (selected >= editor.drafts.size()) return false;
    editor.diagnostics.clear();
    bool reloaded = false;
    if (!project.saveAndReloadSource(selected, editor.drafts[selected], &reloaded,
                                     &editor.diagnostics)) {
        editor.status = "未保存：源码没有通过编译检查。";
        editor.notice = EditorState::Notice::Error;
        return false;
    }
    editor.saved[selected] = editor.drafts[selected];
    if (!reloaded) {
        const tc::verilog::Component* changed = project.component(selected);
        if (changed)
            pendingLive.push_back(
                {PendingLive::Action::Reload, selected, changed->metadata().custom_id});
    }
    editor.status = reloaded ? "保存成功：已热加载，本次运行立即生效。"
                             : "保存成功：端口或状态布局变了，正在刷新运行模型和元件栏。";
    editor.notice = EditorState::Notice::Success;
    if (pluginHost && pluginHost->log)
        pluginHost->log(pluginHost->context,
                        reloaded ? "tc-verilog-ram: editor saved and hot reloaded the program"
                                 : "tc-verilog-ram: editor saved source; queued runtime replacement");
    return true;
}

void report(const TCHost* host, const std::vector<tc::verilog::ProjectDiagnostic>& diagnostics) {
    if (!host || !host->log) return;
    for (const auto& diagnostic : diagnostics) {
        const std::string text = "tc-verilog-ram: " + tc::verilog::formatDiagnostic(diagnostic);
        host->log(host->context, text.c_str());
    }
}

void setManifestField(const std::filesystem::path& manifest) {
    const std::string value = manifest.u8string();
    const size_t count = std::min(value.size(), sizeof(projectBrowser.manifestField) - 1);
    std::memset(projectBrowser.manifestField, 0, sizeof(projectBrowser.manifestField));
    std::memcpy(projectBrowser.manifestField, value.data(), count);
}

void setFixedField(char* field, size_t capacity, const std::string& value) {
    if (!field || !capacity) return;
    const size_t count = std::min(value.size(), capacity - 1);
    std::memset(field, 0, capacity);
    std::memcpy(field, value.data(), count);
}

std::string projectSlug(const std::string& name) {
    std::string slug;
    bool separator = false;
    for (unsigned char c : name) {
        const bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        const bool digit = c >= '0' && c <= '9';
        if (alpha || digit) {
            slug.push_back(static_cast<char>(std::tolower(c)));
            separator = false;
        } else if (!slug.empty() && !separator) {
            slug.push_back('-');
            separator = true;
        }
    }
    while (!slug.empty() && slug.back() == '-') slug.pop_back();
    return slug.empty() ? "verilog-project" : slug;
}

void resetNewProjectForm() {
    const std::string name = "New Verilog Project";
    const std::string id = projectSlug(name);
    std::filesystem::path directory = projectSearchRoot / id;
    for (size_t suffix = 2; std::filesystem::exists(directory); ++suffix)
        directory = projectSearchRoot / (id + "-" + std::to_string(suffix));
    setFixedField(projectBrowser.createName, sizeof(projectBrowser.createName), name);
    setFixedField(projectBrowser.createId, sizeof(projectBrowser.createId), id);
    setFixedField(projectBrowser.createDescription, sizeof(projectBrowser.createDescription),
                  "Editable TC-Verilog components.");
    setFixedField(projectBrowser.createDirectoryField,
                  sizeof(projectBrowser.createDirectoryField), directory.u8string());
    setFixedField(projectBrowser.createSourceRoot,
                  sizeof(projectBrowser.createSourceRoot), "src");
    setFixedField(projectBrowser.createTopModule,
                  sizeof(projectBrowser.createTopModule), "main");
    projectBrowser.message.clear();
    projectBrowser.messageIsError = false;
}

ProjectChoice inspectProject(const std::filesystem::path& manifest) {
    ProjectChoice choice;
    choice.manifest = std::filesystem::absolute(manifest).lexically_normal();
    tc::verilog::Project preview;
    std::vector<tc::verilog::ProjectDiagnostic> diagnostics;
    choice.loadable = preview.load(choice.manifest, &diagnostics);
    if (!choice.loadable) {
        choice.id = choice.manifest.parent_path().filename().u8string();
        choice.name = choice.id.empty() ? choice.manifest.stem().u8string() : choice.id;
        choice.issue = diagnostics.empty() ? "无法加载项目清单"
                                           : tc::verilog::formatDiagnostic(diagnostics.front());
        return choice;
    }
    choice.id = preview.metadata().id;
    choice.name = preview.metadata().name.empty() ? preview.metadata().id
                                                   : preview.metadata().name;
    choice.description = preview.metadata().description;
    choice.sourceRoots = preview.metadata().source_roots;
    choice.defaultComponent = preview.metadata().default_component;
    choice.limits = preview.buildSettings().limits;
    choice.componentCount = preview.size();
    if (!diagnostics.empty()) choice.issue = tc::verilog::formatDiagnostic(diagnostics.front());
    return choice;
}

void scanProjects() {
    const std::string previouslySelected = projectBrowser.selected < projectBrowser.choices.size()
                                               ? lowerPath(projectBrowser.choices[projectBrowser.selected].manifest)
                                               : lowerPath(project.manifestPath());
    projectBrowser.choices.clear();
    std::set<std::string> seen;
    const auto add = [&](const std::filesystem::path& manifest) {
        const std::string identity = lowerPath(std::filesystem::absolute(manifest));
        if (!seen.insert(identity).second) return;
        projectBrowser.choices.push_back(inspectProject(manifest));
    };
    if (project.valid()) add(project.manifestPath());
    std::error_code error;
    if (!projectSearchRoot.empty() && std::filesystem::exists(projectSearchRoot, error) && !error) {
        std::filesystem::recursive_directory_iterator iterator(projectSearchRoot, error), end;
        size_t budget = 4096;
        for (; iterator != end && !error && budget; iterator.increment(error), --budget) {
            if (iterator.depth() > 6) {
                iterator.disable_recursion_pending();
                continue;
            }
            std::error_code typeError;
            if (!iterator->is_regular_file(typeError)) continue;
            std::string filename = iterator->path().filename().u8string();
            std::transform(filename.begin(), filename.end(), filename.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (filename == "tc-verilog.json") add(iterator->path());
        }
    }
    std::sort(projectBrowser.choices.begin(), projectBrowser.choices.end(),
              [](const ProjectChoice& left, const ProjectChoice& right) {
                  const bool leftActive = lowerPath(left.manifest) == lowerPath(project.manifestPath());
                  const bool rightActive = lowerPath(right.manifest) == lowerPath(project.manifestPath());
                  if (leftActive != rightActive) return leftActive;
                  std::string a = left.name, b = right.name;
                  std::transform(a.begin(), a.end(), a.begin(),
                                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                  std::transform(b.begin(), b.end(), b.begin(),
                                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                  return a < b;
              });
    projectBrowser.selected = 0;
    for (size_t index = 0; index < projectBrowser.choices.size(); ++index) {
        if (lowerPath(projectBrowser.choices[index].manifest) == previouslySelected) {
            projectBrowser.selected = index;
            break;
        }
    }
    if (!projectBrowser.choices.empty())
        setManifestField(projectBrowser.choices[projectBrowser.selected].manifest);
}

void requestProjectOpen(std::filesystem::path manifest) {
    if (editor.anyDirty()) {
        projectBrowser.message = "当前项目有未保存改动，请先保存或还原。";
        projectBrowser.messageIsError = true;
        return;
    }
    if (manifest.empty()) {
        projectBrowser.message = "请输入项目清单或项目目录。";
        projectBrowser.messageIsError = true;
        return;
    }
    std::error_code error;
    if (std::filesystem::is_directory(manifest, error)) manifest /= "tc-verilog.json";
    manifest = std::filesystem::absolute(manifest).lexically_normal();
    if (!std::filesystem::exists(manifest, error) || error) {
        projectBrowser.message = "找不到项目清单：" + manifest.u8string();
        projectBrowser.messageIsError = true;
        return;
    }
    projectBrowser.pendingManifest = manifest;
    projectBrowser.message = "正在打开 " + manifest.parent_path().filename().u8string() + "…";
    projectBrowser.messageIsError = false;
}

void requestProjectCreate() {
    if (editor.anyDirty()) {
        projectBrowser.message = "当前项目有未保存改动，请先保存或还原。";
        projectBrowser.messageIsError = true;
        return;
    }
    tc::verilog::ProjectCreateOptions create;
    create.name = projectBrowser.createName;
    create.id = projectBrowser.createId;
    create.description = projectBrowser.createDescription;
    create.source_root = std::filesystem::u8path(projectBrowser.createSourceRoot);
    create.top_module = projectBrowser.createTopModule;
    create.component_key = create.top_module;
    create.initial_source = templateSource(create.top_module + ".v");
    create.type_prefix = projectTypePrefix;
    projectBrowser.createOptions = std::move(create);
    projectBrowser.createDirectory =
        std::filesystem::u8path(projectBrowser.createDirectoryField);
    projectBrowser.pendingCreate = true;
    projectBrowser.message = "正在创建项目…";
    projectBrowser.messageIsError = false;
}

void applyPendingProjectCreate() {
    if (!projectBrowser.pendingCreate) return;
    projectBrowser.pendingCreate = false;
    tc::verilog::Project created;
    std::vector<tc::verilog::ProjectDiagnostic> diagnostics;
    if (!created.createNew(projectBrowser.createDirectory, projectBrowser.createOptions,
                           &diagnostics)) {
        projectBrowser.message = diagnostics.empty()
                                     ? "项目创建失败。"
                                     : tc::verilog::formatDiagnostic(diagnostics.front());
        projectBrowser.messageIsError = true;
        return;
    }
    projectBrowser.pendingManifest = created.manifestPath();
    projectBrowser.page = ProjectBrowserState::Page::Browse;
    projectBrowser.message = "项目已创建，正在打开…";
    projectBrowser.messageIsError = false;
    scanProjects();
}

void applyPendingProjectOpen() {
    if (projectBrowser.pendingManifest.empty() || !pluginHost) return;
    const std::filesystem::path manifest = projectBrowser.pendingManifest;
    projectBrowser.pendingManifest.clear();
    if (lowerPath(manifest) == lowerPath(project.manifestPath())) {
        projectBrowser.visible = false;
        editor.status = "已返回当前项目。";
        editor.notice = EditorState::Notice::Neutral;
        return;
    }

    tc::verilog::Project candidate;
    std::vector<tc::verilog::ProjectDiagnostic> diagnostics;
    if (!candidate.load(manifest, &diagnostics)) {
        projectBrowser.message = diagnostics.empty() ? "项目加载失败。"
                                                      : tc::verilog::formatDiagnostic(diagnostics.front());
        projectBrowser.messageIsError = true;
        return;
    }
    candidate.setTypePrefix(projectTypePrefix);

    std::vector<size_t> unregisteredOld;
    for (size_t index = 0; index < project.size(); ++index) {
        const tc::verilog::Component* component = project.component(index);
        if (!component || !component->registered()) continue;
        const int status = project.unregisterLive(component->metadata().custom_id, pluginHost);
        if (status != TC_COMPONENT_TYPES_OK) {
            for (size_t restore : unregisteredOld) project.registerLive(restore, pluginHost);
            projectBrowser.message = std::string("无法切换项目：") +
                                     tc::component_runtime::errorText(status);
            projectBrowser.messageIsError = true;
            return;
        }
        unregisteredOld.push_back(index);
    }

    std::vector<size_t> registeredNew;
    int failure = TC_COMPONENT_TYPES_OK;
    for (size_t index = 0; index < candidate.size(); ++index) {
        if (!candidate.componentInfo(index).enabled) continue;
        const int status = candidate.registerLive(index, pluginHost);
        if (status != TC_COMPONENT_TYPES_OK) {
            failure = status;
            break;
        }
        registeredNew.push_back(index);
    }
    if (failure != TC_COMPONENT_TYPES_OK) {
        for (size_t index : registeredNew) {
            const tc::verilog::Component* component = candidate.component(index);
            if (component) candidate.unregisterLive(component->metadata().custom_id, pluginHost);
        }
        for (size_t restore : unregisteredOld) project.registerLive(restore, pluginHost);
        projectBrowser.message = std::string("新项目无法注册：") +
                                 tc::component_runtime::errorText(failure);
        projectBrowser.messageIsError = true;
        return;
    }

    project = std::move(candidate);
    const size_t defaultIndex = project.findComponent(project.metadata().default_component);
    editor.initialise(defaultIndex == kNoSource ? 0 : defaultIndex);
    editor.status = "项目已打开：" + project.metadata().name;
    editor.notice = EditorState::Notice::Success;
    projectBrowser.visible = false;
    projectBrowser.message.clear();
    scanProjects();
    if (pluginHost->log)
        pluginHost->log(pluginHost->context,
                        ("tc-verilog-ram: opened project " + project.manifestPath().u8string()).c_str());
}

void selectSource(size_t index) {
    if (index == editor.selected || index >= editor.drafts.size()) return;
    editor.captureBuffer();
    editor.selected = index;
    editor.loadBuffer();
    editor.diagnostics.clear();
    editor.status = "已切换源码文件。";
    editor.notice = EditorState::Notice::Neutral;
}

void beginFileOperation(EditorState::FileOp operation, size_t target) {
    editor.operation = operation;
    editor.operationTarget = target;
    editor.operationMessage.clear();
    editor.focusName = operation != EditorState::FileOp::Remove;
    switch (operation) {
        case EditorState::FileOp::Create: setNameField("new_component.v"); break;
        case EditorState::FileOp::Duplicate:
            setNameField(
                (relativeSourcePath(target).parent_path() / (fileStemOf(target) + "_copy.v"))
                    .generic_u8string());
            break;
        case EditorState::FileOp::Rename:
            setNameField(relativeSourcePath(target).generic_u8string());
            break;
        case EditorState::FileOp::Remove: setNameField(""); break;
        case EditorState::FileOp::None: break;
    }
}

void applyFileOperation() {
    using FileOp = EditorState::FileOp;
    if (editor.operation == FileOp::None) return;
    if (editor.anyDirty()) {
        editor.operationMessage = "有未保存的改动：请先保存或撤销，再管理文件。";
        editor.notice = EditorState::Notice::Warning;
        return;
    }
    std::vector<tc::verilog::ProjectDiagnostic> diagnostics;
    const size_t target = editor.operationTarget;
    const std::filesystem::path relative = requestedRelativePath();
    const std::string before = fileNameOf(target);
    const tc::verilog::Component* beforeComponent = project.component(target);
    const uint64_t beforeId = beforeComponent ? beforeComponent->metadata().custom_id : 0;
    bool ok = false;
    bool created = false;
    size_t select = editor.selected;
    std::string done;
    std::string tail = "。正在同步到元件栏。";
    switch (editor.operation) {
        case FileOp::Create:
            ok = project.createSource(relative, templateSource(relative.u8string()), &diagnostics);
            if (ok) {
                select = project.size() - 1;
                created = true;
                done = "已新建 " + relative.u8string();
            }
            break;
        case FileOp::Duplicate:
            ok = project.duplicateSource(target, relative, &diagnostics);
            if (ok) {
                select = project.size() - 1;
                created = true;
                done = "已复制为 " + relative.u8string();
            }
            break;
        case FileOp::Rename:
            ok = project.renameSource(target, relative, &diagnostics);
            if (ok) {
                select = target;
                done = "已把 " + before + " 改名为 " + relative.filename().u8string();
                tail = "。元件身份不变，正在刷新元件栏名称。";
            }
            break;
        case FileOp::Remove:
            ok = project.removeSource(target, &diagnostics);
            if (ok) {
                select = target ? target - 1 : 0;
                done = "已删除 " + before;
                tail = "。正在从元件栏移除。";
            }
            break;
        case FileOp::None: return;
    }
    if (!ok) {
        editor.operationMessage = diagnostics.empty()
                                      ? std::string("操作失败。")
                                      : tc::verilog::formatDiagnostic(diagnostics.front());
        editor.status = editor.operationMessage;
        editor.notice = EditorState::Notice::Error;
        return;
    }
    if (created) {
        const tc::verilog::Component* fresh = project.component(select);
        if (fresh) pendingLive.push_back({PendingLive::Action::Register, select,
                                          fresh->metadata().custom_id});
    } else if (editor.operation == FileOp::Rename && beforeId) {
        pendingLive.push_back({PendingLive::Action::Reload, select, beforeId});
    } else if (editor.operation == FileOp::Remove && beforeId) {
        pendingLive.push_back({PendingLive::Action::Unregister, 0, beforeId});
    }
    editor.initialise(select);
    editor.status = done + tail;
    editor.notice = EditorState::Notice::Success;
    if (pluginHost && pluginHost->log)
        pluginHost->log(pluginHost->context, ("tc-verilog-ram: " + done).c_str());
}

/* Runs once per frame, outside the page's draw call.  Every mutation changes
   the game's component model, so none of them may run while the page is drawn. */
void pumpLiveRegistration() {
    if (pendingLive.empty() || !pluginHost) return;
    std::vector<PendingLive> queue;
    queue.swap(pendingLive);
    for (const PendingLive& entry : queue) {
        int status = TC_COMPONENT_TYPES_ERR_ARGUMENT;
        if (entry.action == PendingLive::Action::Unregister) {
            status = project.unregisterLive(entry.customId, pluginHost);
        } else {
            const tc::verilog::Component* component = project.component(entry.index);
            if (!component || component->metadata().custom_id != entry.customId) continue;
            status = entry.action == PendingLive::Action::Register
                         ? project.registerLive(entry.index, pluginHost)
                         : project.reloadLive(entry.index, pluginHost, &editor.diagnostics);
        }
        switch (status) {
            case TC_COMPONENT_TYPES_OK:
                editor.status = entry.action == PendingLive::Action::Unregister
                                    ? "删除已生效：元件已从运行模型和元件栏数据中移除。"
                                    : (entry.action == PendingLive::Action::Reload
                                           ? "修改已生效：运行模型和元件栏数据已刷新。"
                                           : "新元件已生效：运行模型和元件栏数据已刷新。");
                editor.notice = EditorState::Notice::Success;
                break;
            case TC_COMPONENT_TYPES_ERR_STATE:
                editor.status = "当前有电路板打开，本局不能注册；重启游戏后生效。";
                editor.notice = EditorState::Notice::Warning;
                break;
            case TC_COMPONENT_TYPES_ERR_UNAVAILABLE:
                editor.status = "这个加载器版本不支持完整热更新；重启游戏后生效。";
                editor.notice = EditorState::Notice::Warning;
                break;
            default:
                editor.status = std::string("运行中热更新被拒绝：") +
                                tc::component_runtime::errorText(status) + "；重启游戏后生效。";
                editor.notice = EditorState::Notice::Warning;
                break;
        }
        if (pluginHost->log) {
            const std::string line = "tc-verilog-ram: live mutation status=" + std::to_string(status);
            pluginHost->log(pluginHost->context, line.c_str());
        }
    }
}

void frameTick(void*, const TCFrame*) {
    try {
        /* Early enough to happen while no board is up (the editor page lives on
           the main menu), late enough that the game has finished booting. */
        if (!liveTestName.empty() && ++frameCount > 120) {
            const std::string name = liveTestName + ".v";
            liveTestName.clear();
            std::vector<tc::verilog::ProjectDiagnostic> ignored;
            if (project.createSource(name, templateSource(name), &ignored)) {
                const size_t index = project.size() - 1;
                pendingLive.push_back({PendingLive::Action::Register, index,
                                       project.component(index)->metadata().custom_id});
                editor.initialise(index);
                if (pluginHost && pluginHost->log)
                    pluginHost->log(pluginHost->context,
                                    ("tc-verilog-ram: live test created " + name + " after boot").c_str());
            } else if (pluginHost && pluginHost->log) {
                pluginHost->log(pluginHost->context,
                                "tc-verilog-ram: live test could not create the file");
            }
        }
        pumpLiveRegistration();
        if (pendingLive.empty()) {
            applyPendingProjectCreate();
            applyPendingProjectOpen();
        }
    } catch (...) {
        /* A frame callback must never let an exception cross the C ABI. */
    }
}

void drawFileDialog() {
    using FileOp = EditorState::FileOp;
    const FileOp operation = editor.operation;
    if (operation == FileOp::None) return;
    if (operation != FileOp::Create && editor.operationTarget >= project.size()) {
        editor.operation = FileOp::None;
        return;
    }
    const char* title = operation == FileOp::Create      ? "新建源码文件"
                        : operation == FileOp::Duplicate ? "复制为"
                        : operation == FileOp::Rename    ? "重命名为"
                                                         : "删除文件";
    tc::ui::separatorText(title);
    if (operation == FileOp::Remove) {
        tc::ui::TextWrap wrap;
        colouredText(kWarning, "确定删除 " + fileNameOf(editor.operationTarget) +
                                   " 吗？文件和清单条目都会被移除，无法撤销。");
    } else {
        tc::ui::textDisabled("文件名（可含相对目录，自动补 .v）");
        tc::ui::setNextItemWidth(std::max(120.f, tc::ui::contentRegionAvailable().x));
        if (editor.focusName) {
            tc::ui::setKeyboardFocusHere();
            editor.focusName = false;
        }
        tc::ui::inputText("##tc-verilog-name", editor.nameField, sizeof(editor.nameField));
    }
    if (primaryButton(operation == FileOp::Remove ? "删除###tc-verilog-confirm"
                                                  : "确定###tc-verilog-confirm",
                      {96.f, 0.f}, false))
        applyFileOperation();
    tc::ui::sameLine();
    if (tc::ui::button("取消###tc-verilog-cancel", {96.f, 0.f})) {
        editor.operation = FileOp::None;
        editor.operationMessage.clear();
        editor.status = "已取消文件操作。";
        editor.notice = EditorState::Notice::Neutral;
    }
    if (!editor.operationMessage.empty()) {
        tc::ui::TextWrap wrap;
        colouredText(kError, editor.operationMessage);
    }
    tc::ui::separator();
}

void drawFileToolbar() {
    const bool clean = !editor.anyDirty();
    {
        tc::ui::Disabled locked(!clean);
        if (tc::ui::smallButton("+ 新建"))
            beginFileOperation(EditorState::FileOp::Create, editor.selected);
        tc::ui::sameLine();
        if (tc::ui::smallButton("复制"))
            beginFileOperation(EditorState::FileOp::Duplicate, editor.selected);
        tc::ui::sameLine();
        if (tc::ui::smallButton("改名"))
            beginFileOperation(EditorState::FileOp::Rename, editor.selected);
        tc::ui::sameLine();
        if (tc::ui::smallButton("删除"))
            beginFileOperation(EditorState::FileOp::Remove, editor.selected);
    }
    if (!clean) colouredText(kWarning, "存在未保存改动");
}

bool pathMatchesFilter(const std::filesystem::path& path) {
    if (!editor.fileFilter[0]) return true;
    std::string name = path.generic_u8string();
    std::string needle = editor.fileFilter;
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::transform(needle.begin(), needle.end(), needle.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return name.find(needle) != std::string::npos;
}

bool treeMatchesFilter(const FileTreeNode& node) {
    if (!editor.fileFilter[0] || pathMatchesFilter(node.relative)) return true;
    for (const auto& file : node.files)
        if (pathMatchesFilter(file.relative)) return true;
    for (const auto& child : node.directories)
        if (treeMatchesFilter(child)) return true;
    return false;
}

void drawTreeNode(const FileTreeNode& node, unsigned depth, float width) {
    if (!treeMatchesFilter(node)) return;
    const std::string key = lowerPath(node.relative);
    bool expanded = editor.fileFilter[0] || expandedDirectories.count(key) != 0;
    std::string prefix(depth * 3, ' ');
    const std::string folderLabel = prefix + (expanded ? "v  D  " : ">  D  ") + node.name +
                                    "###directory-" + key;
    if (tc::ui::selectable(folderLabel.c_str(), false, 0,
                           {std::max(80.f, width - 12.f), 0.f}) && !editor.fileFilter[0]) {
        if (expanded) expandedDirectories.erase(key);
        else expandedDirectories.insert(key);
        expanded = !expanded;
    }
    if (!expanded) return;
    for (const auto& child : node.directories) drawTreeNode(child, depth + 1, width);
    for (const auto& file : node.files) {
        if (!pathMatchesFilter(file.relative)) continue;
        std::string label((depth + 1) * 3, ' ');
        label += "V  " + file.name;
        if (file.sourceIndex != kNoSource) {
            if (!project.componentInfo(file.sourceIndex).enabled) label += "  [禁用]";
            if (editor.dirty(file.sourceIndex)) label += "  *";
        } else {
            label += "  [未加入项目]";
        }
        label += "###tree-file-" + lowerPath(file.relative);
        if (tc::ui::selectable(label.c_str(), file.sourceIndex == editor.selected, 0,
                               {std::max(80.f, width - 12.f), 0.f})) {
            if (file.sourceIndex != kNoSource) {
                selectSource(file.sourceIndex);
            } else {
                editor.status = file.relative.generic_u8string() + " 尚未列入清单 components。";
                editor.notice = EditorState::Notice::Warning;
            }
        }
    }
}

void drawFilePanel(float width, float height) {
    tc::ui::Child files("project-tool-window", {width, height}, 1);
    if (!files) return;
    colouredText(kIdeText, "项目");
    tc::ui::sameLine();
    tc::ui::textDisabled(std::to_string(editor.drafts.size()) + " 个 Verilog 文件");
    tc::ui::separator();
    drawFileToolbar();
    tc::ui::setNextItemWidth(std::max(120.f, width - 18.f));
    tc::ui::inputText("##project-filter", editor.fileFilter, sizeof(editor.fileFilter));
    drawFileDialog();
    tc::ui::spacing();
    const std::string projectName = compactLabel(
        project.metadata().name.empty() ? "TC-Verilog" : project.metadata().name);
    colouredText(kIdeAccent, "v  P  " + projectName);
    const std::string manifestLabel = "   J  " + project.manifestPath().filename().u8string() +
                                      "###project-manifest";
    if (tc::ui::selectable(manifestLabel.c_str(), false, 0,
                           {std::max(80.f, width - 12.f), 0.f})) {
        editor.status = "项目清单：" + project.manifestPath().u8string();
        editor.notice = EditorState::Notice::Neutral;
    }
    for (const auto& root : fileTreeRoots) drawTreeNode(root, 1, width);
    tc::ui::spacing();
    tc::ui::separator();
    tc::ui::textDisabled("工作区");
    tc::ui::textDisabled(compactPath(project.manifestPath().parent_path(), 42));
}

void validateSelectedSource() {
    editor.captureBuffer();
    editor.diagnostics.clear();
    if (project.validateSource(editor.selected, editor.drafts[editor.selected],
                               &editor.diagnostics)) {
        editor.status = "检查通过：当前源码可以生成元件。";
        editor.notice = EditorState::Notice::Success;
        editor.bottomTool = EditorState::BottomTool::Build;
    } else {
        editor.status = "检查失败：请根据问题列表修改源码。";
        editor.notice = EditorState::Notice::Error;
        editor.bottomTool = EditorState::BottomTool::Problems;
    }
}

void revertSelectedSource() {
    const size_t selected = editor.selected;
    if (selected >= editor.drafts.size() || !editor.dirty(selected)) return;
    editor.drafts[selected] = editor.saved[selected];
    editor.loadBuffer();
    editor.diagnostics.clear();
    editor.status = "已还原这个文件中尚未保存的改动。";
    editor.notice = EditorState::Notice::Neutral;
}

bool accentButton(const char* label, tc::ui::Vec2 size, bool disabled = false) {
    if (!tc::game_ui::ready()) return primaryButton(label, size, disabled);
    return tc::game_ui::styledButton(
        label, size, disabled ? kIdePanel : tc::ui::Vec4{0.15f, 0.48f, 0.72f, 1.f},
        tc::ui::Vec4{0.20f, 0.58f, 0.86f, 1.f},
        tc::ui::Vec4{0.12f, 0.39f, 0.61f, 1.f}, kIdeText, disabled);
}

bool projectMatchesFilter(const ProjectChoice& choice) {
    if (!projectBrowser.filter[0]) return true;
    std::string haystack = choice.name + "\n" + choice.id + "\n" + choice.manifest.u8string();
    std::string needle = projectBrowser.filter;
    std::transform(haystack.begin(), haystack.end(), haystack.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::transform(needle.begin(), needle.end(), needle.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return haystack.find(needle) != std::string::npos;
}

void selectProjectChoice(size_t index) {
    if (index >= projectBrowser.choices.size()) return;
    projectBrowser.selected = index;
    setManifestField(projectBrowser.choices[index].manifest);
    projectBrowser.message.clear();
}

void drawProjectNavigation(float width, float height) {
    tc::ui::Child navigation("project-browser-navigation", {width, height}, 1);
    if (!navigation) return;
    colouredText(kIdeText, "项目");
    tc::ui::separator();
    if (tc::ui::selectable("<  返回编辑器", false, 0, {std::max(80.f, width - 12.f), 0.f}))
        projectBrowser.visible = false;
    const bool browsing = projectBrowser.page == ProjectBrowserState::Page::Browse;
    if (tc::ui::selectable("O  当前项目", browsing, 0,
                           {std::max(80.f, width - 12.f), 0.f})) {
        projectBrowser.page = ProjectBrowserState::Page::Browse;
        for (size_t index = 0; index < projectBrowser.choices.size(); ++index)
            if (lowerPath(projectBrowser.choices[index].manifest) == lowerPath(project.manifestPath())) {
                selectProjectChoice(index);
                break;
            }
    }
    if (tc::ui::selectable("+  新建项目", !browsing, 0,
                           {std::max(80.f, width - 12.f), 0.f})) {
        projectBrowser.page = ProjectBrowserState::Page::Create;
        resetNewProjectForm();
    }
    if (tc::ui::selectable("R  重新扫描", false, 0, {std::max(80.f, width - 12.f), 0.f}))
        scanProjects();
    tc::ui::spacing();
    tc::ui::separator();
    tc::ui::textDisabled("扫描位置");
    {
        tc::ui::TextWrap wrap;
        tc::ui::textDisabled(compactPath(projectSearchRoot, 42));
    }
}

void drawProjectList(float width, float height) {
    tc::ui::Child list("project-browser-list", {width, height}, 1);
    if (!list) return;
    colouredText(kIdeText, "选择项目");
    tc::ui::textDisabled("从持久工作区中打开，或输入其他项目清单路径。 ");
    tc::ui::spacing();
    tc::ui::setNextItemWidth(std::max(120.f, width - 14.f));
    tc::ui::inputText("##project-browser-filter", projectBrowser.filter,
                      sizeof(projectBrowser.filter));
    tc::ui::separator();
    size_t visible = 0;
    for (size_t index = 0; index < projectBrowser.choices.size(); ++index) {
        const ProjectChoice& choice = projectBrowser.choices[index];
        if (!projectMatchesFilter(choice)) continue;
        ++visible;
        const bool active = lowerPath(choice.manifest) == lowerPath(project.manifestPath());
        std::string label = choice.loadable ? "P  " : "!  ";
        label += choice.name.empty() ? choice.manifest.parent_path().filename().u8string()
                                     : choice.name;
        if (active) label += "  [当前]";
        label += "###project-choice-" + std::to_string(index);
        if (tc::ui::selectable(label.c_str(), index == projectBrowser.selected, 0,
                               {std::max(100.f, width - 14.f), 0.f}))
            selectProjectChoice(index);
        tc::ui::textDisabled("    " + compactPath(choice.manifest.parent_path(), 64));
        tc::ui::textDisabled("    " + std::to_string(choice.componentCount) + " 个组件");
        if (!choice.issue.empty()) {
            tc::ui::TextWrap wrap;
            colouredText(choice.loadable ? kWarning : kError, "    " + choice.issue);
        }
        tc::ui::separator();
    }
    if (!visible) tc::ui::textDisabled("没有匹配的项目。 ");
    tc::ui::spacing();
    tc::ui::separatorText("打开其他项目");
    tc::ui::textDisabled("项目目录或 tc-verilog.json 路径");
    tc::ui::setNextItemWidth(std::max(120.f, width - 14.f));
    tc::ui::inputText("##project-manifest-path", projectBrowser.manifestField,
                      sizeof(projectBrowser.manifestField));
    if (tc::ui::button("查找 tc-verilog.json", {250.f, 0.f}))
        requestProjectOpen(std::filesystem::u8path(projectBrowser.manifestField));
    if (!projectBrowser.message.empty()) {
        tc::ui::TextWrap wrap;
        colouredText(projectBrowser.messageIsError ? kError : kIdeMuted, projectBrowser.message);
    }
}

void drawProjectDetails(float width, float height) {
    tc::ui::Child details("project-browser-details", {width, height}, 1);
    if (!details) return;
    colouredText(kIdeText, "项目详情");
    tc::ui::separator();
    if (projectBrowser.selected >= projectBrowser.choices.size()) {
        tc::ui::textDisabled("选择一个项目查看详情。 ");
        return;
    }
    const ProjectChoice& choice = projectBrowser.choices[projectBrowser.selected];
    {
        tc::ui::TextWrap wrap;
        colouredText(choice.loadable ? kIdeAccent : kError,
                      std::string(choice.loadable ? "P  " : "!  ") + choice.name);
        tc::ui::textDisabled(compactPath(choice.manifest.parent_path(), 72));
    }
    tc::ui::spacing();
    tc::ui::separatorText("项目描述");
    {
        tc::ui::TextWrap wrap;
        tc::ui::text(choice.description.empty() ? "这个项目没有描述。" : choice.description);
    }
    tc::ui::separatorText("源码根目录");
    for (const auto& root : choice.sourceRoots)
        tc::ui::text("D  " + root.generic_u8string());
    tc::ui::separatorText("默认组件");
    tc::ui::text("V  " + choice.defaultComponent);
    tc::ui::separatorText("构建限制");
    tc::ui::textDisabled("源码字节    " + std::to_string(choice.limits.max_source_bytes));
    tc::ui::textDisabled("Token       " + std::to_string(choice.limits.max_tokens));
    tc::ui::textDisabled("状态字      " + std::to_string(choice.limits.max_state_words));
    tc::ui::textDisabled("表达式深度  " + std::to_string(choice.limits.max_expression_depth));
    tc::ui::spacing();
    if (accentButton("打开项目", {std::max(120.f, width - 16.f), 0.f}, !choice.loadable))
        requestProjectOpen(choice.manifest);
    tc::ui::textDisabled("切换会同步运行中的元件类型；有未保存改动时会被阻止。 ");
}

void drawCreateField(const char* label, const char* hint, char* field, size_t capacity,
                     float width) {
    tc::ui::text(label);
    if (hint && *hint) tc::ui::textDisabled(hint);
    tc::ui::setNextItemWidth(std::max(120.f, width - 16.f));
    const std::string id = std::string("##create-") + label;
    tc::ui::inputText(id.c_str(), field, capacity);
    tc::ui::spacing();
}

void drawNewProjectForm(float width, float height) {
    tc::ui::Child form("new-project-form", {width, height}, 1);
    if (!form) return;
    colouredText(kIdeText, "新建 TC-Verilog 项目");
    tc::ui::textDisabled("创建 format 2 清单、源码目录和可立即编译的默认组件。 ");
    tc::ui::separator();
    drawCreateField("项目名称", "显示在项目页和编辑器状态栏中。",
                    projectBrowser.createName, sizeof(projectBrowser.createName), width);
    drawCreateField("项目 ID", "稳定标识：只能使用字母、数字、点、下划线和连字符。",
                    projectBrowser.createId, sizeof(projectBrowser.createId), width);
    drawCreateField("项目目录", "这是新项目的最终目录；不会覆盖已有清单或源码。",
                    projectBrowser.createDirectoryField,
                    sizeof(projectBrowser.createDirectoryField), width);
    drawCreateField("源码根目录", "相对于项目目录，例如 src 或 rtl。",
                    projectBrowser.createSourceRoot,
                    sizeof(projectBrowser.createSourceRoot), width);
    drawCreateField("默认顶层模块", "同时生成同名组件 key 和 .v 文件。",
                    projectBrowser.createTopModule,
                    sizeof(projectBrowser.createTopModule), width);
    tc::ui::text("项目描述");
    tc::ui::setNextItemWidth(std::max(120.f, width - 16.f));
    tc::ui::inputTextMultiline("##create-description", projectBrowser.createDescription,
                               sizeof(projectBrowser.createDescription),
                               {std::max(120.f, width - 16.f), 84.f});
    tc::ui::spacing();
    const bool incomplete = !projectBrowser.createName[0] || !projectBrowser.createId[0] ||
                            !projectBrowser.createDirectoryField[0] ||
                            !projectBrowser.createSourceRoot[0] ||
                            !projectBrowser.createTopModule[0] ||
                            projectBrowser.pendingCreate;
    if (accentButton("创建并打开项目", {230.f, 0.f}, incomplete)) requestProjectCreate();
    tc::ui::sameLine();
    if (tc::ui::button("取消", {96.f, 0.f})) {
        projectBrowser.page = ProjectBrowserState::Page::Browse;
        projectBrowser.message.clear();
    }
    if (!projectBrowser.message.empty()) {
        tc::ui::TextWrap wrap;
        colouredText(projectBrowser.messageIsError ? kError : kIdeMuted, projectBrowser.message);
    }
}

void drawNewProjectSummary(float width, float height) {
    tc::ui::Child summary("new-project-summary", {width, height}, 1);
    if (!summary) return;
    colouredText(kIdeText, "将要创建");
    tc::ui::separator();
    colouredText(kIdeAccent, std::string("P  ") + projectBrowser.createName);
    tc::ui::textDisabled(compactPath(
        std::filesystem::u8path(projectBrowser.createDirectoryField), 72));
    tc::ui::separatorText("项目结构");
    tc::ui::text("J  tc-verilog.json");
    tc::ui::text(std::string("D  ") + projectBrowser.createSourceRoot);
    tc::ui::text(std::string("    V  ") + projectBrowser.createTopModule + ".v");
    tc::ui::separatorText("默认组件");
    tc::ui::text(std::string("key         ") + projectBrowser.createTopModule);
    tc::ui::text(std::string("top_module  ") + projectBrowser.createTopModule);
    tc::ui::separatorText("安全行为");
    {
        tc::ui::TextWrap wrap;
        tc::ui::textDisabled("默认源码会先编译；清单最后写入。若目录里已有同名清单或源码，创建会直接拒绝。 ");
    }
}

void drawProjectBrowser(float width, float height) {
    const float statusHeight = 52.f;
    const float bodyHeight = std::max(160.f, height - statusHeight - 4.f);
    if (width >= 980.f) {
        const float navigationWidth = std::max(230.f, std::min(290.f, width * 0.16f));
        const float detailWidth = std::max(390.f, std::min(500.f, width * 0.27f));
        drawProjectNavigation(navigationWidth, bodyHeight);
        tc::ui::sameLine();
        if (projectBrowser.page == ProjectBrowserState::Page::Create)
            drawNewProjectForm(std::max(320.f, width - navigationWidth - detailWidth - 12.f),
                               bodyHeight);
        else
            drawProjectList(std::max(320.f, width - navigationWidth - detailWidth - 12.f),
                            bodyHeight);
        tc::ui::sameLine();
        if (projectBrowser.page == ProjectBrowserState::Page::Create)
            drawNewProjectSummary(detailWidth, bodyHeight);
        else
            drawProjectDetails(detailWidth, bodyHeight);
    } else if (width >= 680.f) {
        const float listWidth = width * 0.58f;
        if (projectBrowser.page == ProjectBrowserState::Page::Create)
            drawNewProjectForm(listWidth, bodyHeight);
        else
            drawProjectList(listWidth, bodyHeight);
        tc::ui::sameLine();
        if (projectBrowser.page == ProjectBrowserState::Page::Create)
            drawNewProjectSummary(std::max(260.f, width - listWidth - 6.f), bodyHeight);
        else
            drawProjectDetails(std::max(260.f, width - listWidth - 6.f), bodyHeight);
    } else {
        const float listHeight = bodyHeight * 0.58f;
        if (projectBrowser.page == ProjectBrowserState::Page::Create) {
            drawNewProjectForm(width, listHeight);
            drawNewProjectSummary(width, std::max(120.f, bodyHeight - listHeight));
        } else {
            drawProjectList(width, listHeight);
            drawProjectDetails(width, std::max(120.f, bodyHeight - listHeight));
        }
    }
    tc::ui::Child status("project-browser-status", {width, statusHeight}, 1,
                         tc::ui::Window_NoScrollbar);
    if (status) {
        tc::ui::textDisabled(projectBrowser.page == ProjectBrowserState::Page::Create
                                 ? "TC-Verilog IDE  >  新建项目"
                                 : "TC-Verilog IDE  >  项目选择");
        tc::ui::sameLine(std::max(300.f, width - 220.f));
        tc::ui::textDisabled("UTF-8    Verilog");
    }
}

void drawCommandBar(float width) {
    const float barHeight = std::max(50.f, tc::ui::frameHeight() + 16.f);
    tc::ui::Child bar("ide-command-bar", {width, barHeight}, 1,
                      tc::ui::Window_NoScrollbar);
    if (!bar) return;
    colouredText(kIdeAccent, "TC");
    tc::ui::sameLine();
    colouredText(kIdeText, "TC-Verilog IDE");
    tc::ui::sameLine();
    if (tc::ui::button(projectBrowser.visible ? "返回编辑器" : "选择项目", {120.f, 0.f})) {
        projectBrowser.visible = !projectBrowser.visible;
        if (projectBrowser.visible) scanProjects();
    }
    if (projectBrowser.visible) {
        tc::ui::sameLine();
        tc::ui::textDisabled("选择、检查并打开 tc-verilog.json 项目");
        return;
    }
    tc::ui::sameLine();
    if (tc::ui::button("检查  F9", {108.f, 0.f})) validateSelectedSource();
    tc::ui::sameLine();
    if (accentButton("保存并热加载  Ctrl+S", {190.f, 0.f}, !editor.dirty(editor.selected))) {
        editor.captureBuffer();
        saveSelectedSource();
        editor.bottomTool = editor.diagnostics.empty() ? EditorState::BottomTool::Build
                                                       : EditorState::BottomTool::Problems;
    }
    tc::ui::sameLine();
    {
        tc::ui::Disabled disabled(!editor.dirty(editor.selected));
        if (tc::ui::button("还原", {72.f, 0.f})) revertSelectedSource();
    }
    tc::ui::sameLine();
    tc::ui::textDisabled("目标：当前电路    Verilog");
}

void drawEditorTabs(float width) {
    const float tabHeight = std::max(29.f, tc::ui::frameHeight() + 4.f);
    tc::ui::Child tabs("editor-tabs", {width, tabHeight}, 1, tc::ui::Window_NoScrollbar);
    if (!tabs) return;
    for (size_t index = 0; index < editor.drafts.size(); ++index) {
        std::string label = fileNameOf(index);
        if (editor.dirty(index)) label += "  *";
        label += "###tab-" + std::to_string(index);
        if (index) tc::ui::sameLine(0.f, 0.f);
        if (tc::ui::selectable(label.c_str(), index == editor.selected, 0, {144.f, 0.f}))
            selectSource(index);
    }
}

void drawSourcePanel(float width, float height) {
    tc::ui::Child source("editor-workspace", {width, height}, 1);
    if (!source) return;
    drawEditorTabs(std::max(100.f, width - 4.f));
    const size_t selected = editor.selected;
    const tc::ui::Vec2 available = tc::ui::contentRegionAvailable();

    /* The file index is part of the widget id: ImGui keeps one editing state per
       id, so switching files starts a fresh session (with its own undo history)
       instead of carrying the previous file's text into the next one. */
    const std::string editorId = "##tc-verilog-source-" + std::to_string(selected);
    /* Opening the page focuses its source field the way an IDE focuses the file
       it just opened; a few frames are enough, and afterwards the player's own
       focus decisions win.  The developer switch keeps it on for a session. */
    static int lastDrawnFrame = -1;
    const int drawnFrame = tc::ui::frameCount();
    if (lastDrawnFrame < 0 || drawnFrame - lastDrawnFrame > 2) editorFocusFrames = 4;
    lastDrawnFrame = drawnFrame;
    if (editorFocusFrames > 0) {
        --editorFocusFrames;
        tc::ui::setKeyboardFocusHere(0);
    } else if (editorAutoFocus) {
        tc::ui::setKeyboardFocusHere(0);
    }
    if (tc::ui::codeEditor(editorId.c_str(), editor.buffer.data(), editor.buffer.size(),
                           {std::max(100.f, available.x), std::max(96.f, available.y)},
                           &editor.sourceEditor)) {
        editor.captureBuffer();
        editor.diagnostics.clear();
        editor.status = "有尚未保存的改动。";
        editor.notice = EditorState::Notice::Warning;
    }
    if (tc::ui::isItemFocused() &&
        (tc::ui::keys::down(tc::ui::Key_LeftCtrl) || tc::ui::keys::down(tc::ui::Key_RightCtrl)) &&
        tc::ui::keys::pressed(tc::ui::Key_S)) {
        editor.captureBuffer();
        if (editor.dirty(selected)) {
            saveSelectedSource();
        } else {
            editor.status = "已保存：没有需要写入的改动。";
            editor.notice = EditorState::Notice::Neutral;
        }
    }
}

void drawStructurePanel(float width, float height) {
    tc::ui::Child structure("structure-tool-window", {width, height}, 1);
    if (!structure) return;
    colouredText(kIdeText, "结构");
    tc::ui::separator();
    const tc::verilog::Component* component = project.component(editor.selected);
    if (!component) {
        tc::ui::textDisabled("当前文件没有可用结构。 ");
        return;
    }
    const tc::verilog::Program& program = component->program();
    colouredText(kIdeAccent, "M  " + program.moduleName());
    tc::ui::spacing();
    tc::ui::textDisabled("v  端口 (" + std::to_string(program.ports().size()) + ")");
    for (const auto& port : program.ports()) {
        const bool input = port.direction == tc::verilog::PortDirection::Input;
        const std::string widthLabel = port.bits > 1
                                           ? " [" + std::to_string(port.bits - 1) + ":0]"
                                           : "  1 bit";
        colouredText(input ? tc::ui::Vec4{0.32f, 0.68f, 0.94f, 1.f}
                           : tc::ui::Vec4{0.73f, 0.53f, 0.91f, 1.f},
                     std::string("    ") + (input ? "IN   " : "OUT  ") + port.name +
                         widthLabel);
    }
    tc::ui::spacing();
    tc::ui::textDisabled("v  编译信息");
    tc::ui::text("    状态字    " + std::to_string(program.stateWords()));
    tc::ui::text("    输入      " + std::to_string(program.inputCount()));
    tc::ui::text("    输出      " + std::to_string(program.outputCount()));
    tc::ui::spacing();
    tc::ui::separator();
    tc::ui::textDisabled("元件");
    tc::ui::text(component->metadata().name.empty() ? program.moduleName()
                                                     : component->metadata().name);
    tc::ui::textDisabled(component->metadata().type_id);
}

void drawBottomPanel(float width, float height) {
    tc::ui::Child output("bottom-tool-window", {width, height}, 1);
    if (!output) return;
    const std::string problems = "问题  " + std::to_string(editor.diagnostics.size());
    if (tc::ui::selectable(problems.c_str(), editor.bottomTool == EditorState::BottomTool::Problems,
                           0, {112.f, 0.f}))
        editor.bottomTool = EditorState::BottomTool::Problems;
    tc::ui::sameLine(0.f, 0.f);
    if (tc::ui::selectable("构建输出", editor.bottomTool == EditorState::BottomTool::Build,
                           0, {112.f, 0.f}))
        editor.bottomTool = EditorState::BottomTool::Build;
    tc::ui::separator();
    if (editor.bottomTool == EditorState::BottomTool::Problems) {
        if (editor.diagnostics.empty()) {
            colouredText(kSuccess, "OK  未发现问题");
            tc::ui::textDisabled("运行“检查”或保存文件时，诊断会显示在这里。 ");
        } else {
            tc::ui::textDisabled("严重性        文件                    行:列        消息");
            tc::ui::separator();
            for (const auto& diagnostic : editor.diagnostics) {
                colouredText(kError, "ERROR");
                tc::ui::sameLine(88.f);
                tc::ui::text(diagnostic.file.filename().u8string());
                tc::ui::sameLine(252.f);
                tc::ui::text(std::to_string(diagnostic.line) + ":" +
                             std::to_string(diagnostic.column));
                tc::ui::sameLine(330.f);
                tc::ui::text(diagnostic.message);
            }
        }
    } else {
        drawNotice();
        tc::ui::textDisabled("工作区  " + project.manifestPath().parent_path().u8string());
        tc::ui::textDisabled("编译器  TC-Verilog synthesizable kernel");
        if (editor.diagnostics.empty())
            colouredText(kSuccess, "构建完成：0 个错误");
        else
            colouredText(kError, "构建失败：" + std::to_string(editor.diagnostics.size()) +
                                      " 个错误");
    }
}

void drawStatusBar(float width) {
    /* This game's selected UI font reports a smaller frame height than its
       visible CJK glyph box.  Keep a fixed safe band so descenders and Chinese
       labels are never clipped at the bottom edge of the host page. */
    const float statusHeight = 52.f;
    tc::ui::Child status("ide-status-bar", {width, statusHeight}, 1,
                         tc::ui::Window_NoScrollbar);
    if (!status) return;
    colouredText(editor.dirty(editor.selected) ? kWarning : kSuccess,
                 editor.dirty(editor.selected) ? "* 已修改" : "OK 已保存");
    tc::ui::sameLine();
    const std::string breadcrumb = project.metadata().name + "  >  " +
                                   project.componentInfo(editor.selected).source.generic_u8string();
    tc::ui::textDisabled(breadcrumb);
    tc::ui::sameLine(std::max(380.f, width - 430.f));
    tc::ui::textDisabled("行 " + std::to_string(editor.sourceEditor.caretLine + 1) +
                         "，列 " + std::to_string(editor.sourceEditor.caretColumn + 1));
    tc::ui::sameLine();
    tc::ui::textDisabled("UTF-8    LF    Verilog");
}

void drawEditor(void*, const TCFrame*, float width, float height) {
    try {
        if (editor.drafts.empty() || editor.selected >= editor.drafts.size()) {
            tc::ui::textDisabled("没有可编辑的 TC-Verilog 源码。  ");
            return;
        }
        editor.captureBuffer();
        IdeStyle ideStyle;
        /* The game menu uses a deliberately large display face.  A real IDE
           needs a denser chrome and more visible source lines, so the plugin
           scales only its current page window after the host header is drawn. */
        tc::ui::setWindowFontScale(0.48f);
        tc::ui::Vec2 available = tc::ui::contentRegionAvailable();
        if (available.x <= 0.f) available.x = width;
        if (available.y <= 0.f) available.y = std::max(120.f, height - 64.f);
        drawCommandBar(available.x);
        if (projectBrowser.visible) {
            const tc::ui::Vec2 browserArea = tc::ui::contentRegionAvailable();
            drawProjectBrowser(available.x, std::max(180.f, browserArea.y));
            return;
        }
        tc::ui::Vec2 work = tc::ui::contentRegionAvailable();
        const float bottomHeight = std::max(118.f, std::min(174.f, work.y * 0.23f));
        const float statusHeight = 54.f;
        const float editorHeight = std::max(180.f, work.y - bottomHeight - statusHeight - 118.f);
        if (available.x >= 1040.f) {
            const float listWidth = std::max(300.f, std::min(420.f, available.x * 0.18f));
            const float structureWidth = std::max(330.f, std::min(430.f, available.x * 0.18f));
            drawFilePanel(listWidth, editorHeight);
            tc::ui::sameLine();
            drawSourcePanel(std::max(300.f, available.x - listWidth - structureWidth - 12.f),
                            editorHeight);
            tc::ui::sameLine();
            drawStructurePanel(structureWidth, editorHeight);
        } else if (available.x >= 720.f) {
            const float listWidth = std::max(210.f, std::min(280.f, available.x * 0.25f));
            drawFilePanel(listWidth, editorHeight);
            tc::ui::sameLine();
            drawSourcePanel(std::max(300.f, available.x - listWidth - 6.f), editorHeight);
        } else {
            const float listHeight = std::max(100.f, std::min(150.f, editorHeight * 0.28f));
            drawFilePanel(available.x, listHeight);
            const tc::ui::Vec2 below = tc::ui::contentRegionAvailable();
            drawSourcePanel(available.x, std::max(160.f, below.y - bottomHeight - statusHeight));
        }
        drawBottomPanel(available.x, bottomHeight);
        drawStatusBar(available.x);
    } catch (const std::exception& error) {
        colouredText(kError, std::string("TC-Verilog 编辑器异常：") + error.what());
    } catch (...) {
        colouredText(kError, "TC-Verilog 编辑器异常：未知错误");
    }
}

}  // namespace

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* host, TCPlugin* plugin) {
    if (!host || !plugin || host->api_version != TC_MOD_API_VERSION) return 1;
    std::vector<tc::verilog::ProjectDiagnostic> diagnostics;
    tc::verilog::Project bundled;
    if (!bundled.loadAdjacent("tc-verilog.json", &diagnostics)) {
        report(host, diagnostics);
        return 2;
    }
    if (!host->data_directory_utf8 || !*host->data_directory_utf8) return 3;
    projectSearchRoot = std::filesystem::u8path(host->data_directory_utf8);
    const std::filesystem::path workspace = projectSearchRoot / "tc-verilog";
    if (!bundled.bootstrapWorkspace(workspace, &diagnostics)) {
        report(host, diagnostics);
        return 4;
    }
    if (!project.load(bundled.workspaceManifestPath(workspace), &diagnostics)) {
        report(host, diagnostics);
        return 5;
    }
    projectTypePrefix = host->mod_id && *host->mod_id ? host->mod_id : "tc-verilog-ram";
    project.setTypePrefix(projectTypePrefix);
    /* A source that does not compile is reported, but it no longer stops the
       load: the entry stays in the project so the editor can open it. */
    report(host, diagnostics);
    const int status = project.registerAll(host, &diagnostics);
    if (status != TC_COMPONENT_TYPES_OK) {
        report(host, diagnostics);
        if (host->log)
            host->log(host->context,
                      "tc-verilog-ram: some components could not be registered; the editor still loads");
    }
    if (!tc::ui::load(host)) {
        if (host->log) {
            const std::string message = "tc-verilog-ram: UI load failed: " + tc::ui::missing();
            host->log(host->context, message.c_str());
        }
        return 7;
    }
    if (!tc::game_ui::load(host) && host->log) {
        const std::string message = "tc-verilog-ram: game UI style unavailable: " +
                                    tc::game_ui::gameMissing();
        host->log(host->context, message.c_str());
    }
    /* Additive: without these exports the source view falls back to the plain
       multiline input (no gutter, no colouring). */
    tc::ui::loadCodeEditor(host);
    if (host->log) {
        host->log(host->context,
                  tc::ui::codeEditorAvailable()
                      ? "tc-verilog-ram: source editor has line numbers, syntax highlighting and auto indent"
                      : "tc-verilog-ram: source editor falls back to the plain text box");
    }
    pluginHost = host;
    plugin->on_frame = &frameTick;
    editor.initialise();
    scanProjects();
    resetNewProjectForm();
    /* Development aid (TC_VERILOG_LIVE_TEST=<name>): create one component the
       way the editor's "new file" button does, a few seconds after boot, so the
       runtime-registration path is exercised exactly as a player would. */
    if (const char* liveTest = std::getenv("TC_VERILOG_LIVE_TEST")) {
        if (*liveTest) liveTestName = liveTest;
    }
    if (const char* focusTest = std::getenv("TC_VERILOG_EDITOR_FOCUS")) {
        if (*focusTest && focusTest[0] != '0') editorAutoFocus = true;
    }
    if (const char* projectsView = std::getenv("TC_VERILOG_PROJECTS_VIEW")) {
        if (*projectsView && projectsView[0] != '0') projectBrowser.visible = true;
    }
    if (const char* createView = std::getenv("TC_VERILOG_CREATE_VIEW")) {
        if (*createView && createView[0] != '0') {
            projectBrowser.visible = true;
            projectBrowser.page = ProjectBrowserState::Page::Create;
        }
    }
    if (const char* createTarget = std::getenv("TC_VERILOG_CREATE_TARGET")) {
        if (*createTarget) {
            resetNewProjectForm();
            setFixedField(projectBrowser.createName, sizeof(projectBrowser.createName),
                          "Created In Game");
            setFixedField(projectBrowser.createId, sizeof(projectBrowser.createId),
                          "created-in-game");
            setFixedField(projectBrowser.createDirectoryField,
                          sizeof(projectBrowser.createDirectoryField), createTarget);
            requestProjectCreate();
        }
    }
    if (host->log) {
        tc::component_runtime::Api runtime{};
        host->log(host->context, tc::component_runtime::table(host, &runtime)
                                     ? "tc-verilog-ram: live component registration available"
                                     : "tc-verilog-ram: live component registration unavailable (older loader)");
    }
    const int pageStatus = tc::ui::registerPage("tc-verilog", "Verilog 编辑器", drawEditor,
                                                nullptr, host);
    if (pageStatus != 0) {
        if (host->log) {
            const std::string message = "tc-verilog-ram: editor page registration failed: " +
                                        std::to_string(pageStatus);
            host->log(host->context, message.c_str());
        }
        return 8;
    }
    if (host->log)
        host->log(host->context,
                  "tc-verilog-ram: registered project from .v source and editor page 'tc-verilog'");
    return 0;
}
