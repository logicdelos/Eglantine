// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <cstdint>

namespace Dynarmic {

/// Running totals over every JIT instance (read by the frontend's frame spike recorder).
struct JitStats {
    std::atomic<std::uint64_t> blocks_compiled{};
    std::atomic<std::uint64_t> compile_ns{}; ///< Translate, optimize and emit (x64 A64 only)
    std::atomic<std::uint64_t> fastmem_recompiles{}; ///< Blocks invalidated by a fastmem fault
    std::atomic<std::uint64_t> fastmem_faults{}; ///< Host faults taken in fastmem accesses
};

inline JitStats g_jit_stats;

enum class JitPhase : std::uint8_t {
    Compile,      ///< Translate, optimize and emit one block (the span compile_ns totals)
    MemoryCommit, ///< Making the code buffer's memory ready before compiling
};

/// Optional hook told when a phase ended, with its begin and end as nanoseconds of
/// std::chrono::steady_clock since its epoch (the clock of VideoCommon::Nx2Spikes::NowNs). It is
/// installed once by the core, which turns the call into an attribution span, so that Dynarmic
/// does not depend on video_core. Called on the compiling thread after g_jit_stats was updated;
/// it must not throw, block or take a lock the JIT holds. Null: nothing extra is timed.
using JitPhaseHook = void (*)(JitPhase phase, std::uint64_t begin_ns, std::uint64_t end_ns);

inline std::atomic<JitPhaseHook> g_jit_phase_hook{nullptr};

} // namespace Dynarmic
