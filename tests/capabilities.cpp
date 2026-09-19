/* Offline tests for the host contract added in 0.6.0: the version that VERSION
   declares, the capability table that mod.json is checked against, and the
   version-constraint algebra used for dependencies.  No game, no ImGui. */
#include "../src/core.hpp"
#include <cassert>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::string fileText(const char* path) {
    std::ifstream file(path, std::ios::binary);
    assert(file);
    std::ostringstream out;
    out << file.rdbuf();
    return out.str();
}

/* Every capability bit must be reachable by the name the manifest uses, and the
   loader's mask must contain nothing the table cannot name. */
void capabilityTableIsComplete() {
    const auto& table = tc::capability_table();
    assert(!table.empty());
    uint64_t seen = 0;
    for (const auto& entry : table) {
        assert(tc::capability_bit(entry.name) == entry.bit);
        assert((seen & entry.bit) == 0);  // no bit is listed twice
        seen |= entry.bit;
    }
    assert(seen == tc::loader_capabilities());
    assert(tc::capability_bit("no-such-capability") == 0);
    /* The names are what a package writes; keep them lower-case and stable. */
    for (const auto& entry : table) {
        std::string name(entry.name);
        assert(name.find_first_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ ") == std::string::npos);
    }
}

/* The version a build reports is the one in VERSION, not a literal that someone
   forgot to update.  build.ps1 generates src/version.hpp from VERSION, and this
   is the check that the two really agree. */
void versionMatchesTheVersionFile() {
    std::string text = fileText("VERSION");
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    assert(text == TC_MODLOADER_VERSION_STRING);
    assert(TC_MODLOADER_VERSION_CODE ==
           TC_HOST_VERSION_CODE(TC_MODLOADER_VERSION_MAJOR, TC_MODLOADER_VERSION_MINOR, TC_MODLOADER_VERSION_PATCH));
    assert(TC_HOST_VERSION_CODE(0, 6, 0) > TC_HOST_VERSION_CODE(0, 5, 255));
}

void versionOrdering() {
    assert(tc::version_compare("1.0.0", "1.0.0") == 0);
    /* A string comparison gets this wrong; the point of the numeric prefix. */
    assert(tc::version_compare("1.10.0", "1.9.0") > 0);
    assert(tc::version_compare("1.9.0", "1.10.0") < 0);
    assert(tc::version_compare("1.0", "1.0.0") == 0);
    assert(tc::version_compare("v2.0.0", "2.0.0") == 0);
    assert(tc::version_compare("1.0.0-beta", "1.0.0") == 0);
    assert(tc::version_compare("beta", "beta") == 0);
    assert(tc::version_compare("beta", "alpha") > 0);
}

void versionConstraints() {
    assert(tc::version_satisfies("1.2.3", ""));
    assert(tc::version_satisfies("1.2.3", "*"));
    assert(tc::version_satisfies("1.2.3", "1.2.3"));
    assert(tc::version_satisfies("1.2.3", "=1.2.3"));
    assert(tc::version_satisfies("1.2.3", ">=1.2.3"));
    assert(tc::version_satisfies("1.2.3", ">1.2.2"));
    assert(tc::version_satisfies("1.2.3", "<=1.2.3"));
    assert(tc::version_satisfies("1.2.3", ">=1.0.0, <2.0.0"));
    assert(tc::version_satisfies("1.2.3", "!=1.2.4"));
    assert(!tc::version_satisfies("1.2.3", ">=1.3.0"));
    assert(!tc::version_satisfies("1.2.3", ">1.2.3"));
    assert(!tc::version_satisfies("1.2.3", "1.2.4"));
    assert(!tc::version_satisfies("1.2.3", "!=1.2.3"));
    assert(!tc::version_satisfies("1.2.3", ">=1.0.0,<1.2.0"));
    /* An unparsable operand is a rejection, not a silent pass. */
    assert(!tc::version_satisfies("1.2.3", ">="));
    /* A dependency that declares no version can still be constrained by name. */
    assert(!tc::version_satisfies("", ">=1.0.0"));
}

void dependencyShapes() {
    using tc::J;
    std::vector<std::string> ids;
    std::map<std::string, std::string> constraints;
    tc::parse_dependencies(J{{"requires", J::array({"a", "b"})}}, "requires", ids, constraints);
    assert(ids.size() == 2 && ids[0] == "a" && ids[1] == "b");
    assert(tc::constraint_of(constraints, "a").empty());
    ids.clear();
    constraints.clear();
    tc::parse_dependencies(J{{"requires", J{{"a", ">=1.0.0"}, {"b", ""}}}}, "requires", ids, constraints);
    assert(ids.size() == 2);
    assert(tc::constraint_of(constraints, "a") == ">=1.0.0");
    assert(tc::constraint_of(constraints, "b").empty());
    /* Repeating an id keeps the first constraint and does not duplicate it. */
    ids.clear();
    constraints.clear();
    tc::parse_dependencies(J{{"requires", J::array({"a", "a"})}}, "requires", ids, constraints);
    assert(ids.size() == 1);
    /* Rejections: a non-list shape, a bad id, a non-string constraint. */
    for (const auto& bad : {J{{"requires", 5}}, J{{"requires", J::array({"../escape"})}},
                            J{{"requires", J{{"a", 5}}}}}) {
        bool threw = false;
        try {
            std::vector<std::string> scratch;
            std::map<std::string, std::string> scratchConstraints;
            tc::parse_dependencies(bad, "requires", scratch, scratchConstraints);
        } catch (const std::exception&) {
            threw = true;
        }
        assert(threw);
    }
    /* A missing key is not an error: almost every package omits both. */
    ids.clear();
    constraints.clear();
    tc::parse_dependencies(J::object(), "optional", ids, constraints);
    assert(ids.empty());
}

void capabilityDeclarations() {
    using tc::J;
    std::vector<std::string> declared;
    tc::parse_capabilities(J{{"capabilities", J::array({"status", "ui_slot"})}}, tc::loader_capabilities(), declared);
    assert(declared.size() == 2 && declared[0] == "status" && declared[1] == "ui_slot");
    /* A name that does not exist, and one this loader does not provide, are
       two different messages: "unknown" is an author mistake, "not provided"
       is this loader being older than the package. */
    auto rejects = [](const J& manifest, uint64_t loader, const std::string& expected) {
        try {
            std::vector<std::string> scratch;
            tc::parse_capabilities(manifest, loader, scratch);
        } catch (const std::exception& error) {
            return std::string(error.what()).find(expected) != std::string::npos;
        }
        return false;
    };
    assert(rejects(J{{"capabilities", J::array({"telepathy"})}}, tc::loader_capabilities(), "Unknown loader capability"));
    assert(rejects(J{{"capabilities", J::array({"ui_slot"})}}, TC_CAP_LOG, "does not provide the capability"));
    assert(rejects(J{{"capabilities", "ui_slot"}}, tc::loader_capabilities(), "must be an array"));
    assert(rejects(J{{"capabilities", J::array({5})}}, tc::loader_capabilities(), "must be names"));
    /* The list the CLI prints is the same table. */
    assert(tc::capability_names(tc::loader_capabilities()).find("ui_page") != std::string::npos);
}

/* The 0.6.0 fields are a tail extension: everything an older plugin knows keeps
   its offset, and the SDK helpers tolerate a host that ends before the tail. */
void tailExtensionKeepsOlderPluginsWorking() {
    assert(offsetof(TCHost,register_ui_slot) < offsetof(TCHost,host_version));
    assert(offsetof(TCHost,host_version) < offsetof(TCHost,capabilities));
    assert(offsetof(TCHost,capabilities) < offsetof(TCHost,report_status));
    TCHost old{};
    old.size = static_cast<uint32_t>(offsetof(TCHost,host_version));  // ends before the tail
    old.api_version = TC_MOD_API_VERSION;
    assert(tc::hostVersion(&old) == 0u);
    assert(tc::hostCapabilities(&old) == 0ull);
    assert(!tc::hostHas(&old,TC_CAP_LOG));
    assert(tc::reportStatus(&old,0,"hello") == -1);
    TCHost host{};
    host.size = sizeof(host);
    host.host_version = TC_HOST_VERSION_CODE(0,6,0);
    host.capabilities = TC_CAP_LOG | TC_CAP_STATUS;
    int reports = 0;
    host.context = &reports;
    host.report_status = [](void* context,int level,const char* message) {
        ++*static_cast<int*>(context);
        return (level == 0 && message != nullptr) ? 0 : -1;
    };
    assert(tc::hostVersion(&host) == TC_HOST_VERSION_CODE(0,6,0));
    assert(tc::hostHas(&host,TC_CAP_LOG) && tc::hostHas(&host,TC_CAP_STATUS));
    assert(!tc::hostHas(&host,TC_CAP_UI_SLOT));
    assert(tc::reportStatus(&host,0,"hello") == 0 && reports == 1);
    assert(tc::reportStatus(&host,9,"hello") == -1 && reports == 2);
    assert(!tc::hostHas(nullptr,TC_CAP_LOG));
    assert(tc::reportStatus(nullptr,0,"hello") == -1);
}

/* docs/reference/capabilities.md is the page authors read; keep it honest. */
void documentationListsEveryCapability() {
    std::string doc = fileText("docs/reference/capabilities.md");
    for (const auto& entry : tc::capability_table()) {
        std::string needle = std::string("`") + entry.name + "`";
        assert(doc.find(needle) != std::string::npos);
    }
}

}  // namespace

int main() {
    capabilityTableIsComplete();
    versionMatchesTheVersionFile();
    versionOrdering();
    versionConstraints();
    dependencyShapes();
    capabilityDeclarations();
    tailExtensionKeepsOlderPluginsWorking();
    documentationListsEveryCapability();
    std::cout << "PASS host contract: version source of truth, capability table and dependency constraints\n";
    return 0;
}
