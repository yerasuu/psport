// SPDX-License-Identifier: GPL-2.0-or-later
/* libSceJson's sce::Json classes as the C++ ABI of the games sees them. The SDK's objects are bigger
 * than these (Value 0x20 bytes, String, Object and Array 8, Initializer at most 4, as NpCppWebApi and
 * the eboots that link it lay them out), so each one fits in the storage a caller gives it. Elements
 * live in nodes of their own because operator[], back() and getString() hand out references that
 * must survive later insertions. */
#ifndef BB_RUNTIME_JSON_H
#define BB_RUNTIME_JSON_H
#include "runtime.h"
#include <stdbool.h>

enum {
    JSON_TYPE_NULL,
    JSON_TYPE_BOOLEAN,
    JSON_TYPE_INTEGER,
    JSON_TYPE_UINTEGER,
    JSON_TYPE_REAL,
    JSON_TYPE_STRING,
    JSON_TYPE_ARRAY,
    JSON_TYPE_OBJECT,
};

/* Only 0x80848102, out of memory, is confirmed: the MemAllocator Black Ops derives aborts on it in
 * notifyError. The others are this port's choice in the same range. */
#define JSON_ERROR_INVALID_ARGUMENT ((int32_t)0x80848101)
#define JSON_ERROR_NO_MEMORY ((int32_t)0x80848102)
#define JSON_ERROR_PARSE ((int32_t)0x80848103)
#define JSON_ERROR_ALREADY_INITIALIZED ((int32_t)0x80848104)
#define JSON_ERROR_NOT_INITIALIZED ((int32_t)0x80848105)

typedef struct JsonText {
    uint64_t cb_length, cb_capacity;
    char text[];
} JsonText;
typedef struct JsonString { JsonText *p_text; } JsonString;
typedef struct JsonArray { struct JsonArrayData *p_data; } JsonArray;
typedef struct JsonObject { struct JsonObjectData *p_data; } JsonObject;
typedef struct JsonValue {
    uint32_t type, reserved;
    union {
        bool boolean;
        int64_t integer;
        uint64_t uinteger;
        double real;
        JsonString string;
        JsonArray array;
        JsonObject object;
    };
} JsonValue;
typedef struct JsonArrayData {
    uint64_t c_items, c_capacity;
    JsonValue **pp_items;
} JsonArrayData;
typedef struct JsonMember {
    JsonString key;
    JsonValue value;
} JsonMember;
/* Members sorted by key bytes, as the SDK's map orders them. */
typedef struct JsonObjectData {
    uint64_t c_items, c_capacity;
    JsonMember **pp_members;
} JsonObjectData;

void *json_allocate(uint64_t cb_size);
void json_free(void *p_block);
bool json_string_assign(JsonString *p_string, const char *p_text, uint64_t cb_length);
bool json_string_append(JsonString *p_string, const char *p_text, uint64_t cb_length);
void json_string_release(JsonString *p_string);
void json_value_release(JsonValue *p_value);
bool json_array_append(JsonArray *p_array, const JsonValue *p_source);
JsonValue *json_object_member(JsonObject *p_object, const char *p_key, uint64_t cb_key, bool should_insert);
bool json_value_copy(JsonValue *p_target, const JsonValue *p_source);

ABI int32_t json_parse(JsonValue *p_value, const char *p_text, uint64_t cb_length);
ABI int32_t json_parse_terminated(JsonValue *p_value, const char *p_text);
ABI int32_t json_serialize(JsonValue *p_value, JsonString *p_out_text);
#endif
