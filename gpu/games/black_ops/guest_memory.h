// SPDX-License-Identifier: GPL-2.0-or-later
// Call of Duty: Black Ops and Black Ops II: the profile's reads and writes of the game's memory
// from the host (mouse_camera.cpp, crosshair.cpp), each surviving a page that is not mapped.
#pragma once

#include <atomic>
#include <csetjmp>
#include <cstdint>
#include <cstring>

// The runtime's fault recovery (src/main/signals.c): a fault while it is set jumps back to it.
extern "C" __thread sigjmp_buf* g_p_runtime_fault_recover;

namespace PsBlackOps {
/// The guest's pointers fit 40 bits.
constexpr std::uint64_t GuestAddressEnd = 1ull << 40;

inline bool IsGuestAddress(std::uint64_t va, std::uint64_t alignment) {
    return va >= 0x10000 && va < GuestAddressEnd && va % alignment == 0;
}

/// Copies between guest and host memory; false where nothing is mapped (the runtime's fault
/// recovery).
inline bool Copy(void* p_to, const void* p_from, std::size_t cb_copy) {
    sigjmp_buf recover;
    if (sigsetjmp(recover, 0)) {
        g_p_runtime_fault_recover = nullptr;
        return false;
    }
    g_p_runtime_fault_recover = &recover;
    std::atomic_signal_fence(std::memory_order_seq_cst);
    std::memcpy(p_to, p_from, cb_copy);
    std::atomic_signal_fence(std::memory_order_seq_cst);
    g_p_runtime_fault_recover = nullptr;
    return true;
}

template <typename T>
bool Get(std::uint64_t va_from, T* p_out) {
    return Copy(p_out, reinterpret_cast<const void*>(va_from), sizeof(T));
}

template <typename T>
bool Put(std::uint64_t va_to, const T& value) {
    return Copy(reinterpret_cast<void*>(va_to), &value, sizeof(T));
}
} // namespace PsBlackOps
