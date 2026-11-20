// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>
#include <oaknut/oaknut.hpp>
#include <immintrin.h>

#include <cstddef>
#include <cstdint>
#include <optional>

#include "dynarmic/tests/A64/testenv.h"
#include "dynarmic/tests/native/testenv.h"
#include "dynarmic/common/fp/fpsr.h"
#include "dynarmic/interface/exclusive_monitor.h"

#ifdef _WIN32
#    include "dynarmic/backend/x64/block_of_code.h"
#    include <windows.h>
#endif

using namespace Dynarmic;
using namespace oaknut::util;

TEST_CASE("X86: Preserve XMM regs", "[x86]") {
    A64TestEnv env;
    A64::UserConfig jit_user_config{};
    jit_user_config.callbacks = &env;
    A64::Jit jit{jit_user_config};

    oaknut::VectorCodeGenerator code{env.code_mem, nullptr};
    code.SMINP(V2.S2(), V0.S2(), V1.S2());
    code.UMINP(V3.S2(), V0.S2(), V1.S2());
    code.SMINP(V4.S4(), V0.S4(), V1.S4());
    code.UMINP(V5.S4(), V0.S4(), V1.S4());
    code.SMAXP(V6.S2(), V0.S2(), V1.S2());
    code.UMAXP(V7.S2(), V0.S2(), V1.S2());
    code.SMAXP(V8.S4(), V0.S4(), V1.S4());
    code.UMAXP(V9.S4(), V0.S4(), V1.S4());

    constexpr std::array<Vector, 12> vectors = {
        // initial input vectors [0-1]
        Vector{0x00000003'00000002, 0xF1234567'01234567},
        Vector{0x80000000'7FFFFFFF, 0x76543210'76543209},
        // expected output vectors [2-9]
        Vector{0x80000000'00000002, 0},
        Vector{0x7FFFFFFF'00000002, 0},
        Vector{0xF1234567'00000002, 0x76543209'80000000},
        Vector{0x01234567'00000002, 0x76543209'7FFFFFFF},
        Vector{0x7FFFFFFF'00000003, 0},
        Vector{0x80000000'00000003, 0},
        Vector{0x01234567'00000003, 0x76543210'7FFFFFFF},
        Vector{0xF1234567'00000003, 0x76543210'80000000},
        // input vectors with elements swapped pairwise [10-11]
        Vector{0x00000002'00000003, 0x01234567'F1234567},
        Vector{0x7FFFFFFF'80000000, 0x76543209'76543210},
    };

    jit.SetPC(0);
    jit.SetVector(0, vectors[0]);
    jit.SetVector(1, vectors[1]);

    env.ticks_left = env.code_mem.size();
    CheckedRun([&]() { jit.Run(); });

    CHECK(jit.GetVector(2) == vectors[2]);
    CHECK(jit.GetVector(3) == vectors[3]);
    CHECK(jit.GetVector(4) == vectors[4]);
    CHECK(jit.GetVector(5) == vectors[5]);
    CHECK(jit.GetVector(6) == vectors[6]);
    CHECK(jit.GetVector(7) == vectors[7]);
    CHECK(jit.GetVector(8) == vectors[8]);
    CHECK(jit.GetVector(9) == vectors[9]);
}

namespace {

/// Counts the guest instruction words the JIT reads, and can make the words from an address on
/// unreadable, so a hint can be checked without a crashing guest instruction.
class HintEnv final : public A64TestEnv {
public:
    std::size_t code_reads{};
    std::optional<std::uint64_t> unreadable_from{};

    std::optional<std::uint32_t> MemoryReadCode(u64 vaddr) override {
        ++code_reads;
        if (unreadable_from && vaddr >= *unreadable_from) {
            return std::nullopt;
        }
        return A64TestEnv::MemoryReadCode(vaddr);
    }
};

/// What the JIT reported for a block the guest reached: the hint a later launch would carry.
struct RecordedHint {
    std::uint64_t location{};
    std::uint64_t hash{};
    std::uint32_t word_count{};
    unsigned recordings{};
};

void RecordHint(void* user, std::uint64_t location, std::uint64_t hash,
                std::uint32_t word_count) {
    auto& hint = *static_cast<RecordedHint*>(user);
    hint = {location, hash, word_count, hint.recordings + 1};
}

/// Runs the guest from `pc`; the block takes as many ticks as `code_mem` has words.
void RunGuest(A64::Jit& jit, HintEnv& env, std::uint64_t pc) {
    jit.SetPC(pc);
    env.ticks_left = env.code_mem.size();
    CheckedRun([&] { jit.Run(); });
}

} // namespace

TEST_CASE("X86: a stale translation hint is rejected before the frontend reads the block",
          "[x86][jit_prewarm]") {
    HintEnv env;
    // Eight NOPs, and the read past the end answers `B .`: a translation started here reads nine
    // words. A hint taken when this block was one word long is stale; it must be judged after the
    // one word it claims, without the frontend reading the block the guest now has.
    env.code_mem.assign(8, 0xd503201f);
    A64::UserConfig conf{};
    conf.callbacks = &env;
    A64::Jit jit{conf};
    RecordedHint hint{};
    jit.SetBlockCompiledCallback(&RecordHint, &hint);

    RunGuest(jit, env, 0);
    REQUIRE(hint.recordings == 1);
    REQUIRE(hint.location == 0);
    REQUIRE(hint.word_count == 9);

    jit.ClearCache();
    env.code_reads = 0;
    CHECK(jit.PrecompileBlock(hint.location, hint.hash ^ 1, 1) ==
          A64::PrecompileResult::CodeMismatch);
    CHECK(env.code_reads == 1); // not nine: the frontend never ran on a stale hint

    // Nothing was kept: the same hint compiles once the code matches it again.
    env.code_reads = 0;
    CHECK(jit.PrecompileBlock(hint.location, hint.hash, hint.word_count) ==
          A64::PrecompileResult::Compiled);
    CHECK(env.code_reads == hint.word_count);
}

TEST_CASE("X86: a valid translation hint compiles the block and reads each word once",
          "[x86][jit_prewarm]") {
    HintEnv env;
    env.code_mem = {0xd2800540, 0x14000000}; // MOV X0, #42; B .
    A64::UserConfig conf{};
    conf.callbacks = &env;
    A64::Jit jit{conf};
    RecordedHint hint{};
    jit.SetBlockCompiledCallback(&RecordHint, &hint);

    RunGuest(jit, env, 0);
    REQUIRE(hint.recordings == 1);
    REQUIRE(hint.word_count == 2);

    jit.ClearCache();
    env.code_reads = 0;
    CHECK(jit.PrecompileBlock(hint.location, hint.hash, hint.word_count) ==
          A64::PrecompileResult::Compiled);
    CHECK(env.code_reads == hint.word_count); // one validating read of each word
    CHECK(hint.recordings == 1);              // compiling ahead is not a guest block

    env.code_reads = 0;
    CHECK(jit.PrecompileBlock(hint.location, hint.hash, hint.word_count) ==
          A64::PrecompileResult::AlreadyCompiled);
    CHECK(env.code_reads == 0); // a cached block is not read again

    RunGuest(jit, env, 0);
    CHECK(jit.GetRegister(0) == 42);
    CHECK(hint.recordings == 1);
}

TEST_CASE("X86: a hint that cannot be validated is refused without reading the guest",
          "[x86][jit_prewarm]") {
    HintEnv env;
    env.code_mem = {0xd2800540}; // MOV X0, #42
    A64::UserConfig conf{};
    conf.callbacks = &env;
    A64::Jit jit{conf};

    constexpr std::uint64_t location = 0;
    constexpr std::uint64_t hash = 0x0123456789abcdefULL;
    const auto refused = [&](std::uint64_t at, std::uint32_t words) {
        env.code_reads = 0;
        CHECK(jit.PrecompileBlock(at, hash, words) == A64::PrecompileResult::CodeMismatch);
        CHECK(env.code_reads == 0);
    };

    SECTION("no words") {
        refused(location, 0);
    }
    SECTION("more words than a block may have") {
        refused(location, A64::MaxPrecompileWords + 1);
    }
    SECTION("a misaligned block") {
        refused(location + 2, 1);
    }
    SECTION("a single-stepping block") {
        refused(location | (std::uint64_t{1} << 57), 1);
    }
}

TEST_CASE("X86: a hint over an unreadable word is refused and keeps nothing",
          "[x86][jit_prewarm]") {
    HintEnv env;
    env.code_mem = {0xd2800540, 0x14000000}; // MOV X0, #42; B .
    A64::UserConfig conf{};
    conf.callbacks = &env;
    A64::Jit jit{conf};
    RecordedHint hint{};
    jit.SetBlockCompiledCallback(&RecordHint, &hint);

    RunGuest(jit, env, 0);
    REQUIRE(hint.recordings == 1);
    REQUIRE(hint.word_count == 2);
    jit.ClearCache();

    env.unreadable_from = env.code_mem_start_address + 4; // the second word only
    env.code_reads = 0;
    CHECK(jit.PrecompileBlock(hint.location, hint.hash, hint.word_count) ==
          A64::PrecompileResult::CodeMismatch);
    CHECK(env.code_reads <= hint.word_count); // never more than the hint's words

    env.unreadable_from.reset();
    env.code_reads = 0;
    CHECK(jit.PrecompileBlock(hint.location, hint.hash, hint.word_count) ==
          A64::PrecompileResult::Compiled);
    CHECK(env.code_reads == hint.word_count);
}

TEST_CASE("X86: a valid hint whose block crosses a page boundary is accepted",
          "[x86][jit_prewarm]") {
    HintEnv env;
    env.code_mem_start_address = 0x0ffc;      // the second word is at 0x1000
    env.code_mem = {0xd2800540, 0x14000000}; // MOV X0, #42; B .
    A64::UserConfig conf{};
    conf.callbacks = &env;
    A64::Jit jit{conf};
    RecordedHint hint{};
    jit.SetBlockCompiledCallback(&RecordHint, &hint);

    RunGuest(jit, env, env.code_mem_start_address);
    REQUIRE(hint.recordings == 1);
    REQUIRE(hint.location == env.code_mem_start_address);
    REQUIRE(hint.word_count == 2);

    jit.ClearCache();
    env.code_reads = 0;
    CHECK(jit.PrecompileBlock(hint.location, hint.hash, hint.word_count) ==
          A64::PrecompileResult::Compiled);
    CHECK(env.code_reads == 2);

    RunGuest(jit, env, env.code_mem_start_address);
    CHECK(jit.GetRegister(0) == 42);
}

#ifdef _WIN32
TEST_CASE("X86: an unavailable optional code commit leaves the reservation usable", "[x86][jit_prewarm]") {
    using Dynarmic::Backend::X64::CodeCommit;
    using Dynarmic::Backend::X64::detail::CommitCodePages;
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const size_t size = info.dwAllocationGranularity;
    void* const base = VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_NOACCESS);
    REQUIRE(base != nullptr);

    // Cross one reservation boundary: Windows must refuse, without exhausting system memory.
    CHECK_FALSE(CommitCodePages(base, size + info.dwPageSize, CodeCommit::Optional));
    MEMORY_BASIC_INFORMATION region{};
    CHECK(VirtualQuery(base, &region, sizeof(region)) == sizeof(region));
    CHECK(region.State == MEM_RESERVE);
    CHECK(CommitCodePages(base, size, CodeCommit::Required));
    CHECK(VirtualFree(base, 0, MEM_RELEASE) != 0);
}
#endif
