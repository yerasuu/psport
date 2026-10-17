/* The host platform layer (decision #540): what psport asks of the operating system, behind one
 * small C ABI with an implementation per OS in src/platform/<os>/, so that the runtime and the
 * loader name what they need instead of a Linux call. Included from C and C++ (libpsport-gpu.so
 * reaches these through -rdynamic). Only Linux is implemented; host_features.inc says what each
 * OS has. */
#ifndef PS_HOST_H
#define PS_HOST_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* ---- the capability table (host_features.inc) ---- */
typedef enum {
    PS_HOST_FEATURE_STATE_IMPLEMENTED,
    PS_HOST_FEATURE_STATE_EXPERIMENTAL,
    PS_HOST_FEATURE_STATE_UNAVAILABLE,
} PsHostFeatureState;

typedef uint16_t PsHostFeatureId;
enum {
#define FEATURE(id, area, sz_desc) PS_HOST_FEATURE_##id,
#define FEATURE_HOST(id, os, state, sz_reason, sz_ref)
#define FEATURE_PROBED(id)
#include "host_features.inc"
#undef FEATURE
#undef FEATURE_HOST
#undef FEATURE_PROBED
    PS_HOST_FEATURE_COUNT
};

/* psport --list-features: every entry as JSON on stdout, with this host's state. */
void ps_host_features_list(void);
/* Once at launch, before the game's code runs: the "Host features:" line and the exit summary. */
void ps_host_features_log(void);
/* A FEATURE_PROBED entry's run-time answer, from the code that probes it (the GPU library once the
 * Vulkan device is known): logged and kept for the exit summary. sz_result must outlive the
 * process (a string literal); later answers for the same feature are ignored and said. */
void ps_host_feature_probed(PsHostFeatureId id, int is_available, const char *sz_result);
/* The exit summary line, rebuilt at each publication so that the call report can print it from the
 * fault and watchdog handlers. */
const char *ps_host_features_summary(void);

#ifdef __cplusplus
}
#endif
#endif
