// SPDX-License-Identifier: GPL-2.0-or-later
// Handles, locks and the buffer allocator the native Ngs2's objects share (spec 01 §1, §4.1).
#include <chrono>
#include <cstring>
#include <thread>

#include "Internal.h"
#include "core/libraries/kernel/process.h"

namespace Libraries::Ngs2 {

void InitHandle(HandleBlock* p_handle, void* p_owner, std::atomic<s32>* p_refs, u32 type, u32 i_voice) {
    *p_handle = {p_handle, p_owner, p_refs, type, i_voice};
}

HandleBlock* AcquireHandle(OrbisNgs2Handle h_object, u32 type_mask) {
    auto* p_handle = reinterpret_cast<HandleBlock*>(h_object);
    if (!p_handle || p_handle->p_self != p_handle || !p_handle->p_owner || !p_handle->p_refs ||
        (p_handle->type & ~type_mask) != 0) {
        return nullptr;
    }
    s32 c_refs = p_handle->p_refs->load(std::memory_order_relaxed);
    do {
        if (c_refs == 0) {
            return nullptr;
        }
    } while (!p_handle->p_refs->compare_exchange_weak(c_refs, c_refs + 1, std::memory_order_acquire));
    return p_handle;
}

void ReleaseHandle(const HandleBlock* p_handle) {
    p_handle->p_refs->fetch_sub(1, std::memory_order_release);
}

bool RetireHandle(OrbisNgs2Handle h_object, u32 type) {
    auto* p_handle = reinterpret_cast<HandleBlock*>(h_object);
    if (!p_handle || p_handle->p_self != p_handle || !p_handle->p_refs || p_handle->type != type) {
        return false;
    }
    for (;;) {
        s32 c_refs = 1;
        if (p_handle->p_refs->compare_exchange_weak(c_refs, 0, std::memory_order_acq_rel)) {
            break;
        }
        if (c_refs == 0) {
            return false;
        }
        std::this_thread::yield();
    }
    std::memset(static_cast<void*>(p_handle), 0, sizeof(*p_handle));
    return true;
}

s32 InvalidHandleError(OrbisNgs2Handle h_object, u32 type_mask) {
    const auto* sz_format = "Invalid handle ({:#x})";
    s32 code = ORBIS_NGS2_ERROR_INVALID_HANDLE;
    if (h_object) {
        switch (type_mask) {
        case HANDLE_SYSTEM:
            sz_format = "Invalid system handle ({:#x})";
            code = ORBIS_NGS2_ERROR_INVALID_SYSTEM_HANDLE;
            break;
        case HANDLE_RACK:
            sz_format = "Invalid rack handle ({:#x})";
            code = ORBIS_NGS2_ERROR_INVALID_RACK_HANDLE;
            break;
        case HANDLE_VOICE:
            sz_format = "Invalid voice handle ({:#x})";
            code = ORBIS_NGS2_ERROR_INVALID_VOICE_HANDLE;
            break;
        case HANDLE_REPORT:
            sz_format = "Invalid report handle ({:#x})";
            code = ORBIS_NGS2_ERROR_INVALID_REPORT_HANDLE;
            break;
        default:
            break;
        }
    }
    return Fail(h_object, code, fmt::runtime(sz_format), h_object);
}

void RecursiveLock::Lock() {
    const auto tid_self = std::this_thread::get_id();
    std::unique_lock lock{m_mutex};
    m_cv.wait(lock, [&] { return m_c_depth == 0 || m_owner == tid_self; });
    m_owner = tid_self;
    ++m_c_depth;
}

bool RecursiveLock::TryLock() {
    const auto tid_self = std::this_thread::get_id();
    std::scoped_lock lock{m_mutex};
    if (m_c_depth != 0 && m_owner != tid_self) {
        return false;
    }
    m_owner = tid_self;
    ++m_c_depth;
    return true;
}

bool RecursiveLock::Unlock() {
    {
        std::scoped_lock lock{m_mutex};
        if (m_c_depth == 0 || m_owner != std::this_thread::get_id()) {
            return false;
        }
        if (--m_c_depth != 0) {
            return true;
        }
        m_owner = {};
    }
    m_cv.notify_one();
    return true;
}

Arena::Arena(void* p_buffer, std::size_t cb_buffer, bool has_guards)
    : m_va_base{reinterpret_cast<uintptr_t>(p_buffer)}, m_va_next{m_va_base}, m_va_end{m_va_base + cb_buffer},
      m_is_measuring{p_buffer == nullptr}, m_has_guards{has_guards} {}

void* Arena::Allocate(std::size_t cb_block, std::size_t cb_align) {
    constexpr std::size_t kCbGuard = 16;
    if (cb_align > m_cb_max_align) {
        m_cb_max_align = cb_align;
    }
    const uintptr_t va_block = (m_va_next + cb_align - 1) & ~uintptr_t(cb_align - 1);
    const std::size_t cb_used = cb_block + (m_has_guards ? kCbGuard : 0);
    m_va_next = va_block + cb_used;
    if (m_is_measuring) {
        return nullptr;
    }
    if (m_va_next > m_va_end || m_va_next < va_block) {
        m_has_overflowed = true;
        return nullptr;
    }
    std::memset(reinterpret_cast<void*>(va_block), 0, cb_used);
    if (m_has_guards) {
        const uintptr_t va_guard = va_block + cb_block;
        const uintptr_t guard[2] = {m_va_last_guard, m_va_last_guard ^ va_guard};
        std::memcpy(reinterpret_cast<void*>(va_guard), guard, sizeof(guard));
        m_va_last_guard = va_guard;
    }
    return reinterpret_cast<void*>(va_block);
}

VoiceLock::VoiceLock(Voice* p_voice) : m_p_voice{p_voice} {
    for (u32 i_attempt = 0;; ++i_attempt) {
        u32 control = p_voice->control.load(std::memory_order_relaxed);
        if (!(control & VOICE_BUSY) &&
            p_voice->control.compare_exchange_weak(control, control | VOICE_BUSY, std::memory_order_acquire)) {
            return;
        }
        // The firmware backs off the same way (spec 03 §1.1): spin, then sleep 100 to 1000 us.
        if (i_attempt > 16) {
            const auto delay_us = i_attempt > 128 ? 1000 : i_attempt > 64 ? 500 : i_attempt > 32 ? 200 : 100;
            std::this_thread::sleep_for(std::chrono::microseconds(delay_us));
        }
    }
}

VoiceLock::~VoiceLock() {
    m_p_voice->control.fetch_and(~u32(VOICE_BUSY), std::memory_order_release);
}

void NoteUnimplemented(std::uint64_t* p_c_uses, const char* sz_what) {
    if (__atomic_fetch_add(p_c_uses, 1, __ATOMIC_RELAXED) == 0) {
        Common::Log::Stubbed(sz_what, p_c_uses);
        LOG_ERROR(Lib_Ngs2, "{}: not implemented by the native Ngs2 yet", sz_what);
    }
}

void NoteDropped(std::uint64_t* p_c_uses, const char* sz_what) {
    if (__atomic_fetch_add(p_c_uses, 1, __ATOMIC_RELAXED) == 0) {
        Common::Log::Stubbed(sz_what, p_c_uses);
        LOG_WARNING(Lib_Ngs2, "{}: dropped, as the firmware does", sz_what);
    }
}

// Some behaviour depends on the SDK the title was built with (spec 04 §5); without an answer the
// native Ngs2 behaves as for current SDKs.
bool IsCompiledSdkAtLeast(u32 version) {
    static const u32 s_sdk_version = [] {
        s32 sdk_version = 0;
        return Libraries::Kernel::sceKernelGetCompiledSdkVersion(&sdk_version) == ORBIS_OK ? u32(sdk_version)
                                                                                            : 0xffffffffu;
    }();
    return s_sdk_version >= version;
}

} // namespace Libraries::Ngs2
