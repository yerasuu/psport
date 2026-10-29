// SPDX-License-Identifier: GPL-2.0-or-later
#include "cp_decoder.h"

namespace PsCp {

bool Decoder::ApplyRegisters(const Packet& packet) {
    const auto& body = packet.m_body;
    std::array<RegisterWrite, 5> writes;
    u32 c_writes = 0;
    const auto noted = [&](u32 i_register, u32 c_registers) {
        if (c_registers != 0) {
            writes[c_writes++] = {i_register, c_registers};
        }
    };
    const auto write = [&](u32 i_register, u32 value) { noted(i_register, m_registers.Write(i_register, value)); };
    const auto set = [&](u32 bank) {
        if (body.empty()) {
            return;
        }
        const u32 i_register = bank + (body[0] & 0xFFFF);
        noted(i_register, m_registers.Write(i_register, body.subspan(1)));
    };
    switch (packet.Opcode()) {
    case Op::ClearState:
        m_registers.ClearState();
        break;
    case Op::SetConfigReg:
        set(Bank::Config);
        break;
    case Op::SetContextReg:
        set(Bank::Context);
        break;
    case Op::SetShReg: {
        const u32 off_register = packet.Word(0) & 0xFFFF;
        const u32 first = ComputeShFirst - Bank::Sh;
        if (!(m_options.is_compute_sh_to_owner && off_register >= first &&
              off_register <= first + m_options.compute_words)) {
            set(Bank::Sh);
        }
        break;
    }
    case Op::SetUconfigReg:
        set(Bank::Uconfig);
        break;
    case Op::IndexType:
        write(Reg::VgtDmaIndexType, packet.Word(0));
        break;
    case Op::DrawIndex2:
        // max size, base low, base high (8 bits), index count, draw initiator.
        write(Reg::VgtDmaMaxSize, packet.Word(0));
        write(Reg::VgtDmaBase, packet.Word(1));
        noted(Reg::VgtDmaBaseHi, m_registers.WriteBits(Reg::VgtDmaBaseHi, packet.Word(2), 0xFF));
        write(Reg::VgtNumIndices, packet.Word(3));
        write(Reg::VgtDrawInitiator, packet.Word(4));
        break;
    case Op::DrawIndexOffset2:
        // max size, index offset, index count, draw initiator.
        write(Reg::VgtDmaMaxSize, packet.Word(0));
        write(Reg::VgtNumIndices, packet.Word(2));
        write(Reg::VgtDrawInitiator, packet.Word(3));
        break;
    case Op::DrawIndexAuto:
        write(Reg::VgtNumIndices, packet.Word(0));
        write(Reg::VgtDrawInitiator, packet.Word(1));
        break;
    case Op::NumInstances:
        write(Reg::VgtNumInstances, packet.Word(0));
        break;
    case Op::IndexBase:
        write(Reg::VgtDmaBase, packet.Word(0));
        noted(Reg::VgtDmaBaseHi, m_registers.WriteBits(Reg::VgtDmaBaseHi, packet.Word(1), 0xFF));
        break;
    case Op::IndexBufferSize:
        write(Reg::VgtDmaMaxSize, packet.Word(0));
        break;
    case Op::EventWrite:
        // A streamout flush: the offsets are updated at once (no streamout here).
        if ((packet.Word(0) & 0x3F) != Event::SoVgtStreamoutFlush) {
            return false;
        }
        noted(Reg::CpStrmoutCntl, m_registers.WriteBits(Reg::CpStrmoutCntl, 1, 1));
        break;
    default:
        return false;
    }
    if (m_p_observer) {
        m_p_observer->OnRegisterPacket({packet.m_body.data() - 1, packet.m_body.size() + 1},
                                   {writes.data(), c_writes}, packet.Opcode() == Op::ClearState);
    }
    return true;
}

Cursor::Cursor(Decoder& decoder_, std::span<const u32> buffer, Sink& sink_, int depth,
               bool descend_)
    : m_decoder{decoder_}, m_sink{sink_}, m_should_descend{descend_} {
    m_levels.push_back({buffer, depth});
}

const Packet* Cursor::Next() {
    auto& stats = m_decoder.m_stats;
    if (m_is_pending) {
        m_is_pending = false;
        const int depth = m_levels.back().depth + 1;
        if (m_is_chain_pending) {
            // A chained buffer replaces the one that chained it: the CP never returns to its rest.
            m_levels.back().rest = m_sink.ResolveIndirectBuffer(m_va_pending, m_pending_dwords);
        } else if (depth >= MaxDepth) {
            ++stats.bad;
            m_sink.OnBadPacket({}, "indirect buffers nested too deep", depth);
        } else if (const auto nested = m_sink.ResolveIndirectBuffer(m_va_pending, m_pending_dwords);
                   !nested.empty()) {
            m_levels.push_back({nested, depth});
        }
    }
    while (!m_levels.empty()) {
        auto& level = m_levels.back();
        if (level.rest.empty()) {
            m_levels.pop_back();
            continue;
        }
        const Header header{level.rest[0]};
        if (header.Type() == 2) { // filler
            ++stats.filler;
            level.rest = level.rest.subspan(1);
            continue;
        }
        if (header.Type() != 3) {
            ++stats.bad;
            m_sink.OnBadPacket(level.rest, header.Type() == 0 ? "type-0 packet" : "type-1 packet",
                             level.depth);
            m_levels.clear();
            return nullptr;
        }
        const u32 words = header.BodyWords();
        if (words + 1 > level.rest.size()) {
            ++stats.bad;
            m_sink.OnBadPacket(level.rest, "packet past the end of its buffer", level.depth);
            m_levels.clear();
            return nullptr;
        }
        m_current = {header, level.rest.subspan(1, words), level.depth};
        const Packet& packet = m_current;
        level.rest = level.rest.subspan(words + 1);
        ++stats.packets;
        ++stats.by_opcode[static_cast<u32>(packet.Opcode())];
        if (m_decoder.ApplyRegisters(packet)) {
            ++stats.register_packets;
        }
        if (packet.Opcode() == Op::IndirectBuffer) {
            ++stats.indirect_buffers;
            if (m_should_descend) {
                m_is_pending = true;
                m_va_pending = packet.Word(0) | u64(packet.Word(1) & 0xFFFF) << 32;
                m_pending_dwords = packet.Word(2) & 0xFFFFF;
                m_is_chain_pending = (packet.Word(2) >> 20 & 1) != 0;
            }
        }
        return &m_current;
    }
    return nullptr;
}

void Cursor::Skip(u32 dwords) {
    if (m_levels.empty()) {
        return;
    }
    auto& rest = m_levels.back().rest;
    rest = rest.subspan(std::min<size_t>(dwords, rest.size()));
}

void Decoder::Decode(std::span<const u32> buffer, Sink& sink, int depth) {
    Cursor cursor{*this, buffer, sink, depth, true};
    while (const Packet* p_packet = cursor.Next()) {
        sink.OnPacket(*p_packet);
        switch (p_packet->Opcode()) {
        case Op::DrawIndex2:
            ++m_stats.draws;
            sink.OnDraw({DrawKind::Index2, p_packet->Word(3), 0, p_packet->Word(4), p_packet});
            break;
        case Op::DrawIndexOffset2:
            ++m_stats.draws;
            sink.OnDraw({DrawKind::IndexOffset2, p_packet->Word(2), p_packet->Word(1),
                         p_packet->Word(3), p_packet});
            break;
        case Op::DrawIndexAuto:
            ++m_stats.draws;
            sink.OnDraw({DrawKind::IndexAuto, p_packet->Word(0), 0, p_packet->Word(1), p_packet});
            break;
        case Op::DrawIndirect:
        case Op::DrawIndexIndirect:
        case Op::DrawIndirectMulti:
        case Op::DrawIndexIndirectMulti:
        case Op::DrawIndirectCountMulti:
        case Op::DrawIndexIndirectCountMulti: {
            static constexpr auto s_kind = [](Op op) {
                switch (op) {
                case Op::DrawIndirect:
                    return DrawKind::Indirect;
                case Op::DrawIndexIndirect:
                    return DrawKind::IndexIndirect;
                case Op::DrawIndirectMulti:
                    return DrawKind::IndirectMulti;
                case Op::DrawIndexIndirectMulti:
                    return DrawKind::IndexIndirectMulti;
                case Op::DrawIndirectCountMulti:
                    return DrawKind::IndirectCountMulti;
                default:
                    return DrawKind::IndexIndirectCountMulti;
                }
            };
            ++m_stats.draws;
            // The draw initiator is the last dword of each of these.
            sink.OnDraw({s_kind(p_packet->Opcode()), 0, 0, p_packet->m_body.back(), p_packet});
            break;
        }
        case Op::DispatchDirect:
            ++m_stats.dispatches;
            sink.OnDispatch({false, p_packet->Word(0), p_packet->Word(1), p_packet->Word(2), 0,
                             p_packet->Word(3), p_packet});
            break;
        case Op::DispatchIndirect:
            ++m_stats.dispatches;
            sink.OnDispatch({true, 0, 0, 0, p_packet->Word(0), p_packet->Word(1), p_packet});
            break;
        case Op::CondExec: {
            const u64 address = (p_packet->Word(0) & ~3u) | u64(p_packet->Word(1) & 0xFFFF) << 32;
            cursor.Skip(sink.ConditionalSkip(address, p_packet->Word(2) & 0x3FFF, *p_packet));
            break;
        }
        default:
            break;
        }
    }
}

} // namespace PsCp
