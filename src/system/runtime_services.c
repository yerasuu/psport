/* System services for an offline, single-user console:
 *   - one local user (id 1) who is logged in but not signed in to PSN;
 *   - network cable unplugged: NetCtl disconnected, sockets on loopback only (runtime_socket.c),
 *     no HTTP transfer (runtime_http.c);
 *   - common dialogs complete immediately (no UI is drawn yet), except MsgDialog, which the overlay
 *     shows until the player answers;
 *   - no PlayStation VR headset is connected (runtime_vr.c).
 * Every entry here is an explicit contract; unknown functions still stop. */
#define _GNU_SOURCE
#include "runtime.h"
#include "gpu/psport_gpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <pthread.h>
#include <time.h>
#include <arpa/inet.h>
#include <sys/stat.h>

#define USER_ID 1
#define DEFAULT_USER_NAME "Player"
#define ORBIS_OK 0
#define USER_NOT_INITIALIZED ((int32_t)0x80960002)
#define USER_ALREADY_INITIALIZED ((int32_t)0x80960003)
#define USER_INVALID_ARGUMENT ((int32_t)0x80960005)
#define USER_NO_EVENT ((int32_t)0x80960007)
#define SYSTEM_NO_EVENT ((int32_t)0x80A10004)
#define SYSTEM_PARAMETER ((int32_t)0x80A10003)
#define SYSTEM_INTERNAL ((int32_t)0x80A10001)
#define NET_CTL_NOT_CONNECTED ((int32_t)0x80412108)
#define NET_CTL_INVALID_ADDR ((int32_t)0x80412107)
#define NP_SIGNED_OUT ((int32_t)0x80550006)
#define NP_INVALID_ARGUMENT ((int32_t)0x80550003)
#define NET_ENETUNREACH ((int32_t)0x80410133)
#define DIALOG_NOT_INITIALIZED ((int32_t)0x80B80003)
#define AUDIO_IN_NOT_OPENED ((int32_t)0x80260109)
#define MOVE_NO_CONTROLLER 1
#define MOVE_NOT_INIT ((int32_t)0x80EE0001)
#define MOVE_ALREADY_INIT ((int32_t)0x80EE0002)
#define MOVE_INVALID_ARG ((int32_t)0x80EE0003)

static pthread_mutex_t s_lock=PTHREAD_MUTEX_INITIALIZER;
static int s_next_id=1;
static int new_id(void) { pthread_mutex_lock(&s_lock); int id=s_next_id++; pthread_mutex_unlock(&s_lock); return id; }
static void note(const char *sz_what) { printf("Runtime: %s\n",sz_what); }

/* ---- UserService ---- */
static int s_is_login_event_pending=1;
static int s_is_user_initialized;
/* The service thread's priority: 700 without parameters, else within 0x100-0x2FF. No thread runs here. */
static int32_t user_start(int32_t priority) {
    if (priority<0x100 || priority>0x2FF) return USER_INVALID_ARGUMENT;
    if (__atomic_exchange_n(&s_is_user_initialized,1,__ATOMIC_ACQ_REL)) return USER_ALREADY_INITIALIZED;
    note("UserService initialized (user 1 logged in)");
    return 0;
}
static ABI int32_t user_initialize(const int32_t *p_params) { return user_start(p_params ? p_params[0] : 700); }
static ABI int32_t user_initialize2(int32_t priority, uint64_t cpu_mask) { (void)cpu_mask; return user_start(priority); }
static ABI int32_t user_terminate(void) {
    return __atomic_exchange_n(&s_is_user_initialized,0,__ATOMIC_ACQ_REL) ? 0 : USER_NOT_INITIALIZED;
}
static ABI int32_t user_initial(int32_t *p_id) { if (!p_id) return USER_INVALID_ARGUMENT; *p_id=USER_ID; return 0; }
static ABI int32_t user_list(int32_t *p_ids) {
    if (!p_ids) return USER_INVALID_ARGUMENT;
    p_ids[0]=USER_ID; p_ids[1]=p_ids[2]=p_ids[3]=-1; return 0;
}
static ABI int32_t user_name(int32_t id,char *sz_name,uint64_t cb_name) {
    if (id!=USER_ID || !sz_name) return USER_INVALID_ARGUMENT;
    const char *sz_value=runtime_env("PS_USER_NAME",DEFAULT_USER_NAME);
    if (strlen(sz_value)+1>cb_name) return (int32_t)0x8096000a; /* BUFFER_TOO_SHORT */
    strcpy(sz_name,sz_value); return 0;
}
static ABI int32_t user_color(int32_t id,int32_t *p_color) {
    if (id!=USER_ID || !p_color) return USER_INVALID_ARGUMENT;
    *p_color=0; return 0;
}
static ABI int32_t user_event(int32_t *p_event) {
    if (!p_event) return USER_INVALID_ARGUMENT;
    pthread_mutex_lock(&s_lock);
    int is_pending=s_is_login_event_pending; s_is_login_event_pending=0;
    pthread_mutex_unlock(&s_lock);
    if (!is_pending) return USER_NO_EVENT;
    p_event[0]=0; p_event[1]=USER_ID; /* LOGIN */
    return 0;
}

/* ---- SystemService ---- */
int runtime_system_language(void) { const char *sz_value=runtime_env("PS_LANGUAGE",NULL); return sz_value ? atoi(sz_value) : 1; }
static ABI int32_t system_param(int32_t id,int32_t *p_value) {
    if (!p_value) return SYSTEM_PARAMETER;
    switch (id) {
    case 1: *p_value=runtime_system_language(); break; /* language (1 = English US, 8 = Russian) */
    case 2: *p_value=1; break;                    /* date format DD/MM/YYYY */
    case 3: *p_value=1; break;                    /* 24-hour clock */
    case 4: case 5: {                             /* time zone (standard, minutes east), summer time */
        int32_t standard_sec=0, dst_sec=0;
        const int e=ps_host_time_zone_at((int64_t)time(NULL),&standard_sec,&dst_sec);
        if (e) fprintf(stderr,"Runtime: host time zone unreadable (errno %d), reporting UTC\n",e);
        /* Summer time is in minutes (kb #772). A negative daylight saving (Europe/Dublin's winter)
         * goes into the zone, so that summer time never reads negative and zone plus summer time
         * stays the offset in force. */
        *p_value=id==4 ? (standard_sec+(dst_sec<0 ? dst_sec : 0))/60 : (dst_sec>0 ? dst_sec : 0)/60;
        break;
    }
    case 7: *p_value=0; break;                    /* parental level off */
    case 1000: *p_value=1; break;                 /* enter button = cross */
    default: fprintf(stderr,"STOP: unsupported system parameter %d\n",id); runtime_stop(21);
    }
    return 0;
}
/* The only string parameter is the console's name (6, SYSTEM_NAME). */
static ABI int32_t system_param_string(int32_t id,char *sz_buffer,uint64_t cb_buffer) {
    static const char s_sz_name[]="PS4";
    if (id!=6 || !sz_buffer || cb_buffer<sizeof(s_sz_name)) return SYSTEM_PARAMETER;
    memcpy(sz_buffer,s_sz_name,sizeof(s_sz_name));
    return 0;
}
/* SceSystemServiceHdrToneMapLuminance in nits: max full frame, max, min. The console's HDR
 * adjustment is never run here, so a common HDR10 display's range stands in for it. */
static ABI int32_t hdr_tone_map_luminance(float *p_luminance) {
    if (!p_luminance) return SYSTEM_PARAMETER;
    p_luminance[0]=1000.0f; p_luminance[1]=1000.0f; p_luminance[2]=0.01f;
    return 0;
}
static ABI int32_t system_status(unsigned char *p_status) {
    if (!p_status) return SYSTEM_PARAMETER;
    memset(p_status,0,12); /* event_num=0, no overlay, foreground, normal CPU mode */
    return 0;
}
static ABI int32_t system_event(void *p_event) { (void)p_event; return SYSTEM_NO_EVENT; }
static ABI int32_t hide_splash(void) { note("SystemService: splash screen hidden"); return 0; }
static ABI int32_t launch_browser(void) { note("SystemService: web browser request ignored (offline)"); return 0; }
/* The system's controller settings screen, like the common dialogs, has no UI here: it closes at
 * once with nothing changed. */
static ABI int32_t show_controller_settings(int32_t h_user) {
    (void)h_user;
    note("SystemService: controller settings requested; closed at once (no system UI yet)");
    return 0;
}
/* The share of the screen the user's display settings leave for UI: all of it. A title that sizes
 * its render targets by it (Atelier Ryza) gets a 0x0 target and a null texture from 0. */
static ABI int32_t display_safe_area(float *p_ratio) {
    if (!p_ratio) return (int32_t)0x80A10003; /* SCE_SYSTEM_SERVICE_ERROR_PARAMETER */
    *p_ratio=1.0f;
    return 0;
}
/* The console ends the application and starts another executable of its package; the port starts
 * itself again on it (runtime_load_exec), so this returns only when that failed. */
static ABI int32_t load_exec(const char *sz_path, char *const *pp_arguments) {
    char sz_guest[1024], sz_host[4096];
    PsHostStat info;
    if (sz_path) snprintf(sz_guest,sizeof(sz_guest),sz_path[0]=='/' ? "%s" : "/app0/%s",sz_path);
    if (!sz_path || strncmp(sz_guest,"/app0/",6) || runtime_file_translate(sz_guest,sz_host,sizeof(sz_host)) ||
        ps_host_fs_stat(sz_host,&info) || !PS_HOST_MODE_IS_REGULAR(info.mode)) {
        printf("Runtime: sceSystemServiceLoadExec: %s is no executable of the package\n",sz_path ? sz_path : "(null)");
        return SYSTEM_PARAMETER;
    }
    runtime_load_exec(sz_guest,pp_arguments);
    return SYSTEM_INTERNAL;
}

/* ---- NetCtl / Net / Ssl: no network ---- */
static ABI int32_t net_unreachable(void) { *runtime_net_errno()=51; return NET_ENETUNREACH; }
static ABI int32_t net_epoll_create(const char *sz_name,int flags) { (void)sz_name; (void)flags; return new_id(); }
static ABI int32_t net_resolver_create(const char *sz_name,int pool,int flags) { (void)sz_name; (void)pool; (void)flags; return new_id(); }
/* Every lookup fails at once offline (sceNetResolverStartNtoa/Aton), so none is left to abort. */
static ABI int32_t net_resolver_abort(int32_t h_resolver, int32_t flags) { (void)h_resolver; (void)flags; return 0; }
static ABI uint16_t net_htons(uint16_t v) { return htons(v); }
static ABI uint16_t net_ntohs(uint16_t v) { return ntohs(v); }
static ABI uint32_t net_htonl(uint32_t v) { return htonl(v); }
static ABI uint32_t net_ntohl(uint32_t v) { return ntohl(v); }
static ABI int32_t netctl_state(int32_t *p_state) { if (!p_state) return NET_CTL_INVALID_ADDR; *p_state=0; return 0; }
/* The interface's own address does not need a connection; everything else does. */
static ABI int32_t netctl_info(int code,void *p_info) {
    if (code!=2) return NET_CTL_NOT_CONNECTED; /* SCE_NET_CTL_INFO_ETHER_ADDR */
    if (!p_info) return NET_CTL_INVALID_ADDR;
    runtime_net_ether_address(p_info);
    return 0;
}
static ABI int32_t netctl_init(void) { note("NetCtl initialized (network cable unplugged)"); return 0; }
static ABI int32_t netctl_register(void *p_callback,void *p_arg,int32_t *p_cid) { (void)p_arg; if (!p_callback || !p_cid) return NET_CTL_INVALID_ADDR; *p_cid=new_id(); return 0; }
static ABI int32_t netctl_nat(uint32_t *p_info) {
    if (!p_info) return NET_CTL_INVALID_ADDR;
    p_info[1]=0; p_info[2]=3; p_info[3]=0; /* stun failed, NAT type 3, no mapped address */
    return 0;
}

/* ---- NP (PSN): signed out ---- */
static ABI int32_t np_state(int32_t user,int32_t *p_state) {
    if (!p_state) return NP_INVALID_ARGUMENT;
    (void)user; *p_state=1; /* SIGNED_OUT */
    return 0;
}
static ABI int32_t np_signed_out(void) { return NP_SIGNED_OUT; }
static ABI int32_t np_account_id(int32_t user,uint64_t *p_id) {
    if (!p_id) return NP_INVALID_ARGUMENT;
    (void)user; *p_id=0; return NP_SIGNED_OUT;
}
static ABI int32_t np_account_id_online(const void *p_online_id,uint64_t *p_id) {
    if (!p_online_id || !p_id) return NP_INVALID_ARGUMENT;
    *p_id=0; return NP_SIGNED_OUT;
}
static ABI int32_t np_account_info(const void *p_online_id,void *p_info) {
    return p_online_id && p_info ? NP_SIGNED_OUT : NP_INVALID_ARGUMENT;
}
static ABI int32_t np_register(void *p_callback,void *p_arg) { (void)p_callback; (void)p_arg; return new_id(); }
/* Unplugged and signed out, no state change, NetCtl event or request reply ever becomes due, so a
 * check of the registered callbacks or of request timeouts has nothing to run. */
static ABI int32_t nothing_due(void) { return 0; }
static ABI void np_register_void(void *p_callback,void *p_arg) { (void)p_callback; (void)p_arg; }
static ABI int32_t np_register_toolkit(void *p_callback,void *p_arg) { (void)p_arg; return p_callback ? 0 : NP_INVALID_ARGUMENT; }
static ABI int32_t np_request(const void *p_param) { (void)p_param; return new_id(); }
static ABI int32_t np_request_ctx(int32_t ctx,const void *p_param) { (void)ctx; (void)p_param; return new_id(); }
static ABI int32_t np_poll(int32_t request,int32_t *p_result) { (void)request; if (p_result) *p_result=NP_SIGNED_OUT; return 0; }
static ABI int32_t np_compare(const void *p_a,const void *p_b) {
    if (!p_a || !p_b) return NP_INVALID_ARGUMENT;
    return memcmp(p_a,p_b,16) ? (int32_t)0x80550609 : 0; /* NP_UTIL NOT_MATCH */
}

/* ---- Common dialogs: nothing is displayed; an opened dialog finishes (MsgDialog excepted). ---- */
typedef struct { const char *sz_name; int is_initialized, status; } Dialog;
static Dialog s_dialogs[]={{"CommonDialog",0,0},{"MsgDialog",0,0},{"SaveDataDialog",0,0},
                         {"NpProfileDialog",0,0},{"NpCommerceDialog",0,0},{"ImeDialog",0,0},
                         {"ErrorDialog",0,0},{"WebBrowserDialog",0,0},{"InvitationDialog",0,0},
                         {"SigninDialog",0,0},{"SocialScreenDialog",0,0},
                         {"HmdSetupDialog",0,0},{"PlayGoDialog",0,0}};
static int s_is_common_initialized;
static ABI int32_t common_init(void) { s_is_common_initialized=1; return 0; }
static int32_t dialog_init(int i) {
    if (s_dialogs[i].is_initialized) return (int32_t)0x80B80004;
    s_dialogs[i].is_initialized=1; s_dialogs[i].status=1; return 0;
}
static int32_t dialog_open(int i) {
    if (!s_dialogs[i].is_initialized) return DIALOG_NOT_INITIALIZED;
    printf("Runtime: %s opened; completed immediately (no dialog UI yet)\n",s_dialogs[i].sz_name);
    s_dialogs[i].status=3; return 0;
}
static int32_t dialog_status(int i) { return s_dialogs[i].status; }
static int32_t dialog_term(int i) {
    if (!s_dialogs[i].is_initialized) return DIALOG_NOT_INITIALIZED;
    s_dialogs[i].is_initialized=0; s_dialogs[i].status=0; return 0;
}
#define DIALOG(tag,i) \
    static ABI int32_t tag##_init(void) { return dialog_init(i); } \
    static ABI int32_t tag##_open(const void *p) { (void)p; return dialog_open(i); } \
    static ABI int32_t tag##_status(void) { return dialog_status(i); } \
    static ABI int32_t tag##_term(void) { return dialog_term(i); }
DIALOG(profile,3) DIALOG(commerce,4)
DIALOG(web,7) DIALOG(invitation,8) DIALOG(signin,9) DIALOG(social,10) DIALOG(hmd_setup,11) DIALOG(playgo,12)
static ABI int32_t profile_result(void *p_result) { if (p_result) memset(p_result,0,4); return 0; }
/* OrbisNpCommerceDialogParam keeps userData at 0x58 and the result hands it back. Signed out, the
 * store cannot open, so the dialog ends as backed out of: USER_CANCELED, nothing authorized. */
static void *s_p_commerce_user_data;
static ABI int32_t commerce_start(const uint8_t *p_param) {
    if (p_param) memcpy(&s_p_commerce_user_data,p_param+0x58,sizeof(s_p_commerce_user_data));
    return commerce_open(p_param);
}
typedef struct { int32_t result; uint8_t is_authorized; uint8_t reserved_0[3]; void *p_user_data; uint8_t reserved[32]; } CommerceResult;
static ABI int32_t commerce_result(CommerceResult *p_out_result) {
    if (!p_out_result) return (int32_t)0x80B8000D;           /* ARG_NULL */
    if (s_dialogs[4].status!=3) return (int32_t)0x80B80005;   /* NOT_FINISHED */
    memset(p_out_result,0,sizeof(*p_out_result));
    p_out_result->result=1;
    p_out_result->p_user_data=s_p_commerce_user_data;
    return 0;
}
/* The user cannot sign in to PSN here, so the sign-in dialog always ends cancelled. */
static ABI int32_t signin_result(int32_t *p_out_result) {
    if (!p_out_result) return (int32_t)0x80B8000D;           /* ARG_NULL */
    if (s_dialogs[9].status!=3) return (int32_t)0x80B80005;   /* NOT_FINISHED */
    *p_out_result=1;
    return 0;
}
typedef struct {
    void *callback_arg; int32_t error_code, result; uint32_t *sent_count; uint8_t reserved[32];
} InvitationResult;
static void *s_p_invitation_callback_arg;
static ABI int32_t invitation_start(const uint8_t *p_param) {
    if (p_param) memcpy(&s_p_invitation_callback_arg,p_param+64,sizeof(s_p_invitation_callback_arg));
    return invitation_open(p_param);
}
static ABI int32_t invitation_result(InvitationResult *p_result) {
    if (!s_dialogs[8].is_initialized) return DIALOG_NOT_INITIALIZED;
    if (s_dialogs[8].status!=3) return (int32_t)0x80B80005;
    if (!p_result) return (int32_t)0x80B8000D;
    p_result->callback_arg=s_p_invitation_callback_arg; p_result->error_code=0; p_result->result=0;
    if (p_result->sent_count) *p_result->sent_count=0;
    return 0;
}
/* With nothing displayed, a dialog is ready to show whenever it is initialized (Atelier Ayesha
 * waited on these before opening its auto-load dialog; answered 0 they never turned true). */
static ABI int32_t save_ready(void) { return s_dialogs[2].is_initialized; }
/* OrbisSaveDataDialogParam (0x98 bytes): common base 0x30, size, mode at 0x34, items at 0x48,
 * user message parameter at 0x50 (its buttonType first), userData at 0x70. */
static struct { int32_t mode; void *p_user_data; char sz_dir[32]; _Alignas(8) uint8_t param[0x530]; } s_save_dialog;
/* OrbisSaveDataDialogItems: userId, titleId at 8, dirName array and its count at 0x10 and 0x18,
 * newItem at 0x20, focusPos at 0x28, focusPosDirName at 0x30. The list shows the saves that exist
 * and is accepted on the item it opens on; the new item, which heads the list, leaves dir empty. */
static void save_list_focus(const uint8_t *p_items) {
    memset(s_save_dialog.sz_dir,0,sizeof(s_save_dialog.sz_dir));
    if (!p_items) return;
    int32_t user, focus;
    uint32_t c_items;
    const char *sz_title, *sz_dirs, *sz_new_item, *sz_focus_dir;
    memcpy(&user,p_items,sizeof(user));
    memcpy(&sz_title,p_items+0x08,sizeof(sz_title));
    memcpy(&sz_dirs,p_items+0x10,sizeof(sz_dirs));
    memcpy(&c_items,p_items+0x18,sizeof(c_items));
    memcpy(&sz_new_item,p_items+0x20,sizeof(sz_new_item));
    memcpy(&focus,p_items+0x28,sizeof(focus));
    memcpy(&sz_focus_dir,p_items+0x30,sizeof(sz_focus_dir));
    if (sz_dirs && !(focus==0 && sz_new_item)) {
        _Alignas(8) uint8_t param[sizeof(s_save_dialog.param)];
        int64_t best=0;
        for (uint32_t i=0;i<c_items;++i) {
            const char *sz_dir=sz_dirs+32*i;
            if (runtime_savedata_param(user,sz_title,sz_dir,param)) continue;
            int64_t mtime;
            memcpy(&mtime,param+0x508,sizeof(mtime));
            int take;
            switch (focus) {
            case 1: case 3: take=1; break;                                     /* LISTTAIL, DATATAIL */
            case 4: take=!s_save_dialog.sz_dir[0] || mtime>best; break;             /* DATALATEST */
            case 5: take=!s_save_dialog.sz_dir[0] || mtime<best; break;             /* DATAOLDEST */
            case 6: take=sz_focus_dir && !strncmp(sz_dir,sz_focus_dir,32); break;       /* DIRNAME */
            default: take=!s_save_dialog.sz_dir[0]; break;                          /* LISTHEAD, DATAHEAD */
            }
            if (!take) continue;
            best=mtime;
            memcpy(s_save_dialog.sz_dir,sz_dir,sizeof(s_save_dialog.sz_dir));
            memcpy(s_save_dialog.param,param,sizeof(param));
        }
    }
    if (s_save_dialog.sz_dir[0])
        printf("Runtime: SaveDataDialog list (focus %d, %u items) answered with '%.32s'\n",focus,c_items,s_save_dialog.sz_dir);
    else
        printf("Runtime: SaveDataDialog list (focus %d, %u items) answered with %s\n",focus,c_items,
               sz_new_item ? "the new item" : "no save (none of its items exists)");
}
static ABI int32_t save_init(void) { return dialog_init(2); }
static ABI int32_t save_status(void) { return dialog_status(2); }
static ABI int32_t save_term(void) { return dialog_term(2); }
/* A progress bar or a user message without buttons has nothing for the user to answer: it stays
 * running until the game closes it (Atelier Ryza reopened a progress bar that ended by itself, in
 * a loop, and never wrote its save). */
static ABI int32_t save_open(const uint8_t *p_param) {
    if (!s_dialogs[2].is_initialized) return DIALOG_NOT_INITIALIZED;
    if (!p_param) return (int32_t)0x80B8000D; /* SCE_COMMON_DIALOG_ERROR_ARG_NULL */
    int32_t mode;
    const int32_t *p_user_msg;
    memcpy(&mode,p_param+0x34,sizeof(mode));
    memcpy(&p_user_msg,p_param+0x50,sizeof(p_user_msg));
    memcpy(&s_save_dialog.p_user_data,p_param+0x70,sizeof(s_save_dialog.p_user_data));
    s_save_dialog.mode=mode;
    if (mode==1) {
        const uint8_t *p_items;
        memcpy(&p_items,p_param+0x48,sizeof(p_items));
        save_list_focus(p_items);
    }
    if (mode==5 || (mode==2 && p_user_msg && p_user_msg[0]==2)) {
        printf("Runtime: SaveDataDialog opened (mode %d); runs until the game closes it (no dialog UI yet)\n",mode);
        s_dialogs[2].status=2;
        return 0;
    }
    return dialog_open(2);
}
/* OrbisSaveDataDialogResult: mode, result, buttonId, pad, then the caller's own dirName, param
 * and userData pointers (shadPS4's savedatadialog_ui.h). A dialog with no UI ends as if the user
 * accepted it: result OK, button OK/YES, the opened mode and userData, and for a list the chosen
 * directory and its param in the caller's buffers (Atelier Ryza mounted the empty name it got and
 * failed to load). Answered by a stub, Atelier Ayesha read the result it never got
 * and faulted. */
static ABI int32_t save_result(int32_t *p_result) {
    if (!p_result) return (int32_t)0x80B8000D; /* SCE_COMMON_DIALOG_ERROR_ARG_NULL */
    if (s_dialogs[2].status!=3) return (int32_t)0x80B80005; /* SCE_COMMON_DIALOG_ERROR_NOT_FINISHED */
    p_result[0]=s_save_dialog.mode;
    p_result[1]=0;
    p_result[2]=1;
    memcpy(p_result+8,&s_save_dialog.p_user_data,sizeof(s_save_dialog.p_user_data));
    if (s_save_dialog.mode==1) {
        char *sz_dir;
        void *p_param;
        memcpy(&sz_dir,p_result+4,sizeof(sz_dir));
        memcpy(&p_param,p_result+6,sizeof(p_param));
        if (sz_dir) memcpy(sz_dir,s_save_dialog.sz_dir,sizeof(s_save_dialog.sz_dir));
        if (p_param && s_save_dialog.sz_dir[0] && s_save_dialog.param[0]) memcpy(p_param,s_save_dialog.param,sizeof(s_save_dialog.param));
    }
    return 0;
}
static ABI int32_t save_close(const void *p_param) { (void)p_param; s_dialogs[2].status=3; return 0; }
/* MsgDialog runs as on the console, from Open until the player answers or the game closes it: a game
 * that polls UpdateStatus sees RUNNING (2) meanwhile, and code it runs only then (Bloodborne's boot
 * dialog after a session that did not end with Quit Game) runs. The overlay draws it; without a
 * window, or with PS_MSGDIALOG_AUTO_MS=<ms>, the default button is chosen once that time has passed
 * (1000 ms without a window) so unattended runs go on. Dialogs with no button to choose (progress
 * bars, WAIT) are never auto-answered: the game closes them.
 * OrbisMsgDialogParam: common base (0x30), size, mode at 0x38, then the user message, progress bar
 * and system message parameters at 0x40, 0x48 and 0x50. The user message: button type, message
 * at 8, ButtonsParam (two labels) at 0x10; the progress bar: type, message at 8; the system
 * message: its type. OrbisMsgDialogResult: mode, result, buttonId, reserved[32]. */
enum { MSG_MODE_USER=1, MSG_MODE_PROGRESS_BAR=2, MSG_MODE_SYSTEM=3 };
enum { MSG_BUTTONS_OK, MSG_BUTTONS_YESNO, MSG_BUTTONS_NONE, MSG_BUTTONS_OK_CANCEL, MSG_BUTTONS_CANCEL, MSG_BUTTONS_WAIT,
       MSG_BUTTONS_WAIT_CANCEL, MSG_BUTTONS_YESNO_FOCUS_NO, MSG_BUTTONS_OK_CANCEL_FOCUS_CANCEL, MSG_BUTTONS_2BUTTONS };
#define MSG_RESULT_OK 0
#define MSG_RESULT_USER_CANCELED 1
#define MSG_BUTTON_ID_INVALID 0
#define DIALOG_NOT_FINISHED ((int32_t)0x80B80005)
#define DIALOG_INVALID_STATE ((int32_t)0x80B80006)
#define DIALOG_NOT_RUNNING ((int32_t)0x80B8000B)
#define DIALOG_ARG_NULL ((int32_t)0x80B8000D)
#define DIALOG_PARAM_INVALID ((int32_t)0x80B8000A)
static pthread_mutex_t s_msg_lock=PTHREAD_MUTEX_INITIALIZER;
static struct {
    int32_t mode, result, button_id, button_type, system_type;
    int c_buttons, i_default, can_cancel, is_shown;
    int32_t button_ids[2];  /* per button; MSG_BUTTON_ID_INVALID is the cancel button */
    uint64_t auto_at_ns;    /* 0: waits for the player or the game */
} s_msg;
static uint64_t s_c_msg_opened, s_c_msg_by_player, s_c_msg_auto, s_c_msg_closed, s_c_msg_refused;
static void msg_summary(char *sz_out, size_t cb_out) {
    snprintf(sz_out,cb_out,"Runtime: MsgDialog: %llu opened, %llu answered by the player, %llu auto-answered, %llu closed "
             "by the game while running, %llu opens refused\n",
             (unsigned long long)__atomic_load_n(&s_c_msg_opened,__ATOMIC_RELAXED),
             (unsigned long long)__atomic_load_n(&s_c_msg_by_player,__ATOMIC_RELAXED),
             (unsigned long long)__atomic_load_n(&s_c_msg_auto,__ATOMIC_RELAXED),
             (unsigned long long)__atomic_load_n(&s_c_msg_closed,__ATOMIC_RELAXED),
             (unsigned long long)__atomic_load_n(&s_c_msg_refused,__ATOMIC_RELAXED));
}
static void msg_finish(int32_t result, int32_t button_id) {
    s_msg.result=result; s_msg.button_id=button_id; s_dialogs[1].status=3;
    if (s_msg.is_shown) psport_gpu_message_end();
    s_msg.is_shown=0;
}
/* libSceMsgDialog's GetResult, for a game built with SDK 3.00 or later, turns backing out of an OK
 * or yes/no message, or of a system message other than types 3 and 5, into its button (kb #818). */
static int32_t msg_back_button(void) {
    if (runtime_process_sdk_version()<0x03000000u) return MSG_BUTTON_ID_INVALID;
    if (s_msg.mode==MSG_MODE_SYSTEM) return s_msg.system_type==3 || s_msg.system_type==5 ? MSG_BUTTON_ID_INVALID : 1;
    if (s_msg.mode!=MSG_MODE_USER) return MSG_BUTTON_ID_INVALID;
    if (s_msg.button_type==MSG_BUTTONS_OK) return 1;
    return s_msg.button_type==MSG_BUTTONS_YESNO || s_msg.button_type==MSG_BUTTONS_YESNO_FOCUS_NO ? 2 : MSG_BUTTON_ID_INVALID;
}
static void msg_choose(int i_button, const char *sz_how) {
    const int32_t button_id=i_button<0 ? msg_back_button() : s_msg.button_ids[i_button];
    const int32_t result=button_id==MSG_BUTTON_ID_INVALID ? MSG_RESULT_USER_CANCELED : MSG_RESULT_OK;
    printf("Runtime: MsgDialog %s: result %d, button %d\n",sz_how,result,button_id);
    msg_finish(result,button_id);
}
static void msg_poll(void) {
    if (s_dialogs[1].status!=2) return;
    if (s_msg.is_shown) {
        const int answer=psport_gpu_message_poll();
        if (answer) {
            __atomic_add_fetch(&s_c_msg_by_player,1,__ATOMIC_RELAXED);
            msg_choose(answer<0 ? -1 : answer-1,"answered by the player");
            return;
        }
    }
    if (s_msg.auto_at_ns && runtime_wait_clock()>=s_msg.auto_at_ns) {
        __atomic_add_fetch(&s_c_msg_auto,1,__ATOMIC_RELAXED);
        msg_choose(s_msg.i_default,"auto-answered with the default button");
    }
}
static ABI int32_t msg_init(void) { return dialog_init(1); }
static ABI int32_t msg_status(void) {
    pthread_mutex_lock(&s_msg_lock);
    msg_poll();
    const int32_t status=s_dialogs[1].status;
    pthread_mutex_unlock(&s_msg_lock);
    return status;
}
static int32_t msg_refuse(int32_t error, const char *sz_why) {
    __atomic_add_fetch(&s_c_msg_refused,1,__ATOMIC_RELAXED);
    printf("Runtime: MsgDialog open refused (0x%08X): %s\n",(uint32_t)error,sz_why);
    return error;
}
static int is_zero(const uint8_t *p_bytes, size_t off_first, size_t off_end) {
    for (size_t off=off_first; off<off_end; ++off) if (p_bytes[off]) return 0;
    return 1;
}
static const void *read_pointer(const uint8_t *p_bytes, size_t off) {
    const void *p_value;
    memcpy(&p_value,p_bytes+off,sizeof(p_value));
    return p_value;
}
static int32_t read_int(const uint8_t *p_bytes, size_t off) {
    int32_t value;
    memcpy(&value,p_bytes+off,sizeof(value));
    return value;
}
static int is_shorter(const char *sz_text, size_t cb_limit) { return memchr(sz_text,0,cb_limit)!=NULL; }
static int is_button_label(const char *sz_label) {
    return sz_label && is_shorter(sz_label,0x40) && !strpbrk(sz_label,"\r\n");
}
/* libSceMsgDialog's checks of each mode's parameters (kb #818): a NULL one is invalid too. */
static int is_user_message_valid(const uint8_t *p_user) {
    if (!p_user || (uint32_t)read_int(p_user,0)>=10) return 0;
    const char *sz_text=read_pointer(p_user,8);
    const size_t cb_limit=runtime_process_sdk_version()>=0x01500000u ? 0x2000 : 0x200;
    if (!sz_text || !is_shorter(sz_text,cb_limit) || !is_zero(p_user,0x18,0x30)) return 0;
    const uint8_t *p_buttons=read_pointer(p_user,0x10);
    if (read_int(p_user,0)!=MSG_BUTTONS_2BUTTONS) return !p_buttons;
    return p_buttons && is_button_label(read_pointer(p_buttons,0)) && is_button_label(read_pointer(p_buttons,8));
}
static int is_progress_bar_valid(const uint8_t *p_progress) {
    if (!p_progress || (uint32_t)read_int(p_progress,0)>=2) return 0;
    const char *sz_text=read_pointer(p_progress,8);
    if (!sz_text || (runtime_process_sdk_version()>=0x01500000u && !is_shorter(sz_text,0x2000))) return 0;
    return is_zero(p_progress,0x10,0x50);
}
static int is_system_message_valid(const uint8_t *p_system) {
    return p_system && (uint32_t)read_int(p_system,0)<7 && is_zero(p_system,4,0x24);
}
/* The common base: its size, a magic made from its own address, and reserved bytes left zero. */
static int is_base_param_valid(const uint8_t *p_param) {
    uint64_t cb_base;
    memcpy(&cb_base,p_param,sizeof(cb_base));
    return cb_base==0x30 && (uint32_t)read_int(p_param,0x2c)==0xC0D1A109u+(uint32_t)(uintptr_t)p_param && is_zero(p_param,8,0x2c);
}
/* Parameters first, then the dialog's state, in libSceMsgDialog's order (kb #818). */
static int32_t msg_start(const uint8_t *p_param) {
    if (!p_param) return msg_refuse(DIALOG_ARG_NULL,"no parameters");
    uint64_t cb_param;
    memcpy(&cb_param,p_param+0x30,sizeof(cb_param));
    if (!is_base_param_valid(p_param) || cb_param!=0x88) return msg_refuse(DIALOG_PARAM_INVALID,"bad base parameters or size");
    const int32_t mode=read_int(p_param,0x38);
    const uint8_t *p_user=read_pointer(p_param,0x40), *p_progress=read_pointer(p_param,0x48), *p_system=read_pointer(p_param,0x50);
    int is_valid=0;
    if (mode==MSG_MODE_USER) is_valid=!p_system && !p_progress && is_user_message_valid(p_user);
    else if (mode==MSG_MODE_PROGRESS_BAR) is_valid=!p_user && !p_system && is_progress_bar_valid(p_progress);
    else if (mode==MSG_MODE_SYSTEM) is_valid=!p_user && !p_progress && is_system_message_valid(p_system);
    if (!is_valid) return msg_refuse(DIALOG_PARAM_INVALID,"bad mode or mode parameters");
    if (!is_zero(p_param,0x5c,0x84)) return msg_refuse(DIALOG_PARAM_INVALID,"reserved bytes set");
    if (!s_dialogs[1].is_initialized) return msg_refuse(DIALOG_NOT_INITIALIZED,"not initialized");
    if (s_dialogs[1].status==2) return msg_refuse(DIALOG_INVALID_STATE,"one is already running");
    const char *sz_text=NULL, *sz_label1=NULL, *sz_label2=NULL;
    int32_t button_type=MSG_BUTTONS_NONE, system_id=-1;
    if (mode==MSG_MODE_USER) {
        button_type=read_int(p_user,0);
        sz_text=read_pointer(p_user,8);
        if (button_type==MSG_BUTTONS_2BUTTONS) {
            const uint8_t *p_buttons=read_pointer(p_user,0x10);
            sz_label1=read_pointer(p_buttons,0);
            sz_label2=read_pointer(p_buttons,8);
        }
    } else if (mode==MSG_MODE_PROGRESS_BAR) sz_text=read_pointer(p_progress,8);
    else {
        system_id=read_int(p_system,0);
        button_type=MSG_BUTTONS_OK;
    }
    PsMessageButton buttons[2]={{PS_MESSAGE_BUTTON_OK,NULL},{PS_MESSAGE_BUTTON_CANCEL,NULL}};
    int32_t button_ids[2]={1,MSG_BUTTON_ID_INVALID};
    int c_buttons=0, i_default=0, can_cancel=0;
    switch (button_type) {
    case MSG_BUTTONS_OK: c_buttons=1; break;
    case MSG_BUTTONS_YESNO: case MSG_BUTTONS_YESNO_FOCUS_NO:
        buttons[0].kind=PS_MESSAGE_BUTTON_YES; buttons[1].kind=PS_MESSAGE_BUTTON_NO; button_ids[1]=2;
        c_buttons=2; i_default=button_type==MSG_BUTTONS_YESNO_FOCUS_NO; break;
    case MSG_BUTTONS_OK_CANCEL: case MSG_BUTTONS_OK_CANCEL_FOCUS_CANCEL:
        c_buttons=2; can_cancel=1; i_default=button_type==MSG_BUTTONS_OK_CANCEL_FOCUS_CANCEL; break;
    case MSG_BUTTONS_CANCEL: case MSG_BUTTONS_WAIT_CANCEL:
        buttons[0].kind=PS_MESSAGE_BUTTON_CANCEL; button_ids[0]=MSG_BUTTON_ID_INVALID; c_buttons=1; can_cancel=1; break;
    case MSG_BUTTONS_2BUTTONS:
        buttons[0]=(PsMessageButton){PS_MESSAGE_BUTTON_LABEL,sz_label1}; buttons[1]=(PsMessageButton){PS_MESSAGE_BUTTON_LABEL,sz_label2};
        button_ids[1]=2; c_buttons=2; break;
    case MSG_BUTTONS_NONE: case MSG_BUTTONS_WAIT: break;
    }
    /* The console backs out of any dialog with buttons; GetResult reports it as a button for some
     * (msg_back_button). */
    can_cancel|=c_buttons>0;
    char sz_shown[1024];
    snprintf(sz_shown,sizeof(sz_shown),"%s",sz_text ? sz_text : "");
    memset(&s_msg,0,sizeof(s_msg));
    s_msg.mode=mode; s_msg.button_type=button_type; s_msg.system_type=system_id; s_msg.c_buttons=c_buttons; s_msg.i_default=i_default; s_msg.can_cancel=can_cancel;
    memcpy(s_msg.button_ids,button_ids,sizeof(s_msg.button_ids));
    s_msg.is_shown=psport_gpu_message_begin(system_id>=0 ? NULL : sz_shown,system_id,buttons,c_buttons,i_default,can_cancel);
    const int auto_ms=runtime_env_int("PS_MSGDIALOG_AUTO_MS",s_msg.is_shown ? -1 : 1000);
    /* A dialog that only waits is the game's to close; choosing its cancel button would abort it. */
    const int is_auto=auto_ms>=0 && c_buttons>0 && button_type!=MSG_BUTTONS_CANCEL && button_type!=MSG_BUTTONS_WAIT_CANCEL;
    if (is_auto) s_msg.auto_at_ns=runtime_wait_clock()+(uint64_t)auto_ms*1000000u;
    s_dialogs[1].status=2;
    if (__atomic_fetch_add(&s_c_msg_opened,1,__ATOMIC_RELAXED)==0) runtime_calls_add_summary(msg_summary);
    if (system_id>=0) printf("Runtime: MsgDialog opened: system message %d",system_id);
    else printf("Runtime: MsgDialog opened: mode %d, button type %d, \"%s\"",mode,button_type,sz_shown);
    printf(" (%d buttons, default %d%s); %s",c_buttons,i_default,can_cancel ? ", cancellable" : "",
           s_msg.is_shown ? "shown by the overlay" : "not shown (no window or overlay)");
    if (is_auto) printf(", auto-answered in %d ms\n",auto_ms);
    else printf(c_buttons ? ", waits for the player\n" : ", waits for the game to close it\n");
    return 0;
}
static ABI int32_t msg_open(const uint8_t *p_param) {
    pthread_mutex_lock(&s_msg_lock);
    const int32_t error=msg_start(p_param);
    pthread_mutex_unlock(&s_msg_lock);
    return error;
}
/* OrbisMsgDialogResult: mode, result, button id, then 32 reserved bytes the game leaves zero. */
static ABI int32_t msg_result(int32_t *p_result) {
    if (!p_result) return DIALOG_ARG_NULL;
    if (!is_zero((const uint8_t *)p_result,0x0c,0x2c)) return DIALOG_PARAM_INVALID;
    pthread_mutex_lock(&s_msg_lock);
    int32_t error=0;
    if (!s_dialogs[1].is_initialized) error=DIALOG_NOT_INITIALIZED;
    else if (s_dialogs[1].status!=3) error=DIALOG_NOT_FINISHED;
    else { p_result[0]=s_msg.mode; p_result[1]=s_msg.result; p_result[2]=s_msg.button_id; }
    pthread_mutex_unlock(&s_msg_lock);
    return error;
}
/* The game takes it down: it ends with no button chosen. */
static int32_t msg_stop(const char *sz_how) {
    if (s_dialogs[1].status!=2) return DIALOG_NOT_RUNNING;
    __atomic_add_fetch(&s_c_msg_closed,1,__ATOMIC_RELAXED);
    printf("Runtime: MsgDialog %s while running\n",sz_how);
    msg_finish(MSG_RESULT_OK,MSG_BUTTON_ID_INVALID);
    return 0;
}
static ABI int32_t msg_close(void) {
    pthread_mutex_lock(&s_msg_lock);
    const int32_t error=s_dialogs[1].is_initialized ? msg_stop("closed by the game") : DIALOG_NOT_INITIALIZED;
    pthread_mutex_unlock(&s_msg_lock);
    return error;
}
static ABI int32_t msg_term(void) {
    pthread_mutex_lock(&s_msg_lock);
    (void)msg_stop("terminated by the game"); /* NOT_RUNNING when it had already finished */
    const int32_t error=dialog_term(1);
    pthread_mutex_unlock(&s_msg_lock);
    return error;
}
typedef struct { int32_t size, error_code, user, reserved; } ErrorDialogParam;
static ABI int32_t error_init(void) { return dialog_init(6); }
static ABI int32_t error_open(const ErrorDialogParam *p_param) {
    static uint32_t s_logged[32];
    static int s_c_logged;
    if (s_dialogs[6].status!=1 && s_dialogs[6].status!=3) return (int32_t)0x80B80006; /* INVALID_STATE */
    if (!p_param) return (int32_t)0x80B8000D;                                         /* ARG_NULL */
    const uint32_t code=(uint32_t)p_param->error_code;
    int has_seen=0;
    for (int k=0;k<s_c_logged;++k) has_seen|=s_logged[k]==code;
    if (!has_seen && s_c_logged<32) {
        s_logged[s_c_logged++]=code;
        printf("Runtime: ErrorDialog 0x%08X\n",code);
    }
    s_dialogs[6].status=3; return 0;
}
static ABI int32_t error_status(void) { return dialog_status(6); }
static ABI int32_t error_close(void) { return (int32_t)0x80B8000B; } /* NOT_RUNNING: an opened one has already finished */
static ABI int32_t error_term(void) { return dialog_term(6); }
/* OrbisHmdSetupDialogResult: result, reserved. With no headset to set up, the user can only
 * back out of the dialog: SCE_COMMON_DIALOG_RESULT_USER_CANCELED. */
static ABI int32_t hmd_setup_result(int32_t *p_result) {
    if (s_dialogs[11].status!=3) return (int32_t)0x80B80005; /* NOT_FINISHED */
    if (!p_result) return (int32_t)0x80B8000D;               /* ARG_NULL */
    p_result[0]=1;
    return 0;
}
static ABI int32_t hmd_setup_close(void) { s_dialogs[11].status=3; return 0; }
/* SceSocialScreenDialogResult: result, reserved. Separate mode needs the PS VR processor unit, so
 * the user can only back out: SCE_COMMON_DIALOG_RESULT_USER_CANCELED. */
static ABI int32_t social_result(int32_t *p_out_result) {
    if (s_dialogs[10].status!=3) return (int32_t)0x80B80005; /* NOT_FINISHED */
    if (!p_out_result) return (int32_t)0x80B8000D;         /* ARG_NULL */
    p_out_result[0]=1;
    return 0;
}
static ABI int32_t social_close(void) { s_dialogs[10].status=3; return 0; }
/* ScePlayGoDialogResult: result, reserved. Every chunk is installed, so the dialog has nothing to
 * wait for and ends as accepted: SCE_COMMON_DIALOG_RESULT_OK. */
static ABI int32_t playgo_result(int32_t *p_out_result) {
    if (s_dialogs[12].status!=3) return (int32_t)0x80B80005; /* NOT_FINISHED */
    if (!p_out_result) return (int32_t)0x80B8000D;         /* ARG_NULL */
    p_out_result[0]=0;
    return 0;
}
static ABI int32_t playgo_close(void) { s_dialogs[12].status=3; return 0; }
/* LoginDialog keeps its own state and codes. With no UI it finishes on open as backed out of. */
#define LOGIN_NOT_INITIALIZED ((int32_t)0x81340001)
#define LOGIN_ALREADY_INITIALIZED ((int32_t)0x81340002)
#define LOGIN_INVALID_PARAM ((int32_t)0x81340003)
#define LOGIN_INVALID_STATE ((int32_t)0x81340005)
#define LOGIN_INVALID_USER_ID ((int32_t)0x81340007)
enum { LOGIN_NONE, LOGIN_INITIALIZED, LOGIN_RUNNING, LOGIN_FINISHED };
static int s_login_state;
static ABI int32_t login_init(void) {
    if (s_login_state!=LOGIN_NONE) return LOGIN_ALREADY_INITIALIZED;
    s_login_state=LOGIN_INITIALIZED; return 0;
}
static int is_login_user(int32_t user) { return user==-1 || (user>=0x10000000 && user<=0x3FFFFFFF); }
static ABI int32_t login_open(const uint8_t *p_param) {
    if (s_login_state==LOGIN_NONE) return LOGIN_NOT_INITIALIZED;
    if (s_login_state==LOGIN_RUNNING) return LOGIN_INVALID_STATE;
    if (!p_param) return LOGIN_INVALID_PARAM;
    uint32_t cb_param, mode;
    memcpy(&cb_param,p_param,4); memcpy(&mode,p_param+4,4);
    if (cb_param!=0x40 || mode>1) return LOGIN_INVALID_PARAM;
    for (int i=0x2C;i<0x40;++i) if (p_param[i]) return LOGIN_INVALID_PARAM;
    for (int i=0;i<9;++i) {
        int32_t user; memcpy(&user,p_param+8+i*4,4);
        if (!is_login_user(user)) return LOGIN_INVALID_USER_ID;
    }
    printf("Runtime: LoginDialog opened; completed immediately (no dialog UI yet)\n");
    s_login_state=LOGIN_FINISHED; return 0;
}
static ABI int32_t login_status(void) { return s_login_state; }
/* The firmware fills the result as cancelled with a bad user before it looks at the state. */
static ABI int32_t login_result(int32_t *p_out_result) {
    if (!p_out_result) return LOGIN_INVALID_PARAM;
    p_out_result[0]=1; p_out_result[1]=LOGIN_INVALID_USER_ID;
    if (s_login_state==LOGIN_NONE) return LOGIN_NOT_INITIALIZED;
    if (s_login_state!=LOGIN_FINISHED) return LOGIN_INVALID_STATE;
    p_out_result[1]=0;
    return 0;
}
static ABI int32_t login_close(void) {
    if (s_login_state==LOGIN_NONE) return LOGIN_NOT_INITIALIZED;
    if (s_login_state==LOGIN_RUNNING) s_login_state=LOGIN_FINISHED;
    return 0;
}
static ABI int32_t login_term(void) {
    if (s_login_state==LOGIN_NONE) return LOGIN_NOT_INITIALIZED;
    s_login_state=LOGIN_NONE; return 0;
}
/* SceWebBrowserDialogResult: result, then reserved. Offline, no page loads and the user can only
 * back out: SCE_COMMON_DIALOG_RESULT_USER_CANCELED. */
static ABI int32_t web_result(int32_t *p_out_result) {
    if (s_dialogs[7].status!=3) return (int32_t)0x80B80005; /* NOT_FINISHED */
    if (!p_out_result) return (int32_t)0x80B8000D;         /* ARG_NULL */
    p_out_result[0]=1;
    return 0;
}
/* ImeDialog: text typed on the keyboard into the game window (title bar shows it) or with the
 * controller on the overlay's keyboard (gpu/port/psport_keyboard.cpp).
 * OrbisImeDialogParam: user, type, languages(8), enter label, method, filter,
 * option, max length, char16 buffer, position, alignment, placeholder, title. */
typedef struct {
    int32_t user; uint32_t type; uint64_t languages; uint32_t enter_label, input_method;
    void *filter; uint32_t option, max_length; uint16_t *buffer;
    float x, y; uint32_t halign, valign; const uint16_t *placeholder, *title; int8_t reserved[16];
} ImeParam;
static struct { int is_running, has_finished, end_status; uint16_t *p_buffer; uint32_t max_length; } s_ime;
size_t utf16_to_utf8(const uint16_t *p_in, size_t limit, char *sz_out, size_t cb_out) {
    size_t cb_written=0;
    for (size_t i=0; p_in && i<limit && p_in[i] && cb_written+4<cb_out; ++i) {
        uint32_t c=p_in[i];
        if (c>=0xD800 && c<0xDC00 && i+1<limit && p_in[i+1]>=0xDC00 && p_in[i+1]<0xE000) { c=0x10000+((c-0xD800)<<10)+(p_in[i+1]-0xDC00); ++i; }
        if (c<0x80) sz_out[cb_written++]=(char)c;
        else if (c<0x800) { sz_out[cb_written++]=(char)(0xC0|c>>6); sz_out[cb_written++]=(char)(0x80|(c&63)); }
        else if (c<0x10000) { sz_out[cb_written++]=(char)(0xE0|c>>12); sz_out[cb_written++]=(char)(0x80|((c>>6)&63)); sz_out[cb_written++]=(char)(0x80|(c&63)); }
        else { sz_out[cb_written++]=(char)(0xF0|c>>18); sz_out[cb_written++]=(char)(0x80|((c>>12)&63)); sz_out[cb_written++]=(char)(0x80|((c>>6)&63)); sz_out[cb_written++]=(char)(0x80|(c&63)); }
    }
    sz_out[cb_written]=0; return cb_written;
}
void utf8_to_utf16(const char *sz_in, uint16_t *p_out, uint32_t max) {
    uint32_t c_units=0;
    for (const unsigned char *p_cursor=(const unsigned char *)sz_in; *p_cursor && c_units<max;) {
        uint32_t c; int extra;
        if (*p_cursor<0x80) { c=*p_cursor; extra=0; } else if ((*p_cursor&0xE0)==0xC0) { c=*p_cursor&31; extra=1; }
        else if ((*p_cursor&0xF0)==0xE0) { c=*p_cursor&15; extra=2; } else { c=*p_cursor&7; extra=3; }
        ++p_cursor;
        for (int k=0;k<extra && (*p_cursor&0xC0)==0x80;++k) c=(c<<6)|(*p_cursor++&63);
        if (c>=0x10000) { if (c_units+2>max) break; c-=0x10000; p_out[c_units++]=(uint16_t)(0xD800+(c>>10)); p_out[c_units++]=(uint16_t)(0xDC00+(c&1023)); }
        else p_out[c_units++]=(uint16_t)c;
    }
    p_out[c_units<max ? c_units : max]=0;
}
static void ime_complete(int end_status, const char *sz_text) {
    if (!end_status && s_ime.p_buffer && s_ime.max_length) utf8_to_utf16(sz_text,s_ime.p_buffer,s_ime.max_length);
    s_ime.end_status=end_status; s_ime.has_finished=1; s_ime.is_running=0;
    printf("Runtime: ImeDialog %s%s%s\n",end_status ? "cancelled" : "text: ",end_status ? "" : sz_text,"");
}
static ABI int32_t ime_init(const ImeParam *p_param, const void *p_extended) {
    (void)p_extended;
    if (!p_param || !p_param->buffer || !p_param->max_length) return (int32_t)0x80BC0004; /* IME INVALID_ADDRESS */
    if (s_ime.is_running) return (int32_t)0x80BC0003;                                  /* BUSY */
    memset(&s_ime,0,sizeof(s_ime));
    s_ime.p_buffer=p_param->buffer; s_ime.max_length=p_param->max_length; s_ime.is_running=1;
    char sz_initial[512], sz_prompt[256];
    utf16_to_utf8(p_param->buffer,p_param->max_length,sz_initial,sizeof(sz_initial));
    utf16_to_utf8(p_param->title,128,sz_prompt,sizeof(sz_prompt));
    const char *sz_preset=runtime_env("PS_IME_TEXT",NULL);
    if (sz_preset) {
        char sz_entered[512];
        runtime_ime_filter_entered(sz_preset,p_param->type,p_param->max_length,sz_entered,sizeof(sz_entered));
        ime_complete(0,sz_entered);
        return 0;
    }
    if (p_param->filter) printf("Runtime: ImeDialog text filter %p not called: the entry checks only its type %u\n",p_param->filter,p_param->type);
    /* The field starts with the game's text, as on the console: the dialog's parameters say nothing
     * that tells a name entry apart, so the user's name is never put in for it. */
    if (!psport_gpu_text_input_begin(sz_initial,sz_prompt[0] ? sz_prompt : "Text",p_param->max_length,runtime_ime_charset(p_param->type)))
        ime_complete(0,sz_initial);
    else printf("Runtime: ImeDialog opened with \"%s\" (type %u, up to %u characters): type in the game window or on the "
                "on-screen keyboard; Enter or Options confirms, Esc or Circle on an empty field cancels\n",sz_initial,p_param->type,p_param->max_length);
    return 0;
}
static ABI int32_t ime_status(void) {
    if (s_ime.is_running) {
        char sz_text[512];
        int state=psport_gpu_text_input_poll(sz_text,sizeof(sz_text));
        if (state) ime_complete(state==1 ? 0 : 1,sz_text);
    }
    return s_ime.is_running ? 1 : s_ime.has_finished ? 2 : 0; /* Running / Finished / None */
}
static ABI int32_t ime_result(uint32_t *p_result) {
    if (!s_ime.has_finished) return (int32_t)0x80BC0101; /* DIALOG NOT_FINISHED */
    if (p_result) *p_result=(uint32_t)s_ime.end_status;
    return 0;
}
static ABI int32_t ime_term(void) { memset(&s_ime,0,sizeof(s_ime)); return 0; }
/* The dialog's panel is the on-screen keyboard's (runtime_ime.c), whatever the extended parameters ask. */
static ABI int32_t ime_panel_size_extended(const ImeParam *p_param, const void *p_extended, uint32_t *p_width, uint32_t *p_height) {
    (void)p_extended;
    if (!p_param || !p_width || !p_height) return (int32_t)0x80BC0031; /* IME INVALID_ADDRESS */
    runtime_ime_panel_size(p_param->type,p_width,p_height);
    return 0;
}
/* A dialog is in use from its initialization until its termination, and the IME dialog while it runs. */
static ABI int32_t common_is_used(void) {
    for (size_t i=0;i<sizeof(s_dialogs)/sizeof(*s_dialogs);++i) if (s_dialogs[i].is_initialized) return 1;
    return s_ime.is_running;
}

/* ---- DiscMap: the game is fully installed, no disc bitmap exists ---- */
#define DISC_MAP_NO_BITMAP ((int32_t)0x81100004)
/* Every byte of an installed title is on the HDD; libSceFios2 takes a failure here for a request
 * off the disc. */
static ABI int32_t discmap_on_hdd(const char *sz_path,int64_t off_file,int64_t cb_range,int32_t *p_result) {
    (void)sz_path; (void)off_file; (void)cb_range;
    if (!p_result) return (int32_t)0x81100001; /* INVALID_ARGUMENT */
    *p_result=1;
    return 0;
}
/* The one the games call at boot succeeds instead, as in shadPS4, with its three outputs zero:
 * they read them. */
static ABI int32_t discmap_7c98(const char *sz_path,int64_t off_file,int64_t cb_range,int32_t *p_flags,int32_t *p_result1,int32_t *p_result2) {
    (void)sz_path; (void)off_file; (void)cb_range;
    if (!p_flags || !p_result1 || !p_result2) return (int32_t)0x81100001; /* INVALID_ARGUMENT */
    *p_flags=0; *p_result1=0; *p_result2=0;
    return 0;
}
static ABI int32_t discmap_8a82(const char *sz_path,int64_t off_file,int64_t cb_range,int32_t *p_flags,int32_t *p_result1,int32_t *p_result2) {
    (void)sz_path; (void)off_file; (void)cb_range; (void)p_flags; (void)p_result1; (void)p_result2; return DISC_MAP_NO_BITMAP;
}

/* ---- Devices that are absent: microphone ---- */
static ABI int32_t audio_in_open(void) { return AUDIO_IN_NOT_OPENED; }
/* The firmware answers 0 for any port and leaves the output untouched. */
static ABI int32_t audio_in_gain(void) { return 0; }
/* PlayStation Move: the library runs, no motion controller is ever connected. A handle is still
 * handed out per open, distinct from other devices' handles, as on the console. */
static int s_is_move_initialized;
static ABI int32_t move_init(void) {
    if (s_is_move_initialized) return MOVE_ALREADY_INIT;
    s_is_move_initialized=1; return 0;
}
static ABI int32_t move_open(int32_t user,int32_t type,int32_t i_device) {
    (void)user; (void)type; (void)i_device;
    static int32_t s_h_handle=0x30b0000;
    if (!s_is_move_initialized) return MOVE_NOT_INIT;
    return __atomic_add_fetch(&s_h_handle,0x100,__ATOMIC_RELAXED);
}
/* OrbisMoveDeviceInfo: sphere radius and accelerometer offset, four floats. */
static ABI int32_t move_device_info(int32_t h_handle,float *p_info) {
    (void)h_handle;
    if (!s_is_move_initialized) return MOVE_NOT_INIT;
    if (!p_info) return MOVE_INVALID_ARG;
    memset(p_info,0,16);
    return MOVE_NO_CONTROLLER;
}
static ABI int32_t move_read_recent(int32_t h_handle,int64_t timestamp,void *p_data,int32_t *p_count) {
    (void)h_handle;
    if (!s_is_move_initialized) return MOVE_NOT_INIT;
    if (timestamp<0 || !p_data || !p_count) return MOVE_INVALID_ARG;
    *p_count=0;
    return MOVE_NO_CONTROLLER;
}
static ABI int32_t move_no_controller(int32_t h_handle) { (void)h_handle; return s_is_move_initialized ? MOVE_NO_CONTROLLER : MOVE_NOT_INIT; }
static ABI int32_t move_term(void) {
    if (!s_is_move_initialized) return MOVE_NOT_INIT;
    s_is_move_initialized=0; return 0;
}
/* ---- RazorCpu ---- */
/* The profiler's markers reach Razor only on a devkit capturing with it; a retail console, like
 * this port, has no capture running and drops them, so they are no stub. */
static ABI int32_t razor_idle(void) { return 0; }


static const RuntimeExport s_exports[]={
    {"sceUserServiceInitialize",user_initialize}, {"sceUserServiceInitialize2",user_initialize2},
    {"sceUserServiceTerminate",user_terminate},
    {"sceUserServiceGetInitialUser",user_initial}, {"sceUserServiceGetLoginUserIdList",user_list},
    {"sceUserServiceGetUserName",user_name}, {"sceUserServiceGetEvent",user_event},
    {"sceUserServiceGetUserColor",user_color}, {"sceUserServiceRegisterEventCallback",runtime_stub},
    {"sceSystemServiceParamGetInt",system_param}, {"sceSystemServiceParamGetString",system_param_string},
    {"sceSystemServiceGetStatus",system_status}, {"sceSystemServiceGetHdrToneMapLuminance",hdr_tone_map_luminance},
    {"sceSystemServiceDisableMusicPlayer",runtime_stub}, {"sceSystemServiceReenableMusicPlayer",runtime_stub},
    {"sceSystemServiceDisableSuspendConfirmationDialog",runtime_stub},
    {"sceSystemServiceEnableSuspendConfirmationDialog",runtime_stub},
    {"sceSystemServiceDisablePersonalEyeToEyeDistanceSetting",runtime_stub},
    {"sceSystemServiceEnablePersonalEyeToEyeDistanceSetting",runtime_stub},
    {"sceSystemServicePowerTick",runtime_stub}, {"sceSystemServiceReportAbnormalTermination",runtime_stub},
    {"sceSystemServiceReceiveEvent",system_event}, {"sceSystemServiceHideSplashScreen",hide_splash},
    {"sceSystemServiceLaunchWebBrowser",launch_browser},
    {"sceSystemServiceShowControllerSettings",show_controller_settings},
    {"sceSystemServiceGetDisplaySafeAreaInfo",display_safe_area}, {"sceSystemServiceLoadExec",load_exec},
    {"sceNetInit",runtime_stub}, {"sceNetTerm",runtime_stub}, {"sceNetPoolDestroy",runtime_stub},
    {"sceNetEpollCreate",net_epoll_create}, {"sceNetEpollDestroy",runtime_stub},
    {"sceNetResolverCreate",net_resolver_create}, {"sceNetResolverDestroy",runtime_stub},
    {"sceNetResolverAbort",net_resolver_abort},
    {"sceNetHtons",net_htons}, {"sceNetNtohs",net_ntohs}, {"sceNetHtonl",net_htonl}, {"sceNetNtohl",net_ntohl},
    {"sceNetEpollControl",net_unreachable}, {"sceNetEpollWait",net_unreachable}, {"sceNetEpollAbort",net_unreachable},
    {"sceNetResolverStartNtoa",net_unreachable}, {"sceNetResolverStartAton",net_unreachable},
    {"sceNetCtlInit",netctl_init}, {"sceNetCtlGetState",netctl_state}, {"sceNetCtlGetInfo",netctl_info},
    {"sceNetCtlRegisterCallback",netctl_register}, {"sceNetCtlCheckCallback",nothing_due},
    {"sceNetCtlUnregisterCallback",runtime_stub}, {"sceNetCtlGetNatInfo",netctl_nat},
    {"sceNetCtlRegisterCallbackForNpToolkit",netctl_register}, {"sceNetCtlCheckCallbackForNpToolkit",nothing_due},
    {"sceSslTerm",runtime_stub},
    {"sceNpGetState",np_state}, {"sceNpGetOnlineId",np_signed_out}, {"sceNpGetNpId",np_signed_out},
    {"sceNpGetAccountIdA",np_account_id}, {"sceNpGetAccountId",np_account_id_online},
    {"sceNpGetAccountCountry",np_account_info}, {"sceNpGetAccountDateOfBirth",np_account_info},
    {"sceNpRegisterStateCallback",np_register}, {"sceNpUnregisterStateCallback",runtime_stub},
    {"sceNpRegisterStateCallbackA",np_register},
    {"sceNpRegisterGamePresenceCallback",np_register_void}, {"sceNpRegisterPlusEventCallback",np_register},
    {"sceNpUnregisterPlusEventCallback",runtime_stub}, {"sceNpCheckCallback",nothing_due},
    {"sceNpRegisterStateCallbackForToolkit",np_register_toolkit}, {"sceNpCheckCallbackForLib",nothing_due},
    {"sceNpSetNpTitleId",runtime_stub}, {"sceNpNotifyPlusFeature",runtime_stub}, {"sceNpSetContentRestriction",runtime_stub},
    {"sceNpCreateRequest",np_request}, {"sceNpCreateAsyncRequest",np_request},
    {"sceNpDeleteRequest",runtime_stub}, {"sceNpAbortRequest",runtime_stub},
    {"sceNpPollAsync",np_poll}, {"sceNpCheckNpAvailability",np_signed_out},
    {"sceNpGetParentalControlInfo",np_signed_out}, {"sceNpCheckPlus",np_signed_out},
    {"sceNpGetGamePresenceStatus",np_signed_out},
    {"sceNpCmpNpId",np_compare}, {"sceNpCmpOnlineId",np_compare},
    {"sceNpAuthCreateRequest",np_request}, {"sceNpAuthCreateAsyncRequest",np_request},
    {"sceNpAuthDeleteRequest",runtime_stub},
    {"sceNpAuthPollAsync",np_poll}, {"sceNpAuthGetAuthorizationCode",np_signed_out},
    {"sceNpAuthGetIdTokenA",np_signed_out},
    {"sceNpLookupCreateTitleCtx",np_request}, {"sceNpLookupDeleteTitleCtx",runtime_stub},
    {"sceNpLookupCreateAsyncRequest",np_request_ctx}, {"sceNpLookupDeleteRequest",runtime_stub},
    {"sceNpLookupAbortRequest",runtime_stub}, {"sceNpLookupPollAsync",np_poll}, {"sceNpLookupNpId",np_signed_out},
    {"sceNpScoreCreateNpTitleCtx",np_request_ctx}, {"sceNpScoreDeleteNpTitleCtx",runtime_stub},
    {"sceNpScoreCreateRequest",np_request}, {"sceNpScoreDeleteRequest",runtime_stub}, {"sceNpScoreAbortRequest",runtime_stub},
    {"sceNpWebApiCheckTimeout",nothing_due},
    {"sceNpWebApiUtilityParseNpId",np_signed_out},
    {"sceNpMatching2ContextStart",np_signed_out}, {"sceNpMatching2ContextStop",runtime_stub},
    {"sceNpMatching2DestroyContext",runtime_stub},
    {"sceNpMatching2RegisterContextCallback",runtime_stub}, {"sceNpMatching2RegisterLobbyEventCallback",runtime_stub},
    {"sceNpMatching2RegisterRoomEventCallback",runtime_stub}, {"sceNpMatching2RegisterSignalingCallback",runtime_stub},
    {"sceNpMatching2SetDefaultRequestOptParam",runtime_stub},
    {"sceNpMatching2CreateJoinRoom",np_signed_out}, {"sceNpMatching2JoinRoom",np_signed_out},
    {"sceNpMatching2LeaveRoom",np_signed_out}, {"sceNpMatching2SearchRoom",np_signed_out},
    {"sceNpMatching2GetServerId",np_signed_out}, {"sceNpMatching2GetWorldInfoList",np_signed_out},
    {"sceNpMatching2GetLobbyInfoList",np_signed_out}, {"sceNpMatching2JoinLobby",np_signed_out},
    {"sceNpMatching2LeaveLobby",np_signed_out}, {"sceNpMatching2GrantRoomOwner",np_signed_out},
    {"sceNpMatching2KickoutRoomMember",np_signed_out}, {"sceNpMatching2SetRoomDataExternal",np_signed_out},
    {"sceNpMatching2SetRoomDataInternal",np_signed_out}, {"sceNpMatching2SetRoomMemberDataInternal",np_signed_out},
    {"sceNpMatching2SignalingGetConnectionStatus",np_signed_out}, {"sceNpMatching2SignalingGetPingInfo",np_signed_out},
    {"sceNpSignalingDeleteContext",runtime_stub}, {"sceNpSignalingActivateConnection",np_signed_out},
    {"sceNpSignalingDeactivateConnection",runtime_stub}, {"sceNpSignalingGetConnectionStatus",np_signed_out},
    {"sceNpScoreCensorComment",np_signed_out}, {"sceNpScoreSanitizeComment",np_signed_out},
    {"sceNpScoreGetBoardInfo",np_signed_out}, {"sceNpScoreGetGameData",np_signed_out},
    {"sceNpScoreGetRankingByNpIdPcId",np_signed_out}, {"sceNpScoreGetRankingByRange",np_signed_out},
    {"sceNpScoreRecordGameData",np_signed_out}, {"sceNpScoreRecordScore",np_signed_out},
    {"sceNpScoreSetPlayerCharacterId",runtime_stub}, {"sceNpTusCreateNpTitleCtxA",np_signed_out},
    {"sceAudioInInput",audio_in_open}, {"sceAudioInClose",audio_in_open},
    {"sceNpMatching2Initialize",runtime_stub}, {"sceNpMatching2Terminate",runtime_stub},
    {"sceNpMatching2CreateContext",np_signed_out},
    {"sceNpSignalingInitialize",runtime_stub}, {"sceNpSignalingTerminate",runtime_stub},
    {"sceNpSignalingCreateContext",np_signed_out},
    {"sceCommonDialogInitialize",common_init},
    {"sceMsgDialogInitialize",msg_init}, {"sceMsgDialogOpen",msg_open},
    {"sceMsgDialogUpdateStatus",msg_status}, {"sceMsgDialogTerminate",msg_term},
    {"sceMsgDialogGetStatus",msg_status}, {"sceMsgDialogGetResult",msg_result}, {"sceMsgDialogClose",msg_close},
    {"sceMsgDialogProgressBarSetValue",runtime_stub}, {"sceMsgDialogProgressBarSetMsg",runtime_stub},
    {"sceMsgDialogProgressBarInc",runtime_stub},
    {"sceSaveDataDialogInitialize",save_init}, {"sceSaveDataDialogOpen",save_open},
    {"sceSaveDataDialogUpdateStatus",save_status}, {"sceSaveDataDialogTerminate",save_term},
    {"sceSaveDataDialogGetStatus",save_status}, {"sceSaveDataDialogIsReadyToDisplay",save_ready},
    {"sceSaveDataDialogGetResult",save_result}, {"sceSaveDataDialogClose",save_close},
    {"sceSaveDataDialogProgressBarSetValue",runtime_stub}, {"sceSaveDataDialogProgressBarInc",runtime_stub},
    {"sceNpProfileDialogInitialize",profile_init}, {"sceNpProfileDialogOpen",profile_open},
    {"sceNpProfileDialogUpdateStatus",profile_status}, {"sceNpProfileDialogTerminate",profile_term},
    {"sceNpProfileDialogGetResult",profile_result}, {"sceNpProfileDialogOpenA",profile_open},
    {"sceNpProfileDialogGetStatus",profile_status},
    {"sceNpCommerceDialogInitialize",commerce_init}, {"sceNpCommerceDialogOpen",commerce_start},
    {"sceNpCommerceDialogUpdateStatus",commerce_status}, {"sceNpCommerceDialogTerminate",commerce_term},
    {"sceNpCommerceDialogGetStatus",commerce_status}, {"sceNpCommerceDialogGetResult",commerce_result},
    {"sceImeDialogInit",ime_init}, {"sceImeDialogGetStatus",ime_status}, {"sceImeDialogGetResult",ime_result},
    {"sceImeDialogTerm",ime_term}, {"sceImeDialogAbort",ime_term},
    {"sceImeDialogGetPanelSizeExtended",ime_panel_size_extended},
    {"sceWebBrowserDialogInitialize",web_init}, {"sceWebBrowserDialogOpen",web_open},
    {"sceWebBrowserDialogGetResult",web_result}, {"sceWebBrowserDialogSetCookie",runtime_stub},
    {"sceWebBrowserDialogResetCookie",runtime_stub},
    {"sceWebBrowserDialogOpenForPredeterminedContent",web_open}, {"sceWebBrowserDialogUpdateStatus",web_status},
    {"sceWebBrowserDialogGetStatus",web_status}, {"sceWebBrowserDialogTerminate",web_term},
    {"sceInvitationDialogInitialize",invitation_init}, {"sceInvitationDialogOpen",invitation_start},
    {"sceInvitationDialogOpenA",invitation_start}, {"sceInvitationDialogUpdateStatus",invitation_status},
    {"sceInvitationDialogGetStatus",invitation_status}, {"sceInvitationDialogTerminate",invitation_term},
    {"sceInvitationDialogGetResultA",invitation_result},
    {"sceSigninDialogInitialize",signin_init}, {"sceSigninDialogOpen",signin_open},
    {"sceSigninDialogUpdateStatus",signin_status}, {"sceSigninDialogGetStatus",signin_status},
    {"sceSigninDialogTerminate",signin_term}, {"sceSigninDialogGetResult",signin_result},
    {"sceSocialScreenDialogInitialize",social_init}, {"sceSocialScreenDialogOpen",social_open},
    {"sceSocialScreenDialogUpdateStatus",social_status}, {"sceSocialScreenDialogGetStatus",social_status},
    {"sceSocialScreenDialogTerminate",social_term}, {"sceSocialScreenDialogGetResult",social_result},
    {"sceSocialScreenDialogClose",social_close},
    {"sceErrorDialogInitialize",error_init}, {"sceErrorDialogOpen",error_open},
    {"sceErrorDialogUpdateStatus",error_status}, {"sceErrorDialogGetStatus",error_status},
    {"sceErrorDialogClose",error_close}, {"sceErrorDialogTerminate",error_term},
    {"sceHmdSetupDialogInitialize",hmd_setup_init}, {"sceHmdSetupDialogOpen",hmd_setup_open},
    {"sceHmdSetupDialogUpdateStatus",hmd_setup_status}, {"sceHmdSetupDialogGetStatus",hmd_setup_status},
    {"sceHmdSetupDialogGetResult",hmd_setup_result}, {"sceHmdSetupDialogClose",hmd_setup_close},
    {"sceHmdSetupDialogTerminate",hmd_setup_term},
    {"scePlayGoDialogInitialize",playgo_init}, {"scePlayGoDialogOpen",playgo_open},
    {"scePlayGoDialogUpdateStatus",playgo_status}, {"scePlayGoDialogGetStatus",playgo_status},
    {"scePlayGoDialogGetResult",playgo_result}, {"scePlayGoDialogClose",playgo_close},
    {"scePlayGoDialogTerminate",playgo_term},
    {"sceLoginDialogInitialize",login_init}, {"sceLoginDialogOpen",login_open},
    {"sceLoginDialogUpdateStatus",login_status}, {"sceLoginDialogGetStatus",login_status},
    {"sceLoginDialogGetResult",login_result}, {"sceLoginDialogClose",login_close},
    {"sceLoginDialogTerminate",login_term}, {"sceCommonDialogIsUsed",common_is_used},
    {"sceAudioInOpen",audio_in_open}, {"sceAudioInHqOpen",audio_in_open}, {"sceAudioInGetSilentState",audio_in_open},
    {"sceAudioInGetGain",audio_in_gain}, {"sceAudioInGetRerouteCount",audio_in_open},
    {"sceMoveInit",move_init}, {"sceMoveOpen",move_open}, {"sceMoveGetDeviceInfo",move_device_info},
    {"sceMoveReadStateRecent",move_read_recent}, {"sceMoveSetLightSphere",move_no_controller},
    {"sceMoveSetVibration",move_no_controller}, {"sceMoveTerm",move_term},
    {"sceDiscMapIsRequestOnHDD",discmap_on_hdd}, {"sceDiscMap_8A828CAEE7EDD5E9",discmap_8a82},
    {"sceDiscMap_7C980FFB0AA27E7A",discmap_7c98},
    {"sceScreenShotEnable",runtime_stub}, {"sceScreenShotDisable",runtime_stub}, {"sceScreenShotSetParam",runtime_stub},
    {"sceScreenShotSetOverlayImage",runtime_stub}, {"sceScreenShotSetOverlayImageWithOrigin",runtime_stub},
    {"sceGameLiveStreamingInitialize",runtime_stub},
    {"sceGameLiveStreamingEnableLiveStreaming",runtime_stub}, {"sceVoiceQoSInit",runtime_stub},
    {"sceContentExportInit2",runtime_stub},
    {"sceRazorCpuPushMarker",razor_idle}, {"sceRazorCpuPushMarkerStatic",razor_idle},
    {"sceRazorCpuPopMarker",razor_idle}, {"sceRazorCpuSync",razor_idle}, {"sceRazorCpuNamedSync",razor_idle},
    {"sceRazorCpuPlotValue",razor_idle}, {"sceRazorCpuWriteBookmark",razor_idle},
    {"sceRazorCpuIsCapturing",razor_idle},
};
uintptr_t runtime_services_resolve(const char *sz_name) {
    const uintptr_t online=runtime_net_resolve(sz_name); /* PS_ONLINE=1: the online module first */
    if (online) return online;
    const uintptr_t found=RUNTIME_LOOKUP(s_exports,sz_name);
    if (found) return found;
    const uintptr_t va_http=runtime_http_resolve(sz_name);
    if (va_http) return va_http;
    const uintptr_t va_trophy=runtime_trophy_resolve(sz_name);
    if (va_trophy) return va_trophy;
    const uintptr_t va_json=runtime_json_resolve(sz_name);
    if (va_json) return va_json;
    const uintptr_t va_np=runtime_np_resolve(sz_name);
    return va_np ? va_np : runtime_socket_resolve(sz_name);
}
#else
uintptr_t runtime_services_resolve(const char *name) { (void)name; return 0; }
#endif
