// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-3.0-or-later

#include <vector>
#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "shader_recompiler/frontend/ampere/program_capture.h"

namespace {
using Shader::Ampere::Instruction;
using Shader::Ampere::LowerToMaxwell;
using Shader::Ampere::ProgramCaptureState;
using Result = ProgramCaptureState::Result;

constexpr Instruction EXIT{{0x0000794d, 0, 0x03800000, 0}};
constexpr Instruction NOP{{0x00007918, 0, 0, 0}};
// BRA (PT) to this instruction: offset -16.
constexpr Instruction BRA_SELF{{0x00007947, 0xfffffff0, 0x0383ffff, 0}};
// BRA (PT) back to instruction 1 from instruction 4: offset -64.
constexpr Instruction BRA_BACK{{0x00007947, 0xffffffc0, 0x0383ffff, 0}};

// What follows a program in memory: the offending words of logged rejected cutscene
// programs, each of which decodes as a branch or call with an impossible target.
constexpr Instruction LOGGED_DATA[] = {
    {{0x3f42f947, 0xc04d97f5, 0x3fe0dc04, 0x00000000}}, // BRA @!PT, offset beyond 48 bits
    {{0x3ed77944, 0xffffffff, 0x007f0000, 0x7f007f00}}, // CALL, negative offset beyond 48 bits
    {{0x001d7944, 0x00000000, 0x00000016, 0x00000003}}, // CALL, offset 0x16 << 32
    {{0x3f52b945, 0xffffffff, 0x00b9b2bb, 0x7f65b9e8}}, // BSSY, offset -1 (misaligned)
    {{0x00011944, 0x00011945, 0x00011946, 0x0001194d}}, // a table of counting words
};

struct Captured {
    Result result;
    u32 index;
};

Captured Run(const std::vector<Instruction>& code) {
    ProgramCaptureState capture;
    for (u32 index = 0; index < code.size(); ++index) {
        const Result result = capture.Observe(code[index], index);
        if (result != Result::Continue) return {result, index};
    }
    return {Result::Continue, static_cast<u32>(code.size())};
}

void Append(std::vector<Instruction>& code, u32 nops) {
    for (u32 i = 0; i < nops; ++i) code.push_back(NOP);
    for (const Instruction& data : LOGGED_DATA) code.push_back(data);
}

TEST_CASE("Ampere capture ends at the closing branch when out-of-line blocks follow the EXIT",
          "[video_core][ampere][capture]") {
    // @P0 BRA to the block at 3; the main body's EXIT at 2; the block, a branch back, the
    // closing self-branch, NOP padding and then the data after the program.
    std::vector<Instruction> code{
        {{0x00000947, 0x20, 0x03800000, 0}},
        NOP,
        EXIT,
        NOP,
        BRA_BACK,
        BRA_SELF,
    };
    Append(code, 3);

    const Captured captured = Run(code);
    CHECK(captured.result == Result::Complete);
    CHECK(captured.index == 5);

    // The lowering agrees on where the program ends, so nothing after it is read.
    const auto lowered = LowerToMaxwell(code);
    INFO(lowered.error);
    REQUIRE(static_cast<bool>(lowered));
    CHECK(lowered.source_size == 6 * sizeof(Instruction));
}

TEST_CASE("Ampere capture ignores the target of BRA.DIV", "[video_core][ampere][capture]") {
    // BRA.DIV (mode 2) to a slow path at 3, past the EXIT at 2. The lowering never takes it, so
    // the program ends at the EXIT even though the slow path and its branch back follow.
    std::vector<Instruction> code{
        {{0x00007947, 0x20 | 2, 0x03800000, 0}},
        NOP,
        EXIT,
        NOP,
        BRA_BACK,
    };
    Append(code, 3);

    const Captured captured = Run(code);
    CHECK(captured.result == Result::Complete);
    CHECK(captured.index == 2);
    const auto lowered = LowerToMaxwell(code);
    INFO(lowered.error);
    REQUIRE(static_cast<bool>(lowered));
    CHECK(lowered.source_size == 3 * sizeof(Instruction));

    // A BRA.DIV target that is no instruction is still data, not code.
    const Instruction garbage{{0x00007947, 0x20 | 2 | 8, 0x03800000, 0}};
    CHECK(Run({garbage, EXIT}).result == Result::InvalidTarget);
}

TEST_CASE("Ampere capture still ends at an EXIT with nothing beyond it",
          "[video_core][ampere][capture]") {
    std::vector<Instruction> code{NOP, EXIT, BRA_SELF};
    Append(code, 2);
    const Captured captured = Run(code);
    CHECK(captured.result == Result::Complete);
    CHECK(captured.index == 1);
}

TEST_CASE("Ampere capture does not end at a closing branch with targets beyond it",
          "[video_core][ampere][capture]") {
    // BRA to 4 over the EXIT and a self-branch: the self-branch at 2 is not the end.
    const std::vector<Instruction> code{
        {{0x00007947, 0x30, 0x03800000, 0}}, // target 4
        EXIT,
        BRA_SELF,
        NOP,
        EXIT,
    };
    ProgramCaptureState capture;
    CHECK(capture.Observe(code[0], 0) == Result::Continue);
    CHECK(capture.Observe(code[1], 1) == Result::Continue);
    CHECK(capture.Observe(code[2], 2) == Result::Continue);
    CHECK(capture.Observe(code[3], 3) == Result::Continue);
    CHECK(capture.Observe(code[4], 4) == Result::Complete);

    // A call target beyond it likewise: CALL to 3, EXIT, self-branch, then the callee's RET.
    ProgramCaptureState call;
    CHECK(call.Observe({{0x00007944, 0x20, 0x03c00000, 0}}, 0) == Result::Continue);
    CHECK(call.Observe(EXIT, 1) == Result::Continue);
    CHECK(call.Observe(BRA_SELF, 2) == Result::Continue);
    CHECK(call.Observe({{0x00007950, 0xffffffc0, 0x03c3ffff, 0}}, 3) == Result::Complete);
}

TEST_CASE("Ampere capture only ends at an unconditional closing branch after an EXIT",
          "[video_core][ampere][capture]") {
    // No EXIT yet: a self-branch is a hang, not the end of a program.
    CHECK(Run({NOP, BRA_SELF}).result == Result::Continue);

    // CALL to 2 and an EXIT at 1 (which only ends the main body): every target is then at or
    // before the branch at 2, so only the branch's own form decides.
    const auto after_exit = [] {
        ProgramCaptureState capture;
        capture.Observe({{0x00007944, 0x10, 0x03c00000, 0}}, 0);
        capture.Observe(EXIT, 1);
        return capture;
    };
    ProgramCaptureState closing = after_exit();
    CHECK(closing.Observe(BRA_SELF, 2) == Result::Complete);

    // @P0 BRA to itself, BRA.DIV to itself and a BRA that waits on a second predicate.
    const Instruction predicated{{0x00000947, 0xfffffff0, 0x0383ffff, 0}};
    const Instruction diverged{{0x00007947, 0xfffffff2, 0x0383ffff, 0}};
    const Instruction second_predicate{{0x00007947, 0xfffffff0, 0x0083ffff, 0}};
    for (const Instruction& branch : {predicated, diverged, second_predicate}) {
        ProgramCaptureState capture = after_exit();
        CHECK(capture.Observe(branch, 2) == Result::Continue);
    }

    // @Pg EXIT Pc is conditional: it does not end the program.
    const Instruction conditional{{0x0000794d, 0, 0x00800000, 0}};
    const Captured captured = Run({conditional, NOP});
    CHECK(captured.result == Result::Continue);
    CHECK(captured.index == 2);
}

TEST_CASE("Ampere capture ends a jump table program at its closing branch",
          "[video_core][ampere][capture]") {
    // BRX, an EXIT inside the table's targets, then the closing self-branch.
    const std::vector<Instruction> code{
        {{0x00007949, 0xfffffff0, 0x0383ffff, 0}},
        EXIT,
        NOP,
        EXIT,
        BRA_SELF,
    };
    ProgramCaptureState capture;
    CHECK(capture.Observe(code[0], 0) == Result::Continue);
    CHECK(capture.Observe(code[1], 1) == Result::Continue);
    CHECK(capture.Observe(code[2], 2) == Result::Continue);
    CHECK(capture.Observe(code[3], 3) == Result::Continue);
    CHECK(capture.Observe(code[4], 4) == Result::Complete);
    // Without an EXIT before it too: the table's targets are not in the code.
    ProgramCaptureState bare;
    CHECK(bare.Observe(code[0], 0) == Result::Continue);
    CHECK(bare.Observe(BRA_SELF, 1) == Result::Complete);
}

TEST_CASE("Ampere capture still rejects a branch target outside the program",
          "[video_core][ampere][capture]") {
    // A misaligned offset, a target before the start and one past the largest program.
    const Instruction misaligned{{0x00007947, 0x18, 0x03800000, 0}};
    const Instruction before_start{{0x00007947, 0xffffff00, 0x0383ffff, 0}};
    const Instruction past_end{{0x00007947, 0x00000000, 0x03810000, 0}};
    for (const Instruction& branch : {misaligned, before_start, past_end}) {
        const Captured captured = Run({NOP, branch, EXIT});
        CHECK(captured.result == Result::InvalidTarget);
        CHECK(captured.index == 1);
    }
}

TEST_CASE("Ampere capture reads the data after a program as invalid targets",
          "[video_core][ampere][capture]") {
    // Words of the logged rejected programs, each at the index it was met (+0x330 = 51).
    for (const Instruction& data : LOGGED_DATA) {
        ProgramCaptureState capture;
        CHECK(capture.Observe(data, 51) == Result::InvalidTarget);
    }
}
} // namespace
