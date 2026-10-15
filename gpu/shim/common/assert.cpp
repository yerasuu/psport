// psport: GPU-side assertion failures stop the port with exit code 23.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include "common/assert.h"
#include "common/logging/log.h"
#include "psport_toggles.h"
#include <unistd.h>

// psport (probe.c): set while the port restarts itself through run.sh. The device fd is closed
// before exec; Vulkan calls failing then are not errors: this thread waits for the exec instead.
extern "C" volatile int runtime_restarting;
extern "C" void runtime_calls_report(void);

void assert_fail_impl() {
    // Guest memory read ahead of the GPU thread (g_p_runtime_fault_recover set, as by the draw
    // preparation workers) may already be reused: an assertion on that data abandons the read as
    // a fault does, and the GPU thread reads it again in order.
    if (g_p_runtime_fault_recover) {
        static std::atomic<std::uint32_t> s_c_abandoned_reads;
        const auto c_abandoned_reads = s_c_abandoned_reads.fetch_add(1, std::memory_order_relaxed) + 1;
        if ((c_abandoned_reads & (c_abandoned_reads - 1)) == 0) {
            std::printf("GPU: assertion on guest memory read ahead of the GPU thread, read abandoned "
                        "(%u so far)\n",
                        c_abandoned_reads);
        }
        sigjmp_buf* p_recover = g_p_runtime_fault_recover;
        g_p_runtime_fault_recover = nullptr;
        siglongjmp(*p_recover, 1);
    }
    if (runtime_restarting) {
        for (;;) {
            pause();
        }
    }
    std::fflush(stdout);
    std::fputs("STOP: GPU library assertion failed (see GPU log above)\n", stderr);
    runtime_calls_report();
    std::_Exit(23);
}

[[noreturn]] void unreachable_impl() {
    assert_fail_impl();
    throw std::runtime_error("Unreachable code");
}

void assert_fail_debug_msg(const char* msg) {
    LOG_CRITICAL(Debug, "Assertion Failed!\n{}", msg);
    assert_fail_impl();
}
