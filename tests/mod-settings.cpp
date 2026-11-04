#include "../src/mod_settings.hpp"

#include <cassert>
#include <cstring>
#include <iostream>

int main() {
    const char* choices[] = {"Slow", "Normal", "Fast"};
    TCModSettingFieldV1 fields[5]{};
    for (auto& field : fields) field.size = sizeof(field);

    fields[0].type = TC_MOD_SETTING_BOOL;
    fields[0].scope = TC_MOD_STORAGE_SCOPE_GLOBAL;
    fields[0].id = "grid";
    fields[0].label = "Show grid";
    fields[0].default_integer = 1;

    fields[1].type = TC_MOD_SETTING_INT;
    fields[1].scope = TC_MOD_STORAGE_SCOPE_PROFILE;
    fields[1].id = "budget";
    fields[1].label = "Budget";
    fields[1].minimum_integer = 1;
    fields[1].maximum_integer = 20;
    fields[1].step_integer = 1;
    fields[1].default_integer = 5;

    fields[2].type = TC_MOD_SETTING_FLOAT;
    fields[2].scope = TC_MOD_STORAGE_SCOPE_GLOBAL;
    fields[2].id = "scale";
    fields[2].label = "Scale";
    fields[2].minimum_number = 0.5;
    fields[2].maximum_number = 2.0;
    fields[2].step_number = 0.1;
    fields[2].default_number = 1.0;

    fields[3].type = TC_MOD_SETTING_ENUM;
    fields[3].scope = TC_MOD_STORAGE_SCOPE_PROFILE;
    fields[3].id = "speed";
    fields[3].label = "Speed";
    fields[3].choices = choices;
    fields[3].choice_count = 3;
    fields[3].default_integer = 1;

    fields[4].type = TC_MOD_SETTING_TEXT;
    fields[4].scope = TC_MOD_STORAGE_SCOPE_GLOBAL;
    fields[4].id = "label";
    fields[4].label = "Label";
    fields[4].default_text = "demo";

    std::vector<tc::mod_settings::Field> copied;
    std::string reason;
    assert(tc::mod_settings::copyFields(fields, 5, &copied, &reason) == TC_MOD_SETTINGS_OK);
    assert(copied.size() == 5);
    assert(copied[0].storageKey == "settings.grid");
    assert(copied[3].choices[2] == "Fast");

    tc::mod_settings::Value decoded;
    assert(tc::mod_settings::decode(copied[0], "0", &decoded) && decoded.integer == 0);
    assert(!tc::mod_settings::decode(copied[0], "yes", &decoded));
    assert(tc::mod_settings::decode(copied[1], "20", &decoded) && decoded.integer == 20);
    assert(!tc::mod_settings::decode(copied[1], "21", &decoded));
    assert(tc::mod_settings::decode(copied[2], "1.25", &decoded) && decoded.number == 1.25);
    assert(!tc::mod_settings::decode(copied[2], "nan", &decoded));
    assert(tc::mod_settings::decode(copied[3], "2", &decoded) && decoded.enumIndex == 2);
    assert(!tc::mod_settings::decode(copied[3], "3", &decoded));
    assert(tc::mod_settings::decode(copied[4], "hello", &decoded) && decoded.text == "hello");
    const auto publicValue = tc::mod_settings::publicValue(decoded);
    assert(publicValue.size == sizeof(publicValue));
    assert(publicValue.type == TC_MOD_SETTING_TEXT);
    assert(std::strcmp(publicValue.text, "hello") == 0);

    fields[4].id = "grid";
    assert(tc::mod_settings::copyFields(fields, 5, &copied, &reason) ==
           TC_MOD_SETTINGS_ERR_DUPLICATE);
    fields[4].id = "bad/path";
    assert(tc::mod_settings::copyFields(fields, 5, &copied, &reason) ==
           TC_MOD_SETTINGS_ERR_ARGUMENT);
    fields[4].id = "label";
    fields[3].choice_count = TC_MOD_SETTING_MAX_CHOICES + 1;
    assert(tc::mod_settings::copyFields(fields, 5, &copied, &reason) ==
           TC_MOD_SETTINGS_ERR_ARGUMENT);

    std::cout << "PASS tc.mod.settings field validation, defaults and typed codecs\n";
    return 0;
}
