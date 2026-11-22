// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Nx2Sampler {

/// Trace the submit, completion, and guest wait boundaries without altering their policy.
[[nodiscard]] inline bool DependencyTraceEnabled() {
    static const bool enabled = std::getenv("EDEN_NX2_DEPENDENCY_TRACE") != nullptr;
    return enabled;
}

/// Guest thread whose synchronous HLE request is currently dispatched on this host thread.
inline thread_local std::uint64_t dependency_ipc_thread{};

// EDEN_NX2_SAMPLE=<thread names>: a sampling profiler for named host threads (Windows only).
// Names are comma separated; a name ending in '*' matches by prefix (CPUCore_*), and a bare name
// also matches its numbered threads (CPUCore is CPUCore_0, CPUCore_1, ...). From
// EDEN_NX2_SAMPLE_START seconds (default 60) for EDEN_NX2_SAMPLE_SECONDS (default 30), each
// thread is paused about 1000 times a second and its call stack recorded; the counts per
// function (where the thread is, and everything on its stack) and the 25 most frequent full
// stacks go to EDEN_NX2_SAMPLE_OUT (default nx2_samples.txt in the log directory; a run where no
// thread matches writes the names that exist there instead). <out>.timeline lists every sample
// and present with its QPC time.
// JIT samples are also split by guest block, module and region (see g_track_guest), and
// CPUCore threads get a "cpu split:" line.
// Called once by the command line and by the Qt frontend's main(), before any guest code exists
// (a second call does nothing).
void StartFromEnvironment();

/// Guest attribution of JIT samples. Set from EDEN_NX2_SAMPLE (or EDEN_NX2_SAMPLE_GUEST=1) before
/// the first JIT exists; EDEN_NX2_SAMPLE_GUEST=0 turns it off while sampling. While it is set
/// the JIT reports each code cache, each block it emits and each cache clear, and the report maps
/// a sampled host address back to the guest block (module+offset) that ran. Unset: no JIT cost.
inline std::atomic_bool g_track_guest{};
[[nodiscard]] inline bool GuestTracking() noexcept {
    return g_track_guest.load(std::memory_order_relaxed);
}

/// The JIT's code cache [begin, begin+size): blocks and the dispatcher / thunks live in it.
void NoteJitCode(const void* begin, std::size_t size);
/// A block of guest code starting at `guest_pc` was emitted at [host, host+size).
void NoteJitBlock(const void* host, std::size_t size, std::uint64_t guest_pc);
/// The code cache [begin, begin+size) was cleared: its blocks are gone, the space is reused.
void NoteJitClear(const void* begin, std::size_t size);

struct GuestModule {
    std::string name;
    std::uint64_t base;
    std::uint64_t end;
};

/// The loaded guest modules, taken by the core on a JIT thread while the process is alive.
void SetGuestModules(std::vector<GuestModule> modules);

/// Resolve a guest byte to host RAM. The sampler safely copies with ReadProcessMemory.
/// Clearing waits for any report read to finish; call after cores stop, before process teardown.
void SetGuestMemoryPointer(std::function<const void*(std::uint64_t)> pointer);

/// Samples the named threads for `seconds` now (blocking), writes the report to `out` (and
/// the folded stacks to `out`.folded) and returns the report.
std::string Sample(const char* names, double seconds, const std::string& out);

/// A one-shot sample of the first thread named by one of `thread_names` (exact names, or a name
/// before "_": "CPUCore" is CPUCore_0, CPUCore_1, ...): the report goes to `out` as for Sample and
/// the result is one line, "<n> host samples; where it is: <top 3 functions>; commonest stack:
/// <6 frames>". When no thread matches the line says so and lists the thread names.
std::string SampleBrief(const std::vector<std::string>& thread_names, double seconds,
                        const std::string& out);

/// Records a guest present while sampling, so `out`.timeline can place samples in frames.
void NotePresent();

/// Stall watch. The core reports every frame the guest presents and whether the guest is
/// running (not paused, not stopped). When a running guest presents nothing for
/// EDEN_NX2_STALL_SECONDS (default 60, 0 turns the watch off), the name, CPU time and call
/// stack of every thread go to stall_<date>_<time>.txt in the log directory (Windows only):
/// once per stall, three times a run at most.
void NoteGuestFrame();
void SetGuestRunning(bool running);

/// Call stack of the calling thread, one "\n  module+0xoffset function (file:line)" per frame,
/// leaving out `skip` callers (empty off Windows). Slow: for error paths that report once.
std::string CurrentStack(unsigned skip = 0);

} // namespace Nx2Sampler
