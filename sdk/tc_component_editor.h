#ifndef TC_COMPONENT_EDITOR_H
#define TC_COMPONENT_EDITOR_H

#include "tc_mod_api.h"
#include "tc_service_api.h"

#include <cstdint>

/* tc.component.editor V1 - C++ convenience layer over
   TC_SERVICE_COMPONENT_EDITOR.

   V2 (`tableV2`) adds the layout flags, the radio form of an enum, the editable
   hexadecimal widget and a scrolling row window; the field table is the same
   `TCComponentFieldV1` in both, so a Mod that asks for V2 hands over exactly the
   same array.  Ask for V2 and fall back to V1 when it is not there.

   A type declares the fields of its own configuration once, during
   `tc_mod_load`; the host then draws one row per field over the game's own
   component drawer whenever one of the type's instances is selected, and commits
   each edit through the same path `tc.component.storage` writes use.  One
   committed field is one undo step and the type's `on_config_changed` fires.

   ```cpp
   #include "tc_component_editor.h"

   static const char* const kRounding[] = {"RNE", "RTZ", "RDN", "RUP", "RNA"};
   static const TCComponentFieldV1 kFields[] = {
       //          id          label        tooltip kind                      flags offset bytes bits choices labels   min max     minf maxf
       {"enabled", "Enabled",   nullptr, TC_COMPONENT_FIELD_BOOL,  0,  0, 1,  0, 0, nullptr,   0, 0, 0, 0},
       {"period",  "Period",    nullptr, TC_COMPONENT_FIELD_UINT,  0,  2, 2,  0, 0, nullptr,   1, 16, 0, 0},
       {"round",   "Rounding",  nullptr, TC_COMPONENT_FIELD_ENUM,  0,  4, 1,  0, 5, kRounding, 0, 0, 0, 0},
       {"label",   "Label",     nullptr, TC_COMPONENT_FIELD_TEXT,  0,  5, 24, 0, 0, nullptr,   0, 0, 0, 0},
   };

   tc::component_editor::Api editor{};
   if (tc::component_editor::table(host, &editor))
       tc::component_editor::setFields(editor, kMyTypeId, kFields,
                                       sizeof(kFields) / sizeof(kFields[0]));
   ```

   The declared values must fit the configuration the type registered through
   tc.component.types (`config_size`); a field that reaches past it is refused
   with TC_COMPONENT_EDITOR_ERR_FIELD and the reason goes to the log.  Use
   tc_component_storage.h when a Mod wants to write a configuration itself
   instead of letting the player edit a field.

   The offsets are byte offsets into the configuration, read and written
   little-endian, so they do not have to match the padding of a C struct. */

namespace tc {
namespace component_editor {

using Api = TCComponentEditorApiV1;
using ApiV2 = TCComponentEditorApiV2;
using ApiV3 = TCComponentEditorApiV3;
using ApiV4 = TCComponentEditorApiV4;
using Field = TCComponentFieldV1;
using FieldV2 = TCComponentFieldV2;

inline bool table(const TCHost* host, Api* out) {
    if (!host || !out || !host->query_service) return false;
    Api queried{};
    if (host->query_service(host->context, TC_SERVICE_COMPONENT_EDITOR,
                            TC_COMPONENT_EDITOR_API_VERSION_1, &queried,
                            sizeof(queried)) != TC_SERVICE_OK)
        return false;
    if (queried.size < sizeof(queried) ||
        queried.version != TC_COMPONENT_EDITOR_API_VERSION_1 || !queried.context ||
        !queried.set_fields || !queried.clear_fields)
        return false;
    *out = queried;
    return true;
}

/* The same service at V2: the field table may then use the layout flags
   (TC_COMPONENT_FIELD_FULL_ROW / INLINE / SEPARATOR), the radio form of an enum
   and the editable hexadecimal widget, and the host's row window scrolls when a
   type declares more rows than fit under the game's own drawer.

   ```cpp
   tc::component_editor::ApiV2 editor{};
   if (tc::component_editor::tableV2(host, &editor))
       tc::component_editor::setFields(editor, kMyTypeId, kFields, count);
   else
       ... // a V1 loader: the flags would be ignored, so ask for V1 and degrade
   ``` */
inline bool tableV2(const TCHost* host, ApiV2* out) {
    if (!host || !out || !host->query_service) return false;
    ApiV2 queried{};
    if (host->query_service(host->context, TC_SERVICE_COMPONENT_EDITOR,
                            TC_COMPONENT_EDITOR_API_VERSION_2, &queried,
                            sizeof(queried)) != TC_SERVICE_OK)
        return false;
    if (queried.size < sizeof(queried) ||
        queried.version != TC_COMPONENT_EDITOR_API_VERSION_2 || !queried.context ||
        !queried.set_fields || !queried.clear_fields)
        return false;
    *out = queried;
    return true;
}

inline bool tableV3(const TCHost* host, ApiV3* out) {
    if (!host || !out || !host->query_service) return false;
    ApiV3 queried{};
    if (host->query_service(host->context, TC_SERVICE_COMPONENT_EDITOR,
                            TC_COMPONENT_EDITOR_API_VERSION_3, &queried,
                            sizeof(queried)) != TC_SERVICE_OK)
        return false;
    if (queried.size < sizeof(queried) ||
        queried.version != TC_COMPONENT_EDITOR_API_VERSION_3 || !queried.context ||
        !queried.set_fields || !queried.clear_fields || !queried.set_fields_v2 ||
        !queried.begin_edit || !queried.commit_edit || !queried.abort_edit)
        return false;
    *out = queried;
    return true;
}

/* The editor service at V4: the V3 table plus a table-level flag set
   (TC_COMPONENT_EDITOR_TABLE_CONFIRM) and the V4 field shapes - a swatch
   palette, an editable colour hex box and a one-line validation marker.

   ```cpp
   tc::component_editor::ApiV4 editor{};
   if (tc::component_editor::tableV4(host, &editor))
       tc::component_editor::setFieldsV3(editor, kMyTypeId, fields, count,
                                         TC_COMPONENT_EDITOR_TABLE_CONFIRM);
   else if (tc::component_editor::tableV3(host, &editorV3))
       ... // a V3 loader: drop the V4 flags and shapes from the table
   ``` */
inline bool tableV4(const TCHost* host, ApiV4* out) {
    if (!host || !out || !host->query_service) return false;
    ApiV4 queried{};
    if (host->query_service(host->context, TC_SERVICE_COMPONENT_EDITOR,
                            TC_COMPONENT_EDITOR_API_VERSION_4, &queried,
                            sizeof(queried)) != TC_SERVICE_OK)
        return false;
    if (queried.size < sizeof(queried) ||
        queried.version != TC_COMPONENT_EDITOR_API_VERSION_4 || !queried.context ||
        !queried.set_fields || !queried.clear_fields || !queried.set_fields_v2 ||
        !queried.begin_edit || !queried.commit_edit || !queried.abort_edit ||
        !queried.set_fields_v3)
        return false;
    *out = queried;
    return true;
}

/* Declares the rows of one of this Mod's registered types.  Call during
   tc_mod_load, after the type is registered. */
inline int setFields(const Api& api, uint64_t custom_id, const Field* fields,
                     uint32_t count) {
    if (!api.set_fields) return TC_COMPONENT_EDITOR_ERR_UNAVAILABLE;
    return api.set_fields(api.context, custom_id, fields, count);
}

inline int setFields(const ApiV2& api, uint64_t custom_id, const Field* fields,
                     uint32_t count) {
    if (!api.set_fields) return TC_COMPONENT_EDITOR_ERR_UNAVAILABLE;
    return api.set_fields(api.context, custom_id, fields, count);
}

inline int setFields(const ApiV3& api, uint64_t custom_id, const Field* fields,
                     uint32_t count) {
    return api.set_fields ? api.set_fields(api.context, custom_id, fields, count)
                          : TC_COMPONENT_EDITOR_ERR_UNAVAILABLE;
}

inline int setFields(const ApiV4& api, uint64_t custom_id, const Field* fields,
                     uint32_t count) {
    return api.set_fields ? api.set_fields(api.context, custom_id, fields, count)
                          : TC_COMPONENT_EDITOR_ERR_UNAVAILABLE;
}

inline int setFieldsV2(const ApiV3& api, uint64_t custom_id, const FieldV2* fields,
                       uint32_t count) {
    return api.set_fields_v2 ? api.set_fields_v2(api.context, custom_id, fields, count)
                             : TC_COMPONENT_EDITOR_ERR_UNAVAILABLE;
}

inline int setFieldsV2(const ApiV4& api, uint64_t custom_id, const FieldV2* fields,
                       uint32_t count) {
    return api.set_fields_v2 ? api.set_fields_v2(api.context, custom_id, fields, count)
                             : TC_COMPONENT_EDITOR_ERR_UNAVAILABLE;
}

/* The V4 entry point: the same table, plus the table-level flags. */
inline int setFieldsV3(const ApiV4& api, uint64_t custom_id, const FieldV2* fields,
                       uint32_t count, uint32_t table_flags) {
    return api.set_fields_v3 ? api.set_fields_v3(api.context, custom_id, fields, count,
                                                 table_flags)
                             : TC_COMPONENT_EDITOR_ERR_UNAVAILABLE;
}

/* Removes a type's rows, so the host stops drawing an editor for it. */
inline int clearFields(const Api& api, uint64_t custom_id) {
    if (!api.clear_fields) return TC_COMPONENT_EDITOR_ERR_UNAVAILABLE;
    return api.clear_fields(api.context, custom_id);
}

inline int clearFields(const ApiV2& api, uint64_t custom_id) {
    if (!api.clear_fields) return TC_COMPONENT_EDITOR_ERR_UNAVAILABLE;
    return api.clear_fields(api.context, custom_id);
}


inline int clearFields(const ApiV3& api, uint64_t custom_id) {
    return api.clear_fields ? api.clear_fields(api.context, custom_id)
                            : TC_COMPONENT_EDITOR_ERR_UNAVAILABLE;
}

inline int clearFields(const ApiV4& api, uint64_t custom_id) {
    return api.clear_fields ? api.clear_fields(api.context, custom_id)
                            : TC_COMPONENT_EDITOR_ERR_UNAVAILABLE;
}

inline int beginEdit(const ApiV3& api, const TCComponentInstanceHandle* instance) {
    return api.begin_edit ? api.begin_edit(api.context, instance)
                          : TC_COMPONENT_EDITOR_ERR_UNAVAILABLE;
}
inline int commitEdit(const ApiV3& api, const TCComponentInstanceHandle* instance) {
    return api.commit_edit ? api.commit_edit(api.context, instance)
                           : TC_COMPONENT_EDITOR_ERR_UNAVAILABLE;
}
inline int abortEdit(const ApiV3& api, const TCComponentInstanceHandle* instance) {
    return api.abort_edit ? api.abort_edit(api.context, instance)
                          : TC_COMPONENT_EDITOR_ERR_UNAVAILABLE;
}

inline int beginEdit(const ApiV4& api, const TCComponentInstanceHandle* instance) {
    return api.begin_edit ? api.begin_edit(api.context, instance)
                          : TC_COMPONENT_EDITOR_ERR_UNAVAILABLE;
}
inline int commitEdit(const ApiV4& api, const TCComponentInstanceHandle* instance) {
    return api.commit_edit ? api.commit_edit(api.context, instance)
                           : TC_COMPONENT_EDITOR_ERR_UNAVAILABLE;
}
inline int abortEdit(const ApiV4& api, const TCComponentInstanceHandle* instance) {
    return api.abort_edit ? api.abort_edit(api.context, instance)
                          : TC_COMPONENT_EDITOR_ERR_UNAVAILABLE;
}

inline const char* errorText(int status) {
    switch (status) {
        case TC_COMPONENT_EDITOR_OK: return "ok";
        case TC_COMPONENT_EDITOR_ERR_UNAVAILABLE: return "this loader has no tc.component.editor";
        case TC_COMPONENT_EDITOR_ERR_ARGUMENT: return "null, undersized or malformed argument";
        case TC_COMPONENT_EDITOR_ERR_OWNERSHIP: return "the type is not this Mod's";
        case TC_COMPONENT_EDITOR_ERR_FIELD: return "a field does not fit the configuration";
        case TC_COMPONENT_EDITOR_ERR_LIMIT: return "more fields than the host accepts";
        case TC_COMPONENT_EDITOR_ERR_THREAD: return "outside the game thread's tc_mod_load";
        default: return "unknown status";
    }
}

}  // namespace component_editor
}  // namespace tc

#endif  // TC_COMPONENT_EDITOR_H
