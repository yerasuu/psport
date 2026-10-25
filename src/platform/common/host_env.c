/* The environment's reader: the same on every OS (ps_host.h); the setter differs (host_system.c). */
#define _GNU_SOURCE
#include "ps_host.h"
#include <stdlib.h>

const char *ps_host_env_get(const char *sz_name) { return getenv(sz_name); }
