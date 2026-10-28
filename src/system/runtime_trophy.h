// SPDX-License-Identifier: GPL-2.0-or-later
/* The game's trophy pack (sce_sys/trophy/trophy<label>.trp), as libSceNpTrophy (runtime_trophy.c)
 * reads it: the TRP container (runtime_trophy_pack.c) and the trophy configuration and names in its
 * SFM files (runtime_trophy_sfm.c). */
#ifndef BB_RUNTIME_TROPHY_H
#define BB_RUNTIME_TROPHY_H
#include "runtime.h"
#include <stdbool.h>

#define TROPHY_NUM_MAX 128
#define TROPHY_GROUP_MAX 128
#define TROPHY_NAME_MAX 128
#define TROPHY_DETAIL_MAX 1024
#define BASE_GAME_GROUP (-1)
#define TROPHY_NO_PLATINUM (-1)

enum { TROPHY_GRADE_UNKNOWN, TROPHY_GRADE_PLATINUM, TROPHY_GRADE_GOLD, TROPHY_GRADE_SILVER, TROPHY_GRADE_BRONZE };

typedef struct TrophyEntry {
    int32_t grade, group, platinum;
    bool is_hidden;
    char sz_name[TROPHY_NAME_MAX], sz_detail[TROPHY_DETAIL_MAX];
} TrophyEntry;
typedef struct TrophyGroup {
    int32_t id;
    char sz_name[TROPHY_NAME_MAX], sz_detail[TROPHY_DETAIL_MAX];
} TrophyGroup;
typedef struct TrpEntry {
    char sz_name[33];
    uint64_t off_data, cb_data;
    uint32_t flags;
} TrpEntry;
/* Without a readable configuration (has_config false: encrypted, missing or broken) the trophies are
 * the TROP<id>.PNG icons the pack holds and the groups its GR<id>.PNG icons: no names, grades,
 * hidden flags, group membership or platinum link. */
typedef struct TrophyPack {
    char sz_host_path[1024];
    char sz_np_comm_id[16];
    char sz_title[TROPHY_NAME_MAX], sz_detail[TROPHY_DETAIL_MAX];
    char sz_names_file[34], sz_encrypted_file[34];
    bool has_config;
    uint32_t c_trophies, c_groups, c_entries;
    TrophyEntry trophies[TROPHY_NUM_MAX];
    TrophyGroup groups[TROPHY_GROUP_MAX];
    TrpEntry *p_entries;
} TrophyPack;

/* Reads the container's entry table and whatever configuration it can; logs what it could not read.
 * false when the file is no TRP container, *p_out_pack then holds nothing to release. */
bool trophy_pack_open(const char *sz_host_path, int language, TrophyPack *p_out_pack);
void trophy_pack_close(TrophyPack *p_pack);
/* The entry's size, or -1 when the pack has no entry of that name or it cannot be read; with p_buffer
 * set, also copies it there when cb_buffer holds all of it. */
int64_t trophy_pack_read(const TrophyPack *p_pack, const char *sz_entry, void *p_buffer, uint64_t cb_buffer);
TrophyGroup *trophy_pack_group(TrophyPack *p_pack, int32_t id);
/* The NP communication id sce_sys/npbind.dat (at sz_host_path) binds to the service label: false
 * when it binds none or the id is not one (four capitals, five digits, '_', two digits). */
bool trophy_npbind_comm_id(const char *sz_host_path, uint32_t label, char sz_out_id[16]);
/* Merges one SFM file (a trophyconf document) into the pack: title, groups, trophies with their
 * attributes and texts, later files overriding earlier ones; false when it is no trophyconf. */
bool trophy_sfm_parse(const char *p_text, size_t cb_text, TrophyPack *p_pack);
#endif
