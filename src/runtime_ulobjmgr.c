/* ulobjmgr: where libSceUlt registers its user-level objects. Contracts as shadPS4's
 * (core/libraries/ulobjmgr): a registered object gets an id below 0x4000; nothing here tracks
 * the objects themselves, as nothing else asks about them. */
#include "runtime.h"
#include <string.h>

#define ERR_INVALID 22 /* POSIX EINVAL, what these return on PS4 */
#define MAX_ID 0x4000

static uint32_t next_id;

static ABI int32_t register_object(uint64_t object, int32_t kind, uint32_t *id) {
    if (!object || !kind || !id) return ERR_INVALID;
    *id=__atomic_fetch_add(&next_id,1,__ATOMIC_RELAXED)%MAX_ID;
    return 0;
}
static ABI int32_t unregister_object(uint32_t id) { return id>=MAX_ID ? ERR_INVALID : 0; }
static ABI int32_t check_object(uint64_t object) { return object ? 0 : ERR_INVALID; }

uintptr_t runtime_ulobjmgr_resolve(const char *name) {
    static const struct { const char *nid; void *fn; } table[]={
        {"BG26hBGiNlw",register_object}, {"Smf+fUNblPc",unregister_object},
        {"HZ9Q2c+4BU4",runtime_stub}, {"SweJO7t3pkk",check_object},
    };
    for (size_t i=0;i<sizeof(table)/sizeof(*table);++i)
        if (runtime_same_nid(name,table[i].nid)) {
            runtime_last_stub=table[i].fn==(void *)runtime_stub;
            return (uintptr_t)table[i].fn;
        }
    return 0;
}
