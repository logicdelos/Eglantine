// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <string>
#include <utility>
#include <vector>

#include "common/common_types.h"

namespace Dynarmic::A64 {
class InstructionProbes;
}

namespace Kernel {
class KProcess;
}

/// Census of the NVN calls a running game makes. The game's own code is left alone: the JIT emits
/// a counting host call before the first instruction of each NVN function the game can reach (the
/// exports of the library that holds nvnBootstrapLoader, and whatever the game got back from
/// nvnBootstrapLoader / nvnDeviceGetProcAddress), and the guest sees no change in registers,
/// memory or control flow. Off, nothing is installed and no state is built. What remains is a
/// relaxed load of the active flag at each present, at each code-change note in the kernel and in
/// the permission-change SVC, plus one EffectiveOptions() lookup (a mutex and a getenv) at Start,
/// at Stop and whenever a JIT asks for Probes().
///
/// It is on when Configure gave it an output file, or else when EDEN_NX2_NVN_CENSUS names one.
/// With auditing on (EDEN_NX2_NVN_CENSUS_AUDIT=1) the JIT also reports every branch from outside
/// the library into it, which is how a call to a function the census did not know is noticed.
///
/// The report (schema 3) keeps three things apart: whether the run ended cleanly (run_complete),
/// whether every condition of a complete census held (coverage_complete) and whether the report
/// holds what a replay of the library calls needs (library_replay_ready, never true while there
/// is no capture of call arguments). A report written while the game runs is never complete.
/// A replay capture of the same run (--capture) has a field of its own, `replay_capture`: when it
/// fails the run ends as `capture_failed` for the run report, but the census reads the exit that
/// capture replaced, so run_complete and coverage_complete rest on the census's own evidence.
namespace Core::NvnCensus {

/// Which route a presented frame took to the display.
enum class PresentPath : u32 {
    Nx2Vi2 = 0,   ///< the nx2:vi2 service (PresentClientBuffer)
    BufferQueue,  ///< the buffer queue of the legacy display path (BufferQueueProducer::QueueBuffer)
};

struct Options {
    /// Where the report goes; empty turns the census off, whatever the environment says.
    std::string out;
    /// Report branches into the library from outside it (slower; needed for a complete census).
    bool audit = false;
    /// Frames a run must have closed before its census can be complete.
    u64 min_frames = 120;
};

/// Sets the options for this process, replacing the environment. Call before the guest cores are
/// created.
void Configure(Options options);

namespace detail {
extern std::atomic<bool> g_active;
void NotePresentSlow(PresentPath path, u64 stream, u64 token, u64 id);
void NoteAcceptedPresentSlow(u64 present_id);
} // namespace detail

/// Whether a census is armed. The kernel's code-change bookkeeping, which only the census reads,
/// does nothing while this is false.
inline bool IsActive() noexcept {
    return detail::g_active.load(std::memory_order_relaxed);
}

/// The probe set for a JIT of the application process, or null when the census is off.
const Dynarmic::A64::InstructionProbes* Probes();

/// Finds the library (the module that exports nvnBootstrapLoader), arms it and starts the
/// reporter. Call once the application process is loaded and before any guest code runs.
void Start(Kernel::KProcess& process, u64 program_id);

/// Writes the final report. Call after the guest cores have stopped; whether the run ended
/// cleanly is read from Common::RunOutcome.
void Stop();

/// A stream presented a frame (identity bookkeeping only; frames close at NoteAcceptedPresent). `stream` is the owning queue (the nx2:vi2 session, or the
/// buffer-queue core) and `token` is that queue's client-buffer token (0 on the legacy queue, which
/// has none). `id` numbers the frames of one stream: consecutive, so a missing or repeated one is
/// noticed. A second token on the same stream marks it ambiguous; it is not split into another stream.
inline void NotePresent(PresentPath path, u64 stream, u64 token, u64 id) {
    if (detail::g_active.load(std::memory_order_relaxed)) {
        detail::NotePresentSlow(path, stream, token, id);
    }
}

/// A present reached the accepted-present seam (the same call that numbers the capture's frames,
/// VideoCore::Replay::NoteAcceptedPresent). It closes the frame in progress, whose census row takes
/// `present_id` as its frame id (report key frame_id_basis = "closing_accepted_present"). Dropped
/// and refresh-only presents never get here and advance no frame.
inline void NoteAcceptedPresent(u64 present_id) {
    if (detail::g_active.load(std::memory_order_relaxed)) {
        detail::NoteAcceptedPresentSlow(present_id);
    }
}

/// Unit-test seam: a census with synthetic entry addresses and no guest process. Not for emulation.
namespace testing {
struct FrameRows {
    std::vector<std::pair<u64, u64>> rows; ///< (frame id, calls) per recorded row, start-up included
    u64 dropped{};
    u64 closed{};
    u64 accepted_presents{};
};
void Begin(std::size_t targets);
void Call(u64 index);
void AuditBranch(u64 index);
FrameRows End();
std::string EndReport(); ///< Finalize and serialize through the production report builder.
} // namespace testing
} // namespace Core::NvnCensus
