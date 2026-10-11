// SPDX-License-Identifier: GPL-2.0-or-later
// GS stream-out through the actual SPIR-V backend, without a window or the game: a GS whose copy
// shader stores Atelier Ryza's 24-byte record (param0.xyz, param1.x, param2.xy) into buffer 0, and
// layouts transform feedback can't express. Output is validated by spirv-val:
//   out/gpu/xfb-shader-test <dir> && spirv-val --target-env vulkan1.3 <dir>/*.spv
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/ir/ir_emitter.h"

using namespace Shader;
using Store = CopyShaderData::StreamOutStore;

static u32 Emit(const std::filesystem::path& path, const std::vector<Store>& stores, u32 stride) {
    Info info{};
    info.hw_stage = HwStage::Geometry;
    info.sw_stage = SwStage::Geometry;
    info.gs_copy_data.num_attrs = 3;
    info.gs_copy_data.stream_out_stores = stores;
    RuntimeInfo runtime{};
    runtime.Initialize(HwStage::Geometry, SwStage::Geometry);
    runtime.hw.gs.in_primitive = AmdGpu::PrimitiveType::TriangleList;
    runtime.hw.gs.out_primitive.fill(AmdGpu::GsOutputPrimitiveType::TriangleStrip);
    runtime.hw.gs.output_vertices = 3;
    runtime.hw.gs.num_invocations = 1;
    runtime.hw.gs.in_vertex_data_size = 8;
    runtime.hw.gs.xfb_stride = stride;

    Common::ObjectPool<IR::Inst> pool;
    IR::Block block(pool);
    IR::IREmitter ir(block);
    for (u32 vertex = 0; vertex < 3; ++vertex) {
        for (u32 component = 0; component < 4; ++component) {
            info.stores.Set(IR::Attribute::Position0, component);
            ir.SetAttribute(IR::Attribute::Position0, ir.Imm32(0.0f), component);
            for (u32 param = 0; param < 3; ++param) {
                const auto attr = IR::Attribute::Param0 + param;
                info.stores.Set(attr, component);
                ir.SetAttribute(attr, ir.Imm32(float(vertex * 16 + param * 4 + component)), component);
            }
        }
        ir.EmitVertex();
    }
    ir.EmitPrimitive();
    ir.Epilogue();
    IR::Program program(info);
    program.blocks.push_back(&block);
    program.syntax_list.push_back({.data = {.block = &block}, .type = IR::AbstractSyntaxNode::Type::Block});
    program.syntax_list.push_back({.type = IR::AbstractSyntaxNode::Type::Return});

    Profile profile{};
    profile.supported_spirv = 0x00010600;
    profile.support_int64 = true;
    profile.supports_transform_feedback = true;
    profile.max_geometry_output_components = 128;
    profile.max_geometry_total_output_components = 1024;
    Backend::Bindings bindings{};
    const auto code = Backend::SPIRV::EmitSPIRV(profile, runtime, program, bindings);
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(code.data()), code.size() * sizeof(u32));
    return info.xfb_stride;
}

int main(int argc, char** argv) {
    const std::filesystem::path dir = argc > 1 ? argv[1] : ".";
    std::filesystem::create_directories(dir);
    const auto p = [](u32 n) { return IR::Attribute::Param0 + n; };
    // Ryza's stores, in the copy shader's order: TBUFFER XYZ, X (exec-masked), XY.
    const std::vector<Store> ryza{{0, 0, p(0), 0},  {0, 4, p(0), 1},  {0, 8, p(0), 2},
                                  {0, 12, p(1), 0}, {0, 16, p(2), 0}, {0, 20, p(2), 1}};
    const std::vector<Store> overlapping{{0, 0, p(0), 0}, {0, 0, p(1), 0}};
    const std::vector<Store> past_stride{{0, 0, p(0), 0}, {0, 24, p(0), 1}};
    const std::vector<Store> other_buffer{{1, 0, p(0), 0}};
    struct Case {
        const char* name;
        const std::vector<Store>& stores;
        u32 stride;
        u32 expected;
    };
    int failures = 0;
    for (const Case& test : {Case{"ryza", ryza, 24, 24}, Case{"overlapping", overlapping, 24, 0},
                             Case{"past-stride", past_stride, 24, 0}, Case{"other-buffer", other_buffer, 24, 0},
                             Case{"no-stream-out", ryza, 0, 0}}) {
        const u32 stride = Emit(dir / (std::string("gs-") + test.name + ".spv"), test.stores, test.stride);
        if (stride != test.expected) {
            std::fprintf(stderr, "%s: Info::xfb_stride %u, expected %u\n", test.name, stride, test.expected);
            ++failures;
        }
    }
    return failures != 0;
}
