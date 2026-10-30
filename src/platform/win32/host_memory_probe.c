// SPDX-License-Identifier: GPL-2.0-or-later
/* Windows: the host's memory state, GlobalMemoryStatusEx and the process's Job Object limits
 * (ps_host.h). Windows has no PSI. */
#include "ps_host.h"
#include "host_win32.h"
#include <psapi.h>
#include <stdio.h>

/* A launcher's job (decanter's, #1274) may cap the process or the whole job; the process's private
 * bytes stand for the job's use, since the job holds psport alone. */
static void read_job_limit(PsHostMemory *p_memory) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;
    if (!QueryInformationJobObject(NULL, JobObjectExtendedLimitInformation, &limits, sizeof limits, NULL)) return;
    uint64_t cb_limit = UINT64_MAX;
    const char *sz_source = "";
    if ((limits.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_PROCESS_MEMORY) && limits.ProcessMemoryLimit) {
        cb_limit = limits.ProcessMemoryLimit;
        sz_source = "Job Object process memory limit";
    }
    if ((limits.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_JOB_MEMORY) && limits.JobMemoryLimit &&
        limits.JobMemoryLimit < cb_limit) {
        cb_limit = limits.JobMemoryLimit;
        sz_source = "Job Object memory limit";
    }
    if (cb_limit == UINT64_MAX) return;
    PROCESS_MEMORY_COUNTERS_EX counters = {.cb = sizeof counters};
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&counters, sizeof counters))
        p_memory->cb_limit_used = counters.PrivateUsage;
    p_memory->cb_limit = cb_limit;
    snprintf(p_memory->sz_limit_source, sizeof p_memory->sz_limit_source, "%s", sz_source);
}

int ps_host_memory_read(PsHostMemory *p_out_memory) {
    *p_out_memory = (PsHostMemory){.cb_limit = UINT64_MAX, .pressure_permyriad = -1};
    MEMORYSTATUSEX status = {.dwLength = sizeof status};
    if (!GlobalMemoryStatusEx(&status)) return ps_win32_errno(GetLastError());
    p_out_memory->cb_total = status.ullTotalPhys;
    p_out_memory->cb_available = status.ullAvailPhys;
    read_job_limit(p_out_memory);
    return 0;
}
