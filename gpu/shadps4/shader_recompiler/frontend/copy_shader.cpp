// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>

#include "shader_recompiler/frontend/copy_shader.h"
#include "shader_recompiler/frontend/decode.h"
#include "shader_recompiler/ir/attribute.h"

namespace Shader {

namespace {

// GCN dfmt of the 32-bit-per-component formats, by component count.
constexpr std::array<u32, 5> StoreDfmt{0, 4, 11, 13, 14};

struct PendingStore {
    u32 buffer;
    u32 byte_offset;
    s32 ring_offset;
};

} // namespace

CopyShaderData ParseCopyShader(std::span<const u32> code) {
    Gcn::GcnCodeSlice code_slice{code.data(), code.data() + code.size()};
    Gcn::GcnDecodeContext decoder;

    constexpr u32 token_mov_vcchi = 0xBEEB03FF;
    ASSERT_MSG(code[0] == token_mov_vcchi, "First instruction is not s_mov_b32 vcc_hi, #imm");

    std::array<s32, 64> offsets{};
    offsets.fill(-1);

    std::array<s32, 256> sources{};
    sources.fill(-1);

    // S_LOAD_DWORDX4 destination SGPR -> where it loaded from, to find a store's V#.
    std::map<u32, CopyShaderData::StreamOutBuffer> vsharp_loads;
    std::vector<PendingStore> pending_stores;

    CopyShaderData data{};
    // Stores come before the exports that fill attr_map, so they are resolved after the walk.
    const auto record_store = [&](const Gcn::GcnInst& inst, u32 count, u32 offset) {
        const auto load = vsharp_loads.find(inst.src[2].code * 4);
        if (load == vsharp_loads.end()) {
            data.stream_out_parsed = false;
            return;
        }
        const auto& source = load->second;
        // The buffer index is the order in which distinct V#s appear; Ryza's copy shaders use
        // buffer 0 only, so nothing confirms how other titles number theirs.
        const auto it = std::ranges::find_if(data.stream_out_buffers, [&](const auto& buffer) {
            return buffer.sgpr_pair == source.sgpr_pair && buffer.byte_offset == source.byte_offset;
        });
        const u32 buffer = static_cast<u32>(it - data.stream_out_buffers.begin());
        if (it == data.stream_out_buffers.end()) {
            data.stream_out_buffers.push_back(source);
        }
        for (u32 k = 0; k < count; ++k) {
            const u32 vgpr = inst.src[1].code + k;
            const s32 ring_offset = vgpr < offsets.size() ? offsets[vgpr] : -1;
            pending_stores.push_back({buffer, offset + 4 * k, ring_offset});
        }
    };
    auto last_attr{IR::Attribute::Position0};
    while (!code_slice.atEnd()) {
        auto inst = decoder.decodeInstruction(code_slice);
        switch (inst.opcode) {
        case Gcn::Opcode::S_LSHL_B32: {
            // Stream-out shaders shift streamout_offset0 (an SGPR) into soffset.
            if (inst.src[0].field != Gcn::OperandField::SignedConstIntPos ||
                inst.src[1].field != Gcn::OperandField::SignedConstIntPos) {
                sources[inst.dst[0].code] = -1;
                break;
            }
            sources[inst.dst[0].code] =
                (inst.src[0].code - Gcn::OperandFieldRange::SignedConstIntPosMin + 1)
                << (inst.src[1].code - Gcn::OperandFieldRange::SignedConstIntPosMin + 1);
            break;
        }
        case Gcn::Opcode::S_MOVK_I32: {
            sources[inst.dst[0].code] = inst.control.sopk.simm;
            break;
        }
        case Gcn::Opcode::S_MOV_B32: {
            sources[inst.dst[0].code] = inst.src[0].code;
            break;
        }
        case Gcn::Opcode::S_ADDK_I32: {
            sources[inst.dst[0].code] += inst.control.sopk.simm;
            break;
        }
        case Gcn::Opcode::S_BFM_B32: {
            ASSERT(inst.src[0].field == Gcn::OperandField::SignedConstIntPos &&
                   inst.src[1].field == Gcn::OperandField::SignedConstIntPos);
            const auto src0 = inst.src[0].code - Gcn::OperandFieldRange::SignedConstIntPosMin + 1;
            const auto src1 = inst.src[1].code - Gcn::OperandFieldRange::SignedConstIntPosMin + 1;
            sources[inst.dst[0].code] = ((1 << src0) - 1) << src1;
            break;
        }
        case Gcn::Opcode::EXP: {
            const auto& exp = inst.control.exp;
            const IR::Attribute semantic = static_cast<IR::Attribute>(exp.target);
            for (int i = 0; i < inst.src_count; ++i) {
                if ((exp.en & (1 << i)) == 0) {
                    continue;
                }
                const auto ofs = offsets[inst.src[i].code];
                if (ofs != -1) {
                    data.attr_map[ofs] = {semantic, i};
                    if (semantic > last_attr) {
                        last_attr = semantic;
                    }
                }
            }
            break;
        }
        case Gcn::Opcode::BUFFER_LOAD_DWORD: {
            ASSERT_MSG(inst.src[1].code < offsets.size(),
                       "offsets array for geometry shaders is too short");
            offsets[inst.src[1].code] = inst.control.mubuf.offset;
            if (inst.src[3].field != Gcn::OperandField::ConstZero) {
                const u32 index = inst.src[3].code;
                ASSERT(sources[index] != -1);
                offsets[inst.src[1].code] += sources[index];
            }
            data.num_comps++;
            break;
        }
        case Gcn::Opcode::S_LOAD_DWORDX4: {
            const auto& smrd = inst.control.smrd;
            if (smrd.imm) {
                vsharp_loads[inst.dst[0].code] = {inst.src[0].code, smrd.offset * 4u};
            } else {
                vsharp_loads.erase(inst.dst[0].code);
            }
            break;
        }
        case Gcn::Opcode::TBUFFER_STORE_FORMAT_X:
        case Gcn::Opcode::TBUFFER_STORE_FORMAT_XY:
        case Gcn::Opcode::TBUFFER_STORE_FORMAT_XYZ:
        case Gcn::Opcode::TBUFFER_STORE_FORMAT_XYZW: {
            const auto& mtbuf = inst.control.mtbuf;
            // byte_offset is relative to the vertex's record only when the store indexes by vertex.
            if (mtbuf.dfmt != StoreDfmt[mtbuf.count] || mtbuf.offen || !mtbuf.idxen) {
                data.stream_out_parsed = false;
            }
            record_store(inst, mtbuf.count, mtbuf.offset);
            break;
        }
        case Gcn::Opcode::BUFFER_STORE_DWORD:
        case Gcn::Opcode::BUFFER_STORE_DWORDX2:
        case Gcn::Opcode::BUFFER_STORE_DWORDX3:
        case Gcn::Opcode::BUFFER_STORE_DWORDX4: {
            const auto& mubuf = inst.control.mubuf;
            if (mubuf.offen || !mubuf.idxen) {
                data.stream_out_parsed = false;
            }
            record_store(inst, mubuf.count, mubuf.offset);
            break;
        }
        default:
            break;
        }
    }

    if (!IsPosition(last_attr)) {
        data.num_attrs = static_cast<u32>(last_attr) - static_cast<u32>(IR::Attribute::Param0) + 1;
    }
    if (data.attr_map.size() > 1) {
        const auto it = data.attr_map.begin();
        const u32 comp_stride = std::next(it)->first - it->first;
        data.output_vertices = comp_stride / 64;
    }

    data.stream_out_stores.reserve(pending_stores.size());
    for (const auto& store : pending_stores) {
        const auto attr = store.ring_offset == -1 ? data.attr_map.end()
                                                  : data.attr_map.find(store.ring_offset);
        if (attr == data.attr_map.end()) {
            data.stream_out_stores.push_back(
                {store.buffer, store.byte_offset, CopyShaderData::StreamOutUnexported, 0});
            data.stream_out_parsed = false;
            continue;
        }
        const auto [semantic, comp] = attr->second;
        data.stream_out_stores.push_back({store.buffer, store.byte_offset, semantic, comp});
    }

    return data;
}

} // namespace Shader
