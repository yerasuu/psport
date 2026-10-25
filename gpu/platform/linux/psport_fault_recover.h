// psport: the recovery point of speculative guest memory reads on this thread, on Linux: the
// runtime's thread-local, read directly (runtime_memory_gpu.c).
#pragma once
#include "../../../src/platform/ps_host.h"

extern "C" __thread PsHostRecoveryPoint* g_p_runtime_fault_recover;
