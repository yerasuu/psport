/* The process starting again on itself: the settings menu's restart through run.sh, and
 * sceSystemServiceLoadExec on psport's own command line. */
#define _GNU_SOURCE
#include "runtime.h"
#include "gpu/psport_gpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#endif

/* Restarts the game through run.sh (the settings menu: a new render resolution is a patch
 * applied at start). Descriptors are closed first so the old GPU device and its memory are
 * released before the new process opens its own. */
volatile int runtime_restarting;
void runtime_restart(void) {
    psport_gpu_flush_caches(1);
    fflush(NULL);
    puts("Runtime: restarting through run.sh");
    /* The GPU threads still run until exec: their Vulkan calls fail once the device fd is
     * closed below, and an assertion there must not end the process (exit 23) before exec. */
    runtime_restarting = 1;
    __sync_synchronize();
#ifndef _WIN32
    ps_host_close_from(3, -1, 0);
    char *const pp_argv[] = {"bash", "run.sh", NULL};
    ps_host_exec("bash", pp_argv);
    perror("runtime_restart: exec");
    _exit(1);
#endif
}
static char **s_pp_argv;
static int s_c_argv;
void runtime_restart_configure(int c_argv, char **pp_argv) {
    s_pp_argv = pp_argv;
    s_c_argv = c_argv;
}
/* Descriptors are only marked close-on-exec, unlike a restart, so that a failed exec leaves the
 * application running; the old GPU device is released all the same before the new image starts. */
void runtime_load_exec(const char *sz_path, char *const *pp_arguments) {
#ifndef _WIN32
    size_t c_arguments = 0;
    while (pp_arguments && pp_arguments[c_arguments]) ++c_arguments;
    char **pp_argv = calloc((size_t)s_c_argv + 2 * c_arguments + 3, sizeof(*pp_argv));
    if (!pp_argv) return;
    size_t c_argv = 0;
    for (int i = 0; i < s_c_argv; ++i) {
        if (i && i + 1 < s_c_argv && (!strcmp(s_pp_argv[i], "--exec") || !strcmp(s_pp_argv[i], "--arg"))) { ++i; continue; }
        pp_argv[c_argv++] = s_pp_argv[i];
    }
    pp_argv[c_argv++] = "--exec";
    pp_argv[c_argv++] = (char *)sz_path;
    for (size_t i = 0; i < c_arguments; ++i) {
        pp_argv[c_argv++] = "--arg";
        pp_argv[c_argv++] = pp_arguments[i];
    }
    printf("Runtime: sceSystemServiceLoadExec: replacing the application with %s and %zu arguments\n", sz_path, c_arguments);
    for (size_t i = 0; i < c_arguments; ++i) printf("Runtime: sceSystemServiceLoadExec argument %zu: %s\n", i + 1, pp_arguments[i]);
    psport_gpu_flush_caches(1);
    runtime_calls_report();
    fflush(NULL);
    /* The calling thread's signal mask outlives the exec and would be the new main thread's. */
    sigset_t none, previous;
    sigemptyset(&none);
    pthread_sigmask(SIG_SETMASK, &none, &previous);
    ps_host_close_from(3, -1, 1);
    ps_host_exec_self(pp_argv);
    perror("runtime_load_exec: exec");
    pthread_sigmask(SIG_SETMASK, &previous, NULL);
    free(pp_argv);
#else
    (void)sz_path; (void)pp_arguments;
#endif
}
