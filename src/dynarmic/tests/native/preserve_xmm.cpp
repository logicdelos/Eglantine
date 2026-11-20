// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>
#include <oaknut/oaknut.hpp>
#include <immintrin.h>

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

TEST_CASE("X86: a stale translation hint leaves the block available for a matching hint", "[x86][jit_prewarm]") {
    A64TestEnv env;
    env.code_mem = {0x14000000};  // B .
    A64::UserConfig conf{};
    conf.callbacks = &env;
    A64::Jit jit{conf};
    struct Hint {
        std::uint64_t location;
        std::uint64_t hash;
        unsigned recordings;
    } hint{};
    jit.SetBlockCompiledCallback([](void* user, std::uint64_t location, std::uint64_t hash) {
        auto& result = *static_cast<Hint*>(user);
        result = {location, hash, result.recordings + 1};
    }, &hint);
    jit.SetPC(0);
    env.ticks_left = 1;
    CheckedRun([&] { jit.Run(); });
    REQUIRE(hint.recordings == 1);
    jit.ClearCache();

    CHECK(jit.PrecompileBlock(hint.location, hint.hash ^ 1) == A64::PrecompileResult::CodeMismatch);
    CHECK(jit.PrecompileBlock(hint.location, hint.hash) == A64::PrecompileResult::Compiled);
    CHECK(jit.PrecompileBlock(hint.location, hint.hash) == A64::PrecompileResult::AlreadyCompiled);
    CHECK(hint.recordings == 1);
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
