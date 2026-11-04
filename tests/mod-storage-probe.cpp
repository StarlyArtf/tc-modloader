/* Real-game persistence probe for tc.mod.storage V2 + tc.mod.settings V1.

   Every process start increments one global counter and one active-profile
   counter during tc_mod_load.  The playtest switches A -> B -> A and requires
   1/1, 2/1, 3/2: the service must therefore be available during initialization,
   global data must survive and profile data must follow the selected save. */
#include "../sdk/tc_mod_storage.h"
#include "../sdk/tc_mod_settings.h"

#include <cstdlib>
#include <string>

static const TCHost* host;

static void logLine(const std::string& text) {
    if (host && host->log) host->log(host->context, text.c_str());
}

static unsigned readCounter(const tc::mod_storage::ApiV2& storage, uint32_t scope) {
    std::string text;
    const int status = tc::mod_storage::readText(storage, scope, "runs", &text);
    if (status == TC_MOD_STORAGE_ERR_NOT_FOUND) return 0;
    if (status != TC_MOD_STORAGE_OK) return UINT32_MAX;
    char* end = nullptr;
    const unsigned long value = std::strtoul(text.c_str(), &end, 10);
    return !end || *end || value > UINT32_MAX ? UINT32_MAX
                                              : static_cast<unsigned>(value);
}

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* h, TCPlugin* plugin) {
    if (!h || !plugin || h->api_version != TC_MOD_API_VERSION ||
        plugin->size < sizeof(TCPlugin))
        return 1;
    host = h;
    tc::mod_storage::ApiV2 storage{};
    if (!tc::mod_storage::tableV2(h, &storage)) {
        logLine("mod storage probe: service unavailable");
        return 2;
    }
    const char zero = '0';
    uint32_t version = 0;
    if (tc::mod_storage::migrate(storage, TC_MOD_STORAGE_SCOPE_GLOBAL, "runs", 1,
                                 &zero, 1, nullptr, nullptr, &version) != TC_MOD_STORAGE_OK ||
        tc::mod_storage::migrate(storage, TC_MOD_STORAGE_SCOPE_PROFILE, "runs", 1,
                                 &zero, 1, nullptr, nullptr, &version) != TC_MOD_STORAGE_OK) {
        logLine("mod storage probe: migration failed");
        return 3;
    }
    tc::mod_settings::Api settings{};
    if (!tc::mod_settings::table(h, &settings)) {
        logLine("mod storage probe: settings service unavailable");
        return 3;
    }
    TCModSettingFieldV1 fields[2]{};
    fields[0].size = sizeof(fields[0]);
    fields[0].type = TC_MOD_SETTING_BOOL;
    fields[0].scope = TC_MOD_STORAGE_SCOPE_GLOBAL;
    fields[0].id = "enabled";
    fields[0].label = "Probe enabled";
    fields[0].default_integer = 1;
    fields[1].size = sizeof(fields[1]);
    fields[1].type = TC_MOD_SETTING_TEXT;
    fields[1].scope = TC_MOD_STORAGE_SCOPE_PROFILE;
    fields[1].id = "note";
    fields[1].label = "Profile note";
    fields[1].default_text = "profile";
    if (tc::mod_settings::setFields(settings, fields, 2) != TC_MOD_SETTINGS_OK) {
        logLine("mod storage probe: settings registration failed");
        return 3;
    }
    unsigned global = readCounter(storage, TC_MOD_STORAGE_SCOPE_GLOBAL);
    unsigned profile = readCounter(storage, TC_MOD_STORAGE_SCOPE_PROFILE);
    if (global == UINT32_MAX || profile == UINT32_MAX) {
        logLine("mod storage probe: read failed");
        return 4;
    }
    ++global;
    ++profile;
    if (tc::mod_storage::writeText(storage, TC_MOD_STORAGE_SCOPE_GLOBAL, "runs",
                                   std::to_string(global)) != TC_MOD_STORAGE_OK ||
        tc::mod_storage::writeText(storage, TC_MOD_STORAGE_SCOPE_PROFILE, "runs",
                                   std::to_string(profile)) != TC_MOD_STORAGE_OK) {
        logLine("mod storage probe: write failed");
        return 5;
    }
    logLine("mod storage probe: global=" + std::to_string(global) +
            " profile=" + std::to_string(profile));
    return 0;
}
