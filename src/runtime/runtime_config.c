/* psport.ini and the PS_* environment, read through one place. The file is read once, on first
 * use, and kept as its key=value lines in file order, duplicates included: a reader that applies
 * every line (the settings menu's Set, the controls) sees what it saw reading the file itself, and
 * one asking for a key gets its last value, as run.sh and decanter do. */
#define _GNU_SOURCE
#include "runtime.h"
#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { char *sz_key, *sz_value; } ConfigEntry;
static ConfigEntry *s_p_entries;
static size_t s_c_entries;
static int s_is_read;
static pthread_once_t s_once=PTHREAD_ONCE_INIT;

const char *runtime_env(const char *sz_name, const char *sz_default) {
    const char *sz_value=getenv(sz_name);
    return sz_value ? sz_value : sz_default;
}
int runtime_env_int(const char *sz_name, int fallback) {
    const char *sz_value=getenv(sz_name);
    return sz_value && *sz_value ? atoi(sz_value) : fallback;
}
int runtime_env_flag(const char *sz_name, int fallback) {
    const char *sz_value=getenv(sz_name);
    return sz_value && *sz_value ? *sz_value!='0' : fallback;
}
const char *runtime_config_path(void) {
    const char *sz_path=runtime_env("PS_CONFIG","");
    return *sz_path ? sz_path : "psport.ini";
}

static char *trimmed(char *sz_text) {
    while (isspace((unsigned char)*sz_text)) ++sz_text;
    for (char *sz_end=sz_text+strlen(sz_text); sz_end>sz_text && isspace((unsigned char)sz_end[-1]);) *--sz_end=0;
    return sz_text;
}
static void config_read(void) {
    const char *sz_path=runtime_config_path();
    FILE *p_file=fopen(sz_path,"r");
    if (!p_file) {
        if (errno!=ENOENT) printf("Runtime: settings: cannot read %s: %s\n",sz_path,strerror(errno));
        return;
    }
    s_is_read=1;
    size_t c_capacity=0, cb_line=0;
    char *sz_line=NULL;
    while (getline(&sz_line,&cb_line,p_file)>=0) {
        char *sz_key=trimmed(sz_line), *sz_equals=strchr(sz_key,'=');
        if (*sz_key=='#' || !sz_equals) continue;
        *sz_equals=0;
        sz_key=trimmed(sz_key);
        if (!*sz_key) continue;
        if (s_c_entries==c_capacity) {
            c_capacity=c_capacity ? 2*c_capacity : 64;
            ConfigEntry *p_grown=realloc(s_p_entries,c_capacity*sizeof(*p_grown));
            if (!p_grown) { printf("Runtime: settings: out of memory reading %s\n",sz_path); break; }
            s_p_entries=p_grown;
        }
        ConfigEntry *p_entry=&s_p_entries[s_c_entries];
        p_entry->sz_key=strdup(sz_key);
        p_entry->sz_value=strdup(trimmed(sz_equals+1));
        if (!p_entry->sz_key || !p_entry->sz_value) { printf("Runtime: settings: out of memory reading %s\n",sz_path); break; }
        ++s_c_entries;
    }
    free(sz_line);
    fclose(p_file);
}
int runtime_config_is_read(void) {
    pthread_once(&s_once,config_read);
    return s_is_read;
}
const char *runtime_config_get(const char *sz_key) {
    pthread_once(&s_once,config_read);
    for (size_t i=s_c_entries; i-->0;)
        if (!strcmp(s_p_entries[i].sz_key,sz_key)) return s_p_entries[i].sz_value;
    return NULL;
}
int runtime_config_entry(size_t i_entry, const char **pp_out_key, const char **pp_out_value) {
    pthread_once(&s_once,config_read);
    if (i_entry>=s_c_entries) return 0;
    *pp_out_key=s_p_entries[i_entry].sz_key;
    *pp_out_value=s_p_entries[i_entry].sz_value;
    return 1;
}
