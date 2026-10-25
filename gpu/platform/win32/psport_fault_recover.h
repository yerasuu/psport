// psport: the recovery point of speculative guest memory reads on this thread, on Windows: through
// the runtime's accessor, since a DLL cannot import a thread_local of the executable.
#pragma once
#include "../../../src/platform/ps_host.h"

extern "C" PsHostRecoveryPoint** runtime_fault_recover_slot(void);
#define g_p_runtime_fault_recover (*runtime_fault_recover_slot())
