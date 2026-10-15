/* Trophy SFM files (TROPCONF.SFM, TROP.SFM, TROP_<language>.SFM): a signature comment and an XML
 * trophyconf document. Only the elements libSceNpTrophy reports are read: npcommid, title-name,
 * title-detail, group and trophy with their name and detail. */
#define _GNU_SOURCE
#include "runtime_trophy.h"
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32

typedef struct SfmTag {
    const char *p_name, *p_attributes, *p_attributes_end;
    size_t cb_name;
    bool is_closing, is_empty;
} SfmTag;

static const char *find_text(const char *sz_cursor, const char *p_end, const char *sz_needle) {
    const size_t cb_needle = strlen(sz_needle);
    if (sz_cursor >= p_end || (size_t)(p_end - sz_cursor) < cb_needle) return NULL;
    return memmem(sz_cursor, (size_t)(p_end - sz_cursor), sz_needle, cb_needle);
}
static bool has_prefix(const char *sz_cursor, const char *p_end, const char *sz_prefix) {
    const size_t cb_prefix = strlen(sz_prefix);
    return (size_t)(p_end - sz_cursor) >= cb_prefix && !memcmp(sz_cursor, sz_prefix, cb_prefix);
}
static bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

/* The next element tag from p on, past comments, CDATA, declarations and processing instructions;
 * returns the position after its '>', or NULL at the end of the text. */
static const char *next_tag(const char *sz_cursor, const char *p_end, SfmTag *p_out_tag) {
    while (sz_cursor < p_end && (sz_cursor = memchr(sz_cursor, '<', (size_t)(p_end - sz_cursor)))) {
        const char *p_skip_end = NULL;
        if (has_prefix(sz_cursor, p_end, "<!--")) p_skip_end = find_text(sz_cursor + 4, p_end, "-->");
        else if (has_prefix(sz_cursor, p_end, "<![CDATA[")) p_skip_end = find_text(sz_cursor + 9, p_end, "]]>");
        if (p_skip_end) {
            sz_cursor = p_skip_end + 3;
            continue;
        }
        const char *p_close = memchr(sz_cursor, '>', (size_t)(p_end - sz_cursor));
        if (!p_close) return NULL;
        if (sz_cursor + 1 < p_end && (sz_cursor[1] == '?' || sz_cursor[1] == '!')) {
            sz_cursor = p_close + 1;
            continue;
        }
        p_out_tag->is_closing = sz_cursor[1] == '/';
        p_out_tag->is_empty = p_close[-1] == '/';
        p_out_tag->p_name = sz_cursor + 1 + p_out_tag->is_closing;
        const char *p_name_end = p_out_tag->p_name;
        while (p_name_end < p_close && !is_space(*p_name_end) && *p_name_end != '/') ++p_name_end;
        p_out_tag->cb_name = (size_t)(p_name_end - p_out_tag->p_name);
        p_out_tag->p_attributes = p_name_end;
        p_out_tag->p_attributes_end = p_close - p_out_tag->is_empty;
        return p_close + 1;
    }
    return NULL;
}
static bool is_tag(const SfmTag *p_tag, const char *sz_name) {
    return p_tag->cb_name == strlen(sz_name) && !memcmp(p_tag->p_name, sz_name, p_tag->cb_name);
}

static size_t put_utf8(uint32_t code_point, char *p_out) {
    if (code_point < 0x80) { p_out[0] = (char)code_point; return 1; }
    if (code_point < 0x800) {
        p_out[0] = (char)(0xC0 | code_point >> 6);
        p_out[1] = (char)(0x80 | (code_point & 0x3F));
        return 2;
    }
    if (code_point < 0x10000) {
        p_out[0] = (char)(0xE0 | code_point >> 12);
        p_out[1] = (char)(0x80 | (code_point >> 6 & 0x3F));
        p_out[2] = (char)(0x80 | (code_point & 0x3F));
        return 3;
    }
    p_out[0] = (char)(0xF0 | code_point >> 18);
    p_out[1] = (char)(0x80 | (code_point >> 12 & 0x3F));
    p_out[2] = (char)(0x80 | (code_point >> 6 & 0x3F));
    p_out[3] = (char)(0x80 | (code_point & 0x3F));
    return 4;
}
/* Decodes [p, p_end) with its character references and CDATA sections into sz_out (cut at a
 * character boundary when it does not fit), whitespace at both ends trimmed. */
static void decode_text(const char *sz_cursor, const char *p_end, char *sz_out, size_t cb_out) {
    static const struct { const char *sz_entity; char c; } s_entities[] = {
        {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''},
    };
    size_t cb_used = 0;
    while (sz_cursor < p_end && is_space(*sz_cursor)) ++sz_cursor;
    while (sz_cursor < p_end) {
        char sz_piece[4];
        size_t cb_piece = 0;
        const char *p_next = sz_cursor + 1;
        if (has_prefix(sz_cursor, p_end, "<![CDATA[")) {
            const char *p_cdata = sz_cursor + 9, *p_cdata_end = find_text(p_cdata, p_end, "]]>");
            if (!p_cdata_end) p_cdata_end = p_end;
            const size_t cb_cdata = (size_t)(p_cdata_end - p_cdata);
            if (cb_used + cb_cdata >= cb_out) break;
            memcpy(sz_out + cb_used, p_cdata, cb_cdata);
            cb_used += cb_cdata;
            sz_cursor = p_cdata_end + (p_cdata_end < p_end ? 3 : 0);
            continue;
        }
        if (*sz_cursor == '<') break;
        if (*sz_cursor == '&') {
            const char *p_semicolon = memchr(sz_cursor, ';', (size_t)(p_end - sz_cursor));
            if (p_semicolon && sz_cursor[1] == '#') {
                const uint32_t code_point = (uint32_t)(sz_cursor[2] == 'x' || sz_cursor[2] == 'X' ? strtoul(sz_cursor + 3, NULL, 16) : strtoul(sz_cursor + 2, NULL, 10));
                if (code_point && code_point <= 0x10FFFF) cb_piece = put_utf8(code_point, sz_piece);
                p_next = p_semicolon + 1;
            } else if (p_semicolon)
                for (size_t i = 0; i < sizeof(s_entities) / sizeof(*s_entities); ++i)
                    if ((size_t)(p_semicolon + 1 - sz_cursor) == strlen(s_entities[i].sz_entity) &&
                        !memcmp(sz_cursor, s_entities[i].sz_entity, (size_t)(p_semicolon + 1 - sz_cursor))) {
                        sz_piece[0] = s_entities[i].c;
                        cb_piece = 1;
                        p_next = p_semicolon + 1;
                    }
        }
        if (!cb_piece && p_next == sz_cursor + 1) {
            sz_piece[0] = *sz_cursor;
            cb_piece = 1;
            /* A UTF-8 sequence is copied whole or not at all. */
            const unsigned char lead = (unsigned char)*sz_cursor;
            const size_t cb_sequence = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
            if (cb_sequence > 1 && (size_t)(p_end - sz_cursor) >= cb_sequence) {
                memcpy(sz_piece, sz_cursor, cb_sequence);
                cb_piece = cb_sequence;
                p_next = sz_cursor + cb_sequence;
            }
        }
        if (cb_used + cb_piece >= cb_out) break;
        memcpy(sz_out + cb_used, sz_piece, cb_piece);
        cb_used += cb_piece;
        sz_cursor = p_next;
    }
    while (cb_used && is_space(sz_out[cb_used - 1])) --cb_used;
    if (cb_out) sz_out[cb_used] = 0;
}
static bool read_attribute(const SfmTag *p_tag, const char *sz_name, char *sz_out, size_t cb_out) {
    const size_t cb_name = strlen(sz_name);
    for (const char *sz_cursor = p_tag->p_attributes; sz_cursor && sz_cursor < p_tag->p_attributes_end; ++sz_cursor) {
        sz_cursor = find_text(sz_cursor, p_tag->p_attributes_end, sz_name);
        if (!sz_cursor) return false;
        const char *p_after = sz_cursor + cb_name;
        if (!is_space(sz_cursor[-1])) continue;
        while (p_after < p_tag->p_attributes_end && is_space(*p_after)) ++p_after;
        if (p_after >= p_tag->p_attributes_end || *p_after != '=') continue;
        ++p_after;
        while (p_after < p_tag->p_attributes_end && is_space(*p_after)) ++p_after;
        if (p_after >= p_tag->p_attributes_end || (*p_after != '"' && *p_after != '\'')) continue;
        const char *p_value_end = memchr(p_after + 1, *p_after, (size_t)(p_tag->p_attributes_end - p_after - 1));
        if (!p_value_end) return false;
        decode_text(p_after + 1, p_value_end, sz_out, cb_out);
        return true;
    }
    return false;
}
static bool read_number(const SfmTag *p_tag, const char *sz_name, int32_t *p_out_value) {
    char sz_value[16];
    if (!read_attribute(p_tag, sz_name, sz_value, sizeof(sz_value))) return false;
    char *p_end;
    const long value = strtol(sz_value, &p_end, 10);
    if (p_end == sz_value || *p_end) return false;
    *p_out_value = (int32_t)value;
    return true;
}
static int32_t grade_of(char type) {
    switch (type) {
    case 'P': return TROPHY_GRADE_PLATINUM;
    case 'G': return TROPHY_GRADE_GOLD;
    case 'S': return TROPHY_GRADE_SILVER;
    case 'B': return TROPHY_GRADE_BRONZE;
    default: return TROPHY_GRADE_UNKNOWN;
    }
}

TrophyGroup *trophy_pack_group(TrophyPack *p_pack, int32_t id) {
    for (uint32_t i = 0; i < p_pack->c_groups; ++i)
        if (p_pack->groups[i].id == id) return &p_pack->groups[i];
    return NULL;
}

bool trophy_sfm_parse(const char *p_text, size_t cb_text, TrophyPack *p_pack) {
    const char *sz_cursor = p_text, *p_end = p_text + cb_text;
    TrophyEntry *p_trophy = NULL;
    TrophyGroup *p_group = NULL;
    bool is_trophyconf = false;
    SfmTag tag;
    while ((sz_cursor = next_tag(sz_cursor, p_end, &tag))) {
        if (tag.is_closing) {
            if (is_tag(&tag, "trophy")) p_trophy = NULL;
            else if (is_tag(&tag, "group")) p_group = NULL;
            continue;
        }
        if (is_tag(&tag, "trophyconf")) {
            is_trophyconf = true;
            continue;
        }
        if (!is_trophyconf) continue;
        const char *p_text_end = memchr(sz_cursor, '<', (size_t)(p_end - sz_cursor));
        if (!p_text_end) p_text_end = p_end;
        while (has_prefix(p_text_end, p_end, "<![CDATA[")) {
            const char *p_cdata_end = find_text(p_text_end, p_end, "]]>");
            p_text_end = p_cdata_end ? memchr(p_cdata_end, '<', (size_t)(p_end - p_cdata_end)) : NULL;
            if (!p_text_end) p_text_end = p_end;
        }
        int32_t id;
        if (is_tag(&tag, "trophy")) {
            p_group = NULL;
            p_trophy = NULL;
            if (!read_number(&tag, "id", &id) || id < 0 || id >= TROPHY_NUM_MAX) continue;
            p_trophy = &p_pack->trophies[id];
            if ((uint32_t)id >= p_pack->c_trophies) p_pack->c_trophies = (uint32_t)id + 1;
            char sz_value[8];
            if (read_attribute(&tag, "ttype", sz_value, sizeof(sz_value))) p_trophy->grade = grade_of(sz_value[0]);
            if (read_attribute(&tag, "hidden", sz_value, sizeof(sz_value))) p_trophy->is_hidden = !strcmp(sz_value, "yes");
            if (!read_number(&tag, "gid", &p_trophy->group)) p_trophy->group = BASE_GAME_GROUP;
            if (!read_number(&tag, "pid", &p_trophy->platinum)) p_trophy->platinum = TROPHY_NO_PLATINUM;
            if (tag.is_empty) p_trophy = NULL;
        } else if (is_tag(&tag, "group")) {
            p_trophy = NULL;
            p_group = NULL;
            if (!read_number(&tag, "id", &id)) continue;
            p_group = trophy_pack_group(p_pack, id);
            if (!p_group && p_pack->c_groups < TROPHY_GROUP_MAX) {
                p_group = &p_pack->groups[p_pack->c_groups++];
                memset(p_group, 0, sizeof(*p_group));
                p_group->id = id;
            }
            if (tag.is_empty) p_group = NULL;
        } else if (is_tag(&tag, "name")) {
            if (p_trophy) decode_text(sz_cursor, p_text_end, p_trophy->sz_name, sizeof(p_trophy->sz_name));
            else if (p_group) decode_text(sz_cursor, p_text_end, p_group->sz_name, sizeof(p_group->sz_name));
        } else if (is_tag(&tag, "detail")) {
            if (p_trophy) decode_text(sz_cursor, p_text_end, p_trophy->sz_detail, sizeof(p_trophy->sz_detail));
            else if (p_group) decode_text(sz_cursor, p_text_end, p_group->sz_detail, sizeof(p_group->sz_detail));
        } else if (!p_trophy && !p_group) {
            if (is_tag(&tag, "title-name")) decode_text(sz_cursor, p_text_end, p_pack->sz_title, sizeof(p_pack->sz_title));
            else if (is_tag(&tag, "title-detail")) decode_text(sz_cursor, p_text_end, p_pack->sz_detail, sizeof(p_pack->sz_detail));
            else if (is_tag(&tag, "npcommid")) decode_text(sz_cursor, p_text_end, p_pack->sz_np_comm_id, sizeof(p_pack->sz_np_comm_id));
        }
    }
    return is_trophyconf;
}
#endif
