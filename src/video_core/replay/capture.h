// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Frozen: the capture entry points and the hooks other code calls. Slice B implements capture.cpp
// behind this header; every hook is an inline test of a relaxed atomic followed by a call that
// exists only in capture.cpp, so a run without --capture pays one load per hook.
//
// A capture selects consecutive accepted presents [first_frame, first_frame + frame_count). The
// writer records from `first_frame - preroll_frames` (the state checkpoint is taken at the start
// of that frame, or the capture is left incomplete when it cannot be taken cleanly), writes one
// END_PICTURE per selected frame from the live composed picture, and finalizes the file when the
// last selected frame has presented and its producing fence completed. It never blocks the
// emulation: events go through bounded staging and a writer thread, and a bound that would be
// exceeded stops the capture as incomplete (the run itself continues, the file stays .partial).
//
// Hooks applied at integration (contracts_0.6.md lists the exact lines): NoteSubmit in
// Tegra::GPU::PushGPUEntries, NoteGuestWrite in the GPU's CPU-write invalidation path,
// NoteAcceptedPresent before RequestComposite at both present sites. Hooks that live in files
// no 0.5 slice owns (dma_pusher.cpp, memory_manager.cpp, nvdisp_disp0.cpp) are added by slice B
// itself; their declarations are private to slice B (memory_capture.h).

#pragma once

#include <atomic>
#include <span>
#include <string>

#include "common/common_types.h"
#include "core/hle/service/nvdrv/nvdata.h"
#include "video_core/dma_pusher.h"
#include "video_core/framebuffer_config.h"
#include "video_core/replay/events.h"
#include "video_core/replay/library_call.h"
#include "video_core/replay/result.h"
#include "video_core/replay/run_state.h"

namespace Core {
class System;
}

namespace VideoCore::Replay {

namespace detail {
// True from BeginCapture until the capture finalizes or stops. Header-inline: no link dependency.
inline std::atomic<bool> g_capture_active{false};
} // namespace detail

[[nodiscard]] inline bool CaptureActive() noexcept {
    return detail::g_capture_active.load(std::memory_order_relaxed);
}

struct CaptureConfig {
    std::string path;            // the final .egcap path; the writer streams to path + ".partial"
    u64 first_frame = 0;         // first selected accepted-present id, >= 1
    u32 frame_count = 0;         // 1..MaxSelectedFrames
    u32 preroll_frames = 0;      // frames recorded before first_frame, replayed but not compared
    u64 title_id = 0;            // provenance only, 0 when unknown
    SessionLimits limits;        // the bounds this capture runs under
};

// Validates the destination with CheckPrivateOutput, opens the .partial file, registers the
// capture and arms the hooks. Returns false with a message when the capture cannot start (nothing
// is left behind). Must be called before the first selected frame presents.
[[nodiscard]] bool BeginCapture(Core::System& system, const CaptureConfig& config,
                                std::string& error);

// Ends the capture: finalizes it when the selected frames are all complete and no bound was hit,
// otherwise leaves it as .partial marked incomplete. Publishes the CaptureReport either way and
// returns it. `run_ended_normally` is false when the run was interrupted or failed.
CaptureReport EndCapture(bool run_ended_normally);

// ---- hooks ------------------------------------------------------------------------------------------

void NoteGraphicsCallSlow();
inline void NoteGraphicsCall() {
    if (CaptureActive()) {
        NoteGraphicsCallSlow();
    }
}

void NoteSubmitSlow(s32 channel, const Tegra::CommandList& list);
void NoteGuestWriteSlow(u64 device_address, u64 size);
void NoteAcceptedPresentSlow(u64 present_id, PresentPath path,
                             std::span<const Tegra::FramebufferConfig> layers,
                             std::span<const Service::Nvidia::NvFence> fences);

// Tegra::GPU::PushGPUEntries, before the list is queued.
inline void NoteSubmit(s32 channel, const Tegra::CommandList& list) {
    if (CaptureActive()) {
        NoteSubmitSlow(channel, list);
    }
}

// A guest-visible write the GPU must observe (the path that invalidates GPU caches for a CPU
// write): device address and size of the range.
inline void NoteGuestWrite(u64 device_address, u64 size) {
    if (CaptureActive()) {
        NoteGuestWriteSlow(device_address, size);
    }
}

// The single place a present is accepted for display, on either path. Assigns the logical frame
// id (run_state.h), tells the census observer and the capture, and returns the id. Called before
// RequestComposite with the layers and fences that call receives.
inline u64 NoteAcceptedPresent(PresentPath path,
                               std::span<const Tegra::FramebufferConfig> layers,
                               std::span<const Service::Nvidia::NvFence> fences) {
    const u64 id = CountAcceptedPresent();
    NotifyPresentObserver(id, path);
    if (CaptureActive()) {
        NoteAcceptedPresentSlow(id, path, layers, fences);
    }
    return id;
}

} // namespace VideoCore::Replay
