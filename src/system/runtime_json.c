// SPDX-License-Identifier: GPL-2.0-or-later
/* libSceJson: sce::Json values, strings, containers and the library's allocator. Parsing and
 * serializing are in runtime_json_text.c. */
#define _GNU_SOURCE
#include "runtime_json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct JsonMemAllocator JsonMemAllocator;
/* The SDK's MemAllocator virtuals in declaration order, as the allocator Black Ops derives from it
 * fills them: complete and deleting destructors, allocate, deallocate, notifyError. */
typedef struct JsonAllocatorVtable {
    void(ABI *pfn_destroy_complete)(JsonMemAllocator *p_allocator);
    void(ABI *pfn_destroy_deleting)(JsonMemAllocator *p_allocator);
    void *(ABI *pfn_allocate)(JsonMemAllocator *p_allocator, uint64_t cb_size, void *p_user_data);
    void(ABI *pfn_deallocate)(JsonMemAllocator *p_allocator, void *p_block, void *p_user_data);
    void(ABI *pfn_notify_error)(JsonMemAllocator *p_allocator, int32_t error, uint64_t cb_size, void *p_user_data);
} JsonAllocatorVtable;
struct JsonMemAllocator { const JsonAllocatorVtable *p_vtable; };

typedef struct JsonInitParameter2 {
    JsonMemAllocator *p_allocator;
    void *p_user_data;
    uint64_t cb_file_buffer;
    uint32_t special_float_format;
} JsonInitParameter2;

static struct {
    JsonMemAllocator *p_allocator;
    void *p_user_data;
    uint64_t cb_file_buffer;
    uint32_t special_float_format;
    bool is_initialized;
} s_state;

/* Each block remembers the allocator that made it, so values created before initialize or kept
 * past terminate go back where they came from. */
typedef struct JsonBlock {
    JsonMemAllocator *p_allocator;
    void *p_user_data;
} JsonBlock;

void *json_allocate(uint64_t cb_size) {
    JsonMemAllocator *p_allocator = s_state.p_allocator;
    void *p_user_data = s_state.p_user_data;
    if (cb_size > UINT64_MAX / 2) return NULL;
    const uint64_t cb_block = cb_size + sizeof(JsonBlock);
    JsonBlock *p_block;
    if (p_allocator) {
        p_block = p_allocator->p_vtable->pfn_allocate(p_allocator, cb_block, p_user_data);
        if (!p_block) {
            printf("Runtime: libSceJson allocator returned no memory for %llu bytes\n",
                   (unsigned long long)cb_block);
            p_allocator->p_vtable->pfn_notify_error(p_allocator, JSON_ERROR_NO_MEMORY, cb_block, p_user_data);
            return NULL;
        }
    } else {
        static int s_is_reported;
        if (!__atomic_exchange_n(&s_is_reported, 1, __ATOMIC_RELAXED))
            printf("Runtime: libSceJson used before Initializer::initialize; values use the host heap\n");
        if (!(p_block = malloc(cb_block))) return NULL;
    }
    p_block->p_allocator = p_allocator;
    p_block->p_user_data = p_user_data;
    return p_block + 1;
}

void json_free(void *p_memory) {
    if (!p_memory) return;
    JsonBlock *p_block = (JsonBlock *)p_memory - 1;
    if (p_block->p_allocator) p_block->p_allocator->p_vtable->pfn_deallocate(p_block->p_allocator, p_block, p_block->p_user_data);
    else free(p_block);
}

/* ---- String ---- */
static const char s_sz_empty_text[1];
static const JsonString s_empty_string;
static const JsonValue s_null_value;

static const char *json_string_text(const JsonString *p_string) {
    return p_string && p_string->p_text ? p_string->p_text->text : s_sz_empty_text;
}
static uint64_t json_string_length(const JsonString *p_string) {
    return p_string && p_string->p_text ? p_string->p_text->cb_length : 0;
}

static bool json_string_reserve(JsonString *p_string, uint64_t cb_capacity) {
    if (p_string->p_text && p_string->p_text->cb_capacity >= cb_capacity) return true;
    uint64_t cb_grown = p_string->p_text ? p_string->p_text->cb_capacity * 2 : 15;
    if (cb_grown < cb_capacity) cb_grown = cb_capacity;
    JsonText *p_text = json_allocate(sizeof(JsonText) + cb_grown + 1);
    if (!p_text) return false;
    p_text->cb_length = json_string_length(p_string);
    p_text->cb_capacity = cb_grown;
    memcpy(p_text->text, json_string_text(p_string), p_text->cb_length + 1);
    json_free(p_string->p_text);
    p_string->p_text = p_text;
    return true;
}

bool json_string_append(JsonString *p_string, const char *p_text, uint64_t cb_length) {
    if (!cb_length) return true;
    const uint64_t cb_old_length = json_string_length(p_string);
    const char *p_old_text = json_string_text(p_string);
    /* Appending a string to itself: the source moves when the buffer grows. */
    const bool is_self = p_text >= p_old_text && p_text < p_old_text + cb_old_length;
    const uint64_t off_self = is_self ? (uint64_t)(p_text - p_old_text) : 0;
    if (!json_string_reserve(p_string, cb_old_length + cb_length)) return false;
    if (is_self) p_text = p_string->p_text->text + off_self;
    memmove(p_string->p_text->text + cb_old_length, p_text, cb_length);
    p_string->p_text->cb_length = cb_old_length + cb_length;
    p_string->p_text->text[p_string->p_text->cb_length] = 0;
    return true;
}

void json_string_release(JsonString *p_string) {
    json_free(p_string->p_text);
    p_string->p_text = NULL;
}

bool json_string_assign(JsonString *p_string, const char *p_text, uint64_t cb_length) {
    JsonString assigned = {0};
    if (!json_string_append(&assigned, p_text, cb_length)) return false;
    json_string_release(p_string);
    *p_string = assigned;
    return true;
}

static ABI void string_construct(JsonString *p_self) { p_self->p_text = NULL; }
static ABI void string_construct_text(JsonString *p_self, const char *p_text) {
    p_self->p_text = NULL;
    if (p_text) json_string_append(p_self, p_text, strlen(p_text));
}
static ABI void string_construct_copy(JsonString *p_self, const JsonString *p_source) {
    p_self->p_text = NULL;
    json_string_append(p_self, json_string_text(p_source), json_string_length(p_source));
}
static ABI void string_destroy(JsonString *p_self) { json_string_release(p_self); }
static ABI JsonString *string_assign(JsonString *p_self, const JsonString *p_source) {
    if (p_self != p_source) json_string_assign(p_self, json_string_text(p_source), json_string_length(p_source));
    return p_self;
}
static ABI const char *string_c_str(const JsonString *p_self) { return json_string_text(p_self); }
static ABI uint64_t string_length(const JsonString *p_self) { return json_string_length(p_self); }
static ABI bool string_empty(const JsonString *p_self) { return !json_string_length(p_self); }
static ABI uint64_t string_capacity(const JsonString *p_self) { return p_self->p_text ? p_self->p_text->cb_capacity : 0; }
static ABI void string_clear(JsonString *p_self) {
    if (p_self->p_text) {
        p_self->p_text->cb_length = 0;
        p_self->p_text->text[0] = 0;
    }
}
static ABI void string_reserve(JsonString *p_self, uint64_t cb_capacity) { json_string_reserve(p_self, cb_capacity); }
static ABI void string_resize(JsonString *p_self, uint64_t cb_length) {
    const uint64_t cb_old_length = json_string_length(p_self);
    if (cb_length > cb_old_length) {
        if (!json_string_reserve(p_self, cb_length)) return;
        memset(p_self->p_text->text + cb_old_length, 0, cb_length - cb_old_length);
    }
    if (p_self->p_text) {
        p_self->p_text->cb_length = cb_length;
        p_self->p_text->text[cb_length] = 0;
    }
}
static ABI JsonString *string_append_text(JsonString *p_self, const char *p_text) {
    if (p_text) json_string_append(p_self, p_text, strlen(p_text));
    return p_self;
}
static ABI JsonString *string_append_bytes(JsonString *p_self, const char *p_text, uint64_t cb_length) {
    if (p_text) json_string_append(p_self, p_text, cb_length);
    return p_self;
}
static ABI JsonString *string_append_string(JsonString *p_self, const JsonString *p_source) {
    json_string_append(p_self, json_string_text(p_source), json_string_length(p_source));
    return p_self;
}
static ABI JsonString *string_append_char(JsonString *p_self, uint8_t character) {
    const char p_text = (char)character;
    json_string_append(p_self, &p_text, 1);
    return p_self;
}
static int compare_bytes(const char *p_left, uint64_t cb_left, const char *p_right, uint64_t cb_right) {
    const int order = memcmp(p_left, p_right, cb_left < cb_right ? cb_left : cb_right);
    if (order) return order;
    return cb_left < cb_right ? -1 : cb_left > cb_right;
}
static ABI int32_t string_compare_text(const JsonString *p_self, const char *p_text) {
    return compare_bytes(json_string_text(p_self), json_string_length(p_self), p_text ? p_text : "", p_text ? strlen(p_text) : 0);
}
static ABI int32_t string_compare_string(const JsonString *p_self, const JsonString *p_other) {
    return compare_bytes(json_string_text(p_self), json_string_length(p_self), json_string_text(p_other), json_string_length(p_other));
}
static ABI bool string_equals_text(const JsonString *p_self, const char *p_text) { return !string_compare_text(p_self, p_text); }
static ABI bool string_equals_string(const JsonString *p_self, const JsonString *p_other) { return !string_compare_string(p_self, p_other); }

/* ---- Containers ---- */
static bool grow_pointers(void ***pp_ppp_items, uint64_t *p_capacity, uint64_t c_items) {
    if (c_items < *p_capacity) return true;
    const uint64_t c_grown = *p_capacity ? *p_capacity * 2 : 4;
    void **pp_grown = json_allocate(c_grown * sizeof(void *));
    if (!pp_grown) return false;
    if (c_items) memcpy(pp_grown, *pp_ppp_items, c_items * sizeof(void *));
    json_free(*pp_ppp_items);
    *pp_ppp_items = pp_grown;
    *p_capacity = c_grown;
    return true;
}

static bool array_insert(JsonArray *p_array, uint64_t i_position, const JsonValue *p_source) {
    if (!p_array->p_data) {
        if (!(p_array->p_data = json_allocate(sizeof(JsonArrayData)))) return false;
        memset(p_array->p_data, 0, sizeof(JsonArrayData));
    }
    JsonArrayData *p_data = p_array->p_data;
    JsonValue *p_item = json_allocate(sizeof(JsonValue));
    if (!p_item) return false;
    memset(p_item, 0, sizeof(*p_item));
    if (!json_value_copy(p_item, p_source) || !grow_pointers((void ***)&p_data->pp_items, &p_data->c_capacity, p_data->c_items)) {
        json_value_release(p_item);
        json_free(p_item);
        return false;
    }
    memmove(p_data->pp_items + i_position + 1, p_data->pp_items + i_position,
            (p_data->c_items - i_position) * sizeof(*p_data->pp_items));
    p_data->pp_items[i_position] = p_item;
    p_data->c_items++;
    return true;
}

bool json_array_append(JsonArray *p_array, const JsonValue *p_source) {
    return array_insert(p_array, p_array->p_data ? p_array->p_data->c_items : 0, p_source);
}

static void array_release(JsonArray *p_array) {
    JsonArrayData *p_data = p_array->p_data;
    if (!p_data) return;
    for (uint64_t i = 0; i < p_data->c_items; ++i) {
        json_value_release(p_data->pp_items[i]);
        json_free(p_data->pp_items[i]);
    }
    json_free(p_data->pp_items);
    json_free(p_data);
    p_array->p_data = NULL;
}

static bool array_copy(JsonArray *p_target, const JsonArray *p_source) {
    p_target->p_data = NULL;
    for (uint64_t i = 0; p_source->p_data && i < p_source->p_data->c_items; ++i)
        if (!json_array_append(p_target, p_source->p_data->pp_items[i])) {
            array_release(p_target);
            return false;
        }
    return true;
}

static void object_release(JsonObject *p_object) {
    JsonObjectData *p_data = p_object->p_data;
    if (!p_data) return;
    for (uint64_t i = 0; i < p_data->c_items; ++i) {
        json_string_release(&p_data->pp_members[i]->key);
        json_value_release(&p_data->pp_members[i]->value);
        json_free(p_data->pp_members[i]);
    }
    json_free(p_data->pp_members);
    json_free(p_data);
    p_object->p_data = NULL;
}

JsonValue *json_object_member(JsonObject *p_object, const char *p_key, uint64_t cb_key, bool should_insert) {
    JsonObjectData *p_data = p_object->p_data;
    uint64_t i_low = 0, i_high = p_data ? p_data->c_items : 0;
    while (i_low < i_high) {
        const uint64_t i_middle = i_low + (i_high - i_low) / 2;
        const JsonString *p_middle_key = &p_data->pp_members[i_middle]->key;
        const int order = compare_bytes(json_string_text(p_middle_key), json_string_length(p_middle_key), p_key, cb_key);
        if (!order) return &p_data->pp_members[i_middle]->value;
        if (order < 0) i_low = i_middle + 1;
        else i_high = i_middle;
    }
    if (!should_insert) return NULL;
    if (!p_data) {
        if (!(p_data = p_object->p_data = json_allocate(sizeof(JsonObjectData)))) return NULL;
        memset(p_data, 0, sizeof(JsonObjectData));
    }
    JsonMember *p_member = json_allocate(sizeof(JsonMember));
    if (!p_member) return NULL;
    memset(p_member, 0, sizeof(*p_member));
    if (!json_string_assign(&p_member->key, p_key, cb_key) ||
        !grow_pointers((void ***)&p_data->pp_members, &p_data->c_capacity, p_data->c_items)) {
        json_string_release(&p_member->key);
        json_free(p_member);
        return NULL;
    }
    memmove(p_data->pp_members + i_low + 1, p_data->pp_members + i_low, (p_data->c_items - i_low) * sizeof(*p_data->pp_members));
    p_data->pp_members[i_low] = p_member;
    p_data->c_items++;
    return &p_member->value;
}

static bool object_copy(JsonObject *p_target, const JsonObject *p_source) {
    p_target->p_data = NULL;
    for (uint64_t i = 0; p_source->p_data && i < p_source->p_data->c_items; ++i) {
        const JsonMember *p_member = p_source->p_data->pp_members[i];
        JsonValue *p_value = json_object_member(p_target, json_string_text(&p_member->key), json_string_length(&p_member->key), true);
        if (!p_value || !json_value_copy(p_value, &p_member->value)) {
            object_release(p_target);
            return false;
        }
    }
    return true;
}

/* ---- Value ---- */
void json_value_release(JsonValue *p_value) {
    switch (p_value->type) {
    case JSON_TYPE_STRING: json_string_release(&p_value->string); break;
    case JSON_TYPE_ARRAY: array_release(&p_value->array); break;
    case JSON_TYPE_OBJECT: object_release(&p_value->object); break;
    default: break;
    }
    memset(p_value, 0, sizeof(*p_value));
}

/* The target is null on entry and stays null when memory runs out. */
bool json_value_copy(JsonValue *p_target, const JsonValue *p_source) {
    JsonValue copy = *p_source;
    bool is_copied = true;
    switch (p_source->type) {
    case JSON_TYPE_STRING:
        copy.string.p_text = NULL;
        is_copied = json_string_append(&copy.string, json_string_text(&p_source->string), json_string_length(&p_source->string));
        break;
    case JSON_TYPE_ARRAY: is_copied = array_copy(&copy.array, &p_source->array); break;
    case JSON_TYPE_OBJECT: is_copied = object_copy(&copy.object, &p_source->object); break;
    default: break;
    }
    if (is_copied) *p_target = copy;
    return is_copied;
}

static void value_replace(JsonValue *p_self, JsonValue *p_replacement) {
    json_value_release(p_self);
    *p_self = *p_replacement;
}

static void value_init_type(JsonValue *p_self, uint32_t type) {
    memset(p_self, 0, sizeof(*p_self));
    p_self->type = type <= JSON_TYPE_OBJECT ? type : JSON_TYPE_NULL;
}

static ABI void value_construct(JsonValue *p_self) { memset(p_self, 0, sizeof(*p_self)); }
static ABI void value_construct_type(JsonValue *p_self, uint32_t type) { value_init_type(p_self, type); }
static ABI void value_construct_boolean(JsonValue *p_self, bool is_value) {
    value_init_type(p_self, JSON_TYPE_BOOLEAN);
    p_self->boolean = is_value;
}
static ABI void value_construct_integer(JsonValue *p_self, int64_t integer) {
    value_init_type(p_self, JSON_TYPE_INTEGER);
    p_self->integer = integer;
}
static ABI void value_construct_uinteger(JsonValue *p_self, uint64_t uinteger) {
    value_init_type(p_self, JSON_TYPE_UINTEGER);
    p_self->uinteger = uinteger;
}
static ABI void value_construct_real(JsonValue *p_self, double real) {
    value_init_type(p_self, JSON_TYPE_REAL);
    p_self->real = real;
}
static ABI void value_construct_text(JsonValue *p_self, const char *p_text) {
    value_init_type(p_self, JSON_TYPE_STRING);
    if (p_text) json_string_append(&p_self->string, p_text, strlen(p_text));
}
static ABI void value_construct_string(JsonValue *p_self, const JsonString *p_string) {
    value_init_type(p_self, JSON_TYPE_STRING);
    json_string_append(&p_self->string, json_string_text(p_string), json_string_length(p_string));
}
static ABI void value_construct_array(JsonValue *p_self, const JsonArray *p_array) {
    value_init_type(p_self, JSON_TYPE_ARRAY);
    array_copy(&p_self->array, p_array);
}
static ABI void value_construct_object(JsonValue *p_self, const JsonObject *p_object) {
    value_init_type(p_self, JSON_TYPE_OBJECT);
    object_copy(&p_self->object, p_object);
}
static ABI void value_construct_copy(JsonValue *p_self, const JsonValue *p_source) {
    memset(p_self, 0, sizeof(*p_self));
    json_value_copy(p_self, p_source);
}
static ABI void value_destroy(JsonValue *p_self) { json_value_release(p_self); }

/* Every setter builds the new contents before releasing the old, because the source may live inside
 * the value being replaced (v = v["data"]). */
static ABI JsonValue *value_assign(JsonValue *p_self, const JsonValue *p_source) {
    JsonValue copy = {0};
    if (p_self != p_source && json_value_copy(&copy, p_source)) value_replace(p_self, &copy);
    return p_self;
}
static ABI void value_set_value(JsonValue *p_self, const JsonValue *p_source) { value_assign(p_self, p_source); }
static ABI void value_set_type(JsonValue *p_self, uint32_t type) {
    JsonValue replacement;
    value_init_type(&replacement, type);
    value_replace(p_self, &replacement);
}
static ABI void value_set_boolean(JsonValue *p_self, bool is_value) {
    JsonValue replacement;
    value_construct_boolean(&replacement, is_value);
    value_replace(p_self, &replacement);
}
static ABI void value_set_integer(JsonValue *p_self, int64_t integer) {
    JsonValue replacement;
    value_construct_integer(&replacement, integer);
    value_replace(p_self, &replacement);
}
static ABI void value_set_uinteger(JsonValue *p_self, uint64_t uinteger) {
    JsonValue replacement;
    value_construct_uinteger(&replacement, uinteger);
    value_replace(p_self, &replacement);
}
static ABI void value_set_real(JsonValue *p_self, double real) {
    JsonValue replacement;
    value_construct_real(&replacement, real);
    value_replace(p_self, &replacement);
}
static ABI void value_set_text(JsonValue *p_self, const char *p_text) {
    JsonValue replacement;
    value_construct_text(&replacement, p_text);
    value_replace(p_self, &replacement);
}
static ABI void value_set_string(JsonValue *p_self, const JsonString *p_string) {
    JsonValue replacement;
    value_construct_string(&replacement, p_string);
    value_replace(p_self, &replacement);
}
static ABI void value_set_array(JsonValue *p_self, const JsonArray *p_array) {
    JsonValue replacement;
    value_construct_array(&replacement, p_array);
    value_replace(p_self, &replacement);
}
static ABI void value_set_object(JsonValue *p_self, const JsonObject *p_object) {
    JsonValue replacement;
    value_construct_object(&replacement, p_object);
    value_replace(p_self, &replacement);
}
static ABI void value_clear(JsonValue *p_self) { json_value_release(p_self); }
static ABI void value_swap(JsonValue *p_self, JsonValue *p_other) {
    const JsonValue kept = *p_self;
    *p_self = *p_other;
    *p_other = kept;
}

static ABI uint32_t value_get_type(const JsonValue *p_self) { return p_self->type; }
static ABI bool value_get_boolean(const JsonValue *p_self) { return p_self->type == JSON_TYPE_BOOLEAN && p_self->boolean; }
static ABI int64_t value_get_integer(const JsonValue *p_self) {
    switch (p_self->type) {
    case JSON_TYPE_INTEGER: return p_self->integer;
    case JSON_TYPE_UINTEGER: return (int64_t)p_self->uinteger;
    case JSON_TYPE_REAL: return (int64_t)p_self->real;
    default: return 0;
    }
}
static ABI uint64_t value_get_uinteger(const JsonValue *p_self) {
    switch (p_self->type) {
    case JSON_TYPE_INTEGER: return (uint64_t)p_self->integer;
    case JSON_TYPE_UINTEGER: return p_self->uinteger;
    case JSON_TYPE_REAL: return p_self->real > 0 ? (uint64_t)p_self->real : 0;
    default: return 0;
    }
}
static ABI double value_get_real(const JsonValue *p_self) {
    switch (p_self->type) {
    case JSON_TYPE_INTEGER: return (double)p_self->integer;
    case JSON_TYPE_UINTEGER: return (double)p_self->uinteger;
    case JSON_TYPE_REAL: return p_self->real;
    default: return 0.0;
    }
}
static ABI const JsonString *value_get_string(const JsonValue *p_self) {
    return p_self->type == JSON_TYPE_STRING ? &p_self->string : &s_empty_string;
}
static const JsonArray s_empty_array;
static const JsonObject s_empty_object;
static ABI const JsonArray *value_get_array(const JsonValue *p_self) {
    return p_self->type == JSON_TYPE_ARRAY ? &p_self->array : &s_empty_array;
}
static ABI const JsonObject *value_get_object(const JsonValue *p_self) {
    return p_self->type == JSON_TYPE_OBJECT ? &p_self->object : &s_empty_object;
}
static ABI uint64_t value_count(const JsonValue *p_self) {
    if (p_self->type == JSON_TYPE_ARRAY) return p_self->array.p_data ? p_self->array.p_data->c_items : 0;
    if (p_self->type == JSON_TYPE_OBJECT) return p_self->object.p_data ? p_self->object.p_data->c_items : 0;
    return 0;
}
static ABI bool value_is_set(const JsonValue *p_self) { return p_self->type != JSON_TYPE_NULL; }

static const JsonValue *value_member(const JsonValue *p_self, const char *p_key, uint64_t cb_key) {
    if (p_self->type != JSON_TYPE_OBJECT) return &s_null_value;
    const JsonValue *p_member = json_object_member((JsonObject *)&p_self->object, p_key, cb_key, false);
    return p_member ? p_member : &s_null_value;
}
static ABI const JsonValue *value_index_text(const JsonValue *p_self, const char *p_key) {
    return p_key ? value_member(p_self, p_key, strlen(p_key)) : &s_null_value;
}
static ABI const JsonValue *value_index_string(const JsonValue *p_self, const JsonString *p_key) {
    return value_member(p_self, json_string_text(p_key), json_string_length(p_key));
}
static ABI const JsonValue *value_index_position(const JsonValue *p_self, uint64_t i_position) {
    if (p_self->type != JSON_TYPE_ARRAY || !p_self->array.p_data || i_position >= p_self->array.p_data->c_items) return &s_null_value;
    return p_self->array.p_data->pp_items[i_position];
}

static ABI JsonObject *value_refer_object(JsonValue *p_self) { return p_self->type == JSON_TYPE_OBJECT ? &p_self->object : NULL; }
static ABI JsonArray *value_refer_array(JsonValue *p_self) { return p_self->type == JSON_TYPE_ARRAY ? &p_self->array : NULL; }
static ABI JsonString *value_refer_string(JsonValue *p_self) { return p_self->type == JSON_TYPE_STRING ? &p_self->string : NULL; }
static ABI bool *value_refer_boolean(JsonValue *p_self) { return p_self->type == JSON_TYPE_BOOLEAN ? &p_self->boolean : NULL; }
static ABI int64_t *value_refer_integer(JsonValue *p_self) { return p_self->type == JSON_TYPE_INTEGER ? &p_self->integer : NULL; }
static ABI uint64_t *value_refer_uinteger(JsonValue *p_self) { return p_self->type == JSON_TYPE_UINTEGER ? &p_self->uinteger : NULL; }
static ABI double *value_refer_real(JsonValue *p_self) { return p_self->type == JSON_TYPE_REAL ? &p_self->real : NULL; }

/* ---- Array ---- */
static ABI void array_construct(JsonArray *p_self) { p_self->p_data = NULL; }
static ABI void array_construct_copy(JsonArray *p_self, const JsonArray *p_source) { array_copy(p_self, p_source); }
static ABI void array_destroy(JsonArray *p_self) { array_release(p_self); }
static ABI JsonArray *array_assign(JsonArray *p_self, const JsonArray *p_source) {
    JsonArray copy;
    if (p_self != p_source && array_copy(&copy, p_source)) {
        array_release(p_self);
        *p_self = copy;
    }
    return p_self;
}
static ABI void array_push_back(JsonArray *p_self, const JsonValue *p_value) { json_array_append(p_self, p_value); }
static ABI void array_push_front(JsonArray *p_self, const JsonValue *p_value) { array_insert(p_self, 0, p_value); }
static void array_remove(JsonArray *p_self, uint64_t i_position) {
    JsonArrayData *p_data = p_self->p_data;
    if (!p_data || i_position >= p_data->c_items) return;
    json_value_release(p_data->pp_items[i_position]);
    json_free(p_data->pp_items[i_position]);
    memmove(p_data->pp_items + i_position, p_data->pp_items + i_position + 1,
            (p_data->c_items - i_position - 1) * sizeof(*p_data->pp_items));
    p_data->c_items--;
}
static ABI void array_pop_back(JsonArray *p_self) { if (p_self->p_data && p_self->p_data->c_items) array_remove(p_self, p_self->p_data->c_items - 1); }
static ABI void array_pop_front(JsonArray *p_self) { array_remove(p_self, 0); }
static ABI void array_clear(JsonArray *p_self) { array_release(p_self); }
static ABI uint64_t array_size(const JsonArray *p_self) { return p_self->p_data ? p_self->p_data->c_items : 0; }
static ABI bool array_empty(const JsonArray *p_self) { return !array_size(p_self); }
static ABI const JsonValue *array_back(const JsonArray *p_self) {
    return array_size(p_self) ? p_self->p_data->pp_items[p_self->p_data->c_items - 1] : &s_null_value;
}
static ABI const JsonValue *array_front(const JsonArray *p_self) { return array_size(p_self) ? p_self->p_data->pp_items[0] : &s_null_value; }

/* ---- Object ---- */
static ABI void object_construct(JsonObject *p_self) { p_self->p_data = NULL; }
static ABI void object_construct_copy(JsonObject *p_self, const JsonObject *p_source) { object_copy(p_self, p_source); }
static ABI void object_destroy(JsonObject *p_self) { object_release(p_self); }
static ABI JsonObject *object_assign(JsonObject *p_self, const JsonObject *p_source) {
    JsonObject copy;
    if (p_self != p_source && object_copy(&copy, p_source)) {
        object_release(p_self);
        *p_self = copy;
    }
    return p_self;
}
/* operator[] inserts a null member for a missing key, as std::map does; with no memory left the
 * caller gets a scratch value nothing keeps, since a reference cannot be null. */
static ABI JsonValue *object_index(JsonObject *p_self, const JsonString *p_key) {
    JsonValue *p_member = json_object_member(p_self, json_string_text(p_key), json_string_length(p_key), true);
    if (p_member) return p_member;
    static __thread JsonValue s_scratch_value;
    json_value_release(&s_scratch_value);
    return &s_scratch_value;
}
static ABI void object_clear(JsonObject *p_self) { object_release(p_self); }
static ABI uint64_t object_size(const JsonObject *p_self) { return p_self->p_data ? p_self->p_data->c_items : 0; }
static ABI bool object_empty(const JsonObject *p_self) { return !object_size(p_self); }

/* ---- MemAllocator, InitParameter2, Initializer ---- */
static __attribute__((noreturn)) void stop_pure_virtual(void) {
    fprintf(stderr, "STOP: pure virtual sce::Json::MemAllocator function called\n");
    runtime_stop(21);
}
static ABI void allocator_destroy_deleting(JsonMemAllocator *p_allocator) {
    (void)p_allocator;
    stop_pure_virtual();
}
static ABI void *allocator_allocate(JsonMemAllocator *p_allocator, uint64_t cb_size, void *p_user_data) {
    (void)p_allocator; (void)cb_size; (void)p_user_data;
    stop_pure_virtual();
}
static ABI void allocator_deallocate(JsonMemAllocator *p_allocator, void *p_block, void *p_user_data) {
    (void)p_allocator; (void)p_block; (void)p_user_data;
    stop_pure_virtual();
}
static ABI void allocator_notify_error(JsonMemAllocator *p_allocator, int32_t error, uint64_t cb_size, void *p_user_data) {
    (void)p_allocator; (void)p_user_data;
    printf("Runtime: libSceJson MemAllocator::notifyError 0x%08X (%llu bytes)\n", (uint32_t)error, (unsigned long long)cb_size);
}
static ABI void allocator_destroy(JsonMemAllocator *p_self);
/* Itanium layout: offset to top and type info precede the slots the object points at. */
static const struct {
    intptr_t off_to_top;
    const void *p_type_info;
    JsonAllocatorVtable slots;
} s_allocator_vtable = {0, NULL, {allocator_destroy, allocator_destroy_deleting, allocator_allocate,
                               allocator_deallocate, allocator_notify_error}};
static ABI void allocator_construct(JsonMemAllocator *p_self) { p_self->p_vtable = &s_allocator_vtable.slots; }
static ABI void allocator_destroy(JsonMemAllocator *p_self) { p_self->p_vtable = &s_allocator_vtable.slots; }

static ABI void init_parameter_construct(JsonInitParameter2 *p_self) { memset(p_self, 0, sizeof(*p_self)); }
static ABI void init_parameter_set_allocator(JsonInitParameter2 *p_self, JsonMemAllocator *p_allocator, void *p_user_data) {
    p_self->p_allocator = p_allocator;
    p_self->p_user_data = p_user_data;
}
static ABI void init_parameter_set_file_buffer_size(JsonInitParameter2 *p_self, uint64_t cb_size) {
    p_self->cb_file_buffer = cb_size;
}
static ABI void init_parameter_set_special_float_format(JsonInitParameter2 *p_self, uint32_t format) {
    p_self->special_float_format = format;
}
static ABI JsonMemAllocator *init_parameter_get_allocator(const JsonInitParameter2 *p_self) { return p_self->p_allocator; }
static ABI void *init_parameter_get_user_data(const JsonInitParameter2 *p_self) { return p_self->p_user_data; }
static ABI uint64_t init_parameter_get_file_buffer_size(const JsonInitParameter2 *p_self) { return p_self->cb_file_buffer; }
static ABI uint32_t init_parameter_get_special_float_format(const JsonInitParameter2 *p_self) { return p_self->special_float_format; }

/* An Initializer is at most 4 bytes in the callers' objects: its first byte says whether this one
 * initialized the library. */
static ABI void initializer_construct(uint8_t *p_self) { *p_self = 0; }
static ABI int32_t initializer_terminate(uint8_t *p_self) {
    if (!*p_self || !s_state.is_initialized) return JSON_ERROR_NOT_INITIALIZED;
    memset(&s_state, 0, sizeof(s_state));
    *p_self = 0;
    printf("Runtime: libSceJson terminated\n");
    return 0;
}
static ABI void initializer_destroy(uint8_t *p_self) {
    if (*p_self) initializer_terminate(p_self);
}
static ABI int32_t initializer_initialize(uint8_t *p_self, const JsonInitParameter2 *p_parameter) {
    if (!p_parameter || !p_parameter->p_allocator) return JSON_ERROR_INVALID_ARGUMENT;
    if (s_state.is_initialized) return JSON_ERROR_ALREADY_INITIALIZED;
    s_state.p_allocator = p_parameter->p_allocator;
    s_state.p_user_data = p_parameter->p_user_data;
    s_state.cb_file_buffer = p_parameter->cb_file_buffer;
    s_state.special_float_format = p_parameter->special_float_format;
    s_state.is_initialized = true;
    *p_self = 1;
    printf("Runtime: libSceJson initialized with the game's allocator (file buffer %llu bytes)\n",
           (unsigned long long)p_parameter->cb_file_buffer);
    return 0;
}
/* InitParameter is the older form: allocator, user data and file buffer size, the same first fields. */
static ABI int32_t initializer_initialize_v1(uint8_t *p_self, const JsonInitParameter2 *p_parameter) {
    if (!p_parameter) return JSON_ERROR_INVALID_ARGUMENT;
    JsonInitParameter2 converted = *p_parameter;
    converted.special_float_format = 0;
    return initializer_initialize(p_self, &converted);
}

static const RuntimeExport s_exports[] = {
    {"_ZN3sce4Json12MemAllocatorC2Ev", allocator_construct},
    {"_ZN3sce4Json12MemAllocatorD1Ev", allocator_destroy}, {"_ZN3sce4Json12MemAllocatorD2Ev", allocator_destroy},
    {"_ZN3sce4Json12MemAllocator11notifyErrorEimPv", allocator_notify_error},
    {"_ZN3sce4Json14InitParameter2C1Ev", init_parameter_construct}, {"_ZN3sce4Json14InitParameter2C2Ev", init_parameter_construct},
    {"_ZN3sce4Json14InitParameter212setAllocatorEPNS0_12MemAllocatorEPv", init_parameter_set_allocator},
    {"_ZN3sce4Json14InitParameter217setFileBufferSizeEm", init_parameter_set_file_buffer_size},
    {"_ZN3sce4Json14InitParameter225setSpecialFloatFormatTypeENS0_22SpecialFloatFormatTypeE", init_parameter_set_special_float_format},
    {"_ZNK3sce4Json14InitParameter212getAllocatorEv", init_parameter_get_allocator},
    {"_ZNK3sce4Json14InitParameter211getUserDataEv", init_parameter_get_user_data},
    {"_ZNK3sce4Json14InitParameter217getFileBufferSizeEv", init_parameter_get_file_buffer_size},
    {"_ZNK3sce4Json14InitParameter225getSpecialFloatFormatTypeEv", init_parameter_get_special_float_format},
    {"_ZN3sce4Json11InitializerC1Ev", initializer_construct}, {"_ZN3sce4Json11InitializerC2Ev", initializer_construct},
    {"_ZN3sce4Json11InitializerD1Ev", initializer_destroy}, {"_ZN3sce4Json11InitializerD2Ev", initializer_destroy},
    {"_ZN3sce4Json11Initializer10initializeEPKNS0_14InitParameter2E", initializer_initialize},
    {"_ZN3sce4Json11Initializer10initializeEPKNS0_13InitParameterE", initializer_initialize_v1},
    {"_ZN3sce4Json11Initializer9terminateEv", initializer_terminate},
    {"_ZN3sce4Json5ValueC1Ev", value_construct}, {"_ZN3sce4Json5ValueC2Ev", value_construct},
    {"_ZN3sce4Json5ValueC1ENS0_9ValueTypeE", value_construct_type}, {"_ZN3sce4Json5ValueC2ENS0_9ValueTypeE", value_construct_type},
    {"_ZN3sce4Json5ValueC1Eb", value_construct_boolean}, {"_ZN3sce4Json5ValueC2Eb", value_construct_boolean},
    {"_ZN3sce4Json5ValueC1El", value_construct_integer}, {"_ZN3sce4Json5ValueC2El", value_construct_integer},
    {"_ZN3sce4Json5ValueC1Em", value_construct_uinteger}, {"_ZN3sce4Json5ValueC2Em", value_construct_uinteger},
    {"_ZN3sce4Json5ValueC1Ed", value_construct_real}, {"_ZN3sce4Json5ValueC2Ed", value_construct_real},
    {"_ZN3sce4Json5ValueC1EPKc", value_construct_text}, {"_ZN3sce4Json5ValueC2EPKc", value_construct_text},
    {"_ZN3sce4Json5ValueC1ERKNS0_6StringE", value_construct_string}, {"_ZN3sce4Json5ValueC2ERKNS0_6StringE", value_construct_string},
    {"_ZN3sce4Json5ValueC1ERKNS0_5ArrayE", value_construct_array}, {"_ZN3sce4Json5ValueC2ERKNS0_5ArrayE", value_construct_array},
    {"_ZN3sce4Json5ValueC1ERKNS0_6ObjectE", value_construct_object}, {"_ZN3sce4Json5ValueC2ERKNS0_6ObjectE", value_construct_object},
    {"_ZN3sce4Json5ValueC1ERKS1_", value_construct_copy}, {"_ZN3sce4Json5ValueC2ERKS1_", value_construct_copy},
    {"_ZN3sce4Json5ValueD1Ev", value_destroy}, {"_ZN3sce4Json5ValueD2Ev", value_destroy},
    {"_ZN3sce4Json5ValueaSERKS1_", value_assign},
    {"_ZN3sce4Json5Value3setENS0_9ValueTypeE", value_set_type}, {"_ZN3sce4Json5Value3setEb", value_set_boolean},
    {"_ZN3sce4Json5Value3setEl", value_set_integer}, {"_ZN3sce4Json5Value3setEm", value_set_uinteger},
    {"_ZN3sce4Json5Value3setEd", value_set_real}, {"_ZN3sce4Json5Value3setEPKc", value_set_text},
    {"_ZN3sce4Json5Value3setERKNS0_6StringE", value_set_string}, {"_ZN3sce4Json5Value3setERKNS0_5ArrayE", value_set_array},
    {"_ZN3sce4Json5Value3setERKNS0_6ObjectE", value_set_object}, {"_ZN3sce4Json5Value3setERKS1_", value_set_value},
    {"_ZN3sce4Json5Value5clearEv", value_clear}, {"_ZN3sce4Json5Value4swapERS1_", value_swap},
    {"_ZNK3sce4Json5Value7getTypeEv", value_get_type}, {"_ZNK3sce4Json5Value10getBooleanEv", value_get_boolean},
    {"_ZNK3sce4Json5Value10getIntegerEv", value_get_integer}, {"_ZNK3sce4Json5Value11getUIntegerEv", value_get_uinteger},
    {"_ZNK3sce4Json5Value7getRealEv", value_get_real}, {"_ZNK3sce4Json5Value9getStringEv", value_get_string},
    {"_ZNK3sce4Json5Value8getArrayEv", value_get_array}, {"_ZNK3sce4Json5Value9getObjectEv", value_get_object},
    {"_ZNK3sce4Json5Value5countEv", value_count}, {"_ZNK3sce4Json5ValuecvbEv", value_is_set},
    {"_ZNK3sce4Json5ValueixEPKc", value_index_text}, {"_ZNK3sce4Json5ValueixERKNS0_6StringE", value_index_string},
    {"_ZNK3sce4Json5ValueixEm", value_index_position},
    {"_ZNK3sce4Json5Value8getValueERKNS0_6StringE", value_index_string}, {"_ZNK3sce4Json5Value8getValueEm", value_index_position},
    {"_ZN3sce4Json5Value11referObjectEv", value_refer_object}, {"_ZN3sce4Json5Value10referArrayEv", value_refer_array},
    {"_ZN3sce4Json5Value11referStringEv", value_refer_string}, {"_ZN3sce4Json5Value12referBooleanEv", value_refer_boolean},
    {"_ZN3sce4Json5Value12referIntegerEv", value_refer_integer}, {"_ZN3sce4Json5Value13referUIntegerEv", value_refer_uinteger},
    {"_ZN3sce4Json5Value9referRealEv", value_refer_real},
    {"_ZN3sce4Json5Value9serializeERNS0_6StringE", json_serialize},
    {"_ZN3sce4Json6Parser5parseERNS0_5ValueEPKcm", json_parse}, {"_ZN3sce4Json6Parser5parseERNS0_5ValueEPKc", json_parse_terminated},
    {"_ZN3sce4Json6StringC1Ev", string_construct}, {"_ZN3sce4Json6StringC2Ev", string_construct},
    {"_ZN3sce4Json6StringC1EPKc", string_construct_text}, {"_ZN3sce4Json6StringC2EPKc", string_construct_text},
    {"_ZN3sce4Json6StringC1ERKS1_", string_construct_copy}, {"_ZN3sce4Json6StringC2ERKS1_", string_construct_copy},
    {"_ZN3sce4Json6StringD1Ev", string_destroy}, {"_ZN3sce4Json6StringD2Ev", string_destroy},
    {"_ZN3sce4Json6StringaSERKS1_", string_assign},
    {"_ZNK3sce4Json6String5c_strEv", string_c_str}, {"_ZNK3sce4Json6String6lengthEv", string_length},
    {"_ZNK3sce4Json6String4sizeEv", string_length}, {"_ZNK3sce4Json6String5emptyEv", string_empty},
    {"_ZNK3sce4Json6String8capacityEv", string_capacity}, {"_ZN3sce4Json6String5clearEv", string_clear},
    {"_ZN3sce4Json6String7reserveEm", string_reserve}, {"_ZN3sce4Json6String6resizeEm", string_resize},
    {"_ZN3sce4Json6String6appendEPKc", string_append_text}, {"_ZN3sce4Json6String6appendEPKcm", string_append_bytes},
    {"_ZN3sce4Json6String6appendERKS1_", string_append_string}, {"_ZN3sce4Json6StringpLEPKc", string_append_text},
    {"_ZN3sce4Json6StringpLEh", string_append_char},
    {"_ZNK3sce4Json6String7compareEPKc", string_compare_text}, {"_ZNK3sce4Json6String7compareERKS1_", string_compare_string},
    {"_ZNK3sce4Json6StringeqEPKc", string_equals_text}, {"_ZNK3sce4Json6StringeqERKS1_", string_equals_string},
    {"_ZN3sce4Json5ArrayC1Ev", array_construct}, {"_ZN3sce4Json5ArrayC2Ev", array_construct},
    {"_ZN3sce4Json5ArrayC1ERKS1_", array_construct_copy}, {"_ZN3sce4Json5ArrayC2ERKS1_", array_construct_copy},
    {"_ZN3sce4Json5ArrayD1Ev", array_destroy}, {"_ZN3sce4Json5ArrayD2Ev", array_destroy},
    {"_ZN3sce4Json5ArrayaSERKS1_", array_assign},
    {"_ZN3sce4Json5Array9push_backERKNS0_5ValueE", array_push_back},
    {"_ZN3sce4Json5Array10push_frontERKNS0_5ValueE", array_push_front},
    {"_ZN3sce4Json5Array8pop_backEv", array_pop_back}, {"_ZN3sce4Json5Array9pop_frontEv", array_pop_front},
    {"_ZN3sce4Json5Array5clearEv", array_clear},
    {"_ZNK3sce4Json5Array4sizeEv", array_size}, {"_ZNK3sce4Json5Array5emptyEv", array_empty},
    {"_ZNK3sce4Json5Array4backEv", array_back}, {"_ZNK3sce4Json5Array5frontEv", array_front},
    {"_ZN3sce4Json6ObjectC1Ev", object_construct}, {"_ZN3sce4Json6ObjectC2Ev", object_construct},
    {"_ZN3sce4Json6ObjectC1ERKS1_", object_construct_copy}, {"_ZN3sce4Json6ObjectC2ERKS1_", object_construct_copy},
    {"_ZN3sce4Json6ObjectD1Ev", object_destroy}, {"_ZN3sce4Json6ObjectD2Ev", object_destroy},
    {"_ZN3sce4Json6ObjectaSERKS1_", object_assign}, {"_ZN3sce4Json6ObjectixERKNS0_6StringE", object_index},
    {"_ZN3sce4Json6Object5clearEv", object_clear},
    {"_ZNK3sce4Json6Object4sizeEv", object_size}, {"_ZNK3sce4Json6Object5emptyEv", object_empty},
};
uintptr_t runtime_json_resolve(const char *sz_name) { return RUNTIME_LOOKUP(s_exports, sz_name); }
