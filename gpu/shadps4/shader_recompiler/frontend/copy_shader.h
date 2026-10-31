// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <map>
#include <span>
#include <vector>

#include "common/types.h"
#include "shader_recompiler/ir/attribute.h"

namespace Shader {

struct CopyShaderData {
    std::map<u32, std::pair<Shader::IR::Attribute, u32>> attr_map;
    u32 num_attrs{0};
    u32 output_vertices{0};
    u32 num_comps{0};

    /// One dword the copy shader writes to a stream-out buffer per vertex, at byte_offset within
    /// the vertex's record. attr is StreamOutUnexported when the ring slot it copies is not also
    /// exported, which the translation does not support (stream_out_parsed is then false).
    struct StreamOutStore {
        u32 buffer;
        u32 byte_offset;
        IR::Attribute attr;
        u32 comp;

        bool operator==(const StreamOutStore&) const = default;
    };
    /// Where a stream-out buffer's V# comes from: S_LOAD_DWORDX4 from the pointer in user SGPR
    /// pair sgpr_pair (1 = s[2:3]) at byte_offset.
    struct StreamOutBuffer {
        u32 sgpr_pair;
        u32 byte_offset;

        bool operator==(const StreamOutBuffer&) const = default;
    };
    static constexpr IR::Attribute StreamOutUnexported = IR::Attribute::Max;
    std::vector<StreamOutStore> stream_out_stores;
    std::vector<StreamOutBuffer> stream_out_buffers; ///< indexed by StreamOutStore::buffer
    bool stream_out_parsed{true};
};

CopyShaderData ParseCopyShader(std::span<const u32> code);

} // namespace Shader
