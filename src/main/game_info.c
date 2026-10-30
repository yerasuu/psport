// SPDX-License-Identifier: GPL-2.0-or-later
/* What psport knows of the game from its folder or its title: param.sfo, its title's patch file,
 * its external patch folder and its mods' rules. */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "main.h"

/* param.sfo lookup: string or integer value of key; the entry's format (0x0404 an integer), 0 when absent. */
int sfo_value(const char *sz_path, const char *sz_key, char *sz_text, size_t cb_text, uint32_t *p_number) {
    FILE *p_file=ps_host_fs_open_stream(sz_path,"rb");
    if (!p_file) return 0;
    unsigned char data[65536];
    size_t cb_read=fread(data,1,sizeof(data),p_file); fclose(p_file);
    if (cb_read<20 || memcmp(data,"\0PSF",4)) return 0;
    uint32_t keys, values, c_entries;
    memcpy(&keys,data+8,4); memcpy(&values,data+12,4); memcpy(&c_entries,data+16,4);
    for (uint32_t i=0;i<c_entries && 20+i*16+16<=cb_read;++i) {
        const unsigned char *p_entry=data+20+i*16;
        uint16_t off_key, format; uint32_t length, off_data;
        memcpy(&off_key,p_entry,2); memcpy(&format,p_entry+2,2); memcpy(&length,p_entry+4,4); memcpy(&off_data,p_entry+12,4);
        if (keys+off_key>=cb_read || values+off_data+length>cb_read || strcmp((const char *)data+keys+off_key,sz_key)) continue;
        if (format==0x0404 && p_number && length>=4) { memcpy(p_number,data+values+off_data,4); return format; }
        if (sz_text && cb_text) {
            size_t copy=length<cb_text-1 ? length : cb_text-1;
            memcpy(sz_text,data+values+off_data,copy); sz_text[copy]=0;
        }
        return format ? format : 1;
    }
    return 0;
}
/* AppContent's base-game profile: param.sfo's user-defined parameters; the SKU is a launch setting
 * (PS_CONTENT_SKU=full or trial), not a license check. */
void configure_content(const char *sz_sfo) {
    const char *sz_sku=runtime_env("PS_CONTENT_SKU",NULL);
    if (!sz_sku || !*sz_sku) sz_sku="full";
    uint32_t values[5]={0};
    if (!strcmp(sz_sku,"full")) values[0]=3;
    else if (!strcmp(sz_sku,"trial")) values[0]=1;
    else fail("PS_CONTENT_SKU must be full or trial");
    FILE *p_file=ps_host_fs_open_stream(sz_sfo,"rb");
    if (!p_file) { fprintf(stderr,"ERROR: cannot read %s\n",sz_sfo); runtime_exit(1); }
    fclose(p_file);
    for (unsigned i=0;i<4;++i) {
        char sz_key[32];
        snprintf(sz_key,sizeof(sz_key),"USER_DEFINED_PARAM_%u",i+1);
        int format=sfo_value(sz_sfo,sz_key,NULL,0,&values[1+i]);
        if (format && format!=0x0404) fail("invalid user-defined parameter in param.sfo");
    }
    runtime_content_configure(values);
    printf("AppContent profile: SKU=%s (probe setting), user params=[%u, %u, %u, %u], mounted add-ons=0\n",
           sz_sku,values[1],values[2],values[3],values[4]);
}
/* psport's own folder, the one holding patches/ (kept as its marker): PS_INSTALL_DIR when set,
 * else the nearest folder above the executable with a patches/ folder, since the executable sits
 * at different depths below it (out/psport, out/win32/psport.exe, a package's bin/psport, or
 * beside patches/). */
static void install_dir(char *sz_out, size_t cb_out) {
    const char *sz_set=runtime_env("PS_INSTALL_DIR","");
    if (*sz_set) {
        if ((size_t)snprintf(sz_out,cb_out,"%s",sz_set)>=cb_out) fail("PS_INSTALL_DIR too long");
        return;
    }
    char sz_exe[4096];
    if (ps_host_exe_path(sz_exe,sizeof(sz_exe))<0) {
        fprintf(stderr,"psport: the executable's path is unknown: install folder taken as the current one (set PS_INSTALL_DIR)\n");
        snprintf(sz_out,cb_out,".");
        return;
    }
    if ((size_t)snprintf(sz_out,cb_out,"%s",sz_exe)>=cb_out) fail("install path too long");
    for (char *p_slash=strrchr(sz_out,'/'); p_slash; p_slash=strrchr(sz_out,'/')) {
        *p_slash=0;
        char sz_patches[4200];
        PsHostStat stat;
        snprintf(sz_patches,sizeof(sz_patches),"%s/patches",sz_out);
        if (!ps_host_fs_stat(sz_patches,&stat) && PS_HOST_MODE_IS_DIRECTORY(stat.mode)) {
            if (!sz_out[0]) snprintf(sz_out,cb_out,"/");
            return;
        }
    }
    fprintf(stderr,"psport: no patches/ folder above %s: install folder taken as the current one (set PS_INSTALL_DIR)\n",sz_exe);
    snprintf(sz_out,cb_out,".");
}
static void install_path(const char *sz_relative, char *sz_out, size_t cb_out) {
    char sz_dir[4096];
    install_dir(sz_dir,sizeof(sz_dir));
    if ((size_t)snprintf(sz_out,cb_out,"%s/%s",sz_dir,sz_relative)>=cb_out) fail("install path too long");
}
static const char *s_sz_builtin_patches_dir;
void builtin_patches_configure(const char *sz_dir) { s_sz_builtin_patches_dir=sz_dir; }
static const char *file_name(const char *sz_path) {
    const char *p_slash=strrchr(sz_path,'/');
    return p_slash ? p_slash+1 : sz_path;
}
/* The title's built-in patch file is the launcher's (--builtin-patches-dir, else
 * PS_BUILTIN_PATCHES_DIR); without a folder, psport's own folder, where older checkouts keep it.
 * sz_patch_file stays "" when the file is not there. */
static void resolve_builtin_patch_file(const char *sz_title_file, GameInfo *p_info) {
    const char *sz_dir=s_sz_builtin_patches_dir ? s_sz_builtin_patches_dir : runtime_env("PS_BUILTIN_PATCHES_DIR","");
    const size_t cb_path=sizeof(p_info->sz_patch_file);
    if ((size_t)snprintf(p_info->sz_patch_folder,sizeof(p_info->sz_patch_folder),"%s",sz_dir)>=sizeof(p_info->sz_patch_folder) ||
        (*sz_dir && (size_t)snprintf(p_info->sz_patch_file,cb_path,"%s/%s",sz_dir,file_name(sz_title_file))>=cb_path)) {
        p_info->sz_patch_file[0]=0;
        return;
    }
    if (!*sz_dir) install_path(sz_title_file,p_info->sz_patch_file,cb_path);
    PsHostStat stat;
    if (ps_host_fs_stat(p_info->sz_patch_file,&stat) || !PS_HOST_MODE_IS_REGULAR(stat.mode)) p_info->sz_patch_file[0]=0;
}
int announce_builtin_patch_file(const GameInfo *p_info) {
    const char *sz_title_file=p_info->title.sz_patch_file;
    if (!sz_title_file) return 1;
    if (!p_info->sz_patch_file[0]) {
        printf("Patches: the title's built-in patch file %s was not found in %s: its built-in patches are skipped "
               "(set PS_BUILTIN_PATCHES_DIR)\n",file_name(sz_title_file),
               p_info->sz_patch_folder[0] ? p_info->sz_patch_folder : "no folder given");
        return 0;
    }
    printf("Patches: the title's built-in patch file is %s%s\n",p_info->sz_patch_file,
           p_info->sz_patch_folder[0] ? "" : " (psport's folder: no PS_BUILTIN_PATCHES_DIR given)");
    return 1;
}
/* sz_game_dir may be NULL: a launcher asking about a title by its TITLE_ID and APP_VER. */
void game_info_of(const char *sz_game_dir, const char *sz_serial, const char *sz_version, GameInfo *p_out_info) {
    memset(p_out_info,0,sizeof(*p_out_info));
    snprintf(p_out_info->sz_serial,sizeof(p_out_info->sz_serial),"%s",sz_serial);
    snprintf(p_out_info->sz_version,sizeof(p_out_info->sz_version),"%s",sz_version);
    p_out_info->title=psport_gpu_title(p_out_info->sz_serial);
    if (p_out_info->sz_serial[0]) p_out_info->serials[p_out_info->c_serials++]=p_out_info->sz_serial;
    for (size_t i=0; i<p_out_info->title.c_serials && p_out_info->c_serials<64; ++i)
        p_out_info->serials[p_out_info->c_serials++]=p_out_info->title.pp_serials[i];
    const char *sz_patch_file=p_out_info->title.sz_patch_file;
    if (sz_patch_file) resolve_builtin_patch_file(sz_patch_file,p_out_info);
    p_out_info->files=(PsGameFiles){sz_game_dir,p_out_info->sz_serial,p_out_info->sz_version,
                                    p_out_info->sz_patch_file[0] ? p_out_info->sz_patch_file : NULL,loader_eboot_sha256};
}
void game_info(const char *sz_game_dir, GameInfo *p_out_info) {
    char sz_sfo[4096], sz_serial[16]="", sz_version[16]="";
    snprintf(sz_sfo,sizeof(sz_sfo),"%s/sce_sys/param.sfo",sz_game_dir);
    sfo_value(sz_sfo,"TITLE_ID",sz_serial,sizeof(sz_serial),NULL);
    sfo_value(sz_sfo,"APP_VER",sz_version,sizeof(sz_version),NULL);
    game_info_of(sz_game_dir,sz_serial,sz_version,p_out_info);
}
/* The game's external patch folder for loader_patches.c: its title's own file left out. */
LoaderPatchFolder patch_folder(const char *sz_dir, const GameInfo *p_info) {
    return (LoaderPatchFolder){sz_dir,p_info->sz_patch_file[0] ? p_info->sz_patch_file : NULL,p_info->sz_version,
                               p_info->serials,p_info->c_serials,p_info->title.pfn_external_patch_refusal,
                               p_info->title.pfn_replaced_patch_consequence};
}
RuntimeQuirkLaunch quirk_launch(const GameInfo *p_info, uint64_t traits) {
    return (RuntimeQuirkLaunch){p_info->title.sz_name,p_info->sz_serial,p_info->title.p_quirks,p_info->title.c_quirks,traits};
}
/* The mods rules' game data: wrapper folders named after any TITLE_ID of the game's executable,
 * the folders of dvdroot_ps4 its title knows. */
void mod_options(const GameInfo *p_info, LoaderModOptions *p_options, const char **pp_folders, size_t c_capacity) {
    p_options->pp_serials=p_info->serials;
    p_options->c_serials=p_info->c_serials;
    p_options->pp_folders=pp_folders;
    for (p_options->c_folders=0; p_options->c_folders<c_capacity && p_options->c_folders<p_info->title.c_mod_folders; ++p_options->c_folders)
        pp_folders[p_options->c_folders]=p_info->title.pp_mod_folders[p_options->c_folders];
}
