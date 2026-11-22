// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"

namespace Shader::Ampere {
// Finds where a native program ends in memory. The shader cache hands over a start address and
// no size, and whatever follows the program (padding, other programs, constants) is arbitrary
// data, so the capture has to stop at the compiler's own end of program. It must agree with
// LowerToMaxwell about which instructions end a program: the capture may not stop before the
// lowering needs more, and must not go on past where the lowering is done.
//
// The compiler lays a program out as: the main body ending in EXIT, then out-of-line blocks
// (reached by forward branches and calls, ending in a branch back, a RET or an EXIT), then a
// closing unconditional branch to itself and NOP padding. A program therefore ends at
//  - an unconditional EXIT that no forward branch or call target lies beyond,
//  - the RET of a function called from the main body (which has exited),
//  - the closing branch to itself, when no forward branch or call target lies beyond it, or,
//    when a computed branch (a jump table, whose targets are not in the code) was seen, always.
// The last one matters when the out-of-line blocks come last: the EXIT is then below the
// farthest branch target, and nothing but the closing branch marks the end.
class ProgramCaptureState {
public:
    enum class Result { Continue, Complete, InvalidTarget };
    static constexpr u32 MaximumInstructions = 65536;

    Result Observe(const Instruction& instruction, u32 index) {
        const u32 opcode = instruction.Bits(0, 12);
        const bool unconditional = instruction.Bits(12, 4) == 7;
        if (opcode == 0x944 || opcode == 0x947 || opcode == 0x945) {
            // BRA keeps its .DIV/.CONV mode in the offset's two low bits.
            const u32 low = instruction.words[1] & (opcode == 0x947 ? ~3U : ~0U);
            const u64 raw = low | (u64{instruction.words[2] & 0x3ffff} << 32);
            const s64 relative = opcode == 0x945 ? static_cast<s32>(instruction.words[1])
                                                : static_cast<s64>(raw << 14) >> 14;
            const s64 target = s64{index} * 16 + 16 + relative;
            if (target < 0 || target % 16 || target >= MaximumInstructions * 16)
                return Result::InvalidTarget;
            // BRA.DIV (mode 2) is taken only by a diverged warp, which the lowering never
            // models (it drops the branch): its target, usually a slow path placed past the
            // EXIT, is not part of the program the lowering reads.
            const bool diverged_only = opcode == 0x947 && (instruction.words[1] & 3) == 2;
            // CALL.REL.NOINC (bit 86) pushes no return address, but its callee still returns
            // with RET.REL.NODEC through the link register set ahead of the call.
            if (opcode == 0x944) {
                last_call_target = std::max(last_call_target, static_cast<u32>(target / 16));
            } else if (!diverged_only) {
                last_forward_target = std::max(last_forward_target, static_cast<u32>(target / 16));
            }
            // The compiler ends every program with an unconditional branch to itself. With a
            // jump table (whose targets live in a constant buffer) that is the only reliable
            // end; otherwise it ends the program once an EXIT has come and every branch and call
            // target is behind it.
            if (opcode == 0x947 && unconditional && !diverged_only &&
                instruction.Bits(87, 4) == 7 && target == s64{index} * 16 &&
                (computed_branch ||
                 (exit_seen && last_forward_target <= index && last_call_target <= index)))
                return Result::Complete;
        }
        if (opcode == 0x94d && unconditional) exit_seen = true;
        if (opcode == 0x949 || opcode == 0x958) computed_branch = true;
        if (computed_branch) return Result::Continue;
        // An EXIT that also waits on a second predicate (@Pg EXIT Pc) is conditional.
        if (opcode == 0x94d && unconditional && instruction.Bits(87, 4) == 7 &&
            index >= last_forward_target) {
            if (index >= last_call_target) return Result::Complete;
            main_exited = true;
        }
        if (opcode == 0x950 && unconditional && main_exited &&
            index >= last_call_target && index >= last_forward_target)
            return Result::Complete;
        return Result::Continue;
    }

    // The farthest branch and call targets seen so far, in instructions: why a capture that
    // found no end is still looking for one.
    u32 LastForwardTarget() const { return last_forward_target; }
    u32 LastCallTarget() const { return last_call_target; }
    bool ComputedBranch() const { return computed_branch; }

private:
    u32 last_call_target{};
    u32 last_forward_target{};
    bool main_exited{};
    bool computed_branch{};
    bool exit_seen{}; // an unconditional EXIT, which a closing self-branch must follow
};
} // namespace Shader::Ampere
