// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Frozen: the replay of a capture without the game. Slice C implements replay_session.cpp (and the
// narrow GPU/renderer entry points it needs in video_core.cpp and the Vulkan renderer); slice B's
// CaptureReader and RestoreGpuCheckpoint are its inputs; slice A's result.h is its output.
//
// What a replay does
//   1. Open: reads and fully validates the capture (CaptureReader::Validate); any ReadError, an
//      incomplete capture or a pin the build cannot honour ends the run before a device exists.
//   2. ApplyPins: sets the pinned settings (SESSION chunk) for this process, before the renderer
//      is created, and records the device identity that the capture ran on in the report.
//   3. Run: creates the GPU and renderer without a guest process, creates backing memory for every
//      allocation id, applies the checkpoint, then applies the remaining chunks strictly in
//      sequence order. MAP/UNMAP go through the GPU memory manager, EXTERNAL_WRITE and delta
//      blobs through the same visibility path a guest write takes (write the bytes, then notify
//      the rasterizer), FENCE entries are applied and the waiting channel woken, Submit events
//      queue the entries and Consume words are fed to the channel's pusher unchanged. PRESENT
//      composes the frame off-screen into a host buffer (never through the window), FRAME_END
//      waits for the producing fence, reads the picture back unless benchmarking, compares the
//      semantic outputs, and records the frame. Pre-roll frames are executed, not compared.
//   4. Pictures: for every selected frame, live_N.png and replay_N.png plus one frames.jsonl line
//      into the --replay-images directory (result.h); no per-frame readback with
//      --replay-benchmark.
//   The run reports a ReplayReport. Exit reasons reuse Common::RunOutcome::Reason: an unreadable
//   or unsupported capture is BadArguments, a device loss DeviceLost, a shader failure
//   ShaderFailure, an output I/O failure ReportIoError, an interrupt Interrupted, and a replay
//   that ran every frame it was asked for (all selected frames, or --run-frames of them)
//   FrameLimit, which the frontend maps to exit code 0.
//
// The replay never consults a game file, a title id, a build id of the capture (they are
// provenance in the report only) or the host's real guest memory; it needs only the capture and
// the pinned settings.

#pragma once

#include <atomic>
#include <memory>
#include <string>

#include "common/common_types.h"
#include "video_core/replay/checkpoint.h"
#include "video_core/replay/events.h"
#include "video_core/replay/format.h"
#include "video_core/replay/result.h"

namespace Core {
class System;
namespace Frontend {
class EmuWindow;
}
} // namespace Core

namespace VideoCore::Replay {

struct ReplayConfig {
    std::string capture_path;
    std::string images_dir;         // empty: no pictures
    bool benchmark = false;         // timing only: no pictures, no per-frame readback
    u64 max_frames = 0;             // 0: every selected frame; else a prefix of them (--run-frames)
    std::string frame_times_path;   // empty: no raw timing log
};

class ReplaySession {
public:
    ReplaySession();
    ~ReplaySession();
    ReplaySession(const ReplaySession&) = delete;
    ReplaySession& operator=(const ReplaySession&) = delete;

    // Step 1. On failure `detail` is a one-line reason and the return value names the class.
    [[nodiscard]] ReadError Open(const ReplayConfig& config, std::string& detail);
    [[nodiscard]] const FileHeader& Header() const noexcept;
    [[nodiscard]] const SessionRecord& Session() const noexcept;

    // Step 2. False with a message when a pin cannot be honoured (UnsupportedState).
    [[nodiscard]] bool ApplyPins(std::string& error);

    // Steps 3 and 4. `stop` is polled between chunks: a set flag ends the run as interrupted.
    // The returned report is also published (PublishReplayReport) before return.
    [[nodiscard]] ReplayReport Run(Core::System& system, Core::Frontend::EmuWindow& window,
                                   const std::atomic<bool>& stop);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace VideoCore::Replay
