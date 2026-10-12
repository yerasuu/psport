// SPDX-License-Identifier: GPL-2.0-or-later
// gcn_dis FILE: prints a raw GCN program (as BB_DUMP_SKIPPED_GS writes them) one instruction per
// line with the recompiler's own decoder: what the translator sees, operand fields and the
// memory-instruction offsets, to read a title's shaders when LLVM cannot (it does not disassemble
// gfx7). Build: tools/gcn_dis.sh.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

#include <magic_enum/magic_enum.hpp>

#include "common/assert.h"
#include "common/logging/log.h"
#include "shader_recompiler/frontend/decode.h"

using namespace Shader::Gcn;

// What the decoder links against in libbbgpu: messages to stderr, failed asserts end the tool.
namespace Common::Log {
bool ShouldLog(Level) {
    return true;
}
void Write(std::string_view log_class, Level, const char*, int line, const char* function, std::string&& message) {
    std::fprintf(stderr, "%.*s %s:%d %s\n", int(log_class.size()), log_class.data(), function, line, message.c_str());
}
} // namespace Common::Log
void assert_fail_impl() {
    std::fprintf(stderr, "decoder assertion failed\n");
    std::abort();
}
[[noreturn]] void unreachable_impl() {
    std::fprintf(stderr, "decoder reached unreachable code\n");
    std::abort();
}

static void PrintOperand(const InstOperand& op) {
    std::printf(" %s:%u", magic_enum::enum_name(op.field).data(), op.code);
}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: gcn_dis FILE\n");
        return 2;
    }
    std::ifstream file(argv[1], std::ios::binary);
    if (!file) {
        std::fprintf(stderr, "gcn_dis: cannot open %s\n", argv[1]);
        return 1;
    }
    std::vector<char> bytes{std::istreambuf_iterator<char>(file), {}};
    const std::size_t dwords = bytes.size() / 4;
    // A truncated last instruction (64-bit, or with a literal) is decoded from zeros, not past the end.
    std::vector<u32> code(dwords + 2);
    std::memcpy(code.data(), bytes.data(), dwords * 4);
    GcnCodeSlice slice{code.data(), code.data() + code.size()};
    GcnDecodeContext decoder;
    while (slice.position() < code.data() + dwords) {
        const u32 pc = static_cast<u32>(slice.position() - code.data()) * 4;
        const GcnInst inst = decoder.decodeInstruction(slice);
        std::printf("%04x %-28s", pc, magic_enum::enum_name(inst.opcode).data());
        for (u32 i = 0; i < inst.dst_count; ++i) {
            PrintOperand(inst.dst[i]);
        }
        std::printf(" <-");
        for (u32 i = 0; i < inst.src_count; ++i) {
            PrintOperand(inst.src[i]);
        }
        if (inst.encoding == InstEncoding::MUBUF) {
            std::printf("  [offset %u offen %u idxen %u glc %u slc %u]", u32(inst.control.mubuf.offset),
                        u32(inst.control.mubuf.offen), u32(inst.control.mubuf.idxen), u32(inst.control.mubuf.glc),
                        u32(inst.control.mubuf.slc));
        } else if (inst.encoding == InstEncoding::MTBUF) {
            std::printf("  [offset %u offen %u idxen %u dfmt %u nfmt %u]", u32(inst.control.mtbuf.offset),
                        u32(inst.control.mtbuf.offen), u32(inst.control.mtbuf.idxen), u32(inst.control.mtbuf.dfmt),
                        u32(inst.control.mtbuf.nfmt));
        } else if (inst.encoding == InstEncoding::SMRD) {
            std::printf("  [offset %u imm %u]", u32(inst.control.smrd.offset), u32(inst.control.smrd.imm));
        } else if (inst.encoding == InstEncoding::EXP) {
            std::printf("  [target %u en %x done %u]", u32(inst.control.exp.target), u32(inst.control.exp.en),
                        u32(inst.control.exp.done));
        } else if (inst.encoding == InstEncoding::SOPK) {
            std::printf("  [simm %d]", int(inst.control.sopk.simm));
        }
        std::printf("\n");
        if (inst.opcode == Opcode::S_ENDPGM) {
            break;
        }
    }
    return 0;
}
