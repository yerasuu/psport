#define _GNU_SOURCE
#include "loader.h"
#include "loader_xml.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *p_start, *p_cursor, *p_end;
    char sz_error[160];
} XmlParser;

void buffer_add(Buffer *p_buffer, const char *p_data, size_t cb_data) {
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
char *buffer_take(Buffer *p_buffer) {
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
int is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
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
void free_node(XmlNode *p_node) {
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
const char *attribute(const XmlNode *p_node, const char *sz_name) {
    for (size_t i = 0; i < p_node->c_attributes; ++i)
        if (!strcmp(p_node->p_attributes[i].sz_name, sz_name)) return p_node->p_attributes[i].sz_value;
    return NULL;
}
/* Every element named sz_name under p_node, in document order (p_node itself not included; see
 * find_from). */
void find_all(const XmlNode *p_node, const char *sz_name, NodeList *p_out) {
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
void find_from(const XmlNode *p_root, const char *sz_name, NodeList *p_out) {
    if (!strcmp(p_root->sz_name, sz_name)) {
        if (!(p_out->pp_nodes = realloc(p_out->pp_nodes, sizeof(*p_out->pp_nodes) * (p_out->c_capacity = 16))))
            loader_fail("allocation failed");
        p_out->pp_nodes[p_out->c_nodes++] = p_root;
    }
    find_all(p_root, sz_name, p_out);
}
XmlNode *read_xml(const char *sz_path, char *sz_out_error, size_t cb_error) {
    FILE *p_file = ps_host_fs_open_stream(sz_path, "rb");
    PsHostStat st;
    if (!p_file || ps_host_fs_stat_stream(p_file, &st) || !PS_HOST_MODE_IS_REGULAR(st.mode)) {
        snprintf(sz_out_error, cb_error, "%s", p_file ? "not a regular file" : strerror(errno));
        if (p_file) fclose(p_file);
        return NULL;
    }
    char *p_text = malloc((size_t)st.cb_size + 1);
    if (!p_text) loader_fail("allocation failed");
    size_t cb_text = fread(p_text, 1, (size_t)st.cb_size, p_file);
    fclose(p_file);
    XmlNode *p_root = parse_xml(p_text, cb_text, sz_out_error, cb_error);
    free(p_text);
    return p_root;
}
