#pragma once

#include "../sdk/tc_service_api.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace tc::mod_settings {

struct Value {
    uint32_t type = 0;
    int64_t integer = 0;
    double number = 0.0;
    uint32_t enumIndex = 0;
    std::string text;
};

struct Field {
    uint32_t type = 0;
    uint32_t scope = 0;
    std::string id;
    std::string label;
    std::string description;
    std::string storageKey;
    int64_t minimumInteger = 0;
    int64_t maximumInteger = 0;
    int64_t stepInteger = 1;
    double minimumNumber = 0.0;
    double maximumNumber = 0.0;
    double stepNumber = 0.0;
    std::vector<std::string> choices;
    Value defaultValue;
    Value current;
};

inline bool validPart(const char* value, size_t maximum) {
    if (!value) return false;
    const size_t length = std::strlen(value);
    if (!length || length > maximum) return false;
    for (size_t i = 0; i < length; ++i) {
        const unsigned char c = static_cast<unsigned char>(value[i]);
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '.' || c == '_' || c == '-')) return false;
    }
    return true;
}

inline bool boundedString(const char* value, size_t maximum, bool required) {
    if (!value) return !required;
    const size_t length = std::strlen(value);
    return (!required || length != 0) && length <= maximum;
}

inline Value defaultValue(const TCModSettingFieldV1& source) {
    Value value;
    value.type = source.type;
    value.integer = source.default_integer;
    value.number = source.default_number;
    value.enumIndex = static_cast<uint32_t>(source.default_integer);
    if (source.default_text) value.text = source.default_text;
    return value;
}

inline int copyFields(const TCModSettingFieldV1* fields, uint32_t count,
                      std::vector<Field>* out, std::string* reason = nullptr) {
    if (!out || (!fields && count)) return TC_MOD_SETTINGS_ERR_ARGUMENT;
    if (count > TC_MOD_SETTINGS_MAX_FIELDS) return TC_MOD_SETTINGS_ERR_CAPACITY;
    std::set<std::string> ids, keys;
    std::vector<Field> copied;
    copied.reserve(count);
    const auto fail = [&](int status, const std::string& text) {
        if (reason) *reason = text;
        return status;
    };
    for (uint32_t i = 0; i < count; ++i) {
        const auto& source = fields[i];
        if (source.size < sizeof(TCModSettingFieldV1) || source.flags || source.reserved)
            return fail(TC_MOD_SETTINGS_ERR_ARGUMENT, "short field or non-zero flags");
        if (!validPart(source.id, TC_MOD_SETTING_MAX_ID_BYTES) ||
            !boundedString(source.label, 128, true) ||
            !boundedString(source.description, 512, false))
            return fail(TC_MOD_SETTINGS_ERR_ARGUMENT, "invalid id, label or description");
        const std::string id = source.id;
        const std::string key = source.storage_key && source.storage_key[0]
            ? source.storage_key : std::string("settings.") + id;
        if (!validPart(key.c_str(), TC_MOD_STORAGE_MAX_KEY_BYTES))
            return fail(TC_MOD_SETTINGS_ERR_ARGUMENT, "invalid storage key");
        if (!ids.insert(id).second || !keys.insert(key).second)
            return fail(TC_MOD_SETTINGS_ERR_DUPLICATE, "duplicate id or storage key");
        if (source.scope != TC_MOD_STORAGE_SCOPE_GLOBAL &&
            source.scope != TC_MOD_STORAGE_SCOPE_PROFILE)
            return fail(TC_MOD_SETTINGS_ERR_ARGUMENT, "invalid scope");

        Field field;
        field.type = source.type;
        field.scope = source.scope;
        field.id = id;
        field.label = source.label;
        field.description = source.description ? source.description : "";
        field.storageKey = key;
        field.minimumInteger = source.minimum_integer;
        field.maximumInteger = source.maximum_integer;
        field.stepInteger = source.step_integer;
        field.minimumNumber = source.minimum_number;
        field.maximumNumber = source.maximum_number;
        field.stepNumber = source.step_number;
        field.defaultValue = defaultValue(source);

        switch (source.type) {
            case TC_MOD_SETTING_BOOL:
                field.defaultValue.integer = source.default_integer ? 1 : 0;
                break;
            case TC_MOD_SETTING_INT:
                if (source.minimum_integer > source.maximum_integer ||
                    source.minimum_integer < INT32_MIN || source.maximum_integer > INT32_MAX ||
                    source.step_integer <= 0 || source.default_integer < source.minimum_integer ||
                    source.default_integer > source.maximum_integer)
                    return fail(TC_MOD_SETTINGS_ERR_ARGUMENT, "invalid integer bounds/default");
                break;
            case TC_MOD_SETTING_FLOAT:
                if (!std::isfinite(source.minimum_number) ||
                    !std::isfinite(source.maximum_number) ||
                    !std::isfinite(source.step_number) ||
                    !std::isfinite(source.default_number) ||
                    source.minimum_number > source.maximum_number ||
                    source.step_number < 0.0 ||
                    source.default_number < source.minimum_number ||
                    source.default_number > source.maximum_number)
                    return fail(TC_MOD_SETTINGS_ERR_ARGUMENT, "invalid number bounds/default");
                break;
            case TC_MOD_SETTING_ENUM:
                if (!source.choices || !source.choice_count ||
                    source.choice_count > TC_MOD_SETTING_MAX_CHOICES ||
                    source.default_integer < 0 ||
                    static_cast<uint64_t>(source.default_integer) >= source.choice_count)
                    return fail(TC_MOD_SETTINGS_ERR_ARGUMENT, "invalid enum choices/default");
                for (uint32_t choice = 0; choice < source.choice_count; ++choice) {
                    if (!boundedString(source.choices[choice], 64, true))
                        return fail(TC_MOD_SETTINGS_ERR_ARGUMENT, "invalid enum label");
                    field.choices.emplace_back(source.choices[choice]);
                }
                break;
            case TC_MOD_SETTING_TEXT:
                if (!boundedString(source.default_text, TC_MOD_SETTING_MAX_TEXT_BYTES, false))
                    return fail(TC_MOD_SETTINGS_ERR_ARGUMENT, "text default is too long");
                break;
            default:
                return fail(TC_MOD_SETTINGS_ERR_TYPE, "unknown setting type");
        }
        field.current = field.defaultValue;
        copied.push_back(std::move(field));
    }
    *out = std::move(copied);
    return TC_MOD_SETTINGS_OK;
}

inline std::string encode(const Value& value) {
    switch (value.type) {
        case TC_MOD_SETTING_BOOL: return value.integer ? "1" : "0";
        case TC_MOD_SETTING_INT: return std::to_string(value.integer);
        case TC_MOD_SETTING_ENUM: return std::to_string(value.enumIndex);
        case TC_MOD_SETTING_FLOAT: {
            std::ostringstream out;
            out << std::setprecision(std::numeric_limits<double>::max_digits10) << value.number;
            return out.str();
        }
        case TC_MOD_SETTING_TEXT: return value.text;
        default: return {};
    }
}

inline bool decode(const Field& field, const std::string& text, Value* out) {
    if (!out || text.find('\0') != std::string::npos) return false;
    Value value;
    value.type = field.type;
    char* end = nullptr;
    errno = 0;
    switch (field.type) {
        case TC_MOD_SETTING_BOOL:
            if (text == "0") value.integer = 0;
            else if (text == "1") value.integer = 1;
            else return false;
            break;
        case TC_MOD_SETTING_INT: {
            const long long parsed = std::strtoll(text.c_str(), &end, 10);
            if (errno || !end || *end || parsed < field.minimumInteger ||
                parsed > field.maximumInteger) return false;
            value.integer = parsed;
            break;
        }
        case TC_MOD_SETTING_FLOAT: {
            const double parsed = std::strtod(text.c_str(), &end);
            if (errno || !end || *end || !std::isfinite(parsed) ||
                parsed < field.minimumNumber || parsed > field.maximumNumber) return false;
            value.number = parsed;
            break;
        }
        case TC_MOD_SETTING_ENUM: {
            const unsigned long parsed = std::strtoul(text.c_str(), &end, 10);
            if (errno || !end || *end || parsed >= field.choices.size()) return false;
            value.enumIndex = static_cast<uint32_t>(parsed);
            value.integer = value.enumIndex;
            break;
        }
        case TC_MOD_SETTING_TEXT:
            if (text.size() > TC_MOD_SETTING_MAX_TEXT_BYTES) return false;
            value.text = text;
            break;
        default:
            return false;
    }
    *out = std::move(value);
    return true;
}

inline TCModSettingValueV1 publicValue(const Value& value) {
    TCModSettingValueV1 out{};
    out.size = sizeof(out);
    out.version = TC_MOD_SETTING_VALUE_VERSION_1;
    out.type = value.type;
    out.integer = value.integer;
    out.number = value.number;
    out.enum_index = value.enumIndex;
    out.text_size = static_cast<uint32_t>(std::min<size_t>(
        value.text.size(), TC_MOD_SETTING_MAX_TEXT_BYTES));
    if (out.text_size) std::memcpy(out.text, value.text.data(), out.text_size);
    out.text[out.text_size] = 0;
    return out;
}

}  // namespace tc::mod_settings
