// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2018 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include <algorithm>
#include <variant>

#include "dynarmic/frontend/A64/translate/a64_translate.h"

#include "dynarmic/frontend/A64/a64_location_descriptor.h"
#include "dynarmic/frontend/A64/decoder/a64.h"
#include "dynarmic/frontend/A64/translate/impl/impl.h"
#include "dynarmic/interface/A64/config.h"
#include "dynarmic/ir/basic_block.h"
#include "dynarmic/ir/terminal.h"

namespace Dynarmic::A64 {

namespace {

/// The same leaf, but never entered through a link, the return stack or fast dispatch: all
/// of those can skip the halt check that services a cache invalidation. A return to the
/// dispatcher cannot (the callers have already stored the target PC).
IR::Term::LeafTerminal ForceDispatch(const IR::Term::LeafTerminal& leaf) {
    if (const auto* link = std::get_if<IR::Term::LinkBlock>(&leaf)) {
        return IR::Term::LinkBlock{link->next, true};
    }
    if (const auto* fast = std::get_if<IR::Term::LinkBlockFast>(&leaf)) {
        return IR::Term::LinkBlock{fast->next, true};
    }
    if (std::holds_alternative<IR::Term::PopRSBHint>(leaf) || std::holds_alternative<IR::Term::FastDispatchHint>(leaf)) {
        return IR::Term::ReturnToDispatch{};
    }
    return leaf;
}

IR::Term::Terminal ForceDispatch(const IR::Term::Terminal& terminal) {
    if (const auto* leaf = std::get_if<IR::Term::LeafTerminal>(&terminal)) {
        return ForceDispatch(*leaf);
    }
    if (const auto* cond = std::get_if<IR::Term::If>(&terminal)) {
        return IR::Term::If{cond->if_, ForceDispatch(cond->then_), ForceDispatch(cond->else_)};
    }
    if (const auto* bit = std::get_if<IR::Term::CheckBit>(&terminal)) {
        return IR::Term::CheckBit{ForceDispatch(bit->then_), ForceDispatch(bit->else_)};
    }
    if (const auto* halt = std::get_if<IR::Term::CheckHalt>(&terminal)) {
        return IR::Term::CheckHalt{ForceDispatch(halt->else_)};
    }
    return terminal;
}

}  // namespace

void Translate(IR::Block& block, LocationDescriptor descriptor, MemoryReadCodeFuncType memory_read_code, TranslationOptions options) {
    const bool single_step = descriptor.SingleStepping();
    TranslatorVisitor visitor{block, descriptor, std::move(options)};
    const InstructionProbes* const probes = visitor.options.probes;

    const auto operand = [&](const InstructionProbe::Operand& op) -> IR::U64 {
        switch (op.kind) {
        case InstructionProbe::Arg::X0: return visitor.ir.GetX(Reg::R0);
        case InstructionProbe::Arg::X1: return visitor.ir.GetX(Reg::R1);
        case InstructionProbe::Arg::X30: return visitor.ir.GetX(Reg::R30);
        case InstructionProbe::Arg::SP: return visitor.ir.GetSP();
        case InstructionProbe::Arg::XReg:
            return op.imm < 31 ? visitor.ir.GetX(static_cast<Reg>(op.imm)) : visitor.ir.Imm64(0);
        default: return visitor.ir.Imm64(op.imm);
        }
    };

    bool should_continue = true;
    do {
        const u64 pc = visitor.ir.current_location->PC();
        const auto instruction = memory_read_code(pc);
        bool force_dispatch_after = false;
        if (probes) {
            const auto emit = [&](const InstructionProbe& probe) {
                visitor.ir.CallHostFunction(probe.fn, operand(probe.operands[0]), operand(probe.operands[1]),
                                            operand(probe.operands[2]));
                force_dispatch_after = force_dispatch_after || probe.end_block_after;
            };
            InstructionProbeSet set;
            const std::size_t count = std::min(probes->Find(pc, set), set.probes.size());
            for (std::size_t i = 0; i < count; ++i) {
                emit(set.probes[i]);
            }
            if (instruction) {
                InstructionProbe probe;
                const u32 instr = *instruction;
                // Byte offset of a conditional branch immediate, sign-extended from `bits`.
                const auto conditional_target = [&](u32 value, u32 bits) -> u64 {
                    const s32 signed_offset = static_cast<s32>(value << (32 - bits)) >> (32 - bits);
                    return pc + static_cast<u64>(static_cast<s64>(signed_offset) << 2);
                };
                if ((instr & 0x7C000000) == 0x14000000) {  // B, BL
                    const u64 offset = static_cast<u64>(static_cast<s64>(static_cast<s32>(instr << 6) >> 4));
                    if (probes->FindBranch(pc, BranchKind::Direct, pc + offset, probe)) {
                        emit(probe);
                    }
                } else if ((instr & 0xFFDFFC1F) == 0xD61F0000) {  // BR, BLR
                    if (probes->FindBranch(pc, BranchKind::Indirect, (instr >> 5) & 31, probe)) {
                        emit(probe);
                    }
                } else if ((instr & 0xFFFFFC1F) == 0xD65F0000) {  // RET
                    if (probes->FindBranch(pc, BranchKind::Return, (instr >> 5) & 31, probe)) {
                        emit(probe);
                    }
                } else if ((instr & 0xFEDFF800) == 0xD61F0800) {  // BRAA, BRAB, BLRAA, BLRAB and their Z forms
                    // The visitor rejects a no-modifier form whose modifier field is not 31: nothing runs then.
                    const bool runs = ((instr >> 24) & 1) != 0 || (instr & 31) == 31;
                    if (runs && probes->FindBranch(pc, BranchKind::Indirect, (instr >> 5) & 31, probe)) {
                        emit(probe);
                    }
                } else if ((instr & 0xFFFFFBFF) == 0xD65F0BFF) {  // RETAA, RETAB: the target is X30
                    if (probes->FindBranch(pc, BranchKind::Return, 30, probe)) {
                        emit(probe);
                    }
                } else if ((instr & 0xFF000010) == 0x54000000) {  // B.cond
                    const u64 target = conditional_target((instr >> 5) & 0x7FFFF, 19);
                    if (probes->FindBranch(pc, BranchKind::Conditional, target, probe)) {
                        emit(probe);
                    }
                } else if ((instr & 0x7E000000) == 0x34000000) {  // CBZ, CBNZ
                    const u64 target = conditional_target((instr >> 5) & 0x7FFFF, 19);
                    if (probes->FindBranch(pc, BranchKind::Conditional, target, probe)) {
                        emit(probe);
                    }
                } else if ((instr & 0x7E000000) == 0x36000000) {  // TBZ, TBNZ
                    const u64 target = conditional_target((instr >> 5) & 0x3FFF, 14);
                    if (probes->FindBranch(pc, BranchKind::Conditional, target, probe)) {
                        emit(probe);
                    }
                }
            }
        }
        if (instruction) {
            if (auto decoder = Decode<TranslatorVisitor, bool>(visitor, *instruction)) {
                should_continue = *decoder;
            } else {
                should_continue = visitor.RaiseException(Exception::UnallocatedEncoding);
            }
        } else {
            should_continue = visitor.RaiseException(Exception::NoExecuteFault);
        }
        visitor.ir.current_location = visitor.ir.current_location->AdvancePC(4);
        block.CycleCount()++;
        if (force_dispatch_after && !single_step) {
            // The probe asked for invalidations: nothing after the probed instruction may be
            // reached except through the dispatcher, which services them.
            if (should_continue) {
                visitor.ir.SetTerm(IR::Term::LinkBlock{*visitor.ir.current_location, true});
                should_continue = false;
            } else {
                block.ReplaceTerminal(ForceDispatch(block.GetTerminal()));
            }
        }
    } while (should_continue && !single_step);

    if (single_step && should_continue) {
        visitor.ir.SetTerm(IR::Term::LinkBlock{*visitor.ir.current_location});
    }
    ASSERT(block.HasTerminal() && "Terminal has not been set");
    block.SetEndLocation(*visitor.ir.current_location);
}

bool TranslateSingleInstruction(IR::Block& block, LocationDescriptor descriptor, u32 instruction) {
    TranslatorVisitor visitor{block, descriptor, {}};

    bool should_continue = false;
    if (auto const decoder = Decode<TranslatorVisitor, bool>(visitor, instruction))
        should_continue = *decoder;

    visitor.ir.current_location = visitor.ir.current_location->AdvancePC(4);
    block.CycleCount()++;
    block.SetEndLocation(*visitor.ir.current_location);
    return should_continue;
}

}  // namespace Dynarmic::A64

// ls -l | awk '{print "#include \"dynarmic/frontend/A64/translate/impl/" $9 "\""}'
#include "dynarmic/frontend/A64/translate/impl/a64_branch.cpp"
#include "dynarmic/frontend/A64/translate/impl/a64_exception_generating.cpp"
#include "dynarmic/frontend/A64/translate/impl/data_processing_addsub.cpp"
#include "dynarmic/frontend/A64/translate/impl/data_processing_bitfield.cpp"
#include "dynarmic/frontend/A64/translate/impl/data_processing_conditional_compare.cpp"
#include "dynarmic/frontend/A64/translate/impl/data_processing_conditional_select.cpp"
#include "dynarmic/frontend/A64/translate/impl/data_processing_crc32.cpp"
#include "dynarmic/frontend/A64/translate/impl/data_processing_logical.cpp"
#include "dynarmic/frontend/A64/translate/impl/data_processing_multiply.cpp"
#include "dynarmic/frontend/A64/translate/impl/data_processing_pcrel.cpp"
#include "dynarmic/frontend/A64/translate/impl/data_processing_register.cpp"
#include "dynarmic/frontend/A64/translate/impl/data_processing_shift.cpp"
#include "dynarmic/frontend/A64/translate/impl/floating_point_compare.cpp"
#include "dynarmic/frontend/A64/translate/impl/floating_point_conditional_compare.cpp"
#include "dynarmic/frontend/A64/translate/impl/floating_point_conditional_select.cpp"
#include "dynarmic/frontend/A64/translate/impl/floating_point_conversion_fixed_point.cpp"
#include "dynarmic/frontend/A64/translate/impl/floating_point_conversion_integer.cpp"
#include "dynarmic/frontend/A64/translate/impl/floating_point_data_processing_one_register.cpp"
#include "dynarmic/frontend/A64/translate/impl/floating_point_data_processing_three_register.cpp"
#include "dynarmic/frontend/A64/translate/impl/floating_point_data_processing_two_register.cpp"
#include "dynarmic/frontend/A64/translate/impl/impl.cpp"
#include "dynarmic/frontend/A64/translate/impl/impl.h"
#include "dynarmic/frontend/A64/translate/impl/load_store_exclusive.cpp"
#include "dynarmic/frontend/A64/translate/impl/load_store_atomic.cpp"
#include "dynarmic/frontend/A64/translate/impl/memory_tagging.cpp"
#include "dynarmic/frontend/A64/translate/impl/pointer_authentication.cpp"
#include "dynarmic/frontend/A64/translate/impl/load_store_load_literal.cpp"
#include "dynarmic/frontend/A64/translate/impl/load_store_multiple_structures.cpp"
#include "dynarmic/frontend/A64/translate/impl/load_store_no_allocate_pair.cpp"
#include "dynarmic/frontend/A64/translate/impl/load_store_register_immediate.cpp"
#include "dynarmic/frontend/A64/translate/impl/load_store_register_pair.cpp"
#include "dynarmic/frontend/A64/translate/impl/load_store_register_register_offset.cpp"
#include "dynarmic/frontend/A64/translate/impl/load_store_register_unprivileged.cpp"
#include "dynarmic/frontend/A64/translate/impl/load_store_single_structure.cpp"
#include "dynarmic/frontend/A64/translate/impl/move_wide.cpp"
#include "dynarmic/frontend/A64/translate/impl/simd_across_lanes.cpp"
#include "dynarmic/frontend/A64/translate/impl/simd_aes.cpp"
#include "dynarmic/frontend/A64/translate/impl/simd_copy.cpp"
#include "dynarmic/frontend/A64/translate/impl/simd_crypto_four_register.cpp"
#include "dynarmic/frontend/A64/translate/impl/simd_crypto_three_register.cpp"
#include "dynarmic/frontend/A64/translate/impl/simd_extract.cpp"
#include "dynarmic/frontend/A64/translate/impl/simd_modified_immediate.cpp"
#include "dynarmic/frontend/A64/translate/impl/simd_permute.cpp"
#include "dynarmic/frontend/A64/translate/impl/simd_scalar_pairwise.cpp"
#include "dynarmic/frontend/A64/translate/impl/simd_scalar_shift_by_immediate.cpp"
#include "dynarmic/frontend/A64/translate/impl/simd_scalar_three_different.cpp"
#include "dynarmic/frontend/A64/translate/impl/simd_scalar_three_same.cpp"
#include "dynarmic/frontend/A64/translate/impl/simd_scalar_two_register_misc.cpp"
#include "dynarmic/frontend/A64/translate/impl/simd_scalar_x_indexed_element.cpp"
#include "dynarmic/frontend/A64/translate/impl/simd_sha512.cpp"
#include "dynarmic/frontend/A64/translate/impl/simd_sha.cpp"
#include "dynarmic/frontend/A64/translate/impl/simd_shift_by_immediate.cpp"
#include "dynarmic/frontend/A64/translate/impl/simd_table_lookup.cpp"
#include "dynarmic/frontend/A64/translate/impl/simd_three_different.cpp"
#include "dynarmic/frontend/A64/translate/impl/simd_three_same.cpp"
#include "dynarmic/frontend/A64/translate/impl/simd_three_same_extra.cpp"
#include "dynarmic/frontend/A64/translate/impl/simd_two_register_misc.cpp"
#include "dynarmic/frontend/A64/translate/impl/simd_vector_x_indexed_element.cpp"
#include "dynarmic/frontend/A64/translate/impl/sys_dc.cpp"
#include "dynarmic/frontend/A64/translate/impl/sys_ic.cpp"
#include "dynarmic/frontend/A64/translate/impl/system.cpp"
#include "dynarmic/frontend/A64/translate/impl/system_flag_format.cpp"
#include "dynarmic/frontend/A64/translate/impl/system_flag_manipulation.cpp"
