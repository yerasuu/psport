// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstring>
#include "common/logging/log.h"
#include "shader_recompiler/frontend/control_flow_graph.h"
#include "shader_recompiler/ir/condition.h"
#include "shader_recompiler/translation_error.h"

namespace Shader::Gcn {

// psport: an address finds the instruction block beginning there, never a jump table's case
// block, which sorts before it.
struct Compare {
    bool operator()(const Block& lhs, u32 rhs) const noexcept {
        return lhs.begin < rhs || (lhs.begin == rhs && lhs.is_jump_table_case);
    }

    bool operator()(u32 lhs, const Block& rhs) const noexcept {
        return lhs < rhs.begin;
    }

    bool operator()(const Block& lhs, const Block& rhs) const noexcept {
        return lhs < rhs;
    }
};

static IR::Condition MakeCondition(const GcnInst& inst) {
    switch (inst.opcode) {
    case Opcode::S_CBRANCH_SCC0:
        return IR::Condition::Scc0;
    case Opcode::S_CBRANCH_SCC1:
        return IR::Condition::Scc1;
    case Opcode::S_CBRANCH_VCCZ:
        return IR::Condition::Vccz;
    case Opcode::S_CBRANCH_VCCNZ:
        return IR::Condition::Vccnz;
    case Opcode::S_CBRANCH_EXECZ:
        return IR::Condition::Execz;
    case Opcode::S_CBRANCH_EXECNZ:
        return IR::Condition::Execnz;
    default:
        return IR::Condition::True;
    }
}

static bool IgnoresExecMask(const GcnInst& inst) {
    // EXEC mask does not affect scalar instructions or branches.
    switch (inst.category) {
    case InstCategory::ScalarALU:
    case InstCategory::ScalarMemory:
    case InstCategory::FlowControl:
        return true;
    default:
        break;
    }
    // Read/Write Lane instructions are not affected either.
    switch (inst.opcode) {
    case Opcode::V_READLANE_B32:
    case Opcode::V_WRITELANE_B32:
    case Opcode::V_READFIRSTLANE_B32:
        return true;
    default:
        break;
    }
    // psport: an export with VM set takes EXEC as the pixels' valid mask, so it runs in every lane;
    // wrapped in the scope of an `s_mov_b64 exec, 0` kill, its discard of the lanes outside EXEC
    // never ran and the killed pixels were written with undefined colour
    return inst.category == InstCategory::Export && inst.control.exp.vm;
}

static std::optional<u32> ResolveSetPcTarget(std::span<const GcnInst> list, u32 setpc_index,
                                             std::span<const u32> pc_map) {
    if (setpc_index < 3) {
        return std::nullopt;
    }

    const auto& getpc = list[setpc_index - 3];
    const auto& arith = list[setpc_index - 2];
    const auto& setpc = list[setpc_index];

    if (getpc.opcode != Opcode::S_GETPC_B64 ||
        !(arith.opcode == Opcode::S_ADD_U32 || arith.opcode == Opcode::S_SUB_U32) ||
        setpc.opcode != Opcode::S_SETPC_B64)
        return std::nullopt;

    if (getpc.dst[0].code != setpc.src[0].code || arith.dst[0].code != setpc.src[0].code)
        return std::nullopt;

    if (arith.src_count < 2 || arith.src[1].field != OperandField::LiteralConst)
        return std::nullopt;

    const u32 imm = arith.src[1].code;

    const s32 signed_offset =
        (arith.opcode == Opcode::S_ADD_U32) ? static_cast<s32>(imm) : -static_cast<s32>(imm);

    const u32 base_pc = pc_map[setpc_index - 3] + getpc.length;

    const u32 result_pc = static_cast<u32>(static_cast<s32>(base_pc) + signed_offset);
    LOG_DEBUG(Render_Recompiler, "SetPC target: {} + {} = {}", base_pc, signed_offset, result_pc);
    return result_pc & ~0x3u;
}

// psport: a jump table lies in the data the compiler appends after the code and its OrbShdr
// footer; a table that would end further away is taken for a pattern read wrong.
static constexpr u64 CB_DATA_AFTER_CODE_MAX = 0x10000;

static bool IsSgpr(const InstOperand& operand, u32 i_sgpr) {
    return operand.field == OperandField::ScalarGPR && operand.code == i_sgpr;
}

static std::optional<u32> GetConstant(const InstOperand& operand) {
    switch (operand.field) {
    case OperandField::ConstZero:
        return 0U;
    case OperandField::SignedConstIntPos:
        return operand.code - static_cast<u32>(OperandField::ConstZero);
    case OperandField::LiteralConst:
        return operand.code;
    default:
        return std::nullopt;
    }
}

static u32 CountSgprsWritten(const GcnInst& inst, const InstOperand& dst) {
    if (inst.category == InstCategory::ScalarMemory) {
        return inst.opcode == Opcode::S_MEMTIME ? 2U : std::max(u32{inst.control.smrd.count}, 1U);
    }
    switch (dst.type) {
    case ScalarType::Uint64:
    case ScalarType::Sint64:
    case ScalarType::Float64:
        return 2;
    default:
        return 1;
    }
}

// Both destination slots are checked whatever dst_count says, because VOPC and VOP3 compares
// keep their SGPR pair in dst[1].
static bool WritesSgpr(const GcnInst& inst, u32 i_sgpr) {
    return std::ranges::any_of(inst.dst, [&](const InstOperand& dst) {
        return dst.field == OperandField::ScalarGPR && i_sgpr >= dst.code &&
               i_sgpr < dst.code + CountSgprsWritten(inst, dst);
    });
}

static std::optional<u32> FindLastWriter(std::span<const GcnInst> list, u32 i_end, u32 i_sgpr) {
    for (u32 i = i_end; i-- > 0;) {
        if (WritesSgpr(list[i], i_sgpr)) {
            return i;
        }
    }
    return std::nullopt;
}

static std::optional<InstOperand> GetOtherSource(const GcnInst& inst, u32 i_sgpr) {
    if (IsSgpr(inst.src[0], i_sgpr)) {
        return inst.src[1];
    }
    if (IsSgpr(inst.src[1], i_sgpr)) {
        return inst.src[0];
    }
    return std::nullopt;
}

static constexpr size_t LabelReserveSize = 32;

CFG::CFG(Common::ObjectPool<Block>& block_pool_, std::span<const GcnInst> inst_list_,
         std::span<const u32> code_)
    : block_pool{block_pool_}, inst_list{inst_list_}, m_code{code_} {
    index_to_pc.resize(inst_list.size() + 1);
    labels.reserve(LabelReserveSize);
    EmitLabels();
    EmitBlocks();
    LinkBlocks();
    SplitDivergenceScopes();
    RemoveUnreachableBlocks();
}

void CFG::EmitLabels() {
    // psport: every pc first, so that a jump table's targets past it are checked against
    // instruction boundaries.
    u32 pc = 0;
    for (u32 i = 0; i < inst_list.size(); i++) {
        index_to_pc[i] = pc;
        pc += inst_list[i].length;
    }
    index_to_pc[inst_list.size()] = pc;

    // Always set a label at entry point.
    AddLabel(0);

    // Iterate instruction list and add labels to branch targets.
    for (u32 i = 0; i < inst_list.size(); i++) {
        pc = index_to_pc[i];
        const GcnInst inst = inst_list[i];
        if (inst.opcode == Opcode::S_SETPC_B64) {
            if (const auto target = ResolveSetPcTarget(inst_list, i, index_to_pc)) {
                m_setpc_targets.emplace_back(i, *target);
                AddLabel(*target);
            } else {
                const JumpTable& table = m_jump_tables.emplace_back(ResolveJumpTable(i));
                for (const u32 off_target : table.off_targets) {
                    AddLabel(off_target);
                }
            }
            AddLabel(pc + inst.length);
        } else if (inst.IsUnconditionalBranch()) {
            AddLabel(inst.BranchTarget(pc));
            // Emit this label so that the block ends with the branching instruction
            AddLabel(pc + inst.length);
        } else if (inst.IsConditionalBranch()) {
            const u32 true_label = inst.BranchTarget(pc);
            const u32 false_label = pc + inst.length;
            if (true_label != false_label) {
                AddLabel(true_label);
                AddLabel(false_label);
            }
        } else if (inst.opcode == Opcode::S_ENDPGM) {
            const u32 next_label = pc + inst.length;
            AddLabel(next_label);
        }
    }
    for (const JumpTable& table : m_jump_tables) {
        CheckNoBranchInto(table);
    }

    // Sort labels to make sure block insertion is correct.
    std::ranges::sort(labels);
}

JumpTable CFG::ResolveJumpTable(u32 i_setpc) const {
    const u32 off_setpc = index_to_pc[i_setpc];
    const auto require = [off_setpc](bool is_met, std::string_view why) {
        SHADER_ASSERT_MSG(is_met,
                          "S_SETPC_B64 at PC {:#x}: target neither a pc-relative literal nor a "
                          "jump table: {}",
                          off_setpc, why);
    };

    require(i_setpc >= 3, "too close to the program start");
    const GcnInst& setpc = inst_list[i_setpc];
    const GcnInst& addc = inst_list[i_setpc - 1];
    const GcnInst& add = inst_list[i_setpc - 2];
    const GcnInst& getpc = inst_list[i_setpc - 3];
    require(setpc.src[0].field == OperandField::ScalarGPR, "target not in SGPRs");
    const u32 i_sgpr_target = setpc.src[0].code;
    require(getpc.opcode == Opcode::S_GETPC_B64 && IsSgpr(getpc.dst[0], i_sgpr_target) &&
                add.opcode == Opcode::S_ADD_U32 && IsSgpr(add.dst[0], i_sgpr_target) &&
                addc.opcode == Opcode::S_ADDC_U32 && IsSgpr(addc.dst[0], i_sgpr_target + 1),
            "not built by s_getpc_b64, s_add_u32, s_addc_u32");
    const auto entry_lo = GetOtherSource(add, i_sgpr_target);
    const auto entry_hi = GetOtherSource(addc, i_sgpr_target + 1);
    require(entry_lo && entry_lo->field == OperandField::ScalarGPR && entry_hi &&
                IsSgpr(*entry_hi, entry_lo->code + 1),
            "offset not added from an SGPR pair");
    const u32 i_sgpr_entry = entry_lo->code;
    require(i_sgpr_entry + 1 < i_sgpr_target || i_sgpr_entry > i_sgpr_target + 1,
            "offset overlaps the target registers");

    const auto i_load = FindLastWriter(inst_list, i_setpc - 3, i_sgpr_entry);
    require(i_load && FindLastWriter(inst_list, i_setpc - 3, i_sgpr_entry + 1) == i_load,
            "offset not written by one instruction");
    const GcnInst& load = inst_list[*i_load];
    require(load.opcode == Opcode::S_LOAD_DWORDX2 && IsSgpr(load.dst[0], i_sgpr_entry) &&
                !load.control.smrd.imm && load.src[1].field == OperandField::ScalarGPR,
            "offset not loaded by s_load_dwordx2 at an SGPR offset");
    const u32 i_sgpr_table = load.src[0].code * 2;
    const u32 i_sgpr_offset = load.src[1].code;

    const auto i_table_add = FindLastWriter(inst_list, *i_load, i_sgpr_table);
    require(i_table_add && *i_table_add >= 1 &&
                FindLastWriter(inst_list, *i_load, i_sgpr_table + 1) == *i_table_add + 1,
            "table address not written by s_add_u32, s_addc_u32");
    const GcnInst& table_getpc = inst_list[*i_table_add - 1];
    const GcnInst& table_add = inst_list[*i_table_add];
    const GcnInst& table_addc = inst_list[*i_table_add + 1];
    const auto table_literal = GetOtherSource(table_add, i_sgpr_table);
    const auto table_carry = GetOtherSource(table_addc, i_sgpr_table + 1);
    require(table_getpc.opcode == Opcode::S_GETPC_B64 && IsSgpr(table_getpc.dst[0], i_sgpr_table) &&
                table_add.opcode == Opcode::S_ADD_U32 && IsSgpr(table_add.dst[0], i_sgpr_table) &&
                table_literal && table_literal->field == OperandField::LiteralConst &&
                table_addc.opcode == Opcode::S_ADDC_U32 &&
                IsSgpr(table_addc.dst[0], i_sgpr_table + 1) && table_carry &&
                table_carry->field == OperandField::ConstZero,
            "table address not built by s_getpc_b64, s_add_u32 <literal>, s_addc_u32 0");

    u32 off_first_entry = 0;
    auto i_shift = FindLastWriter(inst_list, *i_load, i_sgpr_offset);
    require(i_shift.has_value(), "table offset never written");
    if (inst_list[*i_shift].opcode == Opcode::S_ADD_U32) {
        const GcnInst& offset_add = inst_list[*i_shift];
        const auto first_entry = GetOtherSource(offset_add, i_sgpr_offset);
        const auto off_added = first_entry ? GetConstant(*first_entry) : std::nullopt;
        require(IsSgpr(offset_add.dst[0], i_sgpr_offset) && off_added,
                "table offset added from a non-constant");
        off_first_entry = *off_added;
        i_shift = FindLastWriter(inst_list, *i_shift, i_sgpr_offset);
        require(i_shift.has_value(), "table offset never shifted");
    }
    const GcnInst& shift = inst_list[*i_shift];
    require(shift.opcode == Opcode::S_LSHL_B32 && IsSgpr(shift.dst[0], i_sgpr_offset) &&
                shift.src[0].field == OperandField::ScalarGPR && GetConstant(shift.src[1]) == 3U,
            "table offset not an index shifted left by 3");
    const auto i_clamp = FindLastWriter(inst_list, *i_shift, shift.src[0].code);
    require(i_clamp.has_value(), "table index never written");
    const GcnInst& clamp = inst_list[*i_clamp];
    std::optional<u32> i_last_entry;
    if (clamp.opcode == Opcode::S_MIN_U32 && IsSgpr(clamp.dst[0], shift.src[0].code)) {
        i_last_entry = GetConstant(clamp.src[1]);
        if (!i_last_entry) {
            i_last_entry = GetConstant(clamp.src[0]);
        }
    }
    require(i_last_entry.has_value(), "table index not clamped by s_min_u32 with a constant");

    const u64 c_entries = u64{*i_last_entry} + 1;
    const u64 cb_table = c_entries * sizeof(u64);
    const u64 off_table = u64{index_to_pc[*i_table_add - 1]} + table_getpc.length +
                          table_literal->code + off_first_entry;
    require(u64{off_first_entry} + cb_table <= (u64{1} << 32), "table offset overflows");
    require(off_table % sizeof(u32) == 0 &&
                off_table + cb_table <= m_code.size_bytes() + CB_DATA_AFTER_CODE_MAX,
            "table outside the shader's code and appended data");

    JumpTable table{
        .i_first = std::min(*i_clamp, *i_table_add - 1),
        .i_setpc = i_setpc,
        .i_load = *i_load,
        .off_load = index_to_pc[*i_load],
        .i_sgpr_offset = i_sgpr_offset,
        .i_sgpr_entry = i_sgpr_entry,
        .off_first_entry = off_first_entry,
    };
    table.entries.reserve(c_entries);
    table.off_targets.reserve(c_entries);
    const auto* p_table = reinterpret_cast<const u8*>(m_code.data()) + off_table;
    const u64 off_after_getpc = u64{index_to_pc[i_setpc - 3]} + getpc.length;
    for (u64 i_entry = 0; i_entry < c_entries; ++i_entry) {
        u64 entry;
        std::memcpy(&entry, p_table + i_entry * sizeof(entry), sizeof(entry));
        const u64 off_target = off_after_getpc + entry;
        SHADER_ASSERT_MSG(off_target < index_to_pc.back() &&
                              std::ranges::binary_search(index_to_pc, static_cast<u32>(off_target)),
                          "S_SETPC_B64 at PC {:#x}: jump table entry {} of {} at {:#x} leads to "
                          "{:#x}, not an instruction of the shader",
                          off_setpc, i_entry, c_entries, off_table + i_entry * sizeof(entry),
                          off_target);
        table.entries.push_back(entry);
        table.off_targets.push_back(static_cast<u32>(off_target));
    }
    LOG_INFO(Render_Recompiler, "S_SETPC_B64 at PC {:#x}: jump table at {:#x}, {} entries",
             off_setpc, off_table, c_entries);
    return table;
}

// A branch into the middle of the pattern would reach the S_SETPC_B64 with a table offset the
// s_min_u32 did not clamp, so with a target the table does not list.
void CFG::CheckNoBranchInto(const JumpTable& table) const {
    const u32 off_first = index_to_pc[table.i_first];
    const u32 off_setpc = index_to_pc[table.i_setpc];
    for (const Label label : labels) {
        SHADER_ASSERT_MSG(label <= off_first || label > off_setpc,
                          "S_SETPC_B64 at PC {:#x}: jump table built from {:#x} with a branch "
                          "into it at {:#x}",
                          off_setpc, off_first, label);
    }
}

// psport: an EXEC set to a constant (s_mov_b64 exec, 1 so that one lane does the wave's atomic
// add) masks the vector instructions after it as a saved mask does; run by every lane, such an add
// counted 64 times and wrote past its counter, and a dispatch waiting on the counters never ended.
static bool SetsExecToConstant(const GcnInst& inst) {
    if (inst.opcode != Opcode::S_MOV_B64 || inst.dst[0].field != OperandField::ExecLo) {
        return false;
    }
    const InstOperand& src = inst.src[0];
    switch (src.field) {
    case OperandField::ConstZero:
    case OperandField::SignedConstIntPos:
    case OperandField::LiteralConst:
        return true;
    case OperandField::SignedConstIntNeg:
        return src.code != SignedConstIntNegMin;
    default:
        return false;
    }
}

void CFG::SplitDivergenceScopes() {
    const auto is_open_scope = [](const GcnInst& inst) {
        // An open scope instruction is an instruction that modifies EXEC
        // but also saves the previous value to restore later. This indicates
        // we are entering a scope.
        return inst.opcode == Opcode::S_AND_SAVEEXEC_B64 ||
               // While this instruction does not save EXEC it is often used paired
               // with SAVEEXEC to mask the threads that didn't pass the condition
               // of initial branch.
               (inst.opcode == Opcode::S_ANDN2_B64 && inst.dst[0].field == OperandField::ExecLo) ||
               inst.IsCmpx() || SetsExecToConstant(inst);
    };
    const auto is_close_scope = [](const GcnInst& inst) {
        // Closing an EXEC scope can be either a branch instruction
        // (typical case when S_AND_SAVEEXEC_B64 is right before a branch)
        // or by a move instruction to EXEC that restores the backup.
        return (inst.opcode == Opcode::S_MOV_B64 && inst.dst[0].field == OperandField::ExecLo) ||
               // Sometimes compiler might insert instructions between the SAVEEXEC and the branch.
               // Those instructions need to be wrapped in the condition as well so allow branch
               // as end scope instruction.
               inst.opcode == Opcode::S_CBRANCH_EXECZ || inst.opcode == Opcode::S_ENDPGM ||
               (inst.opcode == Opcode::S_ANDN2_B64 && inst.dst[0].field == OperandField::ExecLo);
    };

    for (auto blk = blocks.begin(); blk != blocks.end(); blk++) {
        if (blk->is_jump_table_case) {
            continue;
        }
        auto next_blk = std::next(blk);
        s32 curr_begin = -1;
        for (size_t index = blk->begin_index; index <= blk->end_index; index++) {
            const auto& inst = inst_list[index];
            const bool is_close = is_close_scope(inst);
            if ((is_close || index == blk->end_index) && curr_begin != -1) {
                // If there are no instructions inside scope don't do anything.
                if (index - curr_begin == 1 && is_close) {
                    curr_begin = is_open_scope(inst) ? static_cast<s32>(index) : -1;
                    continue;
                }
                // If all instructions in the scope ignore exec masking, we shouldn't insert a
                // scope.
                const auto start = inst_list.begin() + curr_begin + 1;
                if (!std::ranges::all_of(start, inst_list.begin() + index + !is_close,
                                         IgnoresExecMask)) {
                    // Determine the first instruction affected by the exec mask.
                    do {
                        ++curr_begin;
                    } while (IgnoresExecMask(inst_list[curr_begin]));

                    // Determine the last instruction affected by the exec mask.
                    s32 curr_end = index;
                    while (IgnoresExecMask(inst_list[curr_end])) {
                        --curr_end;
                    }

                    // Create a new block for the divergence scope.
                    Block* block = block_pool.Create();
                    block->begin = index_to_pc[curr_begin];
                    block->end = index_to_pc[curr_end];
                    block->begin_index = curr_begin;
                    block->end_index = curr_end;
                    block->end_inst = inst_list[curr_end];
                    block->num_predecessors = 1;
                    blocks.insert_before(next_blk, *block);

                    // If we are inside the parent block, make an epilogue block and jump to it.
                    if (curr_end != blk->end_index) {
                        Block* epi_block = block_pool.Create();
                        epi_block->begin = index_to_pc[curr_end + 1];
                        epi_block->end = blk->end;
                        epi_block->begin_index = curr_end + 1;
                        epi_block->end_index = blk->end_index;
                        epi_block->end_inst = blk->end_inst;
                        epi_block->cond = blk->cond;
                        epi_block->end_class = blk->end_class;
                        epi_block->branch_true = blk->branch_true;
                        epi_block->branch_false = blk->branch_false;
                        epi_block->num_predecessors = 2;
                        blocks.insert_before(next_blk, *epi_block);

                        // Have divergence block always jump to epilogue block.
                        block->cond = IR::Condition::True;
                        block->branch_true = epi_block;
                        block->branch_false = nullptr;

                        // If the parent block fails to enter divergence block make it jump to
                        // epilogue too
                        blk->branch_false = epi_block;
                    } else {
                        // No epilogue block is needed since the divergence block
                        // also ends the parent block. Inherit the end condition.
                        auto& parent_blk = *blk;
                        SHADER_ASSERT(blk->cond == IR::Condition::True && blk->branch_true);
                        block->cond = IR::Condition::True;
                        block->branch_true = blk->branch_true;
                        block->branch_false = nullptr;

                        // If the parent block didn't enter the divergence scope
                        // have it jump directly to the next one
                        blk->branch_false = blk->branch_true;
                        blk->branch_true->num_predecessors++;
                    }

                    // Shrink parent block to end right before curr_begin
                    // and make it jump to divergence block
                    --curr_begin;
                    blk->end = index_to_pc[curr_begin];
                    blk->end_index = curr_begin;
                    blk->end_inst = inst_list[curr_begin];
                    blk->cond = IR::Condition::Execnz;
                    blk->end_class = EndClass::Branch;
                    blk->branch_true = block;
                }
                // Reset scope begin.
                curr_begin = -1;
            }
            // Mark a potential start of an exec scope.
            if (is_open_scope(inst)) {
                curr_begin = index;
            }
        }
    }
}

void CFG::EmitBlocks() {
    for (auto it = labels.cbegin(); it != labels.cend(); ++it) {
        const Label start = *it;
        const auto next_it = std::next(it);
        const bool is_last = (next_it == labels.cend());
        if (is_last) {
            // Last label is special.
            return;
        }
        // The end label is the start instruction of next block.
        // The end instruction of this block is the previous one.
        const Label end = *next_it;
        const size_t end_index = GetIndex(end) - 1;
        const auto& end_inst = inst_list[end_index];

        // Insert block between the labels using the last instruction
        // as an indicator for branching type.
        Block* block = block_pool.Create();
        block->begin = start;
        block->end = end;
        block->begin_index = GetIndex(start);
        block->end_index = end_index;
        block->end_inst = end_inst;
        block->cond = MakeCondition(end_inst);
        blocks.insert(*block);
    }
}

void CFG::LinkBlocks() {
    const auto get_block = [this](u32 address) {
        auto it = blocks.find(address, Compare{});
        SHADER_ASSERT_MSG(it != blocks.cend() && it->begin == address, "no block at {:#x}",
                          address);
        return &*it;
    };

    // psport: the case blocks are inserted after the walk, since they join the set it iterates.
    std::vector<std::pair<Block*, const JumpTable*>> jump_table_blocks;
    for (auto it = blocks.begin(); it != blocks.end(); it++) {
        auto& block = *it;
        const auto end_inst{block.end_inst};

        // If the block doesn't end with a branch we simply
        // need to link with the next block.
        if (!end_inst.IsTerminateInstruction()) {
            auto* next_block = get_block(block.end);
            block.cond = IR::Condition::True;
            block.branch_true = next_block;
            block.end_class = EndClass::Branch;
            next_block->num_predecessors++;
            continue;
        }

        // Find the branch targets from the instruction and link the blocks.
        // Note: Block end address is one instruction after end_inst.
        const u32 branch_pc = block.end - end_inst.length;
        u32 target_pc = 0;
        if (end_inst.opcode == Opcode::S_SETPC_B64) {
            const auto it_target =
                std::ranges::find(m_setpc_targets, block.end_index, &std::pair<u32, u32>::first);
            if (it_target == m_setpc_targets.end()) {
                const auto it_table =
                    std::ranges::find(m_jump_tables, block.end_index, &JumpTable::i_setpc);
                SHADER_ASSERT_MSG(it_table != m_jump_tables.end(),
                                  "S_SETPC_B64 at PC {:#x} ends a block but was not resolved",
                                  branch_pc);
                jump_table_blocks.emplace_back(&block, &*it_table);
                continue;
            }
            target_pc = it_target->second;
        } else {
            target_pc = end_inst.BranchTarget(branch_pc);
        }

        if (end_inst.IsUnconditionalBranch()) {
            auto* target_block = get_block(target_pc);
            block.branch_true = target_block;
            block.end_class = EndClass::Branch;
            target_block->num_predecessors++;
        } else if (end_inst.IsConditionalBranch()) {
            auto* end_block = get_block(block.end);
            block.end_class = EndClass::Branch;
            if (target_pc != block.end) {
                auto* target_block = get_block(target_pc);
                block.branch_true = target_block;
                block.branch_false = end_block;
                target_block->num_predecessors++;
            } else {
                block.branch_true = end_block;
                block.cond = IR::Condition::True;
            }
            end_block->num_predecessors++;
        } else if (end_inst.opcode == Opcode::S_ENDPGM) {
            block.end_class = EndClass::Exit;
        } else {
            SHADER_UNREACHABLE();
        }
    }
    for (const auto& [p_block, p_table] : jump_table_blocks) {
        LinkJumpTable(*p_block, *p_table);
    }
}

// psport: the switch becomes a chain of two-way tests on the loaded entry, one per distinct
// target, ending in the last entry's target; the structurizer then handles it as any other
// branches, including inside a loop.
void CFG::LinkJumpTable(Block& block, const JumpTable& table) {
    const auto get_block = [this](u32 address) {
        auto it = blocks.find(address, Compare{});
        SHADER_ASSERT_MSG(it != blocks.cend() && it->begin == address, "no block at {:#x}",
                          address);
        return &*it;
    };

    block.cond = IR::Condition::True;
    block.end_class = EndClass::Branch;
    Block** pp_next_test = &block.branch_true;
    const u64 default_entry = table.entries.back();
    std::vector<u64> tested_entries;
    for (size_t i_entry = 0; i_entry + 1 < table.entries.size(); ++i_entry) {
        const u64 entry = table.entries[i_entry];
        if (entry == default_entry || std::ranges::contains(tested_entries, entry)) {
            continue;
        }
        tested_entries.push_back(entry);
        Block* p_case = block_pool.Create();
        p_case->begin = block.end;
        p_case->end = block.end;
        p_case->i_case = static_cast<u32>(tested_entries.size() - 1);
        p_case->begin_index = block.end_index;
        p_case->end_index = block.end_index;
        p_case->cond = IR::Condition::JumpTableCase;
        p_case->end_class = EndClass::Branch;
        p_case->is_jump_table_case = true;
        p_case->i_sgpr_case = table.i_sgpr_entry;
        p_case->case_value = static_cast<u32>(entry);
        p_case->num_predecessors = 1;
        p_case->branch_true = get_block(table.off_targets[i_entry]);
        p_case->branch_true->num_predecessors++;
        blocks.insert(*p_case);
        *pp_next_test = p_case;
        pp_next_test = &p_case->branch_false;
    }
    *pp_next_test = get_block(table.off_targets.back());
    (*pp_next_test)->num_predecessors++;
}

void CFG::RemoveUnreachableBlocks() {
    for (auto it = std::next(blocks.begin()); it != blocks.end();) {
        if (it->num_predecessors == 0) {
            LOG_WARNING(Render_Recompiler, "Removing unreachable block begin={:#x}", it->begin);
            it = blocks.erase(it);
        } else {
            it++;
        }
    }
}

std::string CFG::Dot() const {
    int node_uid{0};

    const auto name_of = [](const Block& block) {
        if (block.is_jump_table_case) {
            return fmt::format("\"{:#x}.case{}\"", block.begin, block.i_case);
        }
        return fmt::format("\"{:#x}\"", block.begin);
    };

    std::string dot{"digraph shader {\n"};
    dot += fmt::format("\tsubgraph cluster_{} {{\n", 0);
    dot += fmt::format("\t\tnode [style=filled];\n");
    for (const Block& block : blocks) {
        const std::string name{name_of(block)};
        const auto add_branch = [&](Block* branch, bool add_label) {
            dot += fmt::format("\t\t{}->{}", name, name_of(*branch));
            if (add_label && block.cond != IR::Condition::True &&
                block.cond != IR::Condition::False) {
                dot += fmt::format(" [label=\"{}\"]", block.cond);
            }
            dot += '\n';
        };
        dot += fmt::format("\t\t{};\n", name);
        switch (block.end_class) {
        case EndClass::Branch:
            if (block.cond != IR::Condition::False) {
                add_branch(block.branch_true, true);
            }
            if (block.cond != IR::Condition::True) {
                add_branch(block.branch_false, false);
            }
            break;
        case EndClass::Exit:
            dot += fmt::format("\t\t{}->N{};\n", name, node_uid);
            dot +=
                fmt::format("\t\tN{} [label=\"Exit\"][shape=square][style=stripped];\n", node_uid);
            ++node_uid;
            break;
        }
    }
    dot += "\t\tlabel = \"main\";\n\t}\n";
    if (blocks.empty()) {
        dot += "Start;\n";
    } else {
        dot += fmt::format("\tStart -> {};\n", name_of(*blocks.begin()));
    }
    dot += fmt::format("\tStart [shape=diamond];\n");
    dot += "}\n";
    return dot;
}

} // namespace Shader::Gcn
