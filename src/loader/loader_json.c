// SPDX-License-Identifier: GPL-2.0-or-later
/* The launchers' selection files (mods.json, patches.json): a JSON object whose members of interest
 * are arrays of strings. Read here rather than with the runtime's libSceJson, whose state and
 * diagnostics belong to the game. */
#define _GNU_SOURCE
#include "loader.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *p_cursor, *p_end;
    int depth;
} JsonReader;

static void skip_space(JsonReader *p_reader) {
    while (p_reader->p_cursor < p_reader->p_end && strchr(" \t\r\n", *p_reader->p_cursor) && *p_reader->p_cursor)
        ++p_reader->p_cursor;
}
static int take(JsonReader *p_reader, char c) {
    skip_space(p_reader);
    if (p_reader->p_cursor >= p_reader->p_end || *p_reader->p_cursor != c) return 0;
    ++p_reader->p_cursor;
    return 1;
}
static void add_utf8(char *sz_out, size_t *p_cb_out, uint32_t code_point) {
    if (code_point < 0x80) {
        sz_out[(*p_cb_out)++] = (char)code_point;
    } else if (code_point < 0x800) {
        sz_out[(*p_cb_out)++] = (char)(0xc0 | code_point >> 6);
        sz_out[(*p_cb_out)++] = (char)(0x80 | (code_point & 0x3f));
    } else if (code_point < 0x10000) {
        sz_out[(*p_cb_out)++] = (char)(0xe0 | code_point >> 12);
        sz_out[(*p_cb_out)++] = (char)(0x80 | (code_point >> 6 & 0x3f));
        sz_out[(*p_cb_out)++] = (char)(0x80 | (code_point & 0x3f));
    } else {
        sz_out[(*p_cb_out)++] = (char)(0xf0 | code_point >> 18);
        sz_out[(*p_cb_out)++] = (char)(0x80 | (code_point >> 12 & 0x3f));
        sz_out[(*p_cb_out)++] = (char)(0x80 | (code_point >> 6 & 0x3f));
        sz_out[(*p_cb_out)++] = (char)(0x80 | (code_point & 0x3f));
    }
}
static int hex4(const char *sz_digits, uint32_t *p_out) {
    *p_out = 0;
    for (int i = 0; i < 4; ++i) {
        char c = sz_digits[i];
        int digit = (unsigned)(c - '0') < 10 ? c - '0' : (unsigned)((c | 32) - 'a') < 6 ? (c | 32) - 'a' + 10 : -1;
        if (digit < 0) return 0;
        *p_out = *p_out << 4 | (uint32_t)digit;
    }
    return 1;
}
/* A string, malloc'd into *psz_out when psz_out is given. */
static int read_string(JsonReader *p_reader, char **psz_out) {
    if (!take(p_reader, '"')) return 0;
    const char *p_start = p_reader->p_cursor;
    char *sz_text = malloc((size_t)(p_reader->p_end - p_start) + 1);
    if (!sz_text) loader_fail("allocation failed");
    size_t cb_text = 0;
    for (;;) {
        if (p_reader->p_cursor >= p_reader->p_end || (unsigned char)*p_reader->p_cursor < 0x20) break;
        char c = *p_reader->p_cursor++;
        if (c == '"') {
            sz_text[cb_text] = 0;
            if (psz_out) *psz_out = sz_text;
            else free(sz_text);
            return 1;
        }
        if (c != '\\') {
            sz_text[cb_text++] = c;
            continue;
        }
        if (p_reader->p_cursor >= p_reader->p_end) break;
        c = *p_reader->p_cursor++;
        const char *p_simple = strchr("\"\\/bfnrt", c);
        if (p_simple && c) {
            sz_text[cb_text++] = "\"\\/\b\f\n\r\t"[p_simple - "\"\\/bfnrt"];
            continue;
        }
        uint32_t code_point, low;
        if (c != 'u' || p_reader->p_end - p_reader->p_cursor < 4 || !hex4(p_reader->p_cursor, &code_point)) break;
        p_reader->p_cursor += 4;
        if (code_point >= 0xd800 && code_point < 0xdc00 && p_reader->p_end - p_reader->p_cursor >= 6 &&
            p_reader->p_cursor[0] == '\\' && p_reader->p_cursor[1] == 'u' && hex4(p_reader->p_cursor + 2, &low) &&
            low >= 0xdc00 && low < 0xe000) {
            code_point = 0x10000 + ((code_point - 0xd800) << 10) + (low - 0xdc00);
            p_reader->p_cursor += 6;
        }
        add_utf8(sz_text, &cb_text, code_point);
    }
    free(sz_text);
    return 0;
}
static int skip_value(JsonReader *p_reader);
static int skip_container(JsonReader *p_reader, char close, int is_object) {
    if (++p_reader->depth > 64) return 0;
    if (take(p_reader, close)) return --p_reader->depth, 1;
    do {
        if (is_object && (!read_string(p_reader, NULL) || !take(p_reader, ':'))) return 0;
        if (!skip_value(p_reader)) return 0;
    } while (take(p_reader, ','));
    --p_reader->depth;
    return take(p_reader, close);
}
static int skip_value(JsonReader *p_reader) {
    skip_space(p_reader);
    if (p_reader->p_cursor >= p_reader->p_end) return 0;
    char c = *p_reader->p_cursor;
    if (c == '"') return read_string(p_reader, NULL);
    if (c == '{' || c == '[') {
        ++p_reader->p_cursor;
        return skip_container(p_reader, c == '{' ? '}' : ']', c == '{');
    }
    static const char *const s_words[] = {"true", "false", "null"};
    for (size_t i = 0; i < 3; ++i) {
        size_t cb_word = strlen(s_words[i]);
        if ((size_t)(p_reader->p_end - p_reader->p_cursor) >= cb_word && !memcmp(p_reader->p_cursor, s_words[i], cb_word)) {
            p_reader->p_cursor += cb_word;
            return 1;
        }
    }
    const char *p_start = p_reader->p_cursor;
    while (p_reader->p_cursor < p_reader->p_end && *p_reader->p_cursor && strchr("+-.0123456789eE", *p_reader->p_cursor))
        ++p_reader->p_cursor;
    return p_reader->p_cursor > p_start;
}
static void free_list(LoaderJsonList *p_list) {
    for (size_t i = 0; i < p_list->c_items; ++i) free(p_list->pp_items[i]);
    free(p_list->pp_items);
    p_list->pp_items = NULL;
    p_list->c_items = 0;
}
/* The member's value as a list of strings; is_list stays 0 for any other value (skipped). */
static int read_member(JsonReader *p_reader, LoaderJsonList *p_list) {
    free_list(p_list);
    p_list->is_present = 1;
    p_list->is_list = 0;
    JsonReader before = *p_reader;
    if (!take(p_reader, '[')) {
        *p_reader = before;
        return skip_value(p_reader);
    }
    int is_list = 1;
    if (!take(p_reader, ']')) {
        do {
            skip_space(p_reader);
            char *sz_item;
            if (p_reader->p_cursor < p_reader->p_end && *p_reader->p_cursor == '"') {
                if (!read_string(p_reader, &sz_item)) return 0;
                p_list->pp_items = realloc(p_list->pp_items, (p_list->c_items + 1) * sizeof(*p_list->pp_items));
                if (!p_list->pp_items) loader_fail("allocation failed");
                p_list->pp_items[p_list->c_items++] = sz_item;
            } else {
                is_list = 0;
                if (!skip_value(p_reader)) return 0;
            }
        } while (take(p_reader, ','));
        if (!take(p_reader, ']')) return 0;
    }
    p_list->is_list = is_list;
    return 1;
}

int loader_json_lists(const char *sz_path, LoaderJsonList *p_lists, size_t c_lists) {
    size_t cb_text;
    unsigned char *p_text = loader_read_file(sz_path, &cb_text);
    JsonReader reader = {(const char *)p_text, (const char *)p_text + cb_text, 0};
    if (cb_text >= 3 && !memcmp(p_text, "\xEF\xBB\xBF", 3)) reader.p_cursor += 3;
    int is_valid = take(&reader, '{');
    if (is_valid && !take(&reader, '}')) {
        do {
            char *sz_key;
            if (!read_string(&reader, &sz_key) || !take(&reader, ':')) {
                is_valid = 0;
                break;
            }
            LoaderJsonList *p_list = NULL;
            for (size_t i = 0; i < c_lists && !p_list; ++i)
                if (!strcmp(p_lists[i].sz_key, sz_key)) p_list = &p_lists[i];
            free(sz_key);
            if (!(p_list ? read_member(&reader, p_list) : skip_value(&reader))) {
                is_valid = 0;
                break;
            }
        } while (take(&reader, ','));
        is_valid = is_valid && take(&reader, '}');
    }
    skip_space(&reader);
    is_valid = is_valid && reader.p_cursor == reader.p_end;
    free(p_text);
    if (!is_valid)
        for (size_t i = 0; i < c_lists; ++i) free_list(&p_lists[i]);
    return is_valid;
}

int loader_json_has(const LoaderJsonList *p_list, const char *sz_item) {
    for (size_t i = 0; i < p_list->c_items; ++i)
        if (!strcmp(p_list->pp_items[i], sz_item)) return 1;
    return 0;
}

void loader_json_free(LoaderJsonList *p_lists, size_t c_lists) {
    for (size_t i = 0; i < c_lists; ++i) free_list(&p_lists[i]);
}
