/* System services for an offline, single-user console:
 *   - one local user (id 1) who is logged in but not signed in to PSN;
 *   - network cable unplugged: NetCtl disconnected, sockets on loopback only (runtime_socket.c),
 *     no HTTP transfer (runtime_http.c);
 *   - common dialogs complete immediately (no UI is drawn yet);
 *   - no PlayStation VR headset is connected.
 * Every entry here is an explicit contract; unknown functions still stop. */
#define _GNU_SOURCE
#include "runtime.h"
#include "gpu/bbgpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <pthread.h>
#include <time.h>
#include <arpa/inet.h>
#include <sys/stat.h>

#define USER_ID 1
#define ORBIS_OK 0
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
#define NET_EINVAL ((int32_t)0x80410116)
#define DIALOG_NOT_INITIALIZED ((int32_t)0x80B80003)
#define AUDIO_IN_NOT_OPENED ((int32_t)0x80260109)
#define HMD_ALREADY_INITIALIZED ((int32_t)0x81110001)
#define HMD_NOT_INITIALIZED ((int32_t)0x81110002)
#define HMD_HANDLE_INVALID ((int32_t)0x81110003)
#define HMD_PARAMETER_NULL ((int32_t)0x81110008)
#define MOVE_NO_CONTROLLER 1
#define MOVE_NOT_INIT ((int32_t)0x80EE0001)
#define MOVE_ALREADY_INIT ((int32_t)0x80EE0002)
#define MOVE_INVALID_ARG ((int32_t)0x80EE0003)

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static int next_id=1;
static int new_id(void) { pthread_mutex_lock(&lock); int id=next_id++; pthread_mutex_unlock(&lock); return id; }
static void note(const char *what) { printf("Runtime: %s\n",what); }

/* ---- UserService ---- */
static int login_event_pending=1;
static ABI int32_t user_initialize(const void *params) { (void)params; note("UserService initialized (user 1 logged in)"); return 0; }
static ABI int32_t user_initial(int32_t *id) { if (!id) return USER_INVALID_ARGUMENT; *id=USER_ID; return 0; }
static ABI int32_t user_list(int32_t *ids) {
    if (!ids) return USER_INVALID_ARGUMENT;
    ids[0]=USER_ID; ids[1]=ids[2]=ids[3]=-1; return 0;
}
static ABI int32_t user_name(int32_t id,char *name,uint64_t size) {
    if (id!=USER_ID || !name) return USER_INVALID_ARGUMENT;
    const char *value=getenv("PS_USER_NAME") ? getenv("PS_USER_NAME") : "Hunter";
    if (strlen(value)+1>size) return (int32_t)0x8096000a; /* BUFFER_TOO_SHORT */
    strcpy(name,value); return 0;
}
static ABI int32_t user_color(int32_t id,int32_t *color) {
    if (id!=USER_ID || !color) return USER_INVALID_ARGUMENT;
    *color=0; return 0;
}
static ABI int32_t user_event(int32_t *event) {
    if (!event) return USER_INVALID_ARGUMENT;
    pthread_mutex_lock(&lock);
    int pending=login_event_pending; login_event_pending=0;
    pthread_mutex_unlock(&lock);
    if (!pending) return USER_NO_EVENT;
    event[0]=0; event[1]=USER_ID; /* LOGIN */
    return 0;
}

/* ---- SystemService ---- */
int runtime_system_language(void) { const char *v=getenv("PS_LANGUAGE"); return v ? atoi(v) : 1; }
static ABI int32_t system_param(int32_t id,int32_t *value) {
    if (!value) return SYSTEM_PARAMETER;
    switch (id) {
    case 1: *value=runtime_system_language(); break; /* language (1 = English US, 8 = Russian) */
    case 2: *value=1; break;                    /* date format DD/MM/YYYY */
    case 3: *value=1; break;                    /* 24-hour clock */
    case 4: { time_t now=time(NULL); struct tm t; localtime_r(&now,&t); *value=(int32_t)(t.tm_gmtoff/60); break; }
    case 5: *value=0; break;                    /* summer time */
    case 7: *value=0; break;                    /* parental level off */
    case 1000: *value=1; break;                 /* enter button = cross */
    default: fprintf(stderr,"STOP: unsupported system parameter %d\n",id); runtime_stop(21);
    }
    return 0;
}
/* The only string parameter is the console's name (6, SYSTEM_NAME). */
static ABI int32_t system_param_string(int32_t id,char *buffer,uint64_t size) {
    static const char name[]="PS4";
    if (id!=6 || !buffer || size<sizeof(name)) return SYSTEM_PARAMETER;
    memcpy(buffer,name,sizeof(name));
    return 0;
}
/* SceSystemServiceHdrToneMapLuminance in nits: max full frame, max, min. The console's HDR
 * adjustment is never run here, so a common HDR10 display's range stands in for it. */
static ABI int32_t hdr_tone_map_luminance(float *luminance) {
    if (!luminance) return SYSTEM_PARAMETER;
    luminance[0]=1000.0f; luminance[1]=1000.0f; luminance[2]=0.01f;
    return 0;
}
static ABI int32_t system_status(unsigned char *status) {
    if (!status) return SYSTEM_PARAMETER;
    memset(status,0,12); /* event_num=0, no overlay, foreground, normal CPU mode */
    return 0;
}
static ABI int32_t system_event(void *event) { (void)event; return SYSTEM_NO_EVENT; }
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
static ABI int32_t display_safe_area(float *ratio) {
    if (!ratio) return (int32_t)0x80A10003; /* SCE_SYSTEM_SERVICE_ERROR_PARAMETER */
    *ratio=1.0f;
    return 0;
}
/* The console ends the application and starts another executable of its package; the port starts
 * itself again on it (runtime_load_exec), so this returns only when that failed. */
static ABI int32_t load_exec(const char *sz_path, char *const *pp_arguments) {
    char sz_guest[1024], sz_host[4096];
    struct stat info;
    if (sz_path) snprintf(sz_guest,sizeof(sz_guest),sz_path[0]=='/' ? "%s" : "/app0/%s",sz_path);
    if (!sz_path || strncmp(sz_guest,"/app0/",6) || runtime_file_translate(sz_guest,sz_host,sizeof(sz_host)) ||
        stat(sz_host,&info) || !S_ISREG(info.st_mode)) {
        printf("Runtime: sceSystemServiceLoadExec: %s is no executable of the package\n",sz_path ? sz_path : "(null)");
        return SYSTEM_PARAMETER;
    }
    runtime_load_exec(sz_guest,pp_arguments);
    return SYSTEM_INTERNAL;
}

/* ---- NetCtl / Net / Ssl: no network ---- */
static ABI int32_t net_unreachable(void) { *runtime_net_errno()=51; return NET_ENETUNREACH; }
static ABI int32_t net_epoll_create(const char *name,int flags) { (void)name; (void)flags; return new_id(); }
static ABI int32_t net_resolver_create(const char *name,int pool,int flags) { (void)name; (void)pool; (void)flags; return new_id(); }
/* Every lookup fails at once offline (sceNetResolverStartNtoa/Aton), so none is left to abort. */
static ABI int32_t net_resolver_abort(int32_t h_resolver, int32_t flags) { (void)h_resolver; (void)flags; return 0; }
static ABI uint16_t net_htons(uint16_t v) { return htons(v); }
static ABI uint16_t net_ntohs(uint16_t v) { return ntohs(v); }
static ABI uint32_t net_htonl(uint32_t v) { return htonl(v); }
static ABI uint32_t net_ntohl(uint32_t v) { return ntohl(v); }
static ABI int32_t net_pton(int af,const char *src,void *dst) {
    if (af!=2) { *runtime_net_errno()=47; return NET_EINVAL; }
    return inet_pton(AF_INET,src,dst);
}
static ABI const char *net_ntop(int af,const void *src,char *dst,uint32_t size) {
    if (af!=2) { *runtime_net_errno()=47; return NULL; }
    return inet_ntop(AF_INET,src,dst,size);
}
static ABI int32_t netctl_state(int32_t *state) { if (!state) return NET_CTL_INVALID_ADDR; *state=0; return 0; }
/* The interface's own address does not need a connection; everything else does. */
static ABI int32_t netctl_info(int code,void *info) {
    if (code!=2) return NET_CTL_NOT_CONNECTED; /* SCE_NET_CTL_INFO_ETHER_ADDR */
    if (!info) return NET_CTL_INVALID_ADDR;
    runtime_net_ether_address(info);
    return 0;
}
static ABI int32_t netctl_init(void) { note("NetCtl initialized (network cable unplugged)"); return 0; }
static ABI int32_t netctl_register(void *cb,void *arg,int32_t *cid) { (void)arg; if (!cb || !cid) return NET_CTL_INVALID_ADDR; *cid=new_id(); return 0; }
static ABI int32_t netctl_nat(uint32_t *info) {
    if (!info) return NET_CTL_INVALID_ADDR;
    info[1]=0; info[2]=3; info[3]=0; /* stun failed, NAT type 3, no mapped address */
    return 0;
}

/* ---- NP (PSN): signed out ---- */
static ABI int32_t np_state(int32_t user,int32_t *state) {
    if (!state) return NP_INVALID_ARGUMENT;
    (void)user; *state=1; /* SIGNED_OUT */
    return 0;
}
static ABI int32_t np_signed_out(void) { return NP_SIGNED_OUT; }
static ABI int32_t np_account_id(int32_t user,uint64_t *id) {
    if (!id) return NP_INVALID_ARGUMENT;
    (void)user; *id=0; return NP_SIGNED_OUT;
}
static ABI int32_t np_account_id_online(const void *online_id,uint64_t *id) {
    if (!online_id || !id) return NP_INVALID_ARGUMENT;
    *id=0; return NP_SIGNED_OUT;
}
static ABI int32_t np_account_info(const void *online_id,void *info) {
    return online_id && info ? NP_SIGNED_OUT : NP_INVALID_ARGUMENT;
}
static ABI int32_t np_register(void *cb,void *arg) { (void)cb; (void)arg; return new_id(); }
/* Unplugged and signed out, no state change, NetCtl event or request reply ever becomes due, so a
 * check of the registered callbacks or of request timeouts has nothing to run. */
static ABI int32_t nothing_due(void) { return 0; }
static ABI void np_register_void(void *cb,void *arg) { (void)cb; (void)arg; }
static ABI int32_t np_register_toolkit(void *cb,void *arg) { (void)arg; return cb ? 0 : NP_INVALID_ARGUMENT; }
static ABI int32_t np_request(const void *param) { (void)param; return new_id(); }
static ABI int32_t np_request_ctx(int32_t ctx,const void *param) { (void)ctx; (void)param; return new_id(); }
static ABI int32_t np_poll(int32_t request,int32_t *result) { (void)request; if (result) *result=NP_SIGNED_OUT; return 0; }
static ABI int32_t np_compare(const void *a,const void *b) {
    if (!a || !b) return NP_INVALID_ARGUMENT;
    return memcmp(a,b,16) ? (int32_t)0x80550609 : 0; /* NP_UTIL NOT_MATCH */
}

/* ---- Common dialogs: nothing is displayed; an opened dialog finishes. ---- */
typedef struct { const char *name; int initialized, status; } Dialog;
static Dialog dialogs[]={{"CommonDialog",0,0},{"MsgDialog",0,0},{"SaveDataDialog",0,0},
                         {"NpProfileDialog",0,0},{"NpCommerceDialog",0,0},{"ImeDialog",0,0},
                         {"ErrorDialog",0,0},{"WebBrowserDialog",0,0},{"InvitationDialog",0,0},
                         {"SigninDialog",0,0},{"SocialScreenDialog",0,0},
                         {"HmdSetupDialog",0,0},{"PlayGoDialog",0,0}};
static int common_initialized;
static ABI int32_t common_init(void) { common_initialized=1; return 0; }
static int32_t dialog_init(int i) {
    if (dialogs[i].initialized) return (int32_t)0x80B80004;
    dialogs[i].initialized=1; dialogs[i].status=1; return 0;
}
static int32_t dialog_open(int i) {
    if (!dialogs[i].initialized) return DIALOG_NOT_INITIALIZED;
    printf("Runtime: %s opened; completed immediately (no dialog UI yet)\n",dialogs[i].name);
    dialogs[i].status=3; return 0;
}
static int32_t dialog_status(int i) { return dialogs[i].status; }
static int32_t dialog_term(int i) {
    if (!dialogs[i].initialized) return DIALOG_NOT_INITIALIZED;
    dialogs[i].initialized=0; dialogs[i].status=0; return 0;
}
#define DIALOG(tag,i) \
    static ABI int32_t tag##_init(void) { return dialog_init(i); } \
    static ABI int32_t tag##_open(const void *p) { (void)p; return dialog_open(i); } \
    static ABI int32_t tag##_status(void) { return dialog_status(i); } \
    static ABI int32_t tag##_term(void) { return dialog_term(i); }
DIALOG(msg,1) DIALOG(profile,3) DIALOG(commerce,4)
DIALOG(web,7) DIALOG(invitation,8) DIALOG(signin,9) DIALOG(social,10) DIALOG(hmd_setup,11) DIALOG(playgo,12)
static ABI int32_t profile_result(void *result) { if (result) memset(result,0,4); return 0; }
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
    if (dialogs[4].status!=3) return (int32_t)0x80B80005;   /* NOT_FINISHED */
    memset(p_out_result,0,sizeof(*p_out_result));
    p_out_result->result=1;
    p_out_result->p_user_data=s_p_commerce_user_data;
    return 0;
}
/* The user cannot sign in to PSN here, so the sign-in dialog always ends cancelled. */
static ABI int32_t signin_result(int32_t *p_out_result) {
    if (!p_out_result) return (int32_t)0x80B8000D;           /* ARG_NULL */
    if (dialogs[9].status!=3) return (int32_t)0x80B80005;   /* NOT_FINISHED */
    *p_out_result=1;
    return 0;
}
typedef struct {
    void *callback_arg; int32_t error_code, result; uint32_t *sent_count; uint8_t reserved[32];
} InvitationResult;
static void *invitation_callback_arg;
static ABI int32_t invitation_start(const uint8_t *param) {
    if (param) memcpy(&invitation_callback_arg,param+64,sizeof(invitation_callback_arg));
    return invitation_open(param);
}
static ABI int32_t invitation_result(InvitationResult *result) {
    if (!dialogs[8].initialized) return DIALOG_NOT_INITIALIZED;
    if (dialogs[8].status!=3) return (int32_t)0x80B80005;
    if (!result) return (int32_t)0x80B8000D;
    result->callback_arg=invitation_callback_arg; result->error_code=0; result->result=0;
    if (result->sent_count) *result->sent_count=0;
    return 0;
}
/* With nothing displayed, a dialog is ready to show whenever it is initialized (Atelier Ayesha
 * waited on these before opening its auto-load dialog; answered 0 they never turned true). */
static ABI int32_t save_ready(void) { return dialogs[2].initialized; }
/* OrbisSaveDataDialogParam (0x98 bytes): common base 0x30, size, mode at 0x34, items at 0x48,
 * user message parameter at 0x50 (its buttonType first), userData at 0x70. */
static struct { int32_t mode; void *user_data; char dir[32]; _Alignas(8) uint8_t param[0x530]; } save_dialog;
/* OrbisSaveDataDialogItems: userId, titleId at 8, dirName array and its count at 0x10 and 0x18,
 * newItem at 0x20, focusPos at 0x28, focusPosDirName at 0x30. The list shows the saves that exist
 * and is accepted on the item it opens on; the new item, which heads the list, leaves dir empty. */
static void save_list_focus(const uint8_t *items) {
    memset(save_dialog.dir,0,sizeof(save_dialog.dir));
    if (!items) return;
    int32_t user, focus;
    uint32_t count;
    const char *title, *dirs, *new_item, *focus_dir;
    memcpy(&user,items,sizeof(user));
    memcpy(&title,items+0x08,sizeof(title));
    memcpy(&dirs,items+0x10,sizeof(dirs));
    memcpy(&count,items+0x18,sizeof(count));
    memcpy(&new_item,items+0x20,sizeof(new_item));
    memcpy(&focus,items+0x28,sizeof(focus));
    memcpy(&focus_dir,items+0x30,sizeof(focus_dir));
    if (dirs && !(focus==0 && new_item)) {
        _Alignas(8) uint8_t param[sizeof(save_dialog.param)];
        int64_t best=0;
        for (uint32_t i=0;i<count;++i) {
            const char *dir=dirs+32*i;
            if (runtime_savedata_param(user,title,dir,param)) continue;
            int64_t mtime;
            memcpy(&mtime,param+0x508,sizeof(mtime));
            int take;
            switch (focus) {
            case 1: case 3: take=1; break;                                     /* LISTTAIL, DATATAIL */
            case 4: take=!save_dialog.dir[0] || mtime>best; break;             /* DATALATEST */
            case 5: take=!save_dialog.dir[0] || mtime<best; break;             /* DATAOLDEST */
            case 6: take=focus_dir && !strncmp(dir,focus_dir,32); break;       /* DIRNAME */
            default: take=!save_dialog.dir[0]; break;                          /* LISTHEAD, DATAHEAD */
            }
            if (!take) continue;
            best=mtime;
            memcpy(save_dialog.dir,dir,sizeof(save_dialog.dir));
            memcpy(save_dialog.param,param,sizeof(param));
        }
    }
    if (save_dialog.dir[0])
        printf("Runtime: SaveDataDialog list (focus %d, %u items) answered with '%.32s'\n",focus,count,save_dialog.dir);
    else
        printf("Runtime: SaveDataDialog list (focus %d, %u items) answered with %s\n",focus,count,
               new_item ? "the new item" : "no save (none of its items exists)");
}
static ABI int32_t save_init(void) { return dialog_init(2); }
static ABI int32_t save_status(void) { return dialog_status(2); }
static ABI int32_t save_term(void) { return dialog_term(2); }
/* A progress bar or a user message without buttons has nothing for the user to answer: it stays
 * running until the game closes it (Atelier Ryza reopened a progress bar that ended by itself, in
 * a loop, and never wrote its save). */
static ABI int32_t save_open(const uint8_t *param) {
    if (!dialogs[2].initialized) return DIALOG_NOT_INITIALIZED;
    if (!param) return (int32_t)0x80B8000D; /* SCE_COMMON_DIALOG_ERROR_ARG_NULL */
    int32_t mode;
    const int32_t *user_msg;
    memcpy(&mode,param+0x34,sizeof(mode));
    memcpy(&user_msg,param+0x50,sizeof(user_msg));
    memcpy(&save_dialog.user_data,param+0x70,sizeof(save_dialog.user_data));
    save_dialog.mode=mode;
    if (mode==1) {
        const uint8_t *items;
        memcpy(&items,param+0x48,sizeof(items));
        save_list_focus(items);
    }
    if (mode==5 || (mode==2 && user_msg && user_msg[0]==2)) {
        printf("Runtime: SaveDataDialog opened (mode %d); runs until the game closes it (no dialog UI yet)\n",mode);
        dialogs[2].status=2;
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
static ABI int32_t save_result(int32_t *result) {
    if (!result) return (int32_t)0x80B8000D; /* SCE_COMMON_DIALOG_ERROR_ARG_NULL */
    if (dialogs[2].status!=3) return (int32_t)0x80B80005; /* SCE_COMMON_DIALOG_ERROR_NOT_FINISHED */
    result[0]=save_dialog.mode;
    result[1]=0;
    result[2]=1;
    memcpy(result+8,&save_dialog.user_data,sizeof(save_dialog.user_data));
    if (save_dialog.mode==1) {
        char *dir;
        void *param;
        memcpy(&dir,result+4,sizeof(dir));
        memcpy(&param,result+6,sizeof(param));
        if (dir) memcpy(dir,save_dialog.dir,sizeof(save_dialog.dir));
        if (param && save_dialog.dir[0] && save_dialog.param[0]) memcpy(param,save_dialog.param,sizeof(save_dialog.param));
    }
    return 0;
}
static ABI int32_t save_close(const void *param) { (void)param; dialogs[2].status=3; return 0; }
/* OrbisMsgDialogResult: mode, result, buttonId, reserved[32]; ends as accepted, like SaveDataDialog. */
static ABI int32_t msg_result(int32_t *result) {
    if (dialogs[1].status!=3) return (int32_t)0x80B80005; /* SCE_COMMON_DIALOG_ERROR_NOT_FINISHED */
    if (!result) return (int32_t)0x80B8000D;              /* SCE_COMMON_DIALOG_ERROR_ARG_NULL */
    memset(result,0,44);
    result[2]=1;
    return 0;
}
static ABI int32_t msg_close(void) { dialogs[1].status=3; return 0; }
typedef struct { int32_t size, error_code, user, reserved; } ErrorDialogParam;
static ABI int32_t error_init(void) { return dialog_init(6); }
static ABI int32_t error_open(const ErrorDialogParam *param) {
    static uint32_t logged[32];
    static int logged_count;
    if (dialogs[6].status!=1 && dialogs[6].status!=3) return (int32_t)0x80B80006; /* INVALID_STATE */
    if (!param) return (int32_t)0x80B8000D;                                         /* ARG_NULL */
    const uint32_t code=(uint32_t)param->error_code;
    int seen=0;
    for (int k=0;k<logged_count;++k) seen|=logged[k]==code;
    if (!seen && logged_count<32) {
        logged[logged_count++]=code;
        printf("Runtime: ErrorDialog 0x%08X\n",code);
    }
    dialogs[6].status=3; return 0;
}
static ABI int32_t error_status(void) { return dialog_status(6); }
static ABI int32_t error_close(void) { return (int32_t)0x80B8000B; } /* NOT_RUNNING: an opened one has already finished */
static ABI int32_t error_term(void) { return dialog_term(6); }
/* OrbisHmdSetupDialogResult: result, reserved. With no headset to set up, the user can only
 * back out of the dialog: SCE_COMMON_DIALOG_RESULT_USER_CANCELED. */
static ABI int32_t hmd_setup_result(int32_t *result) {
    if (dialogs[11].status!=3) return (int32_t)0x80B80005; /* NOT_FINISHED */
    if (!result) return (int32_t)0x80B8000D;               /* ARG_NULL */
    result[0]=1;
    return 0;
}
static ABI int32_t hmd_setup_close(void) { dialogs[11].status=3; return 0; }
/* ScePlayGoDialogResult: result, reserved. Every chunk is installed, so the dialog has nothing to
 * wait for and ends as accepted: SCE_COMMON_DIALOG_RESULT_OK. */
static ABI int32_t playgo_result(int32_t *p_out_result) {
    if (dialogs[12].status!=3) return (int32_t)0x80B80005; /* NOT_FINISHED */
    if (!p_out_result) return (int32_t)0x80B8000D;         /* ARG_NULL */
    p_out_result[0]=0;
    return 0;
}
static ABI int32_t playgo_close(void) { dialogs[12].status=3; return 0; }
/* SceWebBrowserDialogResult: result, then reserved. Offline, no page loads and the user can only
 * back out: SCE_COMMON_DIALOG_RESULT_USER_CANCELED. */
static ABI int32_t web_result(int32_t *p_out_result) {
    if (dialogs[7].status!=3) return (int32_t)0x80B80005; /* NOT_FINISHED */
    if (!p_out_result) return (int32_t)0x80B8000D;         /* ARG_NULL */
    p_out_result[0]=1;
    return 0;
}
/* ImeDialog: text typed on the keyboard into the game window (title bar shows it).
 * OrbisImeDialogParam: user, type, languages(8), enter label, method, filter,
 * option, max length, char16 buffer, position, alignment, placeholder, title. */
typedef struct {
    int32_t user; uint32_t type; uint64_t languages; uint32_t enter_label, input_method;
    void *filter; uint32_t option, max_length; uint16_t *buffer;
    float x, y; uint32_t halign, valign; const uint16_t *placeholder, *title; int8_t reserved[16];
} ImeParam;
static struct { int running, finished, end_status; uint16_t *buffer; uint32_t max_length; } ime;
size_t utf16_to_utf8(const uint16_t *in, size_t limit, char *out, size_t size) {
    size_t n=0;
    for (size_t i=0; in && i<limit && in[i] && n+4<size; ++i) {
        uint32_t c=in[i];
        if (c>=0xD800 && c<0xDC00 && i+1<limit && in[i+1]>=0xDC00 && in[i+1]<0xE000) { c=0x10000+((c-0xD800)<<10)+(in[i+1]-0xDC00); ++i; }
        if (c<0x80) out[n++]=(char)c;
        else if (c<0x800) { out[n++]=(char)(0xC0|c>>6); out[n++]=(char)(0x80|(c&63)); }
        else if (c<0x10000) { out[n++]=(char)(0xE0|c>>12); out[n++]=(char)(0x80|((c>>6)&63)); out[n++]=(char)(0x80|(c&63)); }
        else { out[n++]=(char)(0xF0|c>>18); out[n++]=(char)(0x80|((c>>12)&63)); out[n++]=(char)(0x80|((c>>6)&63)); out[n++]=(char)(0x80|(c&63)); }
    }
    out[n]=0; return n;
}
void utf8_to_utf16(const char *in, uint16_t *out, uint32_t max) {
    uint32_t n=0;
    for (const unsigned char *p=(const unsigned char *)in; *p && n<max;) {
        uint32_t c; int extra;
        if (*p<0x80) { c=*p; extra=0; } else if ((*p&0xE0)==0xC0) { c=*p&31; extra=1; }
        else if ((*p&0xF0)==0xE0) { c=*p&15; extra=2; } else { c=*p&7; extra=3; }
        ++p;
        for (int k=0;k<extra && (*p&0xC0)==0x80;++k) c=(c<<6)|(*p++&63);
        if (c>=0x10000) { if (n+2>max) break; c-=0x10000; out[n++]=(uint16_t)(0xD800+(c>>10)); out[n++]=(uint16_t)(0xDC00+(c&1023)); }
        else out[n++]=(uint16_t)c;
    }
    out[n<max ? n : max]=0;
}
static void ime_complete(int end_status, const char *text) {
    if (!end_status && ime.buffer && ime.max_length) utf8_to_utf16(text,ime.buffer,ime.max_length);
    ime.end_status=end_status; ime.finished=1; ime.running=0;
    printf("Runtime: ImeDialog %s%s%s\n",end_status ? "cancelled" : "text: ",end_status ? "" : text,"");
}
static ABI int32_t ime_init(const ImeParam *param, const void *extended) {
    (void)extended;
    if (!param || !param->buffer || !param->max_length) return (int32_t)0x80BC0004; /* IME INVALID_ADDRESS */
    if (ime.running) return (int32_t)0x80BC0003;                                  /* BUSY */
    memset(&ime,0,sizeof(ime));
    ime.buffer=param->buffer; ime.max_length=param->max_length; ime.running=1;
    char initial[512], prompt[256];
    utf16_to_utf8(param->buffer,param->max_length,initial,sizeof(initial));
    utf16_to_utf8(param->title,128,prompt,sizeof(prompt));
    const char *preset=getenv("PS_IME_TEXT");
    if (preset) { ime_complete(0,preset); return 0; }
    if (!bbgpu_text_input_begin(initial,prompt[0] ? prompt : "Text")) {
        const char *name=getenv("PS_USER_NAME");
        ime_complete(0,name ? name : initial[0] ? initial : "Hunter");
    } else printf("Runtime: ImeDialog opened: type in the game window, Enter to confirm, Esc to cancel\n");
    return 0;
}
static ABI int32_t ime_status(void) {
    if (ime.running) {
        char text[512];
        int state=bbgpu_text_input_poll(text,sizeof(text));
        if (state) ime_complete(state==1 ? 0 : 1,text);
    }
    return ime.running ? 1 : ime.finished ? 2 : 0; /* Running / Finished / None */
}
static ABI int32_t ime_result(uint32_t *result) {
    if (!ime.finished) return (int32_t)0x80BC0101; /* DIALOG NOT_FINISHED */
    if (result) *result=(uint32_t)ime.end_status;
    return 0;
}
static ABI int32_t ime_term(void) { memset(&ime,0,sizeof(ime)); return 0; }
/* The dialog's panel is the on-screen keyboard's (runtime_ime.c), whatever the extended parameters ask. */
static ABI int32_t ime_panel_size_extended(const ImeParam *param, const void *extended, uint32_t *width, uint32_t *height) {
    (void)extended;
    if (!param || !width || !height) return (int32_t)0x80BC0031; /* IME INVALID_ADDRESS */
    runtime_ime_panel_size(param->type,width,height);
    return 0;
}
/* A dialog is in use from its initialization until its termination, and the IME dialog while it runs. */
static ABI int32_t common_is_used(void) {
    for (size_t i=0;i<sizeof(dialogs)/sizeof(*dialogs);++i) if (dialogs[i].initialized) return 1;
    return ime.running;
}

/* ---- PlayStation VR: the library runs, no headset is detected ---- */
static int hmd_initialized;
static ABI int32_t hmd_init(const void *param) {
    if (hmd_initialized) return HMD_ALREADY_INITIALIZED;
    if (!param) return HMD_PARAMETER_NULL;
    hmd_initialized=1; return 0;
}
static ABI int32_t hmd_term(void) {
    if (!hmd_initialized) return HMD_NOT_INITIALIZED;
    hmd_initialized=0; return 0;
}
/* OrbisHmdDeviceInformation (32 bytes): status, user, reserved, panel size, latencies, HMU mount. */
static ABI int32_t hmd_device_info(int32_t *info) {
    if (!info) return HMD_PARAMETER_NULL;
    if (!hmd_initialized) return HMD_NOT_INITIALIZED;
    memset(info,0,32);
    info[0]=2; /* SCE_HMD_DEVICE_STATUS_NOT_DETECTED */
    info[1]=-1;
    return 0;
}
static ABI int32_t hmd_handle_invalid(void) { return HMD_HANDLE_INVALID; }

/* ---- DiscMap: the game is fully installed, no disc bitmap exists ---- */
#define DISC_MAP_NO_BITMAP ((int32_t)0x81100004)
/* Every byte of an installed title is on the HDD; libSceFios2 takes a failure here for a request
 * off the disc. */
static ABI int32_t discmap_on_hdd(const char *path,int64_t offset,int64_t size,int32_t *result) {
    (void)path; (void)offset; (void)size;
    if (!result) return (int32_t)0x81100001; /* INVALID_ARGUMENT */
    *result=1;
    return 0;
}
/* The one the games call at boot succeeds instead, as in shadPS4, with its three outputs zero:
 * they read them. */
static ABI int32_t discmap_7c98(const char *path,int64_t offset,int64_t size,int32_t *flags,int32_t *r1,int32_t *r2) {
    (void)path; (void)offset; (void)size;
    if (!flags || !r1 || !r2) return (int32_t)0x81100001; /* INVALID_ARGUMENT */
    *flags=0; *r1=0; *r2=0;
    return 0;
}
static ABI int32_t discmap_8a82(const char *path,int64_t offset,int64_t size,int32_t *flags,int32_t *r1,int32_t *r2) {
    (void)path; (void)offset; (void)size; (void)flags; (void)r1; (void)r2; return DISC_MAP_NO_BITMAP;
}

/* ---- Devices that are absent: microphone ---- */
static ABI int32_t audio_in_open(void) { return AUDIO_IN_NOT_OPENED; }
/* PlayStation Move: the library runs, no motion controller is ever connected. A handle is still
 * handed out per open, distinct from other devices' handles, as on the console. */
static int move_initialized;
static ABI int32_t move_init(void) {
    if (move_initialized) return MOVE_ALREADY_INIT;
    move_initialized=1; return 0;
}
static ABI int32_t move_open(int32_t user,int32_t type,int32_t index) {
    (void)user; (void)type; (void)index;
    static int32_t handle=0x30b0000;
    if (!move_initialized) return MOVE_NOT_INIT;
    return __atomic_add_fetch(&handle,0x100,__ATOMIC_RELAXED);
}
/* OrbisMoveDeviceInfo: sphere radius and accelerometer offset, four floats. */
static ABI int32_t move_device_info(int32_t handle,float *info) {
    (void)handle;
    if (!move_initialized) return MOVE_NOT_INIT;
    if (!info) return MOVE_INVALID_ARG;
    memset(info,0,16);
    return MOVE_NO_CONTROLLER;
}
static ABI int32_t move_read_recent(int32_t handle,int64_t timestamp,void *data,int32_t *count) {
    (void)handle;
    if (!move_initialized) return MOVE_NOT_INIT;
    if (timestamp<0 || !data || !count) return MOVE_INVALID_ARG;
    *count=0;
    return MOVE_NO_CONTROLLER;
}
static ABI int32_t move_no_controller(int32_t handle) { (void)handle; return move_initialized ? MOVE_NO_CONTROLLER : MOVE_NOT_INIT; }
static ABI int32_t move_term(void) {
    if (!move_initialized) return MOVE_NOT_INIT;
    move_initialized=0; return 0;
}
/* ---- RazorCpu ---- */
/* The profiler's markers reach Razor only on a devkit capturing with it; a retail console, like
 * this port, has no capture running and drops them, so they are no stub. */
static ABI int32_t razor_idle(void) { return 0; }


static const RuntimeExport exports[]={
    {"sceUserServiceInitialize",user_initialize}, {"sceUserServiceTerminate",runtime_stub},
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
    {"sceNetInetPton",net_pton}, {"sceNetInetNtop",net_ntop},
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
    {"sceSocialScreenDialogTerminate",social_term},
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
    {"scePlayGoDialogTerminate",playgo_term}, {"sceCommonDialogIsUsed",common_is_used},
    {"sceHmdInitialize",hmd_init}, {"sceHmdInitialize315",hmd_init}, {"sceHmdTerminate",hmd_term},
    {"sceHmdGetDeviceInformation",hmd_device_info},
    {"sceHmdGetDeviceInformationByHandle",hmd_handle_invalid}, {"sceHmdGetFieldOfView",hmd_handle_invalid},
    {"sceAudioInOpen",audio_in_open}, {"sceAudioInHqOpen",audio_in_open}, {"sceAudioInGetSilentState",audio_in_open},
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
uintptr_t runtime_services_resolve(const char *name) {
    const uintptr_t online=runtime_net_resolve(name); /* PS_ONLINE=1: the online module first */
    if (online) return online;
    const uintptr_t found=RUNTIME_LOOKUP(exports,name);
    if (found) return found;
    const uintptr_t va_http=runtime_http_resolve(name);
    if (va_http) return va_http;
    const uintptr_t va_trophy=runtime_trophy_resolve(name);
    if (va_trophy) return va_trophy;
    const uintptr_t va_json=runtime_json_resolve(name);
    if (va_json) return va_json;
    const uintptr_t va_np=runtime_np_resolve(name);
    return va_np ? va_np : runtime_socket_resolve(name);
}
#else
uintptr_t runtime_services_resolve(const char *name) { (void)name; return 0; }
#endif
