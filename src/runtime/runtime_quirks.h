/* The quirk registry (runtime_quirks.inc, docs/en/QUIRKS.md): fixes that concern one title, one
 * host or one middleware, declared once, evaluated before the game's code runs and published as a
 * bitset. Included from C and C++ (libpsport-gpu.so reads g_runtime_quirks through -rdynamic). */
#ifndef RUNTIME_QUIRKS_H
#define RUNTIME_QUIRKS_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RUNTIME_QUIRK_KIND_LAUNCH,
    RUNTIME_QUIRK_KIND_OPTIMIZATION,
    RUNTIME_QUIRK_KIND_HOST,
    RUNTIME_QUIRK_KIND_WORKAROUND,
} RuntimeQuirkKind;

typedef uint16_t RuntimeQuirkId;
enum {
#define QUIRK(id, kind, sz_reason, sz_reference) RUNTIME_QUIRK_##id,
#define QUIRK_GPU(id, vendor, driver_id, sz_device, version_first, version_last)
#define QUIRK_TRAITS(id, traits)
#include "runtime_quirks.inc"
#undef QUIRK
#undef QUIRK_GPU
#undef QUIRK_TRAITS
    RUNTIME_QUIRK_COUNT
};

#define RUNTIME_QUIRK_WORDS 2
/* Written at launch (runtime_quirks_evaluate) and once more when the Vulkan device is known
 * (runtime_quirks_note_gpu, HOST quirks only), never after. */
extern uint64_t g_runtime_quirks[RUNTIME_QUIRK_WORDS];

/* For setup code only (pipeline creation, device creation, configuration): never per draw or per
 * call, so that a quirk costs nothing on the hot paths. */
static inline int runtime_quirk_active(RuntimeQuirkId id) {
    return (int)((__atomic_load_n(&g_runtime_quirks[id / 64], __ATOMIC_RELAXED) >> (id % 64)) & 1);
}

typedef struct {
    const char *sz_name;   /* the title's name, NULL for a title psport has none for */
    const char *sz_serial; /* param.sfo TITLE_ID */
    const RuntimeQuirkId *p_title_quirks;
    size_t c_title_quirks;
    /* The loader's middleware traits (QUIRK_TRAITS), 0 when not detected. */
    uint64_t traits;
} RuntimeQuirkLaunch;

/* Once, before the game's code runs: PS_QUIRKS is validated, the title's quirks and the trait
 * matches decided, each logged, and the result published. */
void runtime_quirks_evaluate(const RuntimeQuirkLaunch *p_launch);
/* The GPU library, once the Vulkan physical device is chosen and before the device is created, so
 * that HOST quirks can shape device creation. Later calls are ignored (and said). */
void runtime_quirks_note_gpu(uint32_t vendor, uint32_t driver_id, const char *sz_name, uint32_t version);
/* psport --list-quirks: every entry as JSON on stdout; with p_launch (--game), the ones its title
 * data would apply and what the policy makes of each, without GPU facts. */
void runtime_quirks_list(const RuntimeQuirkLaunch *p_launch);
/* The exit summary line ("Quirks: N active: a, b\n"), built at each publication so that the call
 * report can print it from the fault and watchdog handlers. */
const char *runtime_quirks_summary(void);

#ifdef __cplusplus
}
#endif
#endif
