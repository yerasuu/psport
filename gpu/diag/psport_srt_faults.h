// SPDX-License-Identifier: GPL-2.0-or-later
// psport: names the resource table behind each faulting SRT walker load (#750). The fault handler
// in flatten_extended_userdata_pass.cpp supplies zero and calls Note(), which reads the walker's
// own code up to the load to find the user-data SGPR the table hangs from and the dword offsets of
// the pointers leading to it. The first fault of each (shader, table) is logged with the shader's
// stage and hash, the user-data value and, on the command processor thread, the packet being
// executed and the SET_SH_REG in its command buffer that wrote that SGPR; at most
// C_LINES_PER_SHADER tables per shader are logged, all are counted in the call summary. The hooks
// below are plain thread-local stores, so walkers that never fault pay nothing else.
#pragma once

#include <cstddef>
#include <cstdint>

namespace Shader {
struct Info;
}

namespace PsSrtFaults {

struct Origin {
    const Shader::Info* p_info;
    const std::uint32_t* p_user_data;
    std::size_t c_user_data;
    const std::uint32_t* p_dcb;
    const std::uint32_t* p_packet;
};
extern constinit thread_local Origin g_origin;

/// Before this thread runs `p_info`'s walker over the `c_user_data` dwords at `p_user_data`.
inline void SetWalker(const Shader::Info* p_info, const std::uint32_t* p_user_data, std::size_t c_user_data) {
    g_origin.p_info = p_info;
    g_origin.p_user_data = p_user_data;
    g_origin.c_user_data = c_user_data;
}

/// The packet this command processor thread executes, inside the command buffer starting at
/// `p_dcb` (a packet boundary); null for both once the buffer is done.
inline void SetPacket(const std::uint32_t* p_dcb, const std::uint32_t* p_packet) {
    g_origin.p_dcb = p_dcb;
    g_origin.p_packet = p_packet;
}

/// From the walker fault handler, after it supplied zero for the load at `p_code`.
void Note(const std::uint8_t* p_code, std::uintptr_t va_fault);

} // namespace PsSrtFaults
