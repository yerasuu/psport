/* The command line's other uses: the BB_* names of older launchers, and the questions a launcher
 * asks about a game, answered before any game is loaded. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "main.h"

/* A launcher's question names the game by its folder (--game dir) or, without one, by its title
 * (--title TITLE_ID [--version APP_VER]); the arguments from argv[first] on. 0 when they are
 * neither. *psz_out_game_dir is NULL for a title. */
static int query_game(int argc, char **pp_argv, int first, GameInfo *p_out_info, const char **psz_out_game_dir) {
    *psz_out_game_dir=NULL;
    if (argc == first) { game_info_of(NULL,"","",p_out_info); return 1; }
    if (argc == first+2 && !strcmp(pp_argv[first],"--game")) {
        *psz_out_game_dir=pp_argv[first+1];
        game_info(pp_argv[first+1],p_out_info);
        return 1;
    }
    if (strcmp(pp_argv[first],"--title") || (argc != first+2 && (argc != first+4 || strcmp(pp_argv[first+2],"--version"))))
        return 0;
    game_info_of(NULL,pp_argv[first+1],argc == first+4 ? pp_argv[first+3] : "",p_out_info);
    return 1;
}

#ifndef _WIN32
extern char **environ;
/* bbport, and psport releases before the rename, read BB_* names: launchers written for them keep
   working for a transition. Each one set is copied to its PS_* name unless that is set too, then
   the process starts again, since the GPU library's initializers have already read the
   environment by the time main runs. Through run.sh, which copies them first, nothing is left. */
void accept_deprecated_names(char **pp_argv) {
    size_t c_deprecated=0;
    for (char **pp_entry=environ; *pp_entry; ++pp_entry) c_deprecated+=!strncmp(*pp_entry,"BB_",3);
    if (!c_deprecated) return;
    char **pp_found=calloc(c_deprecated,sizeof(*pp_found));
    if (!pp_found) return;
    size_t c_found=0;
    for (char **pp_entry=environ; *pp_entry && c_found<c_deprecated; ++pp_entry)
        if (!strncmp(*pp_entry,"BB_",3) && strchr(*pp_entry,'=')) pp_found[c_found++]=strdup(*pp_entry);
    char sz_list[4096]="";
    size_t cb_used=0, c_copied=0;
    for (size_t i=0; i<c_found; ++i) {
        if (!pp_found[i]) continue;
        char *sz_value=strchr(pp_found[i],'=');
        *sz_value++='\0';
        char sz_renamed[256];
        if (snprintf(sz_renamed,sizeof(sz_renamed),"PS_%s",pp_found[i]+3)<(int)sizeof(sz_renamed) && !getenv(sz_renamed)) {
            setenv(sz_renamed,sz_value,0);
            int wrote=snprintf(sz_list+cb_used,sizeof(sz_list)-cb_used,"%s%s",c_copied?", ":"",pp_found[i]);
            if (wrote>0 && (size_t)wrote<sizeof(sz_list)-cb_used) cb_used+=(size_t)wrote;
            c_copied++;
        }
        free(pp_found[i]);
    }
    free(pp_found);
    if (!c_copied) return;
    printf("psport: BB_* variables are deprecated, use PS_*: %s\n",sz_list);
    execv("/proc/self/exe",pp_argv);
    perror("psport: restart with the PS_* variables");
}
#endif

int answer_query(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "--check-game")) {
        GameInfo info;
        char sz_kind[64], sz_advice[2048];
        game_info(argv[2],&info);
        if (!info.title.pfn_check_files || !info.title.pfn_check_files(&info.files,sz_kind,sizeof(sz_kind),sz_advice,sizeof(sz_advice)))
            return 0;
        printf("%s\t%s\t%s\n",sz_kind,info.sz_serial[0] ? info.sz_serial : "?",info.sz_version[0] ? info.sz_version : "?");
        fprintf(stderr,"%s\n",sz_advice);
        return 2;
    }
    if (argc == 3 && !strcmp(argv[1], "--has-presets")) {
        GameInfo info;
        game_info(argv[2],&info);
        return info.title.has_presets ? 0 : 1;
    }
    const char *mod_folders[256];
    if (argc >= 3 && (!strcmp(argv[1], "--list-patches") || !strcmp(argv[1], "--list-mods"))) {
        GameInfo info;
        const char *sz_game_dir;
        int is_patches=!strcmp(argv[1], "--list-patches");
        if (!query_game(argc,argv,3,&info,&sz_game_dir) || (is_patches && argc == 3)) {
            fprintf(stderr, "Usage: %s %s dir %s(--game dir | --title TITLE_ID [--version APP_VER])%s\n", argv[0], argv[1],
                    is_patches ? "" : "[", is_patches ? "" : "]");
            return 1;
        }
        if (is_patches) {
            LoaderPatchFolder folder=patch_folder(argv[2],&info);
            loader_patches_list(&folder);
        } else {
            LoaderModOptions mods={.sz_mods_dir=argv[2]};
            mod_options(&info,&mods,mod_folders,256);
            loader_mods_list(sz_game_dir,&mods);
        }
        return 0;
    }
    return -1;
}
