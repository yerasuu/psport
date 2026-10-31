// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>
#include <boost/container/small_vector.hpp>
#include <boost/intrusive/set.hpp>

#include "common/object_pool.h"
#include "common/types.h"
#include "shader_recompiler/frontend/instruction.h"
#include "shader_recompiler/ir/condition.h"
#include "shader_recompiler/translation_error.h"

namespace Shader::IR {
class Block;
}

namespace Shader::Gcn {

using Hook =
    boost::intrusive::set_base_hook<boost::intrusive::link_mode<boost::intrusive::normal_link>>;

enum class EndClass {
    Branch, ///< Block ends with a (un)conditional branch.
    Exit,   ///< Block ends with an exit instruction.
};

/// A block represents a linear range of instructions.
struct Block : Hook {
    [[nodiscard]] bool Contains(u32 pc) const noexcept;

    // psport: a jump table's case blocks begin where the block ending in its S_SETPC_B64 ends,
    // and sort before the block that begins there, in chain order: the structurizer lays the
    // blocks out in this order, and tests placed apart from the switch made it move every case
    // branch across the enclosing loops.
    bool operator<(const Block& rhs) const noexcept {
        return std::tuple(begin, !is_jump_table_case, i_case) <
               std::tuple(rhs.begin, !rhs.is_jump_table_case, rhs.i_case);
    }

    u32 begin;
    u32 end;
    u32 begin_index;
    u32 end_index;
    u32 num_predecessors{};
    IR::Condition cond{};
    GcnInst end_inst{};
    EndClass end_class{};
    Block* branch_true{};
    Block* branch_false{};
    IR::Block* ir_block{};
    bool is_dummy{};
    // psport: one test of a jump table's compare chain, with no instructions: it branches to
    // branch_true when the SGPR holds case_value, the low dword of that case's table entry.
    bool is_jump_table_case{};
    u32 i_case{};
    u32 i_sgpr_case{};
    u32 case_value{};
};

/// psport: a `switch` the compiler lowered to a table of 64-bit offsets read by
/// s_load_dwordx2 from data appended after the OrbShdr footer, then taken by S_SETPC_B64:
///   s_min_u32  sR, sIdx, N
///   s_lshl_b32 sR, sR, 3
///   s_add_u32  sR, sR, <first entry>     (optional)
///   s_getpc_b64 s[T:T+1]; s_add_u32 sT, <lit>, sT; s_addc_u32 sT+1, 0, sT+1
///   s_load_dwordx2 s[X:X+1], s[T:T+1], sR
///   s_getpc_b64 s[B:B+1]; s_add_u32 sB, sB, sX; s_addc_u32 sB+1, sB+1, sX+1
///   s_setpc_b64 s[B:B+1]
/// sR only takes off_first_entry + 8 * k for k <= N, so the table's N + 1 entries are every
/// value s[X:X+1] can load, and entry N is also where an index past N goes.
struct JumpTable {
    u32 i_first;
    u32 i_setpc;
    u32 i_load;
    u32 off_load;
    u32 i_sgpr_offset;
    u32 i_sgpr_entry;
    u32 off_first_entry;
    std::vector<u64> entries;
    std::vector<u32> off_targets;
};

class CFG {
    using Label = u32;
    using BlockList = boost::intrusive::set<Block>;
    using iterator = BlockList::iterator;

public:
    /// code must be the shader in guest memory: jump tables are read from the data the compiler
    /// appends after it, which the hardware reads from the same place.
    explicit CFG(Common::ObjectPool<Block>& block_pool, std::span<const GcnInst> inst_list,
                 std::span<const u32> code);

    [[nodiscard]] iterator begin() {
        return blocks.begin();
    }
    [[nodiscard]] iterator end() {
        return blocks.end();
    }

    [[nodiscard]] std::string Dot() const;

private:
    void EmitLabels();
    void EmitBlocks();
    void LinkBlocks();
    void SplitDivergenceScopes();
    void RemoveUnreachableBlocks();
    [[nodiscard]] JumpTable ResolveJumpTable(u32 i_setpc) const;
    void CheckNoBranchInto(const JumpTable& table) const;
    void LinkJumpTable(Block& block, const JumpTable& table);

    void AddLabel(Label address) {
        const auto it = std::ranges::find(labels, address);
        if (it == labels.end()) {
            labels.push_back(address);
        }
    }

    size_t GetIndex(Label label) {
        if (label == 0) {
            return 0ULL;
        }
        const auto it_index = std::ranges::lower_bound(index_to_pc, label);
        SHADER_ASSERT(it_index != index_to_pc.end() || label > index_to_pc.back());
        return std::distance(index_to_pc.begin(), it_index);
    }

public:
    Common::ObjectPool<Block>& block_pool;
    std::span<const GcnInst> inst_list;
    std::vector<u32> index_to_pc;
    boost::container::small_vector<Label, 16> labels;
    BlockList blocks;
    std::span<const u32> m_code;
    // psport: the target of each S_SETPC_B64 with a pc-relative literal, by instruction index.
    std::vector<std::pair<u32, u32>> m_setpc_targets;
    std::vector<JumpTable> m_jump_tables;
};

} // namespace Shader::Gcn
