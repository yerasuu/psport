/* libSceJson's text side: sce::Json::Parser::parse (RFC 8259) and Value::serialize. */
#define _GNU_SOURCE
#include "runtime_json.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Parsing recurses on the game thread's own stack, which may be small; PSN replies nest a few
 * levels, so a document nested deeper than this is refused rather than risking the stack. */
#define JSON_MAX_DEPTH 128

typedef struct JsonParser {
    const char *p_cursor, *p_start, *p_end;
    uint32_t depth;
    int32_t error;
    const char *sz_reason;
} JsonParser;

static bool parse_value(JsonParser *p_parser, JsonValue *p_out_value);

static bool parse_fail(JsonParser *p_parser, const char *sz_reason) {
    if (!p_parser->error) {
        p_parser->error = JSON_ERROR_PARSE;
        p_parser->sz_reason = sz_reason;
    }
    return false;
}
static bool parse_no_memory(JsonParser *p_parser) {
    p_parser->error = JSON_ERROR_NO_MEMORY;
    p_parser->sz_reason = "out of memory";
    return false;
}

static void skip_space(JsonParser *p_parser) {
    while (p_parser->p_cursor < p_parser->p_end &&
           (*p_parser->p_cursor == ' ' || *p_parser->p_cursor == '\t' || *p_parser->p_cursor == '\n' || *p_parser->p_cursor == '\r'))
        p_parser->p_cursor++;
}

static bool consume_word(JsonParser *p_parser, const char *sz_word) {
    const size_t cb_length = strlen(sz_word);
    if ((size_t)(p_parser->p_end - p_parser->p_cursor) < cb_length || memcmp(p_parser->p_cursor, sz_word, cb_length)) return false;
    p_parser->p_cursor += cb_length;
    return true;
}

static int parse_hex_digit(char digit) {
    if (digit >= '0' && digit <= '9') return digit - '0';
    if (digit >= 'a' && digit <= 'f') return digit - 'a' + 10;
    if (digit >= 'A' && digit <= 'F') return digit - 'A' + 10;
    return -1;
}

static bool parse_quad_hex(JsonParser *p_parser, uint32_t *p_out_code_unit) {
    if (p_parser->p_end - p_parser->p_cursor < 4) return parse_fail(p_parser, "truncated \\u escape");
    uint32_t code_unit = 0;
    for (int i = 0; i < 4; ++i) {
        const int digit = parse_hex_digit(p_parser->p_cursor[i]);
        if (digit < 0) return parse_fail(p_parser, "bad \\u escape");
        code_unit = code_unit << 4 | (uint32_t)digit;
    }
    p_parser->p_cursor += 4;
    *p_out_code_unit = code_unit;
    return true;
}

static bool append_code_point(JsonString *p_string, uint32_t code_point) {
    char encoded[4];
    uint64_t cb_length;
    if (code_point < 0x80) {
        encoded[0] = (char)code_point;
        cb_length = 1;
    } else if (code_point < 0x800) {
        encoded[0] = (char)(0xC0 | code_point >> 6);
        encoded[1] = (char)(0x80 | (code_point & 63));
        cb_length = 2;
    } else if (code_point < 0x10000) {
        encoded[0] = (char)(0xE0 | code_point >> 12);
        encoded[1] = (char)(0x80 | ((code_point >> 6) & 63));
        encoded[2] = (char)(0x80 | (code_point & 63));
        cb_length = 3;
    } else {
        encoded[0] = (char)(0xF0 | code_point >> 18);
        encoded[1] = (char)(0x80 | ((code_point >> 12) & 63));
        encoded[2] = (char)(0x80 | ((code_point >> 6) & 63));
        encoded[3] = (char)(0x80 | (code_point & 63));
        cb_length = 4;
    }
    return json_string_append(p_string, encoded, cb_length);
}

/* The cursor is on the opening quote. */
static bool parse_string(JsonParser *p_parser, JsonString *p_out_string) {
    p_parser->p_cursor++;
    for (;;) {
        const char *p_run_start = p_parser->p_cursor;
        while (p_parser->p_cursor < p_parser->p_end && *p_parser->p_cursor != '"' && *p_parser->p_cursor != '\\' &&
               (unsigned char)*p_parser->p_cursor >= 0x20)
            p_parser->p_cursor++;
        if (!json_string_append(p_out_string, p_run_start, (uint64_t)(p_parser->p_cursor - p_run_start))) return parse_no_memory(p_parser);
        if (p_parser->p_cursor >= p_parser->p_end) return parse_fail(p_parser, "unterminated string");
        const char character = *p_parser->p_cursor++;
        if (character == '"') {
            if (!p_out_string->p_text && !json_string_assign(p_out_string, "", 0)) return parse_no_memory(p_parser);
            return true;
        }
        if (character != '\\') return parse_fail(p_parser, "control character in string");
        if (p_parser->p_cursor >= p_parser->p_end) return parse_fail(p_parser, "unterminated escape");
        const char escape = *p_parser->p_cursor++;
        static const char s_escapes[] = "\"\\/bfnrt", s_decoded[] = "\"\\/\b\f\n\r\t";
        const char *p_known = escape ? strchr(s_escapes, escape) : NULL;
        if (p_known) {
            if (!json_string_append(p_out_string, &s_decoded[p_known - s_escapes], 1)) return parse_no_memory(p_parser);
            continue;
        }
        if (escape != 'u') return parse_fail(p_parser, "unknown escape");
        uint32_t code_point;
        if (!parse_quad_hex(p_parser, &code_point)) return false;
        if (code_point >= 0xD800 && code_point < 0xDC00) {
            uint32_t low_surrogate;
            if (!consume_word(p_parser, "\\u") || !parse_quad_hex(p_parser, &low_surrogate) || low_surrogate < 0xDC00 ||
                low_surrogate >= 0xE000)
                return parse_fail(p_parser, "unpaired surrogate");
            code_point = 0x10000 + ((code_point - 0xD800) << 10) + (low_surrogate - 0xDC00);
        } else if (code_point >= 0xDC00 && code_point < 0xE000) {
            return parse_fail(p_parser, "unpaired surrogate");
        }
        if (!append_code_point(p_out_string, code_point)) return parse_no_memory(p_parser);
    }
}

static bool is_digit(const JsonParser *p_parser) {
    return p_parser->p_cursor < p_parser->p_end && *p_parser->p_cursor >= '0' && *p_parser->p_cursor <= '9';
}

/* Integers keep their type: int64 when they fit, uint64 for larger positive ones, real beyond. */
static bool parse_number(JsonParser *p_parser, JsonValue *p_out_value) {
    const char *p_number_start = p_parser->p_cursor;
    const bool is_negative = p_parser->p_cursor < p_parser->p_end && *p_parser->p_cursor == '-';
    if (is_negative) p_parser->p_cursor++;
    if (!is_digit(p_parser)) return parse_fail(p_parser, "bad number");
    if (*p_parser->p_cursor == '0') p_parser->p_cursor++;
    else while (is_digit(p_parser)) p_parser->p_cursor++;
    bool is_integral = true;
    if (p_parser->p_cursor < p_parser->p_end && *p_parser->p_cursor == '.') {
        p_parser->p_cursor++;
        if (!is_digit(p_parser)) return parse_fail(p_parser, "bad fraction");
        while (is_digit(p_parser)) p_parser->p_cursor++;
        is_integral = false;
    }
    if (p_parser->p_cursor < p_parser->p_end && (*p_parser->p_cursor == 'e' || *p_parser->p_cursor == 'E')) {
        p_parser->p_cursor++;
        if (p_parser->p_cursor < p_parser->p_end && (*p_parser->p_cursor == '+' || *p_parser->p_cursor == '-')) p_parser->p_cursor++;
        if (!is_digit(p_parser)) return parse_fail(p_parser, "bad exponent");
        while (is_digit(p_parser)) p_parser->p_cursor++;
        is_integral = false;
    }
    if (is_integral) {
        uint64_t magnitude = 0;
        bool is_overflow = false;
        for (const char *p_digit = p_number_start + is_negative; p_digit < p_parser->p_cursor; ++p_digit) {
            const uint64_t digit_value = (uint64_t)(*p_digit - '0');
            if (magnitude > (UINT64_MAX - digit_value) / 10) is_overflow = true;
            magnitude = magnitude * 10 + digit_value;
        }
        if (!is_overflow && !is_negative && magnitude <= INT64_MAX) {
            p_out_value->type = JSON_TYPE_INTEGER;
            p_out_value->integer = (int64_t)magnitude;
            return true;
        }
        if (!is_overflow && !is_negative) {
            p_out_value->type = JSON_TYPE_UINTEGER;
            p_out_value->uinteger = magnitude;
            return true;
        }
        if (!is_overflow && magnitude <= (uint64_t)INT64_MAX + 1) {
            p_out_value->type = JSON_TYPE_INTEGER;
            p_out_value->integer = magnitude == (uint64_t)INT64_MAX + 1 ? INT64_MIN : -(int64_t)magnitude;
            return true;
        }
    }
    char digits[128];
    const size_t cb_length = (size_t)(p_parser->p_cursor - p_number_start);
    if (cb_length >= sizeof(digits)) return parse_fail(p_parser, "number too long");
    memcpy(digits, p_number_start, cb_length);
    digits[cb_length] = 0;
    p_out_value->type = JSON_TYPE_REAL;
    p_out_value->real = strtod(digits, NULL);
    return true;
}

static bool parse_array(JsonParser *p_parser, JsonValue *p_out_value) {
    p_parser->p_cursor++;
    p_out_value->type = JSON_TYPE_ARRAY;
    skip_space(p_parser);
    if (p_parser->p_cursor < p_parser->p_end && *p_parser->p_cursor == ']') {
        p_parser->p_cursor++;
        return true;
    }
    for (;;) {
        JsonValue element = {0};
        if (!parse_value(p_parser, &element)) {
            json_value_release(&element);
            return false;
        }
        const bool is_appended = json_array_append(&p_out_value->array, &element);
        json_value_release(&element);
        if (!is_appended) return parse_no_memory(p_parser);
        skip_space(p_parser);
        if (p_parser->p_cursor >= p_parser->p_end) return parse_fail(p_parser, "unterminated array");
        const char separator = *p_parser->p_cursor++;
        if (separator == ']') return true;
        if (separator != ',') return parse_fail(p_parser, "expected ',' or ']'");
    }
}

/* A key given twice keeps its last value. */
static bool parse_object(JsonParser *p_parser, JsonValue *p_out_value) {
    p_parser->p_cursor++;
    p_out_value->type = JSON_TYPE_OBJECT;
    skip_space(p_parser);
    if (p_parser->p_cursor < p_parser->p_end && *p_parser->p_cursor == '}') {
        p_parser->p_cursor++;
        return true;
    }
    for (;;) {
        skip_space(p_parser);
        if (p_parser->p_cursor >= p_parser->p_end || *p_parser->p_cursor != '"') return parse_fail(p_parser, "expected a key");
        JsonString p_key = {0};
        if (!parse_string(p_parser, &p_key)) {
            json_string_release(&p_key);
            return false;
        }
        skip_space(p_parser);
        if (p_parser->p_cursor >= p_parser->p_end || *p_parser->p_cursor != ':') {
            json_string_release(&p_key);
            return parse_fail(p_parser, "expected ':'");
        }
        p_parser->p_cursor++;
        JsonValue *p_member = json_object_member(&p_out_value->object, p_key.p_text->text, p_key.p_text->cb_length, true);
        json_string_release(&p_key);
        if (!p_member) return parse_no_memory(p_parser);
        json_value_release(p_member);
        if (!parse_value(p_parser, p_member)) return false;
        skip_space(p_parser);
        if (p_parser->p_cursor >= p_parser->p_end) return parse_fail(p_parser, "unterminated object");
        const char separator = *p_parser->p_cursor++;
        if (separator == '}') return true;
        if (separator != ',') return parse_fail(p_parser, "expected ',' or '}'");
    }
}

static bool parse_value(JsonParser *p_parser, JsonValue *p_out_value) {
    skip_space(p_parser);
    if (p_parser->p_cursor >= p_parser->p_end) return parse_fail(p_parser, "expected a value");
    switch (*p_parser->p_cursor) {
    case '{':
    case '[': {
        if (++p_parser->depth > JSON_MAX_DEPTH) return parse_fail(p_parser, "nested too deeply");
        const bool is_parsed = *p_parser->p_cursor == '{' ? parse_object(p_parser, p_out_value) : parse_array(p_parser, p_out_value);
        p_parser->depth--;
        return is_parsed;
    }
    case '"':
        p_out_value->type = JSON_TYPE_STRING;
        return parse_string(p_parser, &p_out_value->string);
    case 't':
    case 'f':
        p_out_value->type = JSON_TYPE_BOOLEAN;
        p_out_value->boolean = *p_parser->p_cursor == 't';
        if (consume_word(p_parser, p_out_value->boolean ? "true" : "false")) return true;
        break;
    case 'n':
        if (consume_word(p_parser, "null")) return true;
        break;
    default:
        if (*p_parser->p_cursor == '-' || (*p_parser->p_cursor >= '0' && *p_parser->p_cursor <= '9')) return parse_number(p_parser, p_out_value);
        break;
    }
    return parse_fail(p_parser, "unexpected character");
}

/* Text may end in NULs (a buffer sized with its terminator) and may start with a UTF-8 BOM. */
ABI int32_t json_parse(JsonValue *p_value, const char *p_text, uint64_t cb_length) {
    if (!p_value || (!p_text && cb_length)) return JSON_ERROR_INVALID_ARGUMENT;
    JsonParser p_parser = {p_text, p_text, p_text + cb_length, 0, 0, NULL};
    if (cb_length >= 3 && !memcmp(p_text, "\xEF\xBB\xBF", 3)) p_parser.p_cursor += 3;
    JsonValue parsed = {0};
    if (parse_value(&p_parser, &parsed)) {
        skip_space(&p_parser);
        while (p_parser.p_cursor < p_parser.p_end && !*p_parser.p_cursor) p_parser.p_cursor++;
        if (p_parser.p_cursor < p_parser.p_end) parse_fail(&p_parser, "text after the value");
    }
    json_value_release(p_value);
    if (p_parser.error) {
        json_value_release(&parsed);
        static int s_c_failures_logged;
        if (__atomic_fetch_add(&s_c_failures_logged, 1, __ATOMIC_RELAXED) < 16)
            printf("Runtime: libSceJson parse failed at byte %lld of %llu: %s -> 0x%08X\n",
                   (long long)(p_parser.p_cursor - p_parser.p_start), (unsigned long long)cb_length, p_parser.sz_reason,
                   (uint32_t)p_parser.error);
        return p_parser.error;
    }
    *p_value = parsed;
    return 0;
}

ABI int32_t json_parse_terminated(JsonValue *p_value, const char *p_text) {
    return json_parse(p_value, p_text, p_text ? strlen(p_text) : 0);
}

/* ---- serialize ---- */
static bool serialize_string(JsonString *p_out_text, const char *p_text, uint64_t cb_length) {
    if (!json_string_append(p_out_text, "\"", 1)) return false;
    uint64_t i_run_start = 0;
    for (uint64_t i = 0; i < cb_length; ++i) {
        const unsigned char character = (unsigned char)p_text[i];
        if (character >= 0x20 && character != '"' && character != '\\') continue;
        if (!json_string_append(p_out_text, p_text + i_run_start, i - i_run_start)) return false;
        char escaped[8];
        switch (character) {
        case '"': strcpy(escaped, "\\\""); break;
        case '\\': strcpy(escaped, "\\\\"); break;
        case '\b': strcpy(escaped, "\\b"); break;
        case '\f': strcpy(escaped, "\\f"); break;
        case '\n': strcpy(escaped, "\\n"); break;
        case '\r': strcpy(escaped, "\\r"); break;
        case '\t': strcpy(escaped, "\\t"); break;
        default: snprintf(escaped, sizeof(escaped), "\\u%04x", character); break;
        }
        if (!json_string_append(p_out_text, escaped, strlen(escaped))) return false;
        i_run_start = i + 1;
    }
    return json_string_append(p_out_text, p_text + i_run_start, cb_length - i_run_start) &&
           json_string_append(p_out_text, "\"", 1);
}

/* The shortest form that reads back to the same double, with a fraction or exponent kept so it
 * parses back as a real; JSON has no NaN or infinity, which become null. */
static bool serialize_real(JsonString *p_out_text, double real) {
    if (!isfinite(real)) return json_string_append(p_out_text, "null", 4);
    char digits[40];
    for (int precision = 15; precision <= 17; ++precision) {
        snprintf(digits, sizeof(digits), "%.*g", precision, real);
        if (strtod(digits, NULL) == real) break;
    }
    if (!strpbrk(digits, ".eE")) strcat(digits, ".0");
    return json_string_append(p_out_text, digits, strlen(digits));
}

static bool serialize_value(JsonString *p_out_text, const JsonValue *p_value) {
    char digits[32];
    switch (p_value->type) {
    case JSON_TYPE_BOOLEAN:
        return p_value->boolean ? json_string_append(p_out_text, "true", 4) : json_string_append(p_out_text, "false", 5);
    case JSON_TYPE_INTEGER:
        snprintf(digits, sizeof(digits), "%lld", (long long)p_value->integer);
        return json_string_append(p_out_text, digits, strlen(digits));
    case JSON_TYPE_UINTEGER:
        snprintf(digits, sizeof(digits), "%llu", (unsigned long long)p_value->uinteger);
        return json_string_append(p_out_text, digits, strlen(digits));
    case JSON_TYPE_REAL: return serialize_real(p_out_text, p_value->real);
    case JSON_TYPE_STRING:
        return serialize_string(p_out_text, p_value->string.p_text ? p_value->string.p_text->text : "",
                                p_value->string.p_text ? p_value->string.p_text->cb_length : 0);
    case JSON_TYPE_ARRAY: {
        const JsonArrayData *p_data = p_value->array.p_data;
        if (!json_string_append(p_out_text, "[", 1)) return false;
        for (uint64_t i = 0; p_data && i < p_data->c_items; ++i)
            if ((i && !json_string_append(p_out_text, ",", 1)) || !serialize_value(p_out_text, p_data->pp_items[i])) return false;
        return json_string_append(p_out_text, "]", 1);
    }
    case JSON_TYPE_OBJECT: {
        const JsonObjectData *p_data = p_value->object.p_data;
        if (!json_string_append(p_out_text, "{", 1)) return false;
        for (uint64_t i = 0; p_data && i < p_data->c_items; ++i) {
            const JsonMember *p_member = p_data->pp_members[i];
            if ((i && !json_string_append(p_out_text, ",", 1)) ||
                !serialize_string(p_out_text, p_member->key.p_text ? p_member->key.p_text->text : "",
                                  p_member->key.p_text ? p_member->key.p_text->cb_length : 0) ||
                !json_string_append(p_out_text, ":", 1) || !serialize_value(p_out_text, &p_member->value))
                return false;
        }
        return json_string_append(p_out_text, "}", 1);
    }
    default: return json_string_append(p_out_text, "null", 4);
    }
}

/* The text replaces what out_text held. */
ABI int32_t json_serialize(JsonValue *p_value, JsonString *p_out_text) {
    if (!p_value || !p_out_text) return JSON_ERROR_INVALID_ARGUMENT;
    JsonString serialized = {0};
    if (!serialize_value(&serialized, p_value) || (!serialized.p_text && !json_string_assign(&serialized, "", 0))) {
        json_string_release(&serialized);
        return JSON_ERROR_NO_MEMORY;
    }
    json_string_release(p_out_text);
    *p_out_text = serialized;
    return 0;
}
