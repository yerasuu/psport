// SPDX-License-Identifier: GPL-2.0-or-later
// The GS copy shader parser (shader_recompiler/frontend/copy_shader.cpp) on hand-encoded gfx7
// programs shaped like Atelier Ryza's stream-out copy shader: ring loads, the V# loaded through
// the user-data pointer s[2:3], TBUFFER stores of a 24-byte vertex record, exports (no game code).
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "common/assert.h"
#include "common/logging/log.h"
#include "shader_recompiler/frontend/copy_shader.h"

using Shader::CopyShaderData;
using Shader::IR::Attribute;

// What the decoder links against in libbbgpu; a failed parser assert fails the test.
namespace Common::Log {
bool ShouldLog(Level) {
    return true;
}
void Write(std::string_view log_class, Level, const char*, int line, const char* function, std::string&& message) {
    std::fprintf(stderr, "%.*s %s:%d %s\n", int(log_class.size()), log_class.data(), function, line, message.c_str());
}
} // namespace Common::Log
void assert_fail_impl() {
    std::abort();
}
[[noreturn]] void unreachable_impl() {
    std::abort();
}

// Found by argument-dependent lookup from std::vector's comparison.
namespace Shader {
bool operator==(const CopyShaderData::StreamOutStore& a, const CopyShaderData::StreamOutStore& b) {
    return a.buffer == b.buffer && a.byte_offset == b.byte_offset && a.attr == b.attr && a.comp == b.comp;
}
} // namespace Shader

namespace {

constexpr u32 Vcc = 106, Exec = 126, ConstZero = 128, Literal = 255;
constexpr u32 Int(u32 value) {
    return 128 + value;
}

struct Program {
    std::vector<u32> words;

    Program& Sop1(u32 op, u32 sdst, u32 ssrc0) {
        words.push_back(0x17Du << 23 | sdst << 16 | op << 8 | ssrc0);
        return *this;
    }
    Program& Sop2(u32 op, u32 sdst, u32 ssrc0, u32 ssrc1) {
        words.push_back(2u << 30 | op << 23 | sdst << 16 | ssrc1 << 8 | ssrc0);
        return *this;
    }
    Program& Sopp(u32 op, u16 simm) {
        words.push_back(0x17Fu << 23 | op << 16 | simm);
        return *this;
    }
    Program& Lit(u32 value) {
        words.push_back(value);
        return *this;
    }
    Program& SLoadX4(u32 sdst, u32 sbase_pair, u32 dword_offset) {
        words.push_back(0x18u << 27 | 2u << 22 | sdst << 15 | sbase_pair << 9 | 1u << 8 | dword_offset);
        return *this;
    }
    Program& Vop1(u32 op, u32 vdst, u32 src0) {
        words.push_back(0x3Fu << 25 | vdst << 17 | op << 9 | src0);
        return *this;
    }
    Program& Vop2(u32 op, u32 vdst, u32 src0, u32 vsrc1) {
        words.push_back(op << 25 | vdst << 17 | vsrc1 << 9 | src0);
        return *this;
    }
    Program& Vopc(u32 op, u32 src0, u32 vsrc1) {
        words.push_back(0x3Eu << 25 | op << 17 | vsrc1 << 9 | src0);
        return *this;
    }
    // BUFFER_LOAD_DWORD vdata, vaddr (offen) from the GSVS ring s[8:11], glc slc, soffset 0.
    Program& RingLoad(u32 vdata, u32 vaddr, u32 offset) {
        words.push_back(0x38u << 26 | 0x0Cu << 18 | 1u << 14 | 1u << 12 | offset);
        words.push_back(ConstZero << 24 | 1u << 22 | 2u << 16 | vdata << 8 | vaddr);
        return *this;
    }
    // TBUFFER_STORE_FORMAT_<count> vdata, vaddr (idxen) to the V# in s[0:3], soffset vcc_lo.
    Program& TStore(u32 count, u32 dfmt, u32 vdata, u32 vaddr, u32 offset) {
        words.push_back(0x3Au << 26 | 7u << 23 | dfmt << 19 | (count + 3) << 16 | 1u << 13 | offset);
        words.push_back(Vcc << 24 | 0u << 16 | vdata << 8 | vaddr);
        return *this;
    }
    Program& Exp(u32 target, u32 v0, u32 v1, u32 v2, u32 v3, bool done = false) {
        words.push_back(0x3Eu << 26 | (done ? 1u : 0u) << 11 | target << 4 | 0xF);
        words.push_back(v3 << 24 | v2 << 16 | v1 << 8 | v0);
        return *this;
    }
};

constexpr u32 S_MOV_B32 = 3, S_MOV_B64 = 4, S_AND_SAVEEXEC_B64 = 0x24, S_LSHL_B32 = 0x1E,
              S_BFE_U32 = 0x27, S_ENDPGM = 1, S_CBRANCH_EXECZ = 8, S_WAITCNT = 0xC, V_MOV_B32 = 1,
              V_LSHLREV_B32 = 0x1A, V_CMP_GT_U32 = 0xC4;
constexpr u32 Pos0 = 12, Param0 = 32, Param1 = 33, Param2 = 34;

// The copy shader's layout: s[0:1] the ring table, s[2:3] the stream-out table, s4
// streamout_config, s5 the write index, s6 streamout_offset0. Ring components 0/64/128 go to
// bytes 0..11 and param0, 192 to byte 12 and param1, 256/320 to bytes 16..23 and param2.
// param2_y: the VGPR param2.y exports, v4 (ring offset 320) or v6 (no ring slot).
Program CopyShader(u32 param2_y) {
    Program p;
    p.Sop1(S_MOV_B32, 107, Literal).Lit(32)
        .Vop2(V_LSHLREV_B32, 5, Int(2), 0)
        .SLoadX4(8, 0, 32)
        .Sop2(S_BFE_U32, Vcc, 4, Literal).Lit(7u << 16 | 16)
        .Vopc(V_CMP_GT_U32, Vcc, 0)
        .Sopp(S_WAITCNT, 0x7F)
        .RingLoad(0, 5, 0).RingLoad(1, 5, 64).RingLoad(2, 5, 128)
        .RingLoad(3, 5, 256).RingLoad(4, 5, 320).RingLoad(5, 5, 192)
        .Sop1(S_AND_SAVEEXEC_B64, 12, Vcc)
        .Sopp(S_CBRANCH_EXECZ, 9)
        .Vop1(V_MOV_B32, 6, 5)
        .SLoadX4(0, 1, 0)
        .Sop2(S_LSHL_B32, Vcc, 6, Int(2))
        .Sopp(S_WAITCNT, 0x7F)
        .TStore(3, 13, 0, 6, 0)
        .TStore(1, 4, 5, 6, 12)
        .TStore(2, 11, 3, 6, 16)
        .Sop1(S_MOV_B64, Exec, 12)
        .Vop1(V_MOV_B32, 6, ConstZero)
        .Exp(Pos0, 6, 6, 6, 6, true)
        .Exp(Param2, 3, param2_y, 6, 6)
        .Exp(Param1, 5, 6, 6, 6)
        .Exp(Param0, 0, 1, 2, 6)
        .Sopp(S_ENDPGM, 0);
    return p;
}

void RyzaStreamOut() {
    const auto data = Shader::ParseCopyShader(CopyShader(4).words);
    // What shaders without stores already relied on stays as it was.
    assert(data.num_comps == 6 && data.num_attrs == 3 && data.output_vertices == 1);
    assert(data.attr_map.at(192) == std::pair(Attribute::Param1, 0u));
    assert(data.stream_out_parsed);
    const std::vector<CopyShaderData::StreamOutStore> stores = {
        {0, 0, Attribute::Param0, 0},  {0, 4, Attribute::Param0, 1}, {0, 8, Attribute::Param0, 2},
        {0, 12, Attribute::Param1, 0}, {0, 16, Attribute::Param2, 0}, {0, 20, Attribute::Param2, 1}};
    assert(data.stream_out_stores == stores);
    assert(data.stream_out_buffers.size() == 1);
    assert(data.stream_out_buffers[0].sgpr_pair == 1 && data.stream_out_buffers[0].byte_offset == 0);
}

void StreamedButNotExported() {
    const auto data = Shader::ParseCopyShader(CopyShader(6).words);
    assert(!data.stream_out_parsed);
    assert(data.stream_out_stores.size() == 6);
    const auto& store = data.stream_out_stores[5];
    assert(store.byte_offset == 20 && store.attr == CopyShaderData::StreamOutUnexported);
    assert(data.stream_out_stores[4] == (CopyShaderData::StreamOutStore{0, 16, Attribute::Param2, 0}));
}

} // namespace

int main() {
    RyzaStreamOut();
    StreamedButNotExported();
    std::puts("PASS: copy shader: stream-out stores, V# source, streamed but not exported slot");
}
