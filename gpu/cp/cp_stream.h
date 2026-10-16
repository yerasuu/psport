// SPDX-License-Identifier: GPL-2.0-or-later
// psport cp: the translator's command stream (phase P3 of the native GPU plan, decision #361).
//
// Stage A decodes a submitted graphics command buffer once into a flat list of typed commands, the
// packets of its indirect buffers inline, in the order the command processor runs them; an
// executor walks that list instead of switching over PM4 packets. COND_EXEC is not decided while
// recording: its condition is memory the GPU may still have to write, so every packet after it is
// recorded and the executor decides. StreamCheck compares the stream with the packets an executor
// actually ran (the shadow check while Liverpool still executes).
#pragma once

#include <atomic>
#include <cstddef>

#include "cp_decoder.h"

namespace PsCp {

/// The registers a draw takes from the register file besides its shaders' state: their values at
/// each draw are part of its command.
inline constexpr std::array<u32, 7> DrawRegisters{Reg::VgtDrawInitiator, Reg::VgtNumIndices, Reg::VgtNumInstances,
                                                  Reg::VgtDmaIndexType,  Reg::VgtDmaBase,    Reg::VgtDmaBaseHi,
                                                  Reg::VgtDmaMaxSize};

enum class CommandKind : std::uint8_t { Packet, Draw, Dispatch };

struct Command {
    u64 va_packet;
    u64 va_indirect_base; ///< indirect draws and dispatches: the SET_BASE address their arguments are at
    u64 parameters_hash;  ///< draws and dispatches: their packet's body as decoded
    std::array<u32, DrawRegisters.size()> draw_registers;
    u32 c_words; ///< header included
    u32 i_first_write; ///< its register writes: CommandStream::writes[i_first_write, + c_writes)
    u32 c_writes;
    Op op;
    std::uint8_t depth;
    CommandKind kind;
    bool is_indirect;
};

struct StreamWrite {
    RegisterWrite range;
    u32 off_value; ///< the values written: CommandStream::values[off_value, + range.m_c_registers)
};

struct CommandStream {
    std::vector<Command> commands;
    std::vector<StreamWrite> writes;
    std::vector<u32> values;
    /// Why recording stopped before the end of the buffer, or null.
    const char* sz_bad = nullptr;
};

inline u64 HashWords(std::span<const u32> words) {
    u64 hash = 0xCBF29CE484222325ull;
    for (const u32 word : words) {
        hash = (hash ^ word) * 0x100000001B3ull;
    }
    return hash;
}

/// Stage A's sink: records what its decoder finds into a CommandStream. The game's memory is
/// mapped at its own addresses, so indirect buffers are read in place.
class StreamRecorder final : public Sink, public RegisterObserver {
public:
    /// Becomes `decoder`'s register observer; the decoder must outlive the recorder.
    explicit StreamRecorder(Decoder& decoder);

    /// `va_indirect_base`: the SET_BASE address in force where the buffer starts.
    void Record(std::span<const u32> buffer, u64 va_indirect_base, CommandStream& out_stream);

    void OnPacket(const Packet& packet) override;
    void OnDraw(const Draw& draw) override;
    void OnDispatch(const Dispatch& dispatch) override;
    std::span<const u32> ResolveIndirectBuffer(u64 address, u32 dwords) override;
    void OnBadPacket(std::span<const u32> rest, const char* what, int depth) override;
    void OnRegisterPacket(std::span<const u32> words, std::span<const RegisterWrite> writes,
                          bool is_clear_state) override;

private:
    Decoder& m_decoder;
    CommandStream* m_p_stream = nullptr;
    u32 m_i_pending_write = 0; ///< writes the observer got before the packet's OnPacket
    u64 m_va_indirect_base = 0;
};

enum class Mismatch : std::uint8_t {
    Sequence,     ///< the executor ran another packet than the stream's next
    Missing,      ///< the executor ran a packet past the end of the stream
    Extra,        ///< stream commands the executor never ran
    Registers,    ///< a register the packet wrote holds another value in the executor's file
    DrawState,    ///< a DrawRegisters value differs at a draw
    Parameters,   ///< a draw's or dispatch's packet changed between recording and execution
    IndirectBase, ///< an indirect draw or dispatch reads its arguments from another SET_BASE
    Count,
};

/// Compares a stream with what an executor runs, one buffer at a time, on the executor's thread.
/// Mismatches are logged (the first ones in full, then counts) and summed up by FormatSummary.
class StreamCheck {
public:
    /// `stream` stays unchanged until End.
    void Begin(const CommandStream& stream);
    /// The executor ran `packet` (header first) at indirect buffer `depth`, its register writes
    /// already in `registers` (the whole file, by word index).
    void OnExecuted(std::span<const u32> packet, int depth, std::span<const u32> registers,
                    u64 va_indirect_base);
    /// The executor skipped the `exec_count` dwords after the COND_EXEC it ran last.
    void OnConditionalSkip(u32 exec_count);
    /// True when the recording decoder's registers must be taken from the executor again: a
    /// mismatch, or a skipped COND_EXEC region that wrote registers (recorded, but not run).
    bool End();
    /// One line, newline included; reads only counters (it may run from a signal handler).
    void FormatSummary(char* sz_out, std::size_t cb_out) const;

private:
    [[gnu::format(printf, 3, 4)]] void Report(Mismatch mismatch, const char* sz_format, ...);

    const CommandStream* m_p_stream = nullptr;
    std::size_t m_i_next = 0;
    bool m_is_diverged = false;
    bool m_is_mismatched = false;
    /// A skipped COND_EXEC region wrote registers: the stream's draw state no longer holds.
    bool m_is_draw_state_unknown = false;
    u64 m_c_lines = 0;
    std::array<std::atomic<u64>, static_cast<std::size_t>(Mismatch::Count)> m_c_mismatches{};
    std::atomic<u64> m_c_buffers{0}, m_c_matched{0}, m_c_commands{0}, m_c_draws{0}, m_c_dispatches{0};
    std::atomic<u64> m_c_skips{0}, m_c_skips_with_writes{0}, m_c_unchecked_draws{0}, m_c_resyncs{0},
        m_c_bad{0};
};

} // namespace PsCp
