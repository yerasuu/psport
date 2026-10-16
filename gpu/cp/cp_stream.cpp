// SPDX-License-Identifier: GPL-2.0-or-later
#include "cp_stream.h"

#include <algorithm>
#include <bit>
#include <cstdarg>
#include <cstdio>

namespace PsCp {

namespace {

// Single writer (the executor's thread); atomic only for the summary read from another thread.
void Bump(std::atomic<u64>& counter, u64 amount = 1) {
    counter.store(counter.load(std::memory_order_relaxed) + amount, std::memory_order_relaxed);
}

unsigned long long Load(const std::atomic<u64>& counter) {
    return counter.load(std::memory_order_relaxed);
}

constexpr u64 MaxDetailedLines = 16;

constexpr const char* s_sz_mismatch_names[] = {"sequence",   "missing",    "extra",        "registers",
                                               "draw state", "parameters", "indirect base"};
static_assert(std::size(s_sz_mismatch_names) == static_cast<std::size_t>(Mismatch::Count));

} // namespace

StreamRecorder::StreamRecorder(Decoder& decoder) : m_decoder{decoder} {
    m_decoder.SetObserver(this);
}

void StreamRecorder::Record(std::span<const u32> buffer, u64 va_indirect_base, CommandStream& out_stream) {
    out_stream.commands.clear();
    out_stream.writes.clear();
    out_stream.values.clear();
    out_stream.sz_bad = nullptr;
    m_p_stream = &out_stream;
    m_i_pending_write = 0;
    m_va_indirect_base = va_indirect_base;
    m_decoder.Decode(buffer, *this);
    m_p_stream = nullptr;
}

void StreamRecorder::OnRegisterPacket(std::span<const u32>, std::span<const RegisterWrite> writes, bool) {
    auto& stream = *m_p_stream;
    const auto registers = m_decoder.Registers().Words();
    for (const auto& write : writes) {
        stream.writes.push_back({write, static_cast<u32>(stream.values.size())});
        const auto written = registers.subspan(write.m_i_first, write.m_c_registers);
        stream.values.insert(stream.values.end(), written.begin(), written.end());
    }
}

void StreamRecorder::OnPacket(const Packet& packet) {
    auto& stream = *m_p_stream;
    if (packet.Opcode() == Op::SetBase && (packet.Word(0) & 0xF) == BaseIndex::DrawIndexIndirectPatchTable) {
        m_va_indirect_base = packet.Word(1) | u64(packet.Word(2) & 0xFFFF) << 32;
    }
    const auto c_writes = static_cast<u32>(stream.writes.size());
    stream.commands.push_back({.va_packet = reinterpret_cast<u64>(packet.m_body.data() - 1),
                               .va_indirect_base = 0,
                               .parameters_hash = 0,
                               .draw_registers = {},
                               .c_words = static_cast<u32>(packet.m_body.size() + 1),
                               .i_first_write = m_i_pending_write,
                               .c_writes = c_writes - m_i_pending_write,
                               .op = packet.Opcode(),
                               .depth = static_cast<std::uint8_t>(packet.m_depth),
                               .kind = CommandKind::Packet,
                               .is_indirect = false});
    m_i_pending_write = c_writes;
}

void StreamRecorder::OnDraw(const Draw& draw) {
    auto& command = m_p_stream->commands.back();
    command.kind = CommandKind::Draw;
    command.is_indirect = draw.kind >= DrawKind::Indirect;
    command.va_indirect_base = command.is_indirect ? m_va_indirect_base : 0;
    command.parameters_hash = HashWords(draw.p_packet->m_body);
    const auto& registers = m_decoder.Registers();
    std::ranges::transform(DrawRegisters, command.draw_registers.begin(),
                           [&](u32 i_register) { return registers[i_register]; });
}

void StreamRecorder::OnDispatch(const Dispatch& dispatch) {
    auto& command = m_p_stream->commands.back();
    command.kind = CommandKind::Dispatch;
    command.is_indirect = dispatch.is_indirect;
    command.va_indirect_base = command.is_indirect ? m_va_indirect_base : 0;
    command.parameters_hash = HashWords(dispatch.p_packet->m_body);
}

std::span<const u32> StreamRecorder::ResolveIndirectBuffer(u64 address, u32 dwords) {
    return {reinterpret_cast<const u32*>(address), dwords};
}

void StreamRecorder::OnBadPacket(std::span<const u32>, const char* what, int) {
    m_p_stream->sz_bad = what;
}

void StreamCheck::Begin(const CommandStream& stream) {
    m_p_stream = &stream;
    m_i_next = 0;
    m_is_diverged = false;
    m_is_mismatched = false;
    m_is_draw_state_unknown = false;
}

void StreamCheck::OnExecuted(std::span<const u32> packet, int depth, std::span<const u32> registers,
                             u64 va_indirect_base) {
    if (!m_p_stream || m_is_diverged) {
        return;
    }
    const auto& commands = m_p_stream->commands;
    const Header header{packet[0]};
    const auto va_packet = reinterpret_cast<u64>(packet.data());
    if (m_i_next == commands.size()) {
        m_is_diverged = true;
        Report(Mismatch::Missing, "Liverpool ran %#llx (opcode %#x, depth %d) after the stream's %zu commands (%s)",
               static_cast<unsigned long long>(va_packet), static_cast<u32>(header.Opcode()), depth,
               commands.size(), m_p_stream->sz_bad ? m_p_stream->sz_bad : "its end");
        return;
    }
    const Command& command = commands[m_i_next++];
    Bump(m_c_commands);
    if (command.va_packet != va_packet || command.c_words != packet.size() || command.op != header.Opcode() ||
        command.depth != depth) {
        m_is_diverged = true;
        Report(Mismatch::Sequence,
               "command %zu: Liverpool ran %#llx (opcode %#x, %zu words, depth %d), the stream has %#llx "
               "(opcode %#x, %u words, depth %u); the rest of the buffer is not compared",
               m_i_next - 1, static_cast<unsigned long long>(va_packet), static_cast<u32>(header.Opcode()),
               packet.size(), depth, static_cast<unsigned long long>(command.va_packet),
               static_cast<u32>(command.op), command.c_words, u32(command.depth));
        return;
    }
    const auto writes = std::span{m_p_stream->writes}.subspan(command.i_first_write, command.c_writes);
    for (const auto& write : writes) {
        const auto expected = std::span{m_p_stream->values}.subspan(write.off_value, write.range.m_c_registers);
        const auto actual = registers.subspan(write.range.m_i_first, write.range.m_c_registers);
        if (const auto [p_expected, p_actual] = std::ranges::mismatch(expected, actual);
            p_expected != expected.end()) {
            Report(Mismatch::Registers, "register %#x after %#llx (opcode %#x): stream %#x, Liverpool %#x",
                   write.range.m_i_first + static_cast<u32>(p_expected - expected.begin()),
                   static_cast<unsigned long long>(va_packet), static_cast<u32>(command.op), *p_expected,
                   *p_actual);
        }
    }
    if (command.kind == CommandKind::Packet) {
        return;
    }
    const char* sz_kind = command.kind == CommandKind::Draw ? "draw" : "dispatch";
    if (command.kind == CommandKind::Dispatch) {
        Bump(m_c_dispatches);
    } else if (m_is_draw_state_unknown) {
        Bump(m_c_draws);
        Bump(m_c_unchecked_draws);
    } else {
        Bump(m_c_draws);
        for (std::size_t i = 0; i < DrawRegisters.size(); ++i) {
            if (command.draw_registers[i] != registers[DrawRegisters[i]]) {
                Report(Mismatch::DrawState, "draw at %#llx (opcode %#x): register %#x: stream %#x, Liverpool %#x",
                       static_cast<unsigned long long>(va_packet), static_cast<u32>(command.op), DrawRegisters[i],
                       command.draw_registers[i], registers[DrawRegisters[i]]);
            }
        }
    }
    if (HashWords(packet.subspan(1)) != command.parameters_hash) {
        Report(Mismatch::Parameters, "%s at %#llx (opcode %#x): its packet changed between recording and execution",
               sz_kind, static_cast<unsigned long long>(va_packet), static_cast<u32>(command.op));
    }
    if (command.is_indirect && command.va_indirect_base != va_indirect_base) {
        Report(Mismatch::IndirectBase, "%s at %#llx (opcode %#x): SET_BASE %#llx in the stream, %#llx in Liverpool",
               sz_kind, static_cast<unsigned long long>(va_packet), static_cast<u32>(command.op),
               static_cast<unsigned long long>(command.va_indirect_base),
               static_cast<unsigned long long>(va_indirect_base));
    }
}

void StreamCheck::OnConditionalSkip(u32 exec_count) {
    if (!m_p_stream || m_is_diverged || m_i_next == 0) {
        return;
    }
    Bump(m_c_skips);
    const auto& commands = m_p_stream->commands;
    const Command& condition = commands[m_i_next - 1];
    const u64 va_end = condition.va_packet + (u64(condition.c_words) + exec_count) * sizeof(u32);
    bool has_writes = false;
    for (; m_i_next < commands.size(); ++m_i_next) {
        const Command& command = commands[m_i_next];
        if (command.depth < condition.depth || (command.depth == condition.depth && command.va_packet >= va_end)) {
            break;
        }
        has_writes |= command.c_writes != 0 || command.op == Op::ClearState;
    }
    if (has_writes) {
        Bump(m_c_skips_with_writes);
        m_is_draw_state_unknown = true;
    }
}

bool StreamCheck::End() {
    if (!m_p_stream) {
        return false;
    }
    const auto& commands = m_p_stream->commands;
    if (!m_is_diverged && m_i_next < commands.size()) {
        const Command& command = commands[m_i_next];
        Report(Mismatch::Extra, "%zu stream commands Liverpool did not run, the first %#llx (opcode %#x, depth %u)",
               commands.size() - m_i_next, static_cast<unsigned long long>(command.va_packet),
               static_cast<u32>(command.op), u32(command.depth));
    }
    if (m_p_stream->sz_bad) {
        Bump(m_c_bad);
    }
    Bump(m_c_buffers);
    if (!m_is_mismatched) {
        Bump(m_c_matched);
    }
    const bool should_resync = m_is_mismatched || m_is_draw_state_unknown;
    if (should_resync) {
        Bump(m_c_resyncs);
    }
    m_p_stream = nullptr;
    return should_resync;
}

void StreamCheck::Report(Mismatch mismatch, const char* sz_format, ...) {
    m_is_mismatched = true;
    auto& counter = m_c_mismatches[static_cast<std::size_t>(mismatch)];
    Bump(counter);
    const char* sz_name = s_sz_mismatch_names[static_cast<std::size_t>(mismatch)];
    if (m_c_lines < MaxDetailedLines) {
        ++m_c_lines;
        char sz_detail[384];
        va_list args;
        va_start(args, sz_format);
        std::vsnprintf(sz_detail, sizeof(sz_detail), sz_format, args);
        va_end(args);
        std::printf("CP translator: buffer %llu: %s mismatch: %s\n", Load(m_c_buffers) + 1, sz_name, sz_detail);
        if (m_c_lines == MaxDetailedLines) {
            std::printf("CP translator: further mismatches are counted, one line each time a kind's count "
                        "doubles\n");
        }
    } else if (const u64 c_mismatches = Load(counter); std::has_single_bit(c_mismatches)) {
        std::printf("CP translator: %llu %s mismatches\n", static_cast<unsigned long long>(c_mismatches), sz_name);
    }
}

void StreamCheck::FormatSummary(char* sz_out, std::size_t cb_out) const {
    const auto mismatches = [&](Mismatch mismatch) {
        return Load(m_c_mismatches[static_cast<std::size_t>(mismatch)]);
    };
    std::snprintf(sz_out, cb_out,
                  "CP translator: summary: %llu buffers, %llu matched; %llu commands, %llu draws (%llu not "
                  "checked after a skipped COND_EXEC that wrote registers), %llu dispatches; mismatches: "
                  "sequence %llu, missing %llu, extra %llu, registers %llu, draw state %llu, parameters %llu, "
                  "indirect base %llu; %llu COND_EXEC regions skipped (%llu wrote registers), %llu register "
                  "resyncs, %llu buffers recorded only in part\n",
                  Load(m_c_buffers), Load(m_c_matched), Load(m_c_commands), Load(m_c_draws),
                  Load(m_c_unchecked_draws), Load(m_c_dispatches), mismatches(Mismatch::Sequence),
                  mismatches(Mismatch::Missing), mismatches(Mismatch::Extra), mismatches(Mismatch::Registers),
                  mismatches(Mismatch::DrawState), mismatches(Mismatch::Parameters),
                  mismatches(Mismatch::IndirectBase), Load(m_c_skips), Load(m_c_skips_with_writes),
                  Load(m_c_resyncs), Load(m_c_bad));
}

} // namespace PsCp
