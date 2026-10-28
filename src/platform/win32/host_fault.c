// SPDX-License-Identifier: GPL-2.0-or-later
/* Windows: faults through a vectored exception handler, thread interrupts by redirecting the
 * suspended thread, and the recovery points' own setjmp and longjmp (ps_host.h). Ported by hand from
 * bbport 0.6-pre1 (gpu/platform/win32/posix_win32.cpp: the handler, bb_setjmp and bb_longjmp) and
 * Supermedo's windows branch (src/probe.c: resuming a recovery point outside the dispatcher); the
 * interrupts and ps_host_resume are psport's own. */
#include "ps_host.h"
#include "host_win32.h"
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#define MAX_HANDLERS 4
#define XSAVE_HEADER 512
#define XSAVE_YMM_END 832
/* The System V red zone, which guest leaf functions keep live data in below rsp. */
#define RED_ZONE 128
#define C_THREAD_STATES 4096
#define TID_FREE 0
/* x87, SSE and AVX: the state the PS4's CPU has. */
#define XSAVE_COMPONENTS 7

/* Linux's numbers, so that reports and exit codes (128 + signal) read the same on every host. */
enum {
    HOST_SIGHUP = 1,
    HOST_SIGINT = 2,
    HOST_SIGILL = 4,
    HOST_SIGTRAP = 5,
    HOST_SIGABRT = 6,
    HOST_SIGBUS = 7,
    HOST_SIGFPE = 8,
    HOST_SIGSEGV = 11,
    HOST_SIGUSR2 = 12,
    HOST_SIGALRM = 14,
    HOST_SIGTERM = 15,
    HOST_SIGPROF = 27,
    HOST_SIGRTMIN_3 = 37,
};

struct PsHostFaultContext {
    uint64_t *p_registers;
    const void *p_fxsave;
    void *p_address;
    /* The exception's context, resumed when the handlers return; NULL for interrupts. */
    CONTEXT *p_record;
    /* Back into the exception dispatch, so that a recovery leaves it before resuming; NULL for
     * interrupts. */
    uint64_t *p_dispatch;
    uint64_t error_code;
    uint32_t trap_number;
    int signal;
    PsHostFaultKind kind;
    int interrupt;
    uint16_t code_selector;
};

/* What an interrupted thread resumes from, built on its own stack below the red zone: the xsave
 * image, the registers in PsHostRegister order and an iretq frame. The assembly below uses these
 * offsets. */
typedef struct {
    _Alignas(64) uint8_t xsave[XSAVE_YMM_END];
    uint64_t registers[PS_HOST_REG_COUNT];
    uint64_t iret[5];
} ResumeFrame;
_Static_assert(offsetof(ResumeFrame, registers) == 832, "frame offsets are spelled out in the assembly");
_Static_assert(offsetof(ResumeFrame, iret) == 976, "frame offsets are spelled out in the assembly");
_Static_assert(sizeof(ResumeFrame) == 1024, "frame size is spelled out in the assembly");
_Static_assert(PS_HOST_INTERRUPT_COUNT == 4, "one entry stub per interrupt in the assembly");

/* Per host thread, since Windows has no signal mask: which interrupts wait (held) and which came
 * while they waited (pending), where an interrupt found the thread, the address it waits on. Found
 * by thread id, so that another thread can reach it; entries are never removed, a thread that ends
 * clears its own and a later thread with the same id takes it again. */
typedef struct {
    _Atomic uint32_t tid;
    _Atomic unsigned held;
    _Atomic unsigned pending;
    /* Fault kinds whose handlers run on this thread, touched by the thread only. */
    unsigned running_faults;
    _Atomic(const void *) p_wait_word;
    uint64_t va_rip[PS_HOST_INTERRUPT_COUNT];
    uint64_t va_rsp[PS_HOST_INTERRUPT_COUNT];
} ThreadState;

typedef LONG(NTAPI *PfnCopyVirtualMemory)(HANDLE h_process, void *p_remote, void *p_local, SIZE_T cb_copy,
                                          SIZE_T *p_out_cb_copied);

void ps_win32_interrupt_entry_0(void);
void ps_win32_interrupt_entry_1(void);
void ps_win32_interrupt_entry_2(void);
void ps_win32_interrupt_entry_3(void);
void ps_win32_interrupt_entered(ResumeFrame *p_frame, int interrupt);
__attribute__((noreturn)) void ps_win32_resume_frame(ResumeFrame *p_frame);
void ps_win32_interrupt_self(uint64_t *p_va_rip, uint64_t *p_va_rsp, void (*pfn_entry)(void));

static void (*const s_pfn_interrupt_entries[PS_HOST_INTERRUPT_COUNT])(void) = {
    ps_win32_interrupt_entry_0, ps_win32_interrupt_entry_1, ps_win32_interrupt_entry_2, ps_win32_interrupt_entry_3};
static const int s_fault_signals[PS_HOST_FAULT_KIND_COUNT] = {HOST_SIGSEGV, HOST_SIGBUS, HOST_SIGILL, HOST_SIGFPE,
                                                              HOST_SIGTRAP};
static const size_t s_off_context_registers[PS_HOST_REG_RFLAGS] = {
    offsetof(CONTEXT, Rax), offsetof(CONTEXT, Rbx), offsetof(CONTEXT, Rcx), offsetof(CONTEXT, Rdx),
    offsetof(CONTEXT, Rsi), offsetof(CONTEXT, Rdi), offsetof(CONTEXT, Rbp), offsetof(CONTEXT, Rsp),
    offsetof(CONTEXT, R8),  offsetof(CONTEXT, R9),  offsetof(CONTEXT, R10), offsetof(CONTEXT, R11),
    offsetof(CONTEXT, R12), offsetof(CONTEXT, R13), offsetof(CONTEXT, R14), offsetof(CONTEXT, R15),
    offsetof(CONTEXT, Rip),
};
static _Atomic(PsHostFaultHandler) s_fault_handlers[PS_HOST_FAULT_KIND_COUNT][MAX_HANDLERS];
static _Atomic int s_c_fault_handlers[PS_HOST_FAULT_KIND_COUNT];
static _Atomic unsigned s_fault_flags[PS_HOST_FAULT_KIND_COUNT];
static _Atomic(PsHostInterruptHandler) s_interrupt_handlers[PS_HOST_INTERRUPT_COUNT];
static void *s_p_vectored_handler;
static SRWLOCK s_install_lock = SRWLOCK_INIT;
static ThreadState s_thread_states[C_THREAD_STATES];
static INIT_ONCE s_fls_once = INIT_ONCE_STATIC_INIT;
static DWORD s_i_fls = FLS_OUT_OF_INDEXES;
static void (*s_pfn_termination_report)(int sig);
static _Atomic int s_has_reported_termination;

static void write_error(const char *sz_line) {
    DWORD cb_written;
    (void)WriteFile(GetStdHandle(STD_ERROR_HANDLE), sz_line, (DWORD)strlen(sz_line), &cb_written, NULL);
}

static void write_win32_error(const char *sz_what, DWORD error) {
    char sz_line[160];
    snprintf(sz_line, sizeof(sz_line), "Host: %s failed (Windows error %lu)\n", sz_what, (unsigned long)error);
    write_error(sz_line);
}

static void release_state(void *p_raw) {
    ThreadState *p_state = p_raw;
    if (!p_state) return;
    atomic_store(&p_state->held, 0);
    atomic_store(&p_state->pending, 0);
    atomic_store(&p_state->p_wait_word, NULL);
    p_state->running_faults = 0;
}

static void WINAPI release_state_at_exit(void *p_raw) { release_state(p_raw); }

static BOOL CALLBACK allocate_fls(INIT_ONCE *p_once, void *p_parameter, void **pp_context) {
    (void)p_once;
    (void)p_parameter;
    (void)pp_context;
    s_i_fls = FlsAlloc(release_state_at_exit);
    if (s_i_fls == FLS_OUT_OF_INDEXES) write_win32_error("FlsAlloc for interrupt states", GetLastError());
    return TRUE;
}

/* Lock-free, since an interrupt handler may call it on a thread interrupted inside it: two claims
 * of one id walk the same slots and stop at the same free one. */
static ThreadState *claim_state(uint32_t tid) {
    static _Atomic int s_has_reported_full;
    const size_t i_home = (size_t)((tid >> 2) * 2654435761u) % C_THREAD_STATES;
    for (size_t i = 0; i < C_THREAD_STATES; ++i) {
        ThreadState *p_state = &s_thread_states[(i_home + i) % C_THREAD_STATES];
        uint32_t current = atomic_load(&p_state->tid);
        if (current == TID_FREE && atomic_compare_exchange_strong(&p_state->tid, &current, tid)) return p_state;
        if (current == tid) return p_state;
    }
    if (!atomic_exchange(&s_has_reported_full, 1))
        write_error("Host: interrupt state table full (4096 thread ids): interrupts to further threads fail with "
                    "EAGAIN and their faults do not hold the guest exception\n");
    return NULL;
}

static ThreadState *state_of_self(void) {
    InitOnceExecuteOnce(&s_fls_once, allocate_fls, NULL, NULL);
    if (s_i_fls != FLS_OUT_OF_INDEXES) {
        ThreadState *p_state = FlsGetValue(s_i_fls);
        if (p_state) return p_state;
    }
    ThreadState *p_state = claim_state(GetCurrentThreadId());
    if (p_state && s_i_fls != FLS_OUT_OF_INDEXES) FlsSetValue(s_i_fls, p_state);
    return p_state;
}

void ps_win32_wait_begin(const void *p_word) {
    ThreadState *p_state = state_of_self();
    if (p_state) atomic_store(&p_state->p_wait_word, p_word);
}

void ps_win32_wait_end(void) {
    ThreadState *p_state = state_of_self();
    if (p_state) atomic_store(&p_state->p_wait_word, NULL);
}

int ps_host_interrupt_signal(PsHostInterrupt interrupt) {
    switch (interrupt) {
    case PS_HOST_INTERRUPT_GUEST_EXCEPTION: return HOST_SIGRTMIN_3;
    case PS_HOST_INTERRUPT_THREAD_DUMP: return HOST_SIGUSR2;
    case PS_HOST_INTERRUPT_WATCHDOG: return HOST_SIGALRM;
    case PS_HOST_INTERRUPT_SAMPLE: return HOST_SIGPROF;
    case PS_HOST_INTERRUPT_COUNT: break;
    }
    return 0;
}

PsHostFaultKind ps_host_fault_kind(const PsHostFaultContext *p_context) { return p_context->kind; }

int ps_host_fault_signal(const PsHostFaultContext *p_context) { return p_context->signal; }

void *ps_host_fault_address(const PsHostFaultContext *p_context) { return p_context->p_address; }

/* The page fault's error code as Linux reports it: bit 1 a write, bit 4 an instruction fetch. */
PsHostFaultAccess ps_host_fault_access(const PsHostFaultContext *p_context) {
    if (p_context->kind != PS_HOST_FAULT_ACCESS) return PS_HOST_FAULT_UNKNOWN;
    return p_context->error_code & 0x10  ? PS_HOST_FAULT_EXECUTE
           : p_context->error_code & 0x2 ? PS_HOST_FAULT_WRITE
                                         : PS_HOST_FAULT_READ;
}

uint64_t ps_host_fault_register(const PsHostFaultContext *p_context, PsHostRegister reg) {
    return p_context->p_registers[reg];
}

void ps_host_fault_set_register(PsHostFaultContext *p_context, PsHostRegister reg, uint64_t value) {
    p_context->p_registers[reg] = value;
}

void ps_host_fault_cpu(const PsHostFaultContext *p_context, PsHostFaultCpu *p_out) {
    *p_out = (PsHostFaultCpu){
        .trap_number = p_context->trap_number,
        .error_code = p_context->error_code,
        .code_selector = p_context->code_selector,
        .p_fxsave = p_context->p_fxsave,
    };
}

static uint16_t code_selector(void) {
    uint16_t selector;
    __asm__("mov %%cs,%0" : "=r"(selector));
    return selector;
}

static void read_registers(const CONTEXT *p_record, uint64_t *p_out) {
    for (int reg = 0; reg < PS_HOST_REG_RFLAGS; ++reg)
        memcpy(&p_out[reg], (const char *)p_record + s_off_context_registers[reg], sizeof(*p_out));
    p_out[PS_HOST_REG_RFLAGS] = p_record->EFlags;
}

static void write_registers(const uint64_t *p_registers, CONTEXT *p_record) {
    for (int reg = 0; reg < PS_HOST_REG_RFLAGS; ++reg)
        memcpy((char *)p_record + s_off_context_registers[reg], &p_registers[reg], sizeof(*p_registers));
    p_record->EFlags = (DWORD)p_registers[PS_HOST_REG_RFLAGS];
}

/* The thread resumes in the interrupt's entry stub below the red zone, which takes the registers it
 * really has then: a thread stopped in a system call keeps the call's result, since only rip and rsp
 * change. The caller holds the interrupt for the thread. */
static void redirect(ThreadState *p_state, PsHostInterrupt interrupt, DWORD64 *p_rip, DWORD64 *p_rsp) {
    p_state->va_rip[interrupt] = *p_rip;
    p_state->va_rsp[interrupt] = *p_rsp;
    *p_rsp = (*p_rsp - RED_ZONE) & ~(DWORD64)63;
    *p_rip = (DWORD64)(uintptr_t)s_pfn_interrupt_entries[interrupt];
}

static void fill_iret(ResumeFrame *p_frame) {
    uint16_t stack_selector;
    __asm__("mov %%ss,%0" : "=r"(stack_selector));
    p_frame->iret[0] = p_frame->registers[PS_HOST_REG_RIP];
    p_frame->iret[1] = code_selector();
    p_frame->iret[2] = p_frame->registers[PS_HOST_REG_RFLAGS];
    p_frame->iret[3] = p_frame->registers[PS_HOST_REG_RSP];
    p_frame->iret[4] = stack_selector;
}

/* The interrupt is held for the thread while its handler runs, as a signal is blocked in its own
 * handler; one that came meanwhile runs next. */
static void dispatch_interrupt(ThreadState *p_state, ResumeFrame *p_frame, PsHostInterrupt interrupt) {
    const unsigned bit = 1u << interrupt;
    for (;;) {
        const PsHostInterruptHandler pfn_handler = atomic_load(&s_interrupt_handlers[interrupt]);
        if (pfn_handler) {
            PsHostFaultContext context = {
                .p_registers = p_frame->registers,
                .p_fxsave = p_frame->xsave,
                .signal = ps_host_interrupt_signal(interrupt),
                .kind = PS_HOST_FAULT_KIND_COUNT,
                .interrupt = interrupt,
                .code_selector = code_selector(),
            };
            pfn_handler(&context);
        }
        atomic_fetch_and(&p_state->held, ~bit);
        if (!(atomic_fetch_and(&p_state->pending, ~bit) & bit)) return;
        atomic_fetch_or(&p_state->held, bit);
    }
}

void ps_win32_interrupt_entered(ResumeFrame *p_frame, int interrupt) {
    const DWORD last_error = GetLastError();
    ThreadState *p_state = state_of_self();
    if (!p_state) {
        /* The sender claimed this thread's state before redirecting it, and only the thread itself
         * clears it: without one, where it was is lost. */
        write_error("Host: an interrupted thread has no interrupt state; it cannot resume\n");
        TerminateProcess(GetCurrentProcess(), 128 + (UINT)ps_host_interrupt_signal((PsHostInterrupt)interrupt));
        return;
    }
    p_frame->registers[PS_HOST_REG_RIP] = p_state->va_rip[interrupt];
    p_frame->registers[PS_HOST_REG_RSP] = p_state->va_rsp[interrupt];
    dispatch_interrupt(p_state, p_frame, (PsHostInterrupt)interrupt);
    fill_iret(p_frame);
    SetLastError(last_error);
}

static LONG run_fault_handlers(PsHostFaultContext *p_context) {
    for (int i = atomic_load(&s_c_fault_handlers[p_context->kind]); i-- > 0;) {
        const PsHostFaultHandler pfn_handler = atomic_load(&s_fault_handlers[p_context->kind][i]);
        if (pfn_handler && pfn_handler(p_context)) {
            write_registers(p_context->p_registers, p_context->p_record);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/* Apart, so that nothing of the caller's lives across the recovery point. */
static __attribute__((noinline)) LONG run_recoverable(PsHostFaultContext *p_context) {
    PsHostRecoveryPoint dispatch;
    p_context->p_dispatch = dispatch;
    return ps_host_recovery_set(dispatch) ? EXCEPTION_CONTINUE_EXECUTION : run_fault_handlers(p_context);
}

/* A kind without PS_HOST_FAULT_NESTED that faults again in its own handler goes to Windows' own
 * handling, as Linux ends the process on a blocked synchronous signal. */
static LONG dispatch_fault(PsHostFaultContext *p_context) {
    ThreadState *const p_state = state_of_self();
    const unsigned kind_bit = 1u << p_context->kind;
    const unsigned flags = atomic_load(&s_fault_flags[p_context->kind]);
    const unsigned running = p_state ? p_state->running_faults : 0;
    if ((running & kind_bit) && !(flags & PS_HOST_FAULT_NESTED)) return EXCEPTION_CONTINUE_SEARCH;
    const unsigned exception_bit = 1u << PS_HOST_INTERRUPT_GUEST_EXCEPTION;
    const int should_hold = p_state && (flags & PS_HOST_FAULT_HOLD_GUEST_EXCEPTION) &&
                            !(atomic_fetch_or(&p_state->held, exception_bit) & exception_bit);
    if (p_state) p_state->running_faults = running | kind_bit;
    const LONG result = run_recoverable(p_context);
    if (!p_state) return result;
    p_state->running_faults = running;
    if (should_hold) {
        atomic_fetch_and(&p_state->held, ~exception_bit);
        /* The guest exception that came while the fault was handled runs as the thread resumes. */
        if (result == EXCEPTION_CONTINUE_EXECUTION && (atomic_fetch_and(&p_state->pending, ~exception_bit) & exception_bit)) {
            atomic_fetch_or(&p_state->held, exception_bit);
            redirect(p_state, PS_HOST_INTERRUPT_GUEST_EXCEPTION, &p_context->p_record->Rip, &p_context->p_record->Rsp);
        }
    }
    return result;
}

/* Raised by the exception dispatch or an unwind that cannot go on: a frame without unwind data (guest
 * code, generated code), a stack outside the thread's limits. The frame walk that would reach an
 * unhandled-exception filter is what failed, so Windows ends the process right after the vectored
 * handlers: they are reported here, as SIGABRT, Linux's answer to an unwind that fails. */
static const char *name_dispatch_failure(DWORD code) {
    switch (code) {
    case 0xC0000025: return "STATUS_NONCONTINUABLE_EXCEPTION";
    case 0xC0000026: return "STATUS_INVALID_DISPOSITION";
    case 0xC0000028: return "STATUS_BAD_STACK";
    case 0xC0000029: return "STATUS_INVALID_UNWIND_TARGET";
    case 0xC00000FF: return "STATUS_BAD_FUNCTION_TABLE";
    }
    return NULL;
}

static void write_dispatch_failure(const EXCEPTION_RECORD *p_exception, const char *sz_name) {
    char sz_line[256];
    snprintf(sz_line, sizeof(sz_line), "Host: exception 0x%08lx (%s, flags 0x%lx) at %p on thread %lu\n",
             (unsigned long)p_exception->ExceptionCode, sz_name, (unsigned long)p_exception->ExceptionFlags,
             p_exception->ExceptionAddress, (unsigned long)GetCurrentThreadId());
    write_error(sz_line);
    int depth = 0;
    for (const EXCEPTION_RECORD *p_inner = p_exception->ExceptionRecord; p_inner && depth < 4;
         p_inner = p_inner->ExceptionRecord, ++depth) {
        snprintf(sz_line, sizeof(sz_line), "  nested exception record 0x%08lx (flags 0x%lx) at %p\n",
                 (unsigned long)p_inner->ExceptionCode, (unsigned long)p_inner->ExceptionFlags,
                 p_inner->ExceptionAddress);
        write_error(sz_line);
    }
}

static int describe_exception(const EXCEPTION_RECORD *p_exception, PsHostFaultContext *p_out) {
    const int has_access = p_exception->NumberParameters >= 2;
    const ULONG_PTR access = has_access ? p_exception->ExceptionInformation[0] : 0;
    p_out->p_address = p_exception->ExceptionAddress;
    switch (p_exception->ExceptionCode) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_IN_PAGE_ERROR:
        p_out->kind = p_exception->ExceptionCode == EXCEPTION_IN_PAGE_ERROR ? PS_HOST_FAULT_BUS : PS_HOST_FAULT_ACCESS;
        p_out->trap_number = 14;
        p_out->error_code = 4 | (access == 1 ? 0x2 : 0) | (access == 8 ? 0x10 : 0);
        p_out->p_address = has_access ? (void *)p_exception->ExceptionInformation[1] : NULL;
        /* A general protection fault (a non-canonical address) names all ones; Linux names none. */
        if (p_out->p_address == (void *)UINTPTR_MAX) {
            p_out->trap_number = 13;
            p_out->error_code = 0;
            p_out->p_address = NULL;
        }
        return 1;
    case EXCEPTION_DATATYPE_MISALIGNMENT:
        p_out->kind = PS_HOST_FAULT_BUS;
        p_out->trap_number = 17;
        return 1;
    /* Linux raises SIGSEGV with no address for a privileged instruction. */
    case EXCEPTION_PRIV_INSTRUCTION:
        p_out->kind = PS_HOST_FAULT_ACCESS;
        p_out->trap_number = 13;
        p_out->p_address = NULL;
        return 1;
    case EXCEPTION_ILLEGAL_INSTRUCTION: p_out->kind = PS_HOST_FAULT_ILLEGAL; p_out->trap_number = 6; return 1;
    case EXCEPTION_INT_DIVIDE_BY_ZERO: p_out->kind = PS_HOST_FAULT_ARITHMETIC; p_out->trap_number = 0; return 1;
    case EXCEPTION_INT_OVERFLOW: p_out->kind = PS_HOST_FAULT_ARITHMETIC; p_out->trap_number = 4; return 1;
    case EXCEPTION_FLT_DENORMAL_OPERAND:
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
    case EXCEPTION_FLT_INEXACT_RESULT:
    case EXCEPTION_FLT_INVALID_OPERATION:
    case EXCEPTION_FLT_OVERFLOW:
    case EXCEPTION_FLT_STACK_CHECK:
    case EXCEPTION_FLT_UNDERFLOW: p_out->kind = PS_HOST_FAULT_ARITHMETIC; p_out->trap_number = 16; return 1;
    /* STATUS_FLOAT_MULTIPLE_FAULTS and _TRAPS: SSE exceptions. */
    case 0xC00002B4:
    case 0xC00002B5: p_out->kind = PS_HOST_FAULT_ARITHMETIC; p_out->trap_number = 19; return 1;
    case EXCEPTION_BREAKPOINT: p_out->kind = PS_HOST_FAULT_BREAKPOINT; p_out->trap_number = 3; return 1;
    /* The trap flag's step: SIGTRAP on Linux too. */
    case EXCEPTION_SINGLE_STEP: p_out->kind = PS_HOST_FAULT_BREAKPOINT; p_out->trap_number = 1; return 1;
    }
    const char *sz_failure = name_dispatch_failure(p_exception->ExceptionCode);
    if (!sz_failure) return 0;
    write_dispatch_failure(p_exception, sz_failure);
    p_out->kind = PS_HOST_FAULT_ILLEGAL;
    p_out->signal = HOST_SIGABRT;
    return 1;
}

/* First in line, so that the write traps see a fault before a host library's own handlers. Every
 * exception code described goes to psport's handlers: a first-chance one a host library would
 * catch itself (SEH) reaches them too. */
static LONG CALLBACK on_exception(EXCEPTION_POINTERS *p_pointers) {
    PsHostFaultContext context = {.interrupt = PS_HOST_INTERRUPT_COUNT};
    if (!describe_exception(p_pointers->ExceptionRecord, &context)) return EXCEPTION_CONTINUE_SEARCH;
    if (!atomic_load(&s_c_fault_handlers[context.kind])) return EXCEPTION_CONTINUE_SEARCH;
    const DWORD last_error = GetLastError();
    CONTEXT *p_record = p_pointers->ContextRecord;
    uint64_t registers[PS_HOST_REG_COUNT];
    read_registers(p_record, registers);
    /* Windows leaves rip on the int3; Linux past it. */
    if (p_pointers->ExceptionRecord->ExceptionCode == EXCEPTION_BREAKPOINT) ++registers[PS_HOST_REG_RIP];
    context.p_registers = registers;
    context.p_record = p_record;
    if (!context.signal) context.signal = s_fault_signals[context.kind];
    context.code_selector = p_record->SegCs;
    context.p_fxsave =
        (p_record->ContextFlags & CONTEXT_FLOATING_POINT) == CONTEXT_FLOATING_POINT ? &p_record->FltSave : NULL;
    const LONG result = dispatch_fault(&context);
    SetLastError(last_error);
    return result;
}

/* Inside the exception dispatch the thread is set to resume in ps_host_recovery_jump, then leaves
 * the dispatch, which resumes it: Windows unwinds nothing through guest frames. */
void ps_host_fault_recover(const PsHostFaultContext *p_context, PsHostRecoveryPoint *p_point) {
    if (p_context->p_dispatch) {
        CONTEXT *p_record = p_context->p_record;
        p_record->Rsp = ((p_record->Rsp - RED_ZONE) & ~(DWORD64)15) - 40;
        p_record->Rcx = (DWORD64)(uintptr_t)*p_point;
        p_record->Rdx = 1;
        p_record->Rip = (DWORD64)(uintptr_t)ps_host_recovery_jump;
        ps_host_recovery_jump(p_context->p_dispatch, 1);
    }
    ThreadState *p_state = state_of_self();
    if (p_state && p_context->interrupt < PS_HOST_INTERRUPT_COUNT)
        atomic_fetch_and(&p_state->held, ~(1u << p_context->interrupt));
    ps_host_recovery_jump(*p_point, 1);
}

int ps_host_fault_install(PsHostFaultKind kind, PsHostFaultHandler pfn_handler, unsigned flags) {
    if ((unsigned)kind >= PS_HOST_FAULT_KIND_COUNT) return EINVAL;
    AcquireSRWLockExclusive(&s_install_lock);
    int error = 0;
    const int c_handlers = atomic_load(&s_c_fault_handlers[kind]);
    if (c_handlers == MAX_HANDLERS) error = ENOSPC;
    else if (!s_p_vectored_handler && !(s_p_vectored_handler = AddVectoredExceptionHandler(1, on_exception)))
        error = ENOMEM;
    else {
        atomic_fetch_or(&s_fault_flags[kind], flags);
        atomic_store(&s_fault_handlers[kind][c_handlers], pfn_handler);
        atomic_store(&s_c_fault_handlers[kind], c_handlers + 1);
    }
    ReleaseSRWLockExclusive(&s_install_lock);
    return error;
}

/* Windows ends no wait early for an interrupt, so restarting is what always happens: flags change
 * nothing. */
int ps_host_interrupt_install(PsHostInterrupt interrupt, PsHostInterruptHandler pfn_handler, unsigned flags) {
    (void)flags;
    if ((unsigned)interrupt >= PS_HOST_INTERRUPT_COUNT) return EINVAL;
    atomic_store(&s_interrupt_handlers[interrupt], pfn_handler);
    return 0;
}

/* Runs the handler where the call returns, as a signal to oneself runs as the call that sent it
 * returns. Changes the handler makes to callee-saved registers stop at the C frames between here
 * and the caller, which restore their own. */
static int interrupt_self(PsHostInterrupt interrupt) {
    ThreadState *p_state = state_of_self();
    if (!p_state) return EAGAIN;
    const unsigned bit = 1u << interrupt;
    if (atomic_fetch_or(&p_state->held, bit) & bit) {
        atomic_fetch_or(&p_state->pending, bit);
        return 0;
    }
    ps_win32_interrupt_self(&p_state->va_rip[interrupt], &p_state->va_rsp[interrupt],
                            s_pfn_interrupt_entries[interrupt]);
    return 0;
}

/* Runs what came while held and is open now, as unblocking a pending signal delivers it. */
static void deliver_pending(ThreadState *p_state) {
    for (int interrupt = 0; interrupt < PS_HOST_INTERRUPT_COUNT; ++interrupt) {
        const unsigned bit = 1u << interrupt;
        if (!(atomic_load(&p_state->held) & bit) && (atomic_fetch_and(&p_state->pending, ~bit) & bit))
            interrupt_self((PsHostInterrupt)interrupt);
    }
}

/* A thread's state starts with nothing held: Windows passes no mask to the threads it creates. */
void ps_host_interrupts_hold(int is_holding_all, PsHostInterruptMask *p_out_previous) {
    ThreadState *p_state = state_of_self();
    unsigned previous = 0;
    if (p_state) {
        previous = atomic_exchange(&p_state->held, is_holding_all ? (1u << PS_HOST_INTERRUPT_COUNT) - 1 : 0u);
        if (!is_holding_all) deliver_pending(p_state);
    }
    if (p_out_previous) {
        memset(p_out_previous, 0, sizeof(*p_out_previous));
        p_out_previous->opaque[0] = previous;
    }
}

void ps_host_interrupts_restore(const PsHostInterruptMask *p_mask) {
    ThreadState *p_state = state_of_self();
    if (!p_state) return;
    atomic_store(&p_state->held, (unsigned)p_mask->opaque[0]);
    deliver_pending(p_state);
}

/* The thread is suspended and redirected to the interrupt's entry stub; one held there stays
 * pending. A thread waiting on an address is woken (a spurious wake, which futex allows) so that
 * the interrupt runs now, as SA_RESTART does on Linux; other blocking calls run it when they
 * return. */
int ps_host_interrupt_thread(int32_t tid, PsHostInterrupt interrupt) {
    if ((unsigned)interrupt >= PS_HOST_INTERRUPT_COUNT || !atomic_load(&s_interrupt_handlers[interrupt]))
        return EINVAL;
    if ((DWORD)tid == GetCurrentThreadId()) return interrupt_self(interrupt);
    ThreadState *p_state = claim_state((uint32_t)tid);
    if (!p_state) return EAGAIN;
    HANDLE h_thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, (DWORD)tid);
    if (!h_thread) {
        const DWORD error = GetLastError();
        return error == ERROR_INVALID_PARAMETER ? ESRCH : ps_win32_errno(error);
    }
    if (SuspendThread(h_thread) == (DWORD)-1) {
        const int error = ps_win32_errno(GetLastError());
        CloseHandle(h_thread);
        return error;
    }
    const unsigned bit = 1u << interrupt;
    CONTEXT context;
    memset(&context, 0, sizeof(context));
    context.ContextFlags = CONTEXT_CONTROL;
    /* GetThreadContext also waits until the suspension took effect. */
    int error = GetThreadContext(h_thread, &context) ? 0 : ps_win32_errno(GetLastError());
    int is_redirected = 0;
    if (!error) {
        if (atomic_load(&p_state->held) & bit) atomic_fetch_or(&p_state->pending, bit);
        else {
            atomic_fetch_or(&p_state->held, bit);
            redirect(p_state, interrupt, &context.Rip, &context.Rsp);
            context.ContextFlags = CONTEXT_CONTROL;
            if (SetThreadContext(h_thread, &context)) is_redirected = 1;
            else {
                error = ps_win32_errno(GetLastError());
                atomic_fetch_and(&p_state->held, ~bit);
            }
        }
    }
    const void *p_wait_word = atomic_load(&p_state->p_wait_word);
    ResumeThread(h_thread);
    CloseHandle(h_thread);
    if (is_redirected && p_wait_word) WakeByAddressAll((void *)p_wait_word);
    return error;
}

static DWORD WINAPI run_watchdog(void *p_timer) {
    for (;;) {
        if (WaitForSingleObject(p_timer, INFINITE) != WAIT_OBJECT_0) {
            write_win32_error("waiting on the watchdog timer", GetLastError());
            return 1;
        }
        const int error = ps_host_interrupt_thread((int32_t)GetCurrentThreadId(), PS_HOST_INTERRUPT_WATCHDOG);
        if (error) {
            char sz_line[96];
            snprintf(sz_line, sizeof(sz_line), "Host: the watchdog interrupt failed (errno %d)\n", error);
            write_error(sz_line);
        }
    }
}

static HANDLE s_h_watchdog_timer;

static BOOL CALLBACK start_watchdog(INIT_ONCE *p_once, void *p_parameter, void **pp_context) {
    (void)p_once;
    (void)p_parameter;
    (void)pp_context;
    HANDLE h_timer = CreateWaitableTimerW(NULL, FALSE, NULL);
    if (!h_timer) {
        write_win32_error("CreateWaitableTimerW for the watchdog", GetLastError());
        return TRUE;
    }
    HANDLE h_thread = CreateThread(NULL, 256 * 1024, run_watchdog, h_timer, 0, NULL);
    if (!h_thread) {
        write_win32_error("CreateThread for the watchdog", GetLastError());
        CloseHandle(h_timer);
        return TRUE;
    }
    CloseHandle(h_thread);
    s_h_watchdog_timer = h_timer;
    return TRUE;
}

/* A thread of its own waits on a timer and interrupts itself: the handler runs on whichever
 * thread the host picks, as SIGALRM's does. */
void ps_host_watchdog_arm(unsigned seconds) {
    static INIT_ONCE s_once = INIT_ONCE_STATIC_INIT;
    if (!seconds) {
        if (s_h_watchdog_timer) CancelWaitableTimer(s_h_watchdog_timer);
        return;
    }
    InitOnceExecuteOnce(&s_once, start_watchdog, NULL, NULL);
    if (!s_h_watchdog_timer) return;
    const LARGE_INTEGER due = {.QuadPart = -(LONGLONG)seconds * 10000000};
    if (!SetWaitableTimer(s_h_watchdog_timer, &due, 0, NULL, NULL, FALSE))
        write_win32_error("SetWaitableTimer for the watchdog", GetLastError());
}

/* Returning FALSE lets the next handler, and past the last Windows' own ending, take the event. */
static BOOL WINAPI on_console_event(DWORD event) {
    const int sig = event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT ? HOST_SIGINT
                    : event == CTRL_CLOSE_EVENT                       ? HOST_SIGHUP
                                                                      : HOST_SIGTERM;
    if (!atomic_exchange(&s_has_reported_termination, 1)) s_pfn_termination_report(sig);
    return FALSE;
}

void ps_host_on_termination(void (*pfn_report)(int sig)) {
    s_pfn_termination_report = pfn_report;
    if (!SetConsoleCtrlHandler(on_console_event, TRUE)) write_win32_error("SetConsoleCtrlHandler", GetLastError());
}

static uint32_t mxcsr_mask(void) {
    _Alignas(16) uint8_t fxsave[512];
    __asm__("fxsave64 %0" : "=m"(fxsave));
    uint32_t mask;
    memcpy(&mask, fxsave + 28, sizeof(mask));
    return mask ? mask : 0xffbf;
}

static uint64_t enabled_xsave_components(void) {
    uint32_t low, high;
    __asm__("xgetbv" : "=a"(low), "=d"(high) : "c"(0));
    return ((uint64_t)high << 32 | low) & XSAVE_COMPONENTS;
}

/* xrstor faults on a header or an MXCSR the CPU rejects, so the guest's image is made one it takes. */
static void load_fpu(uint8_t *p_xsave, const void *p_fpu) {
    uint32_t mxcsr = 0x1f80;
    if (p_fpu) {
        memcpy(p_xsave, p_fpu, XSAVE_YMM_END);
        memcpy(&mxcsr, p_xsave + 24, sizeof(mxcsr));
        mxcsr &= mxcsr_mask();
        uint64_t xstate_bv;
        memcpy(&xstate_bv, p_xsave + XSAVE_HEADER, sizeof(xstate_bv));
        xstate_bv &= enabled_xsave_components();
        memset(p_xsave + XSAVE_HEADER, 0, 64);
        memcpy(p_xsave + XSAVE_HEADER, &xstate_bv, sizeof(xstate_bv));
    } else {
        const uint16_t control_word = 0x37f;
        memcpy(p_xsave, &control_word, sizeof(control_word));
    }
    memcpy(p_xsave + 24, &mxcsr, sizeof(mxcsr));
}

/* Through iretq from a frame below everything the restored code may use, which sets rsp, rip and
 * rflags at once; xrstor brings back the YMM upper halves too. */
void ps_host_resume(const uint64_t p_registers[PS_HOST_REG_COUNT], const void *p_fpu, int exception_signal,
                    int is_exception_masked) {
    ResumeFrame frame;
    memset(&frame, 0, sizeof(frame));
    memcpy(frame.registers, p_registers, sizeof(frame.registers));
    load_fpu(frame.xsave, p_fpu);
    ThreadState *p_state = state_of_self();
    for (int interrupt = 0; p_state && interrupt < PS_HOST_INTERRUPT_COUNT; ++interrupt) {
        if (ps_host_interrupt_signal((PsHostInterrupt)interrupt) != exception_signal) continue;
        const unsigned bit = 1u << interrupt;
        if (is_exception_masked) atomic_fetch_or(&p_state->held, bit);
        else {
            atomic_fetch_and(&p_state->held, ~bit);
            if (atomic_fetch_and(&p_state->pending, ~bit) & bit) {
                atomic_fetch_or(&p_state->held, bit);
                dispatch_interrupt(p_state, &frame, (PsHostInterrupt)interrupt);
            }
        }
    }
    fill_iret(&frame);
    ps_win32_resume_frame(&frame);
}

/* Through the native calls, page by page, since WriteProcessMemory makes a read-only page writable
 * for the copy, which would write past the write traps. */
static int64_t copy_memory(const char *sz_function, void *p_remote, void *p_local, size_t cb_copy) {
    if (!cb_copy) return 0;
    const HMODULE h_ntdll = GetModuleHandleW(L"ntdll.dll");
    const PfnCopyVirtualMemory pfn_copy =
        h_ntdll ? (PfnCopyVirtualMemory)(void (*)(void))GetProcAddress(h_ntdll, sz_function) : NULL;
    if (!pfn_copy) return -ENOSYS;
    size_t cb_done = 0;
    while (cb_done < cb_copy) {
        const uintptr_t va_remote = (uintptr_t)p_remote + cb_done;
        size_t cb_chunk = 4096 - (va_remote & 4095);
        if (cb_chunk > cb_copy - cb_done) cb_chunk = cb_copy - cb_done;
        SIZE_T cb_copied = 0;
        const LONG status = pfn_copy(GetCurrentProcess(), (void *)va_remote, (char *)p_local + cb_done, cb_chunk, &cb_copied);
        cb_done += cb_copied;
        if (status < 0 || cb_copied < cb_chunk) break;
    }
    return cb_done ? (int64_t)cb_done : -EFAULT;
}

int64_t ps_host_read_memory(void *p_out, const void *p_from, size_t cb_copy) {
    return copy_memory("NtReadVirtualMemory", (void *)(uintptr_t)p_from, p_out, cb_copy);
}

int64_t ps_host_write_memory(void *p_to, const void *p_from, size_t cb_copy) {
    return copy_memory("NtWriteVirtualMemory", p_to, (void *)(uintptr_t)p_from, cb_copy);
}

static int is_in_module(DWORD64 va_code) {
    HMODULE h_module;
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              (const wchar_t *)(uintptr_t)va_code, &h_module);
}

/* The unwind tables of each module; guest code has none, so the first guest frame ends the chain,
 * as on Linux. A first frame without an entry in a module is a leaf function, whose return address
 * is on top of the stack. */
int ps_host_fault_backtrace(const PsHostFaultContext *p_context, void **pp_frames, int c_frames) {
    CONTEXT unwind;
    memset(&unwind, 0, sizeof(unwind));
    unwind.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    write_registers(p_context->p_registers, &unwind);
    int c_stored = 0;
    while (c_stored < c_frames && unwind.Rip) {
        pp_frames[c_stored++] = (void *)(uintptr_t)unwind.Rip;
        const DWORD64 va_previous_rsp = unwind.Rsp;
        DWORD64 va_image = 0;
        RUNTIME_FUNCTION *p_function = RtlLookupFunctionEntry(unwind.Rip, &va_image, NULL);
        if (p_function) {
            void *p_handler_data = NULL;
            DWORD64 va_establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, va_image, unwind.Rip, p_function, &unwind, &p_handler_data,
                             &va_establisher, NULL);
        } else if (c_stored == 1 && is_in_module(unwind.Rip)) {
            uint64_t va_return;
            if (ps_host_read_memory(&va_return, (const void *)(uintptr_t)unwind.Rsp, sizeof(va_return)) !=
                (int64_t)sizeof(va_return))
                break;
            unwind.Rip = va_return;
            unwind.Rsp += sizeof(va_return);
        } else
            break;
        if (unwind.Rsp <= va_previous_rsp) break;
    }
    return c_stored;
}

/* ps_host_recovery_set and ps_host_recovery_jump: rbx rbp rdi rsi r12-r15 (0-56), rsp after the
 * return (64), the return address (72), MXCSR (80), the x87 control word (84), xmm6-15 (96-240),
 * which is all the Win64 callee-saved state.
 *
 * ps_win32_interrupt_entry_<interrupt>: where an interrupted thread resumes, rsp already below the
 * red zone and 64-aligned. It builds a ResumeFrame (xsave of x87, SSE and AVX; the registers as
 * they are, rflags unchanged by lea, mov and jmp), calls ps_win32_interrupt_entered, and resumes
 * from the frame. ps_win32_resume_frame resumes from a frame the caller built: xrstor, the
 * registers, then iretq from the frame's own iret slots.
 *
 * ps_win32_interrupt_self(p_va_rip, p_va_rsp, pfn_entry): records its return address and the
 * caller's rsp, then enters the stub as an interrupted thread would at its return. */
__asm__(".text\n"
        ".globl ps_host_recovery_set\n"
        ".def ps_host_recovery_set; .scl 2; .type 32; .endef\n"
        "ps_host_recovery_set:\n"
        "    mov %rbx,0(%rcx)\n    mov %rbp,8(%rcx)\n    mov %rdi,16(%rcx)\n    mov %rsi,24(%rcx)\n"
        "    mov %r12,32(%rcx)\n    mov %r13,40(%rcx)\n    mov %r14,48(%rcx)\n    mov %r15,56(%rcx)\n"
        "    lea 8(%rsp),%rdx\n    mov %rdx,64(%rcx)\n    mov (%rsp),%rdx\n    mov %rdx,72(%rcx)\n"
        "    stmxcsr 80(%rcx)\n    fnstcw 84(%rcx)\n"
        "    movdqu %xmm6,96(%rcx)\n    movdqu %xmm7,112(%rcx)\n    movdqu %xmm8,128(%rcx)\n"
        "    movdqu %xmm9,144(%rcx)\n    movdqu %xmm10,160(%rcx)\n    movdqu %xmm11,176(%rcx)\n"
        "    movdqu %xmm12,192(%rcx)\n    movdqu %xmm13,208(%rcx)\n    movdqu %xmm14,224(%rcx)\n"
        "    movdqu %xmm15,240(%rcx)\n"
        "    xor %eax,%eax\n    ret\n"
        ".globl ps_host_recovery_jump\n"
        ".def ps_host_recovery_jump; .scl 2; .type 32; .endef\n"
        "ps_host_recovery_jump:\n"
        "    mov %edx,%eax\n    test %eax,%eax\n    jnz 1f\n    inc %eax\n"
        "1:  mov 0(%rcx),%rbx\n    mov 8(%rcx),%rbp\n    mov 16(%rcx),%rdi\n    mov 24(%rcx),%rsi\n"
        "    mov 32(%rcx),%r12\n    mov 40(%rcx),%r13\n    mov 48(%rcx),%r14\n    mov 56(%rcx),%r15\n"
        "    ldmxcsr 80(%rcx)\n    fnclex\n    fldcw 84(%rcx)\n"
        "    movdqu 96(%rcx),%xmm6\n    movdqu 112(%rcx),%xmm7\n    movdqu 128(%rcx),%xmm8\n"
        "    movdqu 144(%rcx),%xmm9\n    movdqu 160(%rcx),%xmm10\n    movdqu 176(%rcx),%xmm11\n"
        "    movdqu 192(%rcx),%xmm12\n    movdqu 208(%rcx),%xmm13\n    movdqu 224(%rcx),%xmm14\n"
        "    movdqu 240(%rcx),%xmm15\n"
        "    mov 64(%rcx),%rsp\n    jmp *72(%rcx)\n"
        ".irp interrupt,0,1,2,3\n"
        ".globl ps_win32_interrupt_entry_\\interrupt\n"
        ".def ps_win32_interrupt_entry_\\interrupt; .scl 2; .type 32; .endef\n"
        "ps_win32_interrupt_entry_\\interrupt:\n"
        "    lea -1024(%rsp),%rsp\n    mov %rcx,848(%rsp)\n    mov $\\interrupt,%ecx\n"
        "    jmp ps_win32_interrupt_common\n"
        ".endr\n"
        "ps_win32_interrupt_common:\n"
        "    mov %rax,832(%rsp)\n    mov %rbx,840(%rsp)\n    mov %rdx,856(%rsp)\n    mov %rsi,864(%rsp)\n"
        "    mov %rdi,872(%rsp)\n    mov %rbp,880(%rsp)\n    mov %r8,896(%rsp)\n    mov %r9,904(%rsp)\n"
        "    mov %r10,912(%rsp)\n    mov %r11,920(%rsp)\n    mov %r12,928(%rsp)\n    mov %r13,936(%rsp)\n"
        "    mov %r14,944(%rsp)\n    mov %r15,952(%rsp)\n"
        "    pushfq\n    pop %rax\n    mov %rax,968(%rsp)\n"
        "    xor %eax,%eax\n"
        "    mov %rax,512(%rsp)\n    mov %rax,520(%rsp)\n    mov %rax,528(%rsp)\n    mov %rax,536(%rsp)\n"
        "    mov %rax,544(%rsp)\n    mov %rax,552(%rsp)\n    mov %rax,560(%rsp)\n    mov %rax,568(%rsp)\n"
        "    mov $7,%eax\n    xor %edx,%edx\n    xsave64 (%rsp)\n"
        "    mov %ecx,%edx\n    mov %rsp,%rcx\n    sub $32,%rsp\n    call ps_win32_interrupt_entered\n"
        "    add $32,%rsp\n    jmp ps_win32_resume_common\n"
        ".globl ps_win32_resume_frame\n"
        ".def ps_win32_resume_frame; .scl 2; .type 32; .endef\n"
        "ps_win32_resume_frame:\n"
        "    mov %rcx,%rsp\n"
        "ps_win32_resume_common:\n"
        "    mov $7,%eax\n    xor %edx,%edx\n    xrstor64 (%rsp)\n"
        "    mov 832(%rsp),%rax\n    mov 840(%rsp),%rbx\n    mov 848(%rsp),%rcx\n    mov 856(%rsp),%rdx\n"
        "    mov 864(%rsp),%rsi\n    mov 872(%rsp),%rdi\n    mov 880(%rsp),%rbp\n    mov 896(%rsp),%r8\n"
        "    mov 904(%rsp),%r9\n    mov 912(%rsp),%r10\n    mov 920(%rsp),%r11\n    mov 928(%rsp),%r12\n"
        "    mov 936(%rsp),%r13\n    mov 944(%rsp),%r14\n    mov 952(%rsp),%r15\n"
        "    lea 976(%rsp),%rsp\n    iretq\n"
        ".globl ps_win32_interrupt_self\n"
        ".def ps_win32_interrupt_self; .scl 2; .type 32; .endef\n"
        "ps_win32_interrupt_self:\n"
        "    mov (%rsp),%rax\n    mov %rax,(%rcx)\n    lea 8(%rsp),%rax\n    mov %rax,(%rdx)\n"
        "    mov %rax,%rsp\n    lea -128(%rsp),%rsp\n    and $-64,%rsp\n    jmp *%r8\n");
