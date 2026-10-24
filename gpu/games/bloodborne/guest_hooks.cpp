// SPDX-License-Identifier: GPL-2.0-or-later
#include "guest_hooks.h"
#include "heap_sites.h"
#include "psport_quirks.h"
#include "psport_toggles.h"
#include "../../../src/platform/ps_host.h"
#include "common/types.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <unistd.h>

extern "C" void runtime_memory_note_write(uintptr_t va_range, uint64_t cb_range);
extern "C" void runtime_memory_prepare_overwrite(uintptr_t va_range, uint64_t cb_range);

namespace PsGuestHooks {
namespace {
using u64 = std::uint64_t;
using u8 = std::uint8_t;
constexpr u64 ImageBase = 0x800000000ull;

/// The return of the game's GPU memory range allocator 0x26aa070 (rdi allocator, rsi size,
/// rdx alignment; it returns a node: [node + 8] the start, [[node + 0x18] + 8] the end, the next
/// node's start): `add rsp, 0x18` in its epilogue, reached by its own branches only.
constexpr u64 AllocReturn = 0x26aa255;
constexpr unsigned char AllocReturnCode[4] = {0x48, 0x83, 0xc4, 0x18};
RangeCallback s_pfn_on_range_allocated = nullptr;

/// The game's other GPU memory heap (TLSF-like, headers in the memory: free at 0x20858a0, alloc at
/// 0x2085a60), reached through a pointer at 0x54b9f38 set at startup. Its only three callers
/// (wrappers that record {address, size} in a map) pass r8 = rbp - 0x50 for the size handed out and
/// follow the call with `mov rbx, rax` (rax: the address).
constexpr std::array<u64, 3> HeapAllocReturns = {0x11e6629, 0x11e6736, 0x11e6846};
constexpr unsigned char HeapAllocReturnCode[3] = {0x48, 0x89, 0xc3};

/// PS_FRAME_STATS: the range allocator's collector (0x26aa860, `push rbp`; rdi the allocator): its
/// live range count [+0xc8] and byte count [+0xd0] are noted for the frame stats.
constexpr u64 CollectorEntry = 0x26aa860;
constexpr unsigned char CollectorEntryCode[1] = {0x55};

/// PS_HEAP_SITES=1: the game's deferred object release (0xbb5f10) skips the destruction when the
/// registry check (0xbf0790) fails: `js 0xbb5fc7` there, its result counted (eax).
constexpr u64 ReleaseCheck = 0xbb5fb0;
constexpr unsigned char ReleaseCheckCode[2] = {0x78, 0x15};

/// `call memcpy` (0x261a1b0: rdi destination, rsi source, rdx size) in the resource loaders:
/// 0x216d420 and 0x2171d60 copy texture and buffer data into GPU memory (most of the write
/// faults while areas stream in).
struct CallSite {
    u64 off_code;
    unsigned char code[5];
};
constexpr std::array<CallSite, 2> MemcpySites = {{
    {0x216d5ea, {0xe8, 0xc1, 0xcb, 0x4a, 0x00}},
    {0x2171fea, {0xe8, 0xc1, 0x81, 0x4a, 0x00}},
}};
std::array<std::atomic<u64>, MemcpySites.size()> s_hits{};

/// The GX swap chain's per-frame render command (bbport #162, #192): its 16-byte node comes from
/// a per-frame bump allocator that returns null once the frame's heap is used up, and the game
/// writes through it unchecked (`mov [r12 + 8], r13` at 0x2199310, then posts it to the render
/// thread): a guest fault at address 8. Without a node the post is skipped and the pending flag
/// (this + 0x3ab) stays set, so the command goes out with the next frame. A WORKAROUND quirk
/// (bloodborne_gx_node_guard) until #504 finds why the heap fills under psport.
constexpr u64 GxNodeSite = 0x2199310;
constexpr u64 GxNodeSkip = 0x219934e; // past the post and the flag's clear
constexpr unsigned char GxNodeCode[9] = {0x4d, 0x89, 0x6c, 0x24, 0x08,  // mov [r12 + 8], r13
                                         0x49, 0x8b, 0x87, 0xb0};        // mov rax, [r15 + 0xb0]
std::atomic<u64> s_va_gx_null_trap{0};
std::atomic<u64> s_c_gx_node_skips{0};
u64 s_c_gx_node_skips_reported = 0;

/// #504 (kb note #816): the GX swap chain's state when a node comes back null, so a night run
/// tells which way the heap fills. The site runs in 0x2198fd0 with r15 the swap chain: its two
/// frame contexts at +0x360/+0x368, the current one picked by +0x380 (switched, and the incoming
/// one's heap reset, on the main thread only, with no GPU wait); nodes and command data come from
/// the context's chunked bump allocators at +0x48 and +0x78 (+0x10 chunks, +0x14 allocations);
/// +0x38/+0x3c the job list's count and capacity; +0x3a9 && +0x131 the path that skips the
/// switch and the reset; +0x600/+0x608 the frames begun and done.
constexpr u64 GxSingleton = 0x55406c8;
constexpr u64 GxNullLogEvery = 64;
std::atomic<u64> s_gx_null_log_ns{0};
std::atomic<u64> s_gx_null_log_frame{0};

template <typename T>
T ReadAt(const u8* p_block, std::size_t off_field) {
    T value;
    std::memcpy(&value, p_block + off_field, sizeof(value));
    return value;
}

void LogGxNodeNull(u64 c_nulls, u64 va_gx) {
    const u64 now_ns = ps_host_time_monotonic_ns();
    const u64 host_frame = PsStats::g_c_gpu_frames.load(std::memory_order_relaxed);
    const u64 last_ns = s_gx_null_log_ns.exchange(now_ns, std::memory_order_relaxed);
    const u64 last_frame = s_gx_null_log_frame.exchange(host_frame, std::memory_order_relaxed);
    const u64 fps_x10 = last_ns != 0 && now_ns > last_ns && host_frame >= last_frame
                            ? (host_frame - last_frame) * 10'000'000'000ull / (now_ns - last_ns)
                            : 0;
    u64 va_singleton = 0;
    std::array<u8, 0x610> gx{};
    std::array<u8, 0x90> context{};
    char sz_line[512];
    int cb_line = 0;
    const bool has_singleton =
        ps_host_read_memory(&va_singleton, reinterpret_cast<const void*>(ImageBase + GxSingleton),
                            sizeof(va_singleton)) == std::int64_t(sizeof(va_singleton));
    if (ps_host_read_memory(gx.data(), reinterpret_cast<const void*>(va_gx), gx.size()) != std::int64_t(gx.size())) {
        cb_line = std::snprintf(sz_line, sizeof(sz_line),
                                "Guest hooks: GX node null %llu: host_frame=%llu swap chain 0x%llx unreadable\n",
                                static_cast<unsigned long long>(c_nulls), static_cast<unsigned long long>(host_frame),
                                static_cast<unsigned long long>(va_gx));
    } else {
        const auto i_context = ReadAt<std::int32_t>(gx.data(), 0x380);
        const u64 va_context = ReadAt<u64>(gx.data(), i_context == 0 ? 0x360 : 0x368);
        const bool has_context =
            (i_context == 0 || i_context == 1) &&
            ps_host_read_memory(context.data(), reinterpret_cast<const void*>(va_context), context.size()) ==
                std::int64_t(context.size());
        cb_line = std::snprintf(
            sz_line, sizeof(sz_line),
            "Guest hooks: GX node null %llu: host_frame=%llu fps=%llu.%llu gx=0x%llx singleton=%s "
            "ctx=%d f=0x%llx%s f+0x38 jobs=%d/%d f+0x48 chunks=%u allocs=%u f+0x78 chunks=%u allocs=%u "
            "gx+0x3a9=%u gx+0x131=%u gx+0x84=%u gx+0x3ab=%u gx+0x600=%llu gx+0x608=%llu\n",
            static_cast<unsigned long long>(c_nulls), static_cast<unsigned long long>(host_frame),
            static_cast<unsigned long long>(fps_x10 / 10), static_cast<unsigned long long>(fps_x10 % 10),
            static_cast<unsigned long long>(va_gx),
            !has_singleton ? "unreadable" : va_singleton == va_gx ? "yes" : "no", i_context,
            static_cast<unsigned long long>(va_context), has_context ? "" : " unreadable",
            ReadAt<std::int32_t>(context.data(), 0x38), ReadAt<std::int32_t>(context.data(), 0x3c),
            ReadAt<std::uint32_t>(context.data(), 0x48 + 0x10), ReadAt<std::uint32_t>(context.data(), 0x48 + 0x14),
            ReadAt<std::uint32_t>(context.data(), 0x78 + 0x10), ReadAt<std::uint32_t>(context.data(), 0x78 + 0x14),
            unsigned(gx[0x3a9]), unsigned(gx[0x131]), unsigned(gx[0x84]), unsigned(gx[0x3ab]),
            static_cast<unsigned long long>(ReadAt<u64>(gx.data(), 0x600)),
            static_cast<unsigned long long>(ReadAt<u64>(gx.data(), 0x608)));
    }
    if (cb_line > 0) {
        // NOTE: written with write(2), never through a FILE stream or the logger, because with
        // PS_GUEST_HOOK_TRAPS=1 this runs in the breakpoint handler, where neither is
        // async-signal-safe. The snprintf above only formats into the stack buffer: with these
        // integer and string conversions glibc's takes no lock and does not allocate, though
        // POSIX does not list it as async-signal-safe.
        [[maybe_unused]] const auto cb_written =
            write(STDOUT_FILENO, sz_line, std::min(std::size_t(cb_line), sizeof(sz_line) - 1));
    }
}

/// The game's memcpy at those sites: the GPU side is told before (its write traps lift: no fault
/// per page) and after the copy (the data now there), as for file reads. The copy writes every
/// byte, so GPU data in its whole pages is dropped first instead of read back (unless the source
/// is inside it: it is read after part of it was written).
void* LoaderCopy(void* p_dst, const void* p_src, std::size_t cb_copy) {
    const u64 va_dst = reinterpret_cast<u64>(p_dst), va_src = reinterpret_cast<u64>(p_src);
    if (va_src >= va_dst + cb_copy || va_dst >= va_src + cb_copy) {
        runtime_memory_prepare_overwrite(va_dst, cb_copy);
    }
    runtime_memory_note_write(va_dst, cb_copy);
    std::memcpy(p_dst, p_src, cb_copy);
    runtime_memory_note_write(va_dst, cb_copy);
    return p_dst;
}

/// psport: native detours (default; PS_GUEST_HOOK_TRAPS=1: int3 traps). Each hooked site jumps to
/// a stub next to the image that calls the handler and runs the instructions the jump covers;
/// the copy call sites call LoaderCopy through a trampoline there. An int3 trap costs a signal
/// (a few microseconds in the kernel) per call; a detour costs a call. The int3 written first
/// stays the fallback while the jump's bytes go in: OnTrap sends a thread that meets it to the stub.
struct NativeSite {
    u64 off_code;
    std::atomic<u64> stub{0};
};
std::array<NativeSite, 4> s_native_sites{{{AllocReturn}, {HeapAllocReturns[0]}, {HeapAllocReturns[1]},
                                        {HeapAllocReturns[2]}}};
u8* s_p_stub_page = nullptr;
std::size_t s_stub_used = 0;
constexpr std::size_t StubPageSize = 4096;

/// A page within +-2 GiB of the image (below its base: the image grows upwards).
u8* MapStubPage() {
    for (u64 k = 1; k <= 64; ++k) {
        const u64 hint = ImageBase - k * (16ull << 20);
        void* p_mapping = ps_host_vm_map(reinterpret_cast<void*>(hint), StubPageSize,
                                         PS_HOST_VM_READ | PS_HOST_VM_WRITE | PS_HOST_VM_EXECUTE, PS_HOST_VM_EXACT);
        if (p_mapping) {
            return static_cast<u8*>(p_mapping);
        }
    }
    return nullptr;
}

struct Emitter {
    std::array<u8, 256> m_code{};
    std::size_t m_cb_code = 0;
    void Bytes(std::initializer_list<u8> bytes) {
        for (const u8 b : bytes) {
            m_code[m_cb_code++] = b;
        }
    }
    void U32(std::uint32_t v) {
        std::memcpy(&m_code[m_cb_code], &v, 4);
        m_cb_code += 4;
    }
    void U64(u64 v) {
        std::memcpy(&m_code[m_cb_code], &v, 8);
        m_cb_code += 8;
    }
    /// Saves the caller-saved registers (the handler may change them), aligns the stack.
    void SaveVolatile() {
        Bytes({0x50, 0x51, 0x52, 0x56, 0x57, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53});
        Bytes({0x41, 0x54});             // push r12 (holds the unaligned stack pointer)
        Bytes({0x49, 0x89, 0xe4});       // mov r12, rsp
        Bytes({0x48, 0x83, 0xe4, 0xf0}); // and rsp, -16
    }
    void CallAbs(const void* p_function) {
        Bytes({0x48, 0xb8}); // movabs rax, fn
        U64(reinterpret_cast<u64>(p_function));
        Bytes({0xff, 0xd0}); // call rax
    }
    void RestoreVolatile() {
        Bytes({0x4c, 0x89, 0xe4}); // mov rsp, r12
        Bytes({0x41, 0x5c});       // pop r12
        Bytes({0x41, 0x5b, 0x41, 0x5a, 0x41, 0x59, 0x41, 0x58, 0x5f, 0x5e, 0x5a, 0x59, 0x58});
    }
};

/// Copies `e` into the stub page; returns its address there, or 0.
u64 PlaceStub(const Emitter& e) {
    if (!s_p_stub_page || s_stub_used + e.m_cb_code > StubPageSize) {
        return 0;
    }
    u8* p_at = s_p_stub_page + s_stub_used;
    std::memcpy(p_at, e.m_code.data(), e.m_cb_code);
    s_stub_used += (e.m_cb_code + 15) & ~std::size_t(15);
    return reinterpret_cast<u64>(p_at);
}

bool Rel32To(u64 from_next, u64 to, std::uint32_t& rel) {
    const std::int64_t d = std::int64_t(to) - std::int64_t(from_next);
    if (d < INT32_MIN || d > INT32_MAX) {
        return false;
    }
    rel = std::uint32_t(std::int32_t(d));
    return true;
}

/// Replaces bytes [1, 5) at `va_guest` (byte 0 is the trap already there), then byte 0.
void WriteJumpOverTrap(u64 va_guest, u8 opcode, std::uint32_t rel) {
    const long cb_page = long(ps_host_vm_page_size());
    void* p_page = reinterpret_cast<void*>(va_guest & ~u64(cb_page - 1));
    const std::size_t span = (va_guest + 5) - reinterpret_cast<u64>(p_page);
    ps_host_vm_protect(p_page, span, PS_HOST_VM_READ | PS_HOST_VM_WRITE | PS_HOST_VM_EXECUTE);
    auto* p_code = reinterpret_cast<u8*>(va_guest);
    for (int i = 0; i < 4; ++i) {
        __atomic_store_n(p_code + 1 + i, u8(rel >> (8 * i)), __ATOMIC_SEQ_CST);
    }
    __atomic_store_n(p_code, opcode, __ATOMIC_SEQ_CST);
    ps_host_vm_protect(p_page, span, PS_HOST_VM_READ | PS_HOST_VM_EXECUTE);
}

void PS4_SYSV_ABI AllocHook(u64 node) {
    if (!node) {
        return;
    }
    const u64 start = *reinterpret_cast<const u64*>(node + 8);
    const u64 next = *reinterpret_cast<const u64*>(node + 0x18);
    const u64 end = next ? *reinterpret_cast<const u64*>(next + 8) : start;
    if (end > start && end - start < (u64(1) << 32)) {
        s_pfn_on_range_allocated(start, end - start);
    }
}

void PS4_SYSV_ABI HeapHook(u64 va_range, u64 cb_range) {
    if (va_range != 0 && cb_range != 0 && cb_range < (u64(1) << 32)) {
        s_pfn_on_range_allocated(va_range, cb_range);
    }
}

/// The detour of a site whose int3 is in place (Patch). False: left as a trap.
bool InstallDetour(NativeSite& site) {
    const u64 address = ImageBase + site.off_code;
    Emitter e;
    u64 back = 0;
    if (site.off_code == AllocReturn) {
        // `add rsp, 0x18; pop rbx` (5 bytes) in the allocator's epilogue; rax: the node.
        if (reinterpret_cast<const u8*>(address)[4] != 0x5b) {
            return false;
        }
        e.SaveVolatile();
        e.Bytes({0x48, 0x89, 0xc7}); // mov rdi, rax (unchanged by the saving)
        e.CallAbs(reinterpret_cast<const void*>(&AllocHook));
        e.RestoreVolatile();
        e.Bytes({0x48, 0x83, 0xc4, 0x18}); // add rsp, 0x18
        e.Bytes({0x5b});                   // pop rbx
        back = address + 5;
    } else {
        // `mov rbx, rax; test rbx, rbx` (6 bytes) after the heap call; [rbp - 0x50]: the size.
        const auto* p_code = reinterpret_cast<const u8*>(address);
        if (p_code[3] != 0x48 || p_code[4] != 0x85 || p_code[5] != 0xdb) {
            return false;
        }
        e.Bytes({0x48, 0x89, 0xc3});                               // mov rbx, rax
        e.Bytes({0x48, 0x8d, 0xa4, 0x24, 0x80, 0xff, 0xff, 0xff}); // lea rsp, [rsp - 128]
        e.SaveVolatile();
        e.Bytes({0x48, 0x89, 0xdf});       // mov rdi, rbx
        e.Bytes({0x48, 0x8b, 0x75, 0xb0}); // mov rsi, [rbp - 0x50]
        e.CallAbs(reinterpret_cast<const void*>(&HeapHook));
        e.RestoreVolatile();
        e.Bytes({0x48, 0x8d, 0xa4, 0x24, 0x80, 0x00, 0x00, 0x00}); // lea rsp, [rsp + 128]
        e.Bytes({0x48, 0x85, 0xdb});                               // test rbx, rbx
        back = address + 6;
    }
    // jmp back
    const u64 jmp_at = reinterpret_cast<u64>(s_p_stub_page + s_stub_used) + e.m_cb_code;
    std::uint32_t rel_back = 0;
    if (!Rel32To(jmp_at + 5, back, rel_back)) {
        return false;
    }
    e.Bytes({0xe9});
    e.U32(rel_back);
    const u64 stub = PlaceStub(e);
    std::uint32_t rel = 0;
    if (!stub || !Rel32To(address + 5, stub, rel)) {
        return false;
    }
    site.stub.store(stub, std::memory_order_release); // a thread at the trap now goes there
    WriteJumpOverTrap(address, 0xe9, rel);
    return true;
}

/// The copy call sites: the call's target becomes a trampoline to LoaderCopy.
bool InstallCallDetour(u64 off_code) {
    Emitter e;
    e.Bytes({0xff, 0x25, 0x00, 0x00, 0x00, 0x00}); // jmp [rip]
    e.U64(reinterpret_cast<u64>(&LoaderCopy));
    const u64 stub = PlaceStub(e);
    const u64 address = ImageBase + off_code;
    std::uint32_t rel = 0;
    if (!stub || !Rel32To(address + 5, stub, rel)) {
        return false;
    }
    WriteJumpOverTrap(address, 0xe8, rel);
    return true;
}
/// The detour of the GX node site whose int3 is in place (Patch): a null node reaches the stub's
/// int3, which OnTrap sends past the post.
bool InstallGxNodeGuard() {
    const u64 address = ImageBase + GxNodeSite;
    Emitter e;
    e.Bytes({0x4d, 0x85, 0xe4});                   // test r12, r12
    e.Bytes({0x74, 0x13});                         // jz null (past the next 19 bytes)
    e.Bytes({0x4d, 0x89, 0x6c, 0x24, 0x08});       // mov [r12 + 8], r13
    e.Bytes({0xff, 0x25, 0x00, 0x00, 0x00, 0x00}); // jmp [rip]
    e.U64(address + 5);
    const std::size_t off_trap = e.m_cb_code;
    e.Bytes({0xcc}); // null
    const u64 stub = PlaceStub(e);
    std::uint32_t rel = 0;
    if (!stub || !Rel32To(address + 5, stub, rel)) {
        return false;
    }
    s_va_gx_null_trap.store(stub + off_trap, std::memory_order_release);
    WriteJumpOverTrap(address, 0xe9, rel);
    return true;
}

u64 Get(const PsHostFaultContext* p_context, PsHostRegister reg) {
    return ps_host_fault_register(p_context, reg);
}
void Set(PsHostFaultContext* p_context, PsHostRegister reg, u64 value) {
    ps_host_fault_set_register(p_context, reg, value);
}

int OnTrap(PsHostFaultContext* p_context) {
    const u64 rip = Get(p_context, PS_HOST_REG_RIP) - 1; // past the int3
    const bool is_gx_site = rip == ImageBase + GxNodeSite;
    if (is_gx_site || (rip != 0 && rip == s_va_gx_null_trap.load(std::memory_order_acquire))) {
        if (is_gx_site && Get(p_context, PS_HOST_REG_R12) != 0) {
            *reinterpret_cast<u64*>(Get(p_context, PS_HOST_REG_R12) + 8) = Get(p_context, PS_HOST_REG_R13);
            Set(p_context, PS_HOST_REG_RIP, ImageBase + GxNodeSite + 5);
            return 1;
        }
        const u64 c_nulls = s_c_gx_node_skips.fetch_add(1, std::memory_order_relaxed) + 1;
        if (c_nulls == 1) {
            static const char sz_text[] = "Guest hooks: the GX per-frame command heap was full: a render "
                                          "command waits for the next frame (bbport #162)\n";
            [[maybe_unused]] const auto cb_written = write(STDOUT_FILENO, sz_text, sizeof(sz_text) - 1);
        }
        if (c_nulls == 1 || c_nulls % GxNullLogEvery == 0) {
            LogGxNodeNull(c_nulls, Get(p_context, PS_HOST_REG_R15));
        }
        Set(p_context, PS_HOST_REG_RIP, ImageBase + GxNodeSkip);
        return 1;
    }
    for (const auto& site : s_native_sites) {
        if (rip == ImageBase + site.off_code) {
            if (const u64 stub = site.stub.load(std::memory_order_acquire)) {
                Set(p_context, PS_HOST_REG_RIP, stub); // the detour is going in: its stub runs the site
                return 1;
            }
        }
    }
    if (rip == ImageBase + ReleaseCheck) {
        const auto result = static_cast<std::int32_t>(Get(p_context, PS_HOST_REG_RAX) & 0xffffffff);
        PsHeapSites::NoteReleaseCheck(std::uint32_t(result));
        Set(p_context, PS_HOST_REG_RIP, ImageBase + (result < 0 ? 0xbb5fc7 : ReleaseCheck + 2));
        return 1;
    }
    if (rip == ImageBase + CollectorEntry) {
        const u64 allocator = Get(p_context, PS_HOST_REG_RDI);
        for (std::size_t i = 0; i < PsStats::g_range_allocators.size(); ++i) {
            u64 expected = 0;
            if (PsStats::g_range_allocators[i].load(std::memory_order_relaxed) == allocator ||
                PsStats::g_range_allocators[i].compare_exchange_strong(expected, allocator)) {
                PsStats::g_range_live[i].store(*reinterpret_cast<const u64*>(allocator + 0xc8),
                                             std::memory_order_relaxed);
                PsStats::g_range_bytes[i].store(*reinterpret_cast<const u64*>(allocator + 0xd0),
                                              std::memory_order_relaxed);
                break;
            }
        }
        Set(p_context, PS_HOST_REG_RSP, Get(p_context, PS_HOST_REG_RSP) - 8); // push rbp
        *reinterpret_cast<u64*>(Get(p_context, PS_HOST_REG_RSP)) = Get(p_context, PS_HOST_REG_RBP);
        Set(p_context, PS_HOST_REG_RIP, ImageBase + CollectorEntry + 1);
        return 1;
    }
    for (const u64 site : HeapAllocReturns) {
        if (rip != ImageBase + site) {
            continue;
        }
        const u64 address = Get(p_context, PS_HOST_REG_RAX);
        const u64 cb_copy = *reinterpret_cast<const u64*>(Get(p_context, PS_HOST_REG_RBP) - 0x50);
        if (address != 0 && cb_copy != 0 && cb_copy < (u64(1) << 32)) {
            s_pfn_on_range_allocated(address, cb_copy);
        }
        Set(p_context, PS_HOST_REG_RBX, Get(p_context, PS_HOST_REG_RAX));
        Set(p_context, PS_HOST_REG_RIP, ImageBase + site + sizeof(HeapAllocReturnCode));
        return 1;
    }
    if (rip == ImageBase + AllocReturn) {
        if (const u64 node = Get(p_context, PS_HOST_REG_RAX)) {
            const u64 start = *reinterpret_cast<const u64*>(node + 8);
            const u64 next = *reinterpret_cast<const u64*>(node + 0x18);
            const u64 end = next ? *reinterpret_cast<const u64*>(next + 8) : start;
            if (end > start && end - start < (u64(1) << 32)) {
                s_pfn_on_range_allocated(start, end - start);
            }
        }
        Set(p_context, PS_HOST_REG_RSP, Get(p_context, PS_HOST_REG_RSP) + 0x18);
        Set(p_context, PS_HOST_REG_RIP, ImageBase + AllocReturn + sizeof(AllocReturnCode));
        return 1;
    }
    for (std::size_t i = 0; i < MemcpySites.size(); ++i) {
        const auto& site = MemcpySites[i];
        if (rip != ImageBase + site.off_code) {
            continue;
        }
        s_hits[i].fetch_add(1, std::memory_order_relaxed);
        // The call, to LoaderCopy instead of the game's memcpy: push the return address, jump.
        const u64 ret = ImageBase + site.off_code + 5;
        Set(p_context, PS_HOST_REG_RSP, Get(p_context, PS_HOST_REG_RSP) - 8);
        *reinterpret_cast<u64*>(Get(p_context, PS_HOST_REG_RSP)) = ret;
        Set(p_context, PS_HOST_REG_RIP, reinterpret_cast<u64>(&LoaderCopy));
        return 1;
    }
    return 0;
}

bool Patch(u64 va_range, const unsigned char* p_expected, std::size_t cb_expected) {
    unsigned char bytes[16]{};
    if (ps_host_read_memory(bytes, reinterpret_cast<const void*>(va_range), cb_expected) != std::int64_t(cb_expected) ||
        std::memcmp(bytes, p_expected, cb_expected) != 0) {
        return false;
    }
    const long cb_page = long(ps_host_vm_page_size());
    void* p_page = reinterpret_cast<void*>(va_range & ~u64(cb_page - 1));
    const std::size_t span = (va_range + cb_expected) - reinterpret_cast<u64>(p_page);
    if (ps_host_vm_protect(p_page, span, PS_HOST_VM_READ | PS_HOST_VM_WRITE | PS_HOST_VM_EXECUTE) != 0) {
        return false;
    }
    __atomic_store_n(reinterpret_cast<unsigned char*>(va_range), static_cast<unsigned char>(0xcc),
                     __ATOMIC_SEQ_CST);
    ps_host_vm_protect(p_page, span, PS_HOST_VM_READ | PS_HOST_VM_EXECUTE);
    return true;
}
} // namespace

void Report() {
    const u64 c_skips = s_c_gx_node_skips.load(std::memory_order_relaxed);
    if (c_skips != s_c_gx_node_skips_reported) {
        std::printf("Guest hooks: %llu render commands waited for a GX node (per-frame heap full)\n",
                    static_cast<unsigned long long>(c_skips));
        s_c_gx_node_skips_reported = c_skips;
    }
}

void Install(RangeCallback pfn_on_gpu_range_allocated) {
    static std::atomic<bool> s_is_installed{false};
    if (s_is_installed.exchange(true)) {
        return;
    }
    if (const int error = ps_host_fault_install(PS_HOST_FAULT_BREAKPOINT, OnTrap, PS_HOST_FAULT_NESTED)) {
        std::printf("Guest hooks: the breakpoint handler could not be installed (%s): no hooks\n", std::strerror(error));
        return;
    }
    int c_patched = 0;
    for (const auto& site : MemcpySites) {
        c_patched += Patch(ImageBase + site.off_code, site.code, sizeof(site.code));
    }
    std::printf("Guest hooks: %d of %zu resource copy sites tell the GPU side of their writes\n",
                c_patched, MemcpySites.size());
    s_pfn_on_range_allocated = pfn_on_gpu_range_allocated;
    const bool has_allocator_hook = Patch(ImageBase + AllocReturn, AllocReturnCode, sizeof(AllocReturnCode));
    // PS_HEAP_HOOKS=0 (bisecting): the heap's ranges are not reported.
    const char* sz_heap_env = std::getenv("PS_HEAP_HOOKS");
    const bool should_hook_heap = !sz_heap_env || sz_heap_env[0] != '0';
    if (PsStats::g_is_enabled) {
        Patch(ImageBase + CollectorEntry, CollectorEntryCode, sizeof(CollectorEntryCode));
    }
    int heap_sites = 0;
    for (const u64 site : HeapAllocReturns) {
        if (!should_hook_heap) {
            break;
        }
        heap_sites += Patch(ImageBase + site, HeapAllocReturnCode, sizeof(HeapAllocReturnCode));
    }
    std::printf("Guest hooks: GPU heap allocations seen at %d of %zu call sites\n", heap_sites,
                HeapAllocReturns.size());
    std::printf("Guest hooks: GPU memory allocator %s\n", has_allocator_hook ? "tells the GPU side of new ranges" : "not found");
    const bool is_gx_node_guard_on = PsQuirk::Active(RUNTIME_QUIRK_bloodborne_gx_node_guard);
    const bool has_gx_node_trap =
        is_gx_node_guard_on && Patch(ImageBase + GxNodeSite, GxNodeCode, sizeof(GxNodeCode));
    bool has_gx_node_detour = false;
    // Native detours over the traps just placed (PS_GUEST_HOOK_TRAPS=1 keeps the traps).
    const char* sz_traps_env = std::getenv("PS_GUEST_HOOK_TRAPS");
    if (!(sz_traps_env && sz_traps_env[0] == '1') && (s_p_stub_page = MapStubPage()) != nullptr) {
        int detours = 0;
        for (const auto& site : MemcpySites) {
            if (reinterpret_cast<const u8*>(ImageBase + site.off_code)[0] == 0xcc) {
                detours += InstallCallDetour(site.off_code);
            }
        }
        for (auto& site : s_native_sites) {
            if (reinterpret_cast<const u8*>(ImageBase + site.off_code)[0] == 0xcc) {
                detours += InstallDetour(site);
            }
        }
        has_gx_node_detour = has_gx_node_trap && InstallGxNodeGuard();
        detours += has_gx_node_detour;
        ps_host_vm_protect(s_p_stub_page, StubPageSize, PS_HOST_VM_READ | PS_HOST_VM_EXECUTE);
        std::printf("Guest hooks: %d native detours (no traps; PS_GUEST_HOOK_TRAPS=1: int3)\n",
                    detours);
    }
    std::printf("Guest hooks: GX command heap guard %s\n",
                !is_gx_node_guard_on ? "off (quirk bloodborne_gx_node_guard disabled)"
                : has_gx_node_detour ? "in (bbport #162)"
                : has_gx_node_trap   ? "in as a trap (bbport #162)"
                                     : "not installed (other code there)");
    PsHeapSites::Install();
    if (const char* sz_env = std::getenv("PS_HEAP_SITES"); sz_env && sz_env[0] == '1') {
        Patch(ImageBase + ReleaseCheck, ReleaseCheckCode, sizeof(ReleaseCheckCode));
    }
}
} // namespace PsGuestHooks
