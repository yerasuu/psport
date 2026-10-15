/* Patches to the game's executable in shadPS4/GoldHEN's XML format: <Metadata Name AppVer AppElf
 * isEnabled> entries with <Line Type Address Value> writes, in files whose <ID>s name the titles
 * they are for. Literal writes only (bytes, bytes16/32/64, float32/64, utf8, utf16); pattern
 * ("mask") lines are refused. They are applied after relocation (bind.c), the external ones after
 * the profile's own, so an external patch of the same bytes wins. */
#define _GNU_SOURCE
#include "loader.h"
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define EBOOT_BASE 0x400000

/* ---- XML ---- */

typedef struct { char *sz_name, *sz_value; } XmlAttribute;
typedef struct XmlNode {
    char *sz_name;
    XmlAttribute *p_attributes;
    size_t c_attributes;
    char *sz_text; /* the character data before the first child element */
    struct XmlNode **pp_children;
    size_t c_children;
} XmlNode;
typedef struct {
    const char *p_start, *p_cursor, *p_end;
    char sz_error[160];
} XmlParser;
typedef struct { char *p_data; size_t cb_length, cb_capacity; } Buffer;

static void buffer_add(Buffer *p_buffer, const char *p_data, size_t cb_data) {
    if (p_buffer->cb_length + cb_data + 1 > p_buffer->cb_capacity) {
        size_t cb_capacity = p_buffer->cb_capacity ? p_buffer->cb_capacity : 64;
        while (cb_capacity < p_buffer->cb_length + cb_data + 1) cb_capacity *= 2;
        if (!(p_buffer->p_data = realloc(p_buffer->p_data, cb_capacity))) loader_fail("allocation failed");
        p_buffer->cb_capacity = cb_capacity;
    }
    memcpy(p_buffer->p_data + p_buffer->cb_length, p_data, cb_data);
    p_buffer->cb_length += cb_data;
    p_buffer->p_data[p_buffer->cb_length] = 0;
}
static char *buffer_take(Buffer *p_buffer) {
    if (!p_buffer->p_data) buffer_add(p_buffer, "", 0);
    char *sz_text = p_buffer->p_data;
    memset(p_buffer, 0, sizeof(*p_buffer));
    return sz_text;
}

/* Lines from 1, columns from 0, as expat (and so ElementTree) reports them. */
static int xml_fail(XmlParser *p_parser, const char *p_at, const char *sz_reason) {
    if (p_parser->sz_error[0]) return 0;
    unsigned line = 1, column = 0;
    for (const char *sz_cursor = p_parser->p_start; sz_cursor < p_at && sz_cursor < p_parser->p_end; ++sz_cursor) {
        if (*sz_cursor == '\n') ++line, column = 0;
        else if (((unsigned char)*sz_cursor & 0xc0) != 0x80) ++column;
    }
    snprintf(p_parser->sz_error, sizeof(p_parser->sz_error), "%s: line %u, column %u", sz_reason, line, column);
    return 0;
}
static int at_end(const XmlParser *p_parser) { return p_parser->p_cursor >= p_parser->p_end; }
static int starts(const XmlParser *p_parser, const char *sz_text) {
    size_t cb_text = strlen(sz_text);
    return (size_t)(p_parser->p_end - p_parser->p_cursor) >= cb_text && !memcmp(p_parser->p_cursor, sz_text, cb_text);
}
static int is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
static void skip_space(XmlParser *p_parser) {
    while (!at_end(p_parser) && is_space(*p_parser->p_cursor)) ++p_parser->p_cursor;
}
static int is_name_start(unsigned char c) { return c >= 0x80 || c == '_' || c == ':' || (unsigned)((c | 32) - 'a') < 26; }
static int is_name_char(unsigned char c) { return is_name_start(c) || c == '-' || c == '.' || (unsigned)(c - '0') < 10; }
static char *parse_name(XmlParser *p_parser) {
    const char *p_name = p_parser->p_cursor;
    if (at_end(p_parser) || !is_name_start((unsigned char)*p_name)) {
        xml_fail(p_parser, p_name, "not well-formed (invalid token)");
        return NULL;
    }
    while (!at_end(p_parser) && is_name_char((unsigned char)*p_parser->p_cursor)) ++p_parser->p_cursor;
    return strndup(p_name, (size_t)(p_parser->p_cursor - p_name));
}
/* Skips to just past sz_close; 0 at the end of the text. */
static int skip_past(XmlParser *p_parser, const char *sz_close, const char *p_from) {
    const char *p_found = memmem(p_parser->p_cursor, (size_t)(p_parser->p_end - p_parser->p_cursor), sz_close, strlen(sz_close));
    if (!p_found) return xml_fail(p_parser, p_from, "unclosed token");
    p_parser->p_cursor = p_found + strlen(sz_close);
    return 1;
}
static void add_utf8(Buffer *p_buffer, uint32_t code_point) {
    char sz_bytes[4];
    size_t cb_bytes;
    if (code_point < 0x80) sz_bytes[0] = (char)code_point, cb_bytes = 1;
    else if (code_point < 0x800)
        sz_bytes[0] = (char)(0xc0 | code_point >> 6), sz_bytes[1] = (char)(0x80 | (code_point & 0x3f)), cb_bytes = 2;
    else if (code_point < 0x10000)
        sz_bytes[0] = (char)(0xe0 | code_point >> 12), sz_bytes[1] = (char)(0x80 | (code_point >> 6 & 0x3f)),
        sz_bytes[2] = (char)(0x80 | (code_point & 0x3f)), cb_bytes = 3;
    else
        sz_bytes[0] = (char)(0xf0 | code_point >> 18), sz_bytes[1] = (char)(0x80 | (code_point >> 12 & 0x3f)),
        sz_bytes[2] = (char)(0x80 | (code_point >> 6 & 0x3f)), sz_bytes[3] = (char)(0x80 | (code_point & 0x3f)), cb_bytes = 4;
    buffer_add(p_buffer, sz_bytes, cb_bytes);
}
/* Character data up to '<' (text) or the closing quote (an attribute value, whose literal line
 * breaks and tabs become spaces, as XML normalizes them). Line breaks are \n either way. */
static int parse_data(XmlParser *p_parser, char quote, Buffer *p_out) {
    while (!at_end(p_parser) && *p_parser->p_cursor != (quote ? quote : '<')) {
        const char *sz_cursor = p_parser->p_cursor;
        if (quote && *sz_cursor == '<') return xml_fail(p_parser, sz_cursor, "not well-formed (invalid token)");
        if (*sz_cursor == '&') {
            const char *p_semicolon = memchr(sz_cursor, ';', (size_t)(p_parser->p_end - sz_cursor));
            if (!p_semicolon) return xml_fail(p_parser, sz_cursor, "not well-formed (invalid token)");
            size_t cb_entity = (size_t)(p_semicolon - sz_cursor - 1);
            static const char *const s_entities[][2] = {{"lt", "<"}, {"gt", ">"}, {"amp", "&"}, {"quot", "\""}, {"apos", "'"}};
            int is_known = 0;
            for (size_t i = 0; i < sizeof(s_entities) / sizeof(*s_entities) && !is_known; ++i)
                if (strlen(s_entities[i][0]) == cb_entity && !memcmp(sz_cursor + 1, s_entities[i][0], cb_entity))
                    buffer_add(p_out, s_entities[i][1], 1), is_known = 1;
            if (!is_known) {
                if (cb_entity < 2 || sz_cursor[1] != '#') return xml_fail(p_parser, sz_cursor, "undefined entity");
                int is_hex = sz_cursor[2] == 'x';
                const char *p_digits = sz_cursor + 2 + is_hex;
                uint32_t code_point = 0;
                if (p_digits == p_semicolon) return xml_fail(p_parser, sz_cursor, "not well-formed (invalid token)");
                for (const char *sz_digit = p_digits; sz_digit < p_semicolon; ++sz_digit) {
                    unsigned digit = (unsigned)(*sz_digit - '0') < 10 ? (unsigned)(*sz_digit - '0')
                                   : is_hex && (unsigned)((*sz_digit | 32) - 'a') < 6 ? (unsigned)((*sz_digit | 32) - 'a' + 10) : 99;
                    if (digit == 99) return xml_fail(p_parser, sz_cursor, "not well-formed (invalid token)");
                    code_point = code_point * (is_hex ? 16 : 10) + digit;
                    if (code_point > 0x10ffff) return xml_fail(p_parser, sz_cursor, "reference to invalid character number");
                }
                if ((code_point < 0x20 && code_point != 9 && code_point != 10 && code_point != 13) ||
                    (code_point >= 0xd800 && code_point < 0xe000) || code_point == 0xfffe || code_point == 0xffff)
                    return xml_fail(p_parser, sz_cursor, "reference to invalid character number");
                add_utf8(p_out, code_point);
            }
            p_parser->p_cursor = p_semicolon + 1;
        } else if (*sz_cursor == '\r') {
            buffer_add(p_out, quote ? " " : "\n", 1);
            p_parser->p_cursor += sz_cursor + 1 < p_parser->p_end && sz_cursor[1] == '\n' ? 2 : 1;
        } else if (quote && (*sz_cursor == '\n' || *sz_cursor == '\t')) {
            buffer_add(p_out, " ", 1);
            ++p_parser->p_cursor;
        } else {
            buffer_add(p_out, sz_cursor, 1);
            ++p_parser->p_cursor;
        }
    }
    return 1;
}
static void free_node(XmlNode *p_node) {
    if (!p_node) return;
    for (size_t i = 0; i < p_node->c_attributes; ++i) {
        free(p_node->p_attributes[i].sz_name);
        free(p_node->p_attributes[i].sz_value);
    }
    for (size_t i = 0; i < p_node->c_children; ++i) free_node(p_node->pp_children[i]);
    free(p_node->p_attributes);
    free(p_node->pp_children);
    free(p_node->sz_name);
    free(p_node->sz_text);
    free(p_node);
}
/* Comments, processing instructions and a document type, which carry nothing a patch uses. */
static int skip_markup(XmlParser *p_parser) {
    const char *p_from = p_parser->p_cursor;
    if (starts(p_parser, "<!--")) return skip_past(p_parser, "-->", p_from);
    if (starts(p_parser, "<?")) return skip_past(p_parser, "?>", p_from);
    if (starts(p_parser, "<!DOCTYPE")) {
        int depth = 0;
        for (; !at_end(p_parser); ++p_parser->p_cursor) {
            char c = *p_parser->p_cursor;
            if (c == '[') ++depth;
            else if (c == ']') --depth;
            else if (c == '>' && depth <= 0) {
                ++p_parser->p_cursor;
                return 1;
            }
        }
        return xml_fail(p_parser, p_from, "unclosed token");
    }
    return xml_fail(p_parser, p_from, "syntax error");
}
static XmlNode *parse_element(XmlParser *p_parser, int depth) {
    if (depth > 256) {
        xml_fail(p_parser, p_parser->p_cursor, "nested too deeply");
        return NULL;
    }
    XmlNode *p_node = calloc(1, sizeof(*p_node));
    if (!p_node) loader_fail("allocation failed");
    ++p_parser->p_cursor;
    if (!(p_node->sz_name = parse_name(p_parser))) goto failed;
    for (;;) {
        const char *p_before = p_parser->p_cursor;
        skip_space(p_parser);
        if (at_end(p_parser)) {
            xml_fail(p_parser, p_parser->p_cursor, "unclosed token");
            goto failed;
        }
        if (starts(p_parser, "/>")) {
            p_parser->p_cursor += 2;
            if (!p_node->sz_text) p_node->sz_text = strdup("");
            return p_node;
        }
        if (*p_parser->p_cursor == '>') {
            ++p_parser->p_cursor;
            break;
        }
        if (p_before == p_parser->p_cursor) {
            xml_fail(p_parser, p_parser->p_cursor, "not well-formed (invalid token)");
            goto failed;
        }
        const char *p_attribute = p_parser->p_cursor;
        char *sz_name = parse_name(p_parser);
        if (!sz_name) goto failed;
        for (size_t i = 0; i < p_node->c_attributes; ++i)
            if (!strcmp(p_node->p_attributes[i].sz_name, sz_name)) {
                free(sz_name);
                xml_fail(p_parser, p_attribute, "duplicate attribute");
                goto failed;
            }
        skip_space(p_parser);
        if (at_end(p_parser) || *p_parser->p_cursor != '=') {
            free(sz_name);
            xml_fail(p_parser, p_parser->p_cursor, "not well-formed (invalid token)");
            goto failed;
        }
        ++p_parser->p_cursor;
        skip_space(p_parser);
        char quote = at_end(p_parser) ? 0 : *p_parser->p_cursor;
        Buffer value = {0};
        int is_value = quote == '"' || quote == '\'';
        if (is_value) ++p_parser->p_cursor;
        if (!is_value || !parse_data(p_parser, quote, &value) || at_end(p_parser)) {
            free(sz_name);
            free(value.p_data);
            xml_fail(p_parser, p_parser->p_cursor, "not well-formed (invalid token)");
            goto failed;
        }
        ++p_parser->p_cursor;
        p_node->p_attributes = realloc(p_node->p_attributes, (p_node->c_attributes + 1) * sizeof(*p_node->p_attributes));
        if (!p_node->p_attributes) loader_fail("allocation failed");
        p_node->p_attributes[p_node->c_attributes++] = (XmlAttribute){sz_name, buffer_take(&value)};
    }
    Buffer text = {0};
    for (;;) {
        if (at_end(p_parser)) {
            xml_fail(p_parser, p_parser->p_cursor, "no element found");
            free(text.p_data);
            goto failed;
        }
        if (*p_parser->p_cursor != '<') {
            Buffer data = {0};
            if (!parse_data(p_parser, 0, &data)) {
                free(data.p_data);
                free(text.p_data);
                goto failed;
            }
            if (!p_node->c_children && data.p_data) buffer_add(&text, data.p_data, data.cb_length);
            free(data.p_data);
        } else if (starts(p_parser, "<![CDATA[")) {
            p_parser->p_cursor += 9;
            const char *p_data = p_parser->p_cursor;
            if (!skip_past(p_parser, "]]>", p_data - 9)) {
                free(text.p_data);
                goto failed;
            }
            if (!p_node->c_children) buffer_add(&text, p_data, (size_t)(p_parser->p_cursor - 3 - p_data));
        } else if (starts(p_parser, "</")) {
            p_parser->p_cursor += 2;
            const char *p_name = p_parser->p_cursor;
            char *sz_name = parse_name(p_parser);
            if (!sz_name || strcmp(sz_name, p_node->sz_name)) {
                free(sz_name);
                free(text.p_data);
                xml_fail(p_parser, p_name, "mismatched tag");
                goto failed;
            }
            free(sz_name);
            skip_space(p_parser);
            if (at_end(p_parser) || *p_parser->p_cursor != '>') {
                free(text.p_data);
                xml_fail(p_parser, p_parser->p_cursor, "not well-formed (invalid token)");
                goto failed;
            }
            ++p_parser->p_cursor;
            if (p_node->c_children) free(text.p_data);
            else p_node->sz_text = buffer_take(&text);
            return p_node;
        } else if (starts(p_parser, "<!") || starts(p_parser, "<?")) {
            if (starts(p_parser, "<!DOCTYPE") || !skip_markup(p_parser)) {
                free(text.p_data);
                xml_fail(p_parser, p_parser->p_cursor, "syntax error");
                goto failed;
            }
        } else {
            if (!p_node->c_children) p_node->sz_text = buffer_take(&text);
            XmlNode *p_child = parse_element(p_parser, depth + 1);
            if (!p_child) goto failed;
            p_node->pp_children = realloc(p_node->pp_children, (p_node->c_children + 1) * sizeof(*p_node->pp_children));
            if (!p_node->pp_children) loader_fail("allocation failed");
            p_node->pp_children[p_node->c_children++] = p_child;
        }
    }
failed:
    free_node(p_node);
    return NULL;
}
/* The document's root element, or NULL with the reason (and where) in sz_out_error. */
static XmlNode *parse_xml(const char *p_text, size_t cb_text, char *sz_out_error, size_t cb_error) {
    XmlParser parser = {p_text, p_text, p_text + cb_text, ""};
    if (cb_text >= 3 && !memcmp(p_text, "\xEF\xBB\xBF", 3)) parser.p_cursor += 3;
    XmlNode *p_root = NULL;
    for (;;) {
        skip_space(&parser);
        if (at_end(&parser)) break;
        if (*parser.p_cursor != '<' || (p_root && !starts(&parser, "<!--") && !starts(&parser, "<?"))) {
            xml_fail(&parser, parser.p_cursor, p_root ? "junk after document element" : "syntax error");
            break;
        }
        if (starts(&parser, "<!") || starts(&parser, "<?")) {
            if (!skip_markup(&parser)) break;
        } else if (!(p_root = parse_element(&parser, 0))) {
            break;
        }
    }
    if (!parser.sz_error[0] && !p_root) xml_fail(&parser, parser.p_cursor, "no element found");
    if (parser.sz_error[0]) {
        free_node(p_root);
        snprintf(sz_out_error, cb_error, "%s", parser.sz_error);
        return NULL;
    }
    return p_root;
}
static const char *attribute(const XmlNode *p_node, const char *sz_name) {
    for (size_t i = 0; i < p_node->c_attributes; ++i)
        if (!strcmp(p_node->p_attributes[i].sz_name, sz_name)) return p_node->p_attributes[i].sz_value;
    return NULL;
}
/* Every element named sz_name under p_node, in document order (p_node itself not included; see
 * find_from). */
typedef struct { const XmlNode **pp_nodes; size_t c_nodes, c_capacity; } NodeList;
static void find_all(const XmlNode *p_node, const char *sz_name, NodeList *p_out) {
    for (size_t i = 0; i < p_node->c_children; ++i) {
        const XmlNode *p_child = p_node->pp_children[i];
        if (!strcmp(p_child->sz_name, sz_name)) {
            if (p_out->c_nodes == p_out->c_capacity) {
                p_out->c_capacity = p_out->c_capacity ? p_out->c_capacity * 2 : 16;
                if (!(p_out->pp_nodes = realloc(p_out->pp_nodes, p_out->c_capacity * sizeof(*p_out->pp_nodes))))
                    loader_fail("allocation failed");
            }
            p_out->pp_nodes[p_out->c_nodes++] = p_child;
        }
        find_all(p_child, sz_name, p_out);
    }
}
/* p_root too, as ElementTree's iter() does. */
static void find_from(const XmlNode *p_root, const char *sz_name, NodeList *p_out) {
    if (!strcmp(p_root->sz_name, sz_name)) {
        if (!(p_out->pp_nodes = realloc(p_out->pp_nodes, sizeof(*p_out->pp_nodes) * (p_out->c_capacity = 16))))
            loader_fail("allocation failed");
        p_out->pp_nodes[p_out->c_nodes++] = p_root;
    }
    find_all(p_root, sz_name, p_out);
}
static XmlNode *read_xml(const char *sz_path, char *sz_out_error, size_t cb_error) {
    FILE *p_file = fopen(sz_path, "rb");
    struct stat st;
    if (!p_file || fstat(fileno(p_file), &st) || !S_ISREG(st.st_mode)) {
        snprintf(sz_out_error, cb_error, "%s", p_file ? "not a regular file" : strerror(errno));
        if (p_file) fclose(p_file);
        return NULL;
    }
    char *p_text = malloc((size_t)st.st_size + 1);
    if (!p_text) loader_fail("allocation failed");
    size_t cb_text = fread(p_text, 1, (size_t)st.st_size, p_file);
    fclose(p_file);
    XmlNode *p_root = parse_xml(p_text, cb_text, sz_out_error, cb_error);
    free(p_text);
    return p_root;
}

/* ---- Lines ---- */

typedef struct { unsigned char bytes[4096]; size_t cb_used; } Bytes;

static int line_error(char *sz_out_error, size_t cb_error, const char *sz_format, ...) {
    va_list args;
    va_start(args, sz_format);
    vsnprintf(sz_out_error, cb_error, sz_format, args);
    va_end(args);
    return 0;
}
static int bytes_add(Bytes *p_out, const void *p_data, size_t cb_data, char *sz_out_error, size_t cb_error) {
    if (p_out->cb_used + cb_data > sizeof(p_out->bytes))
        return line_error(sz_out_error, cb_error, "a line writes more than 4096 bytes");
    memcpy(p_out->bytes + p_out->cb_used, p_data, cb_data);
    p_out->cb_used += cb_data;
    return 1;
}
/* Python's int(text, 0), as the patch format's tools read numbers: 0x/0o/0b prefixes, underscores
 * between digits, no leading zeros in decimal. *p_out_is_negative carries the sign; values past
 * 128 bits are refused as too large. */
static int parse_integer(const char *sz_text, unsigned __int128 *p_out_value, int *p_out_is_negative, char *sz_out_error,
                         size_t cb_error) {
    const char *sz_cursor = sz_text, *p_end = sz_text + strlen(sz_text);
    while (sz_cursor < p_end && is_space(*sz_cursor)) ++sz_cursor;
    while (p_end > sz_cursor && is_space(p_end[-1])) --p_end;
    *p_out_is_negative = sz_cursor < p_end && *sz_cursor == '-';
    if (sz_cursor < p_end && (*sz_cursor == '-' || *sz_cursor == '+')) ++sz_cursor;
    unsigned base = 10;
    if (p_end - sz_cursor >= 2 && sz_cursor[0] == '0' && strchr("xXoObB", sz_cursor[1])) {
        base = (sz_cursor[1] | 32) == 'x' ? 16 : (sz_cursor[1] | 32) == 'o' ? 8 : 2;
        sz_cursor += 2;
        if (sz_cursor < p_end && *sz_cursor == '_') ++sz_cursor;
    }
    unsigned __int128 value = 0;
    int c_digits = 0, is_previous_underscore = 0, is_too_large = 0, is_leading_zero = 0, has_nonzero = 0;
    for (; sz_cursor < p_end; ++sz_cursor) {
        if (*sz_cursor == '_') {
            if (!c_digits || is_previous_underscore) break;
            is_previous_underscore = 1;
            continue;
        }
        unsigned digit = (unsigned)(*sz_cursor - '0') < 10          ? (unsigned)(*sz_cursor - '0')
                         : (unsigned)((*sz_cursor | 32) - 'a') < 26 ? (unsigned)((*sz_cursor | 32) - 'a' + 10)
                                                            : 99;
        if (digit >= base) break;
        if (base == 10 && !c_digits && digit == 0) is_leading_zero = 1;
        if (digit) has_nonzero = 1;
        if (value > (~(unsigned __int128)0 - digit) / base) is_too_large = 1;
        value = value * base + digit;
        ++c_digits;
        is_previous_underscore = 0;
    }
    if (sz_cursor != p_end || !c_digits || is_previous_underscore || (is_leading_zero && has_nonzero))
        return line_error(sz_out_error, cb_error, "invalid literal for int() with base 0: '%s'", sz_text);
    if (is_too_large) return line_error(sz_out_error, cb_error, "int too big to convert: '%s'", sz_text);
    *p_out_value = value;
    return 1;
}
static const char *skip_digits(const char *sz_cursor, const char *p_end, char *sz_clean, size_t *p_cb_clean) {
    if (sz_cursor >= p_end || (unsigned)(*sz_cursor - '0') >= 10) return NULL;
    while (sz_cursor < p_end) {
        if ((unsigned)(*sz_cursor - '0') < 10) sz_clean[(*p_cb_clean)++] = *sz_cursor++;
        else if (*sz_cursor == '_' && sz_cursor + 1 < p_end && (unsigned)(sz_cursor[1] - '0') < 10) ++sz_cursor;
        else break;
    }
    return sz_cursor;
}
/* Python's float(text): whitespace around, underscores between digits, inf/infinity/nan. */
static int parse_float(const char *sz_text, double *p_out_value, char *sz_out_error, size_t cb_error) {
    char sz_clean[512];
    size_t cb_clean = 0;
    const char *sz_cursor = sz_text, *p_end = sz_text + strlen(sz_text);
    while (sz_cursor < p_end && is_space(*sz_cursor)) ++sz_cursor;
    while (p_end > sz_cursor && is_space(p_end[-1])) --p_end;
    int is_valid = (size_t)(p_end - sz_cursor) < sizeof(sz_clean) / 2;
    if (is_valid && sz_cursor < p_end && (*sz_cursor == '-' || *sz_cursor == '+')) sz_clean[cb_clean++] = *sz_cursor++;
    size_t cb_word = (size_t)(p_end - sz_cursor);
    if (is_valid && ((cb_word == 3 && (!strncasecmp(sz_cursor, "inf", 3) || !strncasecmp(sz_cursor, "nan", 3))) ||
                     (cb_word == 8 && !strncasecmp(sz_cursor, "infinity", 8)))) {
        memcpy(sz_clean + cb_clean, sz_cursor, cb_word);
        cb_clean += cb_word;
        sz_cursor = p_end;
    } else if (is_valid) {
        const char *p_after = skip_digits(sz_cursor, p_end, sz_clean, &cb_clean);
        if (p_after) sz_cursor = p_after;
        if (sz_cursor < p_end && *sz_cursor == '.') {
            sz_clean[cb_clean++] = *sz_cursor++;
            const char *p_fraction = skip_digits(sz_cursor, p_end, sz_clean, &cb_clean);
            if (p_fraction) sz_cursor = p_fraction;
            else if (!p_after) is_valid = 0;
        } else if (!p_after) {
            is_valid = 0;
        }
        if (is_valid && sz_cursor < p_end && (*sz_cursor | 32) == 'e') {
            sz_clean[cb_clean++] = *sz_cursor++;
            if (sz_cursor < p_end && (*sz_cursor == '-' || *sz_cursor == '+')) sz_clean[cb_clean++] = *sz_cursor++;
            if (!(sz_cursor = skip_digits(sz_cursor, p_end, sz_clean, &cb_clean))) is_valid = 0;
        }
    }
    if (!is_valid || sz_cursor != p_end) return line_error(sz_out_error, cb_error, "could not convert string to float: '%s'", sz_text);
    sz_clean[cb_clean] = 0;
    *p_out_value = strtod(sz_clean, NULL);
    return 1;
}
static int hex_digit(char c) {
    return (unsigned)(c - '0') < 10 ? c - '0' : (unsigned)((c | 32) - 'a') < 6 ? (c | 32) - 'a' + 10 : -1;
}
/* A line's bytes, as shadPS4's patch compiler writes them. */
static int encode_line(const XmlNode *p_line, Bytes *p_out, char *sz_out_error, size_t cb_error) {
    const char *sz_type = attribute(p_line, "Type"), *sz_value = attribute(p_line, "Value");
    p_out->cb_used = 0;
    if (!sz_type) sz_type = "";
    static const char *const s_types[] = {"bytes", "bytes16", "bytes32", "bytes64", "float32", "float64", "utf8", "utf16"};
    int is_known = 0;
    for (size_t i = 0; i < sizeof(s_types) / sizeof(*s_types); ++i) is_known |= !strcmp(sz_type, s_types[i]);
    if (!is_known) return line_error(sz_out_error, cb_error, "unsupported patch type '%s'", sz_type);
    if (!sz_value) return line_error(sz_out_error, cb_error, "a %s line without a Value", sz_type);
    if (!strcmp(sz_type, "bytes")) {
        /* bytes.fromhex() after the spaces are removed: other whitespace only between bytes. */
        char sz_digits[sizeof(p_out->bytes) * 4];
        size_t cb_digits = 0;
        for (const char *sz_cursor = sz_value; *sz_cursor && cb_digits < sizeof(sz_digits) - 1; ++sz_cursor)
            if (*sz_cursor != ' ') sz_digits[cb_digits++] = *sz_cursor;
        sz_digits[cb_digits] = 0;
        for (size_t i = 0; i < cb_digits;) {
            if (is_space(sz_digits[i]) || sz_digits[i] == '\f' || sz_digits[i] == '\v') {
                ++i;
                continue;
            }
            int high = hex_digit(sz_digits[i]);
            int low = i + 1 < cb_digits ? hex_digit(sz_digits[i + 1]) : -1;
            if (high < 0 || (low < 0 && i + 1 < cb_digits))
                return line_error(sz_out_error, cb_error, "non-hexadecimal number found in fromhex() arg at position %zu",
                                  high < 0 ? i : i + 1);
            if (low < 0)
                return line_error(sz_out_error, cb_error, "fromhex() arg must contain an even number of hexadecimal digits");
            unsigned char byte = (unsigned char)(high << 4 | low);
            if (!bytes_add(p_out, &byte, 1, sz_out_error, cb_error)) return 0;
            i += 2;
        }
        return 1;
    }
    if (!strcmp(sz_type, "bytes16") || !strcmp(sz_type, "bytes32") || !strcmp(sz_type, "bytes64")) {
        unsigned __int128 value;
        int is_negative;
        if (!parse_integer(sz_value, &value, &is_negative, sz_out_error, cb_error)) return 0;
        unsigned c_bytes = (unsigned)atoi(sz_type + 5) / 8;
        if (is_negative && value) return line_error(sz_out_error, cb_error, "can't convert negative int to unsigned");
        if (c_bytes < 16 && value >> (8 * c_bytes)) return line_error(sz_out_error, cb_error, "int too big to convert");
        for (unsigned i = 0; i < c_bytes; ++i) p_out->bytes[i] = (unsigned char)(value >> (8 * i));
        p_out->cb_used = c_bytes;
        return 1;
    }
    if (!strcmp(sz_type, "float32") || !strcmp(sz_type, "float64")) {
        double value;
        if (!parse_float(sz_value, &value, sz_out_error, cb_error)) return 0;
        if (sz_type[5] == '6') return bytes_add(p_out, &value, 8, sz_out_error, cb_error);
        float single = (float)value;
        if (isinf(single) && !isinf(value)) return line_error(sz_out_error, cb_error, "float too large to pack with f format");
        return bytes_add(p_out, &single, 4, sz_out_error, cb_error);
    }
    if (!strcmp(sz_type, "utf8")) return bytes_add(p_out, sz_value, strlen(sz_value) + 1, sz_out_error, cb_error);
    if (!strcmp(sz_type, "utf16")) {
        for (const unsigned char *p_cursor = (const unsigned char *)sz_value; *p_cursor;) {
            uint32_t code_point = *p_cursor;
            int c_continuation = code_point >= 0xf0 ? 3 : code_point >= 0xe0 ? 2 : code_point >= 0xc0 ? 1 : 0;
            code_point &= c_continuation ? 0x3fu >> c_continuation : 0x7fu;
            ++p_cursor;
            for (int i = 0; i < c_continuation && (*p_cursor & 0xc0) == 0x80; ++i) code_point = code_point << 6 | (*p_cursor++ & 0x3f);
            uint16_t units[2] = {(uint16_t)code_point, 0};
            size_t c_units = 1;
            if (code_point >= 0x10000) {
                units[0] = (uint16_t)(0xd800 + ((code_point - 0x10000) >> 10));
                units[1] = (uint16_t)(0xdc00 + ((code_point - 0x10000) & 0x3ff));
                c_units = 2;
            }
            if (!bytes_add(p_out, units, c_units * 2, sz_out_error, cb_error)) return 0;
        }
        static const unsigned char s_terminator[2] = {0, 0};
        return bytes_add(p_out, s_terminator, 2, sz_out_error, cb_error);
    }
    return 0;
}

/* ---- Patch lists ---- */

static void add_write(PsPatchList *p_list, uint64_t off_image, const unsigned char *p_data, size_t cb_data) {
    if (p_list->c_writes == p_list->c_capacity) {
        p_list->c_capacity = p_list->c_capacity ? p_list->c_capacity * 2 : 256;
        if (!(p_list->p_writes = realloc(p_list->p_writes, p_list->c_capacity * sizeof(*p_list->p_writes))))
            loader_fail("allocation failed");
    }
    unsigned char *p_copy = malloc(cb_data);
    if (!p_copy) loader_fail("allocation failed");
    memcpy(p_copy, p_data, cb_data);
    p_list->p_writes[p_list->c_writes++] = (LoaderPatchWrite){off_image, cb_data, p_copy};
}
static void truncate_list(PsPatchList *p_list, size_t c_writes) {
    while (p_list->c_writes > c_writes) free(p_list->p_writes[--p_list->c_writes].p_data);
}
/* A metadata entry's lines appended, or none of them with the reason. */
static int add_entry(PsPatchList *p_list, const XmlNode *p_entry, char *sz_out_error, size_t cb_error) {
    size_t c_before = p_list->c_writes;
    NodeList lines = {0};
    find_all(p_entry, "Line", &lines);
    int is_added = 1;
    for (size_t i = 0; i < lines.c_nodes && is_added; ++i) {
        const char *sz_address = attribute(lines.pp_nodes[i], "Address");
        unsigned __int128 address;
        int is_negative;
        Bytes data;
        is_added = parse_integer(sz_address ? sz_address : "", &address, &is_negative, sz_out_error, cb_error) &&
                   encode_line(lines.pp_nodes[i], &data, sz_out_error, cb_error);
        if (is_added && !data.cb_used) is_added = line_error(sz_out_error, cb_error, "a line writes no bytes");
        if (!is_added) break;
        int is_inside = 0;
        for (size_t s = 0; s < p_list->c_loads && !is_negative && address >= EBOOT_BASE; ++s) {
            unsigned __int128 off_image = address - EBOOT_BASE;
            const LoaderSegment *p_load = &p_list->p_loads[s];
            is_inside |= off_image >= p_load->va_segment && off_image + data.cb_used <= p_load->va_segment + p_load->cb_segment;
        }
        if (!is_inside) {
            is_added = line_error(sz_out_error, cb_error, "address %s is outside the eboot", sz_address);
            break;
        }
        add_write(p_list, (uint64_t)(address - EBOOT_BASE), data.bytes, data.cb_used);
    }
    free(lines.pp_nodes);
    if (!is_added) truncate_list(p_list, c_before);
    return is_added;
}
static int is_for(const XmlNode *p_entry, const char *sz_version) {
    const char *sz_app_version = attribute(p_entry, "AppVer"), *sz_elf = attribute(p_entry, "AppElf");
    return sz_app_version && !strcmp(sz_app_version, sz_version) && !strcmp(sz_elf ? sz_elf : "eboot.bin", "eboot.bin");
}
/* The last entry of that name for the version, as a later definition replaces an earlier one. */
static const XmlNode *find_entry(const NodeList *p_entries, const char *sz_name, const char *sz_version) {
    const XmlNode *p_found = NULL;
    for (size_t i = 0; i < p_entries->c_nodes; ++i) {
        const char *sz_entry = attribute(p_entries->pp_nodes[i], "Name");
        if (sz_entry && !strcmp(sz_entry, sz_name) && is_for(p_entries->pp_nodes[i], sz_version)) p_found = p_entries->pp_nodes[i];
    }
    return p_found;
}
static int add_named(PsPatchList *p_list, const char *sz_file, const char *const *pp_names, size_t c_names,
                     const char *sz_app_version, char *sz_out_error, size_t cb_error) {
    char sz_reason[256];
    XmlNode *p_root = read_xml(sz_file, sz_reason, sizeof(sz_reason));
    if (!p_root) return line_error(sz_out_error, cb_error, "%s: %s", sz_file, sz_reason);
    NodeList entries = {0};
    find_from(p_root, "Metadata", &entries);
    size_t c_before = p_list->c_writes, c_missing = 0;
    char sz_missing[512] = "";
    for (size_t i_name = 0; i_name < c_names; ++i_name) {
        const XmlNode *p_found = find_entry(&entries, pp_names[i_name], sz_app_version);
        if (!p_found) {
            size_t cb_used = strlen(sz_missing);
            snprintf(sz_missing + cb_used, sizeof(sz_missing) - cb_used, "%s'%s'", c_missing++ ? ", " : "", pp_names[i_name]);
        }
    }
    int is_added = !c_missing;
    if (!is_added) line_error(sz_out_error, cb_error, "patches not found for app version %s: [%s]", sz_app_version, sz_missing);
    for (size_t i_name = 0; i_name < c_names && is_added; ++i_name) {
        const XmlNode *p_found = find_entry(&entries, pp_names[i_name], sz_app_version);
        char sz_reason_line[256];
        if (!add_entry(p_list, p_found, sz_reason_line, sizeof(sz_reason_line)))
            is_added = line_error(sz_out_error, cb_error, "%s: %s", pp_names[i_name], sz_reason_line);
    }
    if (!is_added) truncate_list(p_list, c_before);
    free(entries.pp_nodes);
    free_node(p_root);
    return is_added;
}
static size_t count_writes(const PsPatchList *p_list) { return p_list->c_writes; }
static unsigned char *get_write(PsPatchList *p_list, size_t i_write, uint64_t *p_out_offset, uint64_t *p_out_length) {
    if (i_write >= p_list->c_writes) return NULL;
    *p_out_offset = p_list->p_writes[i_write].off_image;
    *p_out_length = p_list->p_writes[i_write].cb_data;
    return p_list->p_writes[i_write].p_data;
}
void loader_patch_services(PsPatchList *p_list, PsPatchServices *p_out) {
    *p_out = (PsPatchServices){p_list, add_named, count_writes, get_write};
}

/* ---- External patch folders ---- */

typedef struct {
    char sz_key[512];
    XmlNode *p_root;          /* owned by the first entry of its file */
    const XmlNode *p_entry;
} External;
typedef struct { External *p_items; size_t c_items, c_capacity; } Externals;

static int compare_names(const void *p_a, const void *p_b) { return strcmp(*(char *const *)p_a, *(char *const *)p_b); }
static int has_suffix(const char *sz_name, const char *sz_suffix) {
    size_t cb_name = strlen(sz_name), cb_suffix = strlen(sz_suffix);
    return cb_name >= cb_suffix && !strcmp(sz_name + cb_name - cb_suffix, sz_suffix);
}
static void find_externals(const LoaderPatchFolder *p_folder, Externals *p_out) {
    DIR *p_dir = opendir(p_folder->sz_dir);
    if (!p_dir) return;
    char **pp_files = NULL;
    size_t c_files = 0;
    for (struct dirent *p_entry; (p_entry = readdir(p_dir));) {
        if (!has_suffix(p_entry->d_name, ".xml")) continue;
        pp_files = realloc(pp_files, (c_files + 1) * sizeof(*pp_files));
        if (!pp_files || !(pp_files[c_files++] = strdup(p_entry->d_name))) loader_fail("allocation failed");
    }
    closedir(p_dir);
    if (c_files) qsort(pp_files, c_files, sizeof(*pp_files), compare_names);
    char sz_excluded[PATH_MAX] = "";
    if (p_folder->sz_excluded && !realpath(p_folder->sz_excluded, sz_excluded)) sz_excluded[0] = 0;
    for (size_t f = 0; f < c_files; ++f) {
        char sz_path[PATH_MAX], sz_real[PATH_MAX], sz_error[256];
        snprintf(sz_path, sizeof(sz_path), "%s/%s", p_folder->sz_dir, pp_files[f]);
        if (sz_excluded[0] && realpath(sz_path, sz_real) && !strcmp(sz_real, sz_excluded)) continue;
        XmlNode *p_root = read_xml(sz_path, sz_error, sizeof(sz_error));
        if (!p_root) {
            fprintf(stderr, "Patches: %s: %s\n", pp_files[f], sz_error);
            continue;
        }
        NodeList ids = {0}, entries = {0};
        find_from(p_root, "ID", &ids);
        int c_ids = 0, is_ours = 0;
        for (size_t i = 0; i < ids.c_nodes; ++i) {
            const char *sz_text = ids.pp_nodes[i]->sz_text;
            if (!sz_text || !*sz_text) continue;
            ++c_ids;
            while (is_space(*sz_text)) ++sz_text;
            size_t cb_id = strlen(sz_text);
            while (cb_id && is_space(sz_text[cb_id - 1])) --cb_id;
            for (size_t s = 0; s < p_folder->c_serials; ++s)
                if (strlen(p_folder->pp_serials[s]) == cb_id && !memcmp(p_folder->pp_serials[s], sz_text, cb_id)) is_ours = 1;
        }
        free(ids.pp_nodes);
        if (c_ids && !is_ours) {
            free_node(p_root);
            continue;
        }
        find_from(p_root, "Metadata", &entries);
        int owns_root = 0;
        for (size_t i = 0; i < entries.c_nodes; ++i) {
            if (!is_for(entries.pp_nodes[i], p_folder->sz_version)) continue;
            if (p_out->c_items == p_out->c_capacity) {
                p_out->c_capacity = p_out->c_capacity ? p_out->c_capacity * 2 : 64;
                p_out->p_items = realloc(p_out->p_items, p_out->c_capacity * sizeof(*p_out->p_items));
                if (!p_out->p_items) loader_fail("allocation failed");
            }
            External *p_external = &p_out->p_items[p_out->c_items++];
            const char *sz_name = attribute(entries.pp_nodes[i], "Name");
            snprintf(p_external->sz_key, sizeof(p_external->sz_key), "%s/%s", pp_files[f], sz_name ? sz_name : "None");
            p_external->p_root = owns_root++ ? NULL : p_root;
            p_external->p_entry = entries.pp_nodes[i];
        }
        free(entries.pp_nodes);
        if (!owns_root) free_node(p_root);
    }
    for (size_t f = 0; f < c_files; ++f) free(pp_files[f]);
    free(pp_files);
}
static void free_externals(Externals *p_externals) {
    for (size_t i = 0; i < p_externals->c_items; ++i) free_node(p_externals->p_items[i].p_root);
    free(p_externals->p_items);
}
static int is_enabled_by_default(const XmlNode *p_entry) {
    const char *sz_enabled = attribute(p_entry, "isEnabled");
    return sz_enabled && !strcasecmp(sz_enabled, "true");
}
void loader_patches_external(PsPatchList *p_list, const LoaderPatchFolder *p_folder, const char *sz_config) {
    Externals externals = {0};
    find_externals(p_folder, &externals);
    LoaderJsonList settings[2] = {{.sz_key = "enabled"}, {.sz_key = "disabled"}};
    struct stat st;
    if (sz_config && !stat(sz_config, &st) && S_ISREG(st.st_mode) && !loader_json_lists(sz_config, settings, 2)) {
        fprintf(stderr, "ERROR: Patches: %s is not a JSON object\n", sz_config);
        exit(1);
    }
    for (size_t i = 0; i < externals.c_items; ++i) {
        const External *p_external = &externals.p_items[i];
        if (!loader_json_has(&settings[0], p_external->sz_key) &&
            (loader_json_has(&settings[1], p_external->sz_key) || !is_enabled_by_default(p_external->p_entry)))
            continue;
        char sz_error[256];
        size_t c_before = p_list->c_writes;
        if (!add_entry(p_list, p_external->p_entry, sz_error, sizeof(sz_error))) {
            fprintf(stderr, "Patches: skipped %s: %s\n", p_external->sz_key, sz_error);
            continue;
        }
        printf("Patches: external %s (%zu writes)\n", p_external->sz_key, p_list->c_writes - c_before);
    }
    loader_json_free(settings, 2);
    free_externals(&externals);
}

static void print_json_string(const char *sz_text) {
    putchar('"');
    for (const unsigned char *p_cursor = (const unsigned char *)sz_text; *p_cursor; ++p_cursor) {
        if (*p_cursor == '"' || *p_cursor == '\\') printf("\\%c", *p_cursor);
        else if (*p_cursor == '\n') fputs("\\n", stdout);
        else if (*p_cursor < 0x20) printf("\\u%04x", *p_cursor);
        else putchar(*p_cursor);
    }
    putchar('"');
}
void loader_patches_list(const LoaderPatchFolder *p_folder) {
    Externals externals = {0};
    find_externals(p_folder, &externals);
    putchar('[');
    for (size_t i = 0; i < externals.c_items; ++i) {
        const XmlNode *p_entry = externals.p_items[i].p_entry;
        const char *sz_name = attribute(p_entry, "Name"), *sz_author = attribute(p_entry, "Author");
        const char *sz_note = attribute(p_entry, "Note");
        /* Notes spell line breaks as \n. */
        Buffer note = {0};
        for (const char *sz_cursor = sz_note ? sz_note : ""; *sz_cursor; ++sz_cursor) {
            if (sz_cursor[0] == '\\' && sz_cursor[1] == 'n') buffer_add(&note, "\n", 1), ++sz_cursor;
            else buffer_add(&note, sz_cursor, 1);
        }
        char *sz_text = buffer_take(&note);
        fputs(i ? ", {\"key\": " : "{\"key\": ", stdout);
        print_json_string(externals.p_items[i].sz_key);
        fputs(", \"name\": ", stdout);
        print_json_string(sz_name && *sz_name ? sz_name : externals.p_items[i].sz_key);
        fputs(", \"author\": ", stdout);
        print_json_string(sz_author ? sz_author : "");
        fputs(", \"note\": ", stdout);
        print_json_string(sz_text);
        printf(", \"default\": %s}", is_enabled_by_default(p_entry) ? "true" : "false");
        free(sz_text);
    }
    puts("]");
    free_externals(&externals);
}
