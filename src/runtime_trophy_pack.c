/* TRP containers: a big-endian header (magic 0xDCA24D00, version, file size, entry count and size)
 * and a table of named entries. The icons are plain PNG; the SFM files are XML, or ESFM when the
 * console's trophy key encrypts them, as in every retail pack: the port has no key, so it reads such a
 * pack's trophies from its icons alone. */
#define _GNU_SOURCE
#include "runtime_trophy.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32

#define TRP_MAGIC 0xDCA24D00u
#define CB_TRP_HEADER 0x60
#define CB_TRP_ENTRY 0x40
#define C_TRP_ENTRIES_MAX 4096
#define CB_SFM_MAX (16u << 20)

static uint32_t read_be32(const unsigned char *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static uint64_t read_be64(const unsigned char *p) { return (uint64_t)read_be32(p) << 32 | read_be32(p + 4); }

static const TrpEntry *find_entry(const TrophyPack *p_pack, const char *sz_name) {
    for (uint32_t i = 0; i < p_pack->c_entries; ++i)
        if (!strcmp(p_pack->p_entries[i].sz_name, sz_name)) return &p_pack->p_entries[i];
    return NULL;
}

int64_t trophy_pack_read(const TrophyPack *p_pack, const char *sz_entry, void *p_buffer, uint64_t cb_buffer) {
    const TrpEntry *p_entry = find_entry(p_pack, sz_entry);
    if (!p_entry || p_entry->cb_data > INT64_MAX) return -1;
    if (!p_buffer || cb_buffer < p_entry->cb_data) return (int64_t)p_entry->cb_data;
    FILE *p_file = fopen(p_pack->sz_host_path, "rb");
    if (!p_file) return -1;
    const bool is_read = !fseeko(p_file, (off_t)p_entry->off_data, SEEK_SET) &&
                         fread(p_buffer, 1, p_entry->cb_data, p_file) == p_entry->cb_data;
    fclose(p_file);
    return is_read ? (int64_t)p_entry->cb_data : -1;
}

/* Merges the SFM entry sz_name into the pack; logs and returns false when it cannot. */
static bool merge_sfm(TrophyPack *p_pack, const char *sz_name) {
    const int64_t cb_text = trophy_pack_read(p_pack, sz_name, NULL, 0);
    if (cb_text <= 0 || (uint64_t)cb_text > CB_SFM_MAX) return false;
    char *p_text = malloc((size_t)cb_text);
    const bool is_merged = p_text && trophy_pack_read(p_pack, sz_name, p_text, (uint64_t)cb_text) == cb_text &&
                           trophy_sfm_parse(p_text, (size_t)cb_text, p_pack);
    free(p_text);
    if (!is_merged) printf("Runtime: NpTrophy: %s in %s is not a trophy configuration\n", sz_name, p_pack->sz_host_path);
    return is_merged;
}

/* The trophies and groups as the icons give them, when no configuration can be read. */
static void count_icons(TrophyPack *p_pack) {
    for (uint32_t i = 0; i < p_pack->c_entries; ++i) {
        const char *sz_name = p_pack->p_entries[i].sz_name;
        unsigned id;
        char sz_extension[5];
        if (sscanf(sz_name, "TROP%3u.%4s", &id, sz_extension) == 2 && strlen(sz_name) == 11 &&
            !strcmp(sz_extension, "PNG") && id < TROPHY_NUM_MAX && id >= p_pack->c_trophies)
            p_pack->c_trophies = id + 1;
        else if (sscanf(sz_name, "GR%3u.%4s", &id, sz_extension) == 2 && strlen(sz_name) == 9 &&
                 !strcmp(sz_extension, "PNG") && !trophy_pack_group(p_pack, (int32_t)id) &&
                 p_pack->c_groups < TROPHY_GROUP_MAX)
            p_pack->groups[p_pack->c_groups++].id = (int32_t)id;
    }
    for (uint32_t i = 0; i < p_pack->c_trophies; ++i) {
        p_pack->trophies[i].group = BASE_GAME_GROUP;
        p_pack->trophies[i].platinum = TROPHY_NO_PLATINUM;
    }
}

bool trophy_pack_open(const char *sz_host_path, int language, TrophyPack *p_out_pack) {
    memset(p_out_pack, 0, sizeof(*p_out_pack));
    snprintf(p_out_pack->sz_host_path, sizeof(p_out_pack->sz_host_path), "%s", sz_host_path);
    FILE *p_file = fopen(sz_host_path, "rb");
    if (!p_file) return false;
    unsigned char header[CB_TRP_HEADER];
    bool is_valid = fread(header, 1, sizeof(header), p_file) == sizeof(header) && read_be32(header) == TRP_MAGIC;
    const uint32_t version = is_valid ? read_be32(header + 4) : 0;
    const uint32_t c_entries = is_valid ? read_be32(header + 16) : 0, cb_entry = is_valid ? read_be32(header + 20) : 0;
    off_t cb_file = 0;
    is_valid = is_valid && cb_entry == CB_TRP_ENTRY && c_entries && c_entries <= C_TRP_ENTRIES_MAX &&
               !fseeko(p_file, 0, SEEK_END) && (cb_file = ftello(p_file)) > 0 && !fseeko(p_file, CB_TRP_HEADER, SEEK_SET);
    if (is_valid) p_out_pack->p_entries = calloc(c_entries, sizeof(*p_out_pack->p_entries));
    is_valid = is_valid && p_out_pack->p_entries;
    for (uint32_t i = 0; is_valid && i < c_entries; ++i) {
        unsigned char raw[CB_TRP_ENTRY];
        TrpEntry *p_entry = &p_out_pack->p_entries[i];
        is_valid = fread(raw, 1, sizeof(raw), p_file) == sizeof(raw);
        memcpy(p_entry->sz_name, raw, 32);
        p_entry->off_data = read_be64(raw + 32);
        p_entry->cb_data = read_be64(raw + 40);
        p_entry->flags = read_be32(raw + 48);
        is_valid = is_valid && p_entry->off_data <= (uint64_t)cb_file && p_entry->cb_data <= (uint64_t)cb_file - p_entry->off_data;
    }
    fclose(p_file);
    if (!is_valid) {
        printf("Runtime: NpTrophy: %s is not a TRP container the port can read (version %u, %u entries of %u bytes)\n",
               sz_host_path, version, c_entries, cb_entry);
        trophy_pack_close(p_out_pack);
        return false;
    }
    p_out_pack->c_entries = c_entries;
    if (find_entry(p_out_pack, "TROPCONF.SFM")) {
        p_out_pack->has_config = merge_sfm(p_out_pack, "TROPCONF.SFM");
        char sz_names[34];
        snprintf(sz_names, sizeof(sz_names), "TROP_%02d.SFM", language);
        if (!find_entry(p_out_pack, sz_names)) snprintf(sz_names, sizeof(sz_names), "TROP.SFM");
        if (p_out_pack->has_config && find_entry(p_out_pack, sz_names) && merge_sfm(p_out_pack, sz_names))
            snprintf(p_out_pack->sz_names_file, sizeof(p_out_pack->sz_names_file), "%s", sz_names);
        if (!p_out_pack->has_config) {
            memset(p_out_pack->trophies, 0, sizeof(p_out_pack->trophies));
            p_out_pack->c_trophies = p_out_pack->c_groups = 0;
        }
    } else
        for (uint32_t i = 0; i < c_entries && !p_out_pack->sz_encrypted_file[0]; ++i) {
            const char *sz_name = p_out_pack->p_entries[i].sz_name;
            const size_t cb_name = strlen(sz_name);
            if (cb_name > 5 && !strcmp(sz_name + cb_name - 5, ".ESFM"))
                snprintf(p_out_pack->sz_encrypted_file, sizeof(p_out_pack->sz_encrypted_file), "%s", sz_name);
        }
    if (!p_out_pack->has_config) count_icons(p_out_pack);
    return true;
}

/* npbind.dat: a big-endian header (magic 0xD294A018, entry size at 0x10, entry count at 0x18), entries
 * from 0x80 on, each a run of type/length/value fields; type 0x10 is the NP communication id. */
bool trophy_npbind_comm_id(const char *sz_host_path, uint32_t label, char sz_out_id[16]) {
    unsigned char data[0x8000];
    FILE *p_file = fopen(sz_host_path, "rb");
    if (!p_file) return false;
    const size_t cb_data = fread(data, 1, sizeof(data), p_file);
    fclose(p_file);
    if (cb_data < 0x80 || read_be32(data) != 0xD294A018u) return false;
    const uint64_t cb_entry = read_be64(data + 0x10), c_entries = read_be64(data + 0x18);
    if (label >= c_entries || !cb_entry || cb_entry > cb_data || 0x80 + (label + 1) * cb_entry > cb_data) return false;
    const unsigned char *p = data + 0x80 + label * cb_entry, *p_end = p + cb_entry;
    while (p + 4 <= p_end) {
        const unsigned type = (unsigned)p[0] << 8 | p[1], cb_value = (unsigned)p[2] << 8 | p[3];
        if (p + 4 + cb_value > p_end) return false;
        if (type == 0x10) {
            static const char s_sz_shape[] = "AAAA99999_99";
            if (cb_value != 12) return false;
            for (size_t i = 0; i < 12; ++i) {
                const char c = (char)p[4 + i], shape = s_sz_shape[i];
                if ((shape == 'A' && (c < 'A' || c > 'Z')) || (shape == '9' && (c < '0' || c > '9')) || (shape == '_' && c != '_'))
                    return false;
            }
            memcpy(sz_out_id, p + 4, 12);
            sz_out_id[12] = 0;
            return true;
        }
        p += 4 + cb_value;
    }
    return false;
}

void trophy_pack_close(TrophyPack *p_pack) {
    free(p_pack->p_entries);
    p_pack->p_entries = NULL;
    p_pack->c_entries = 0;
}
#endif
