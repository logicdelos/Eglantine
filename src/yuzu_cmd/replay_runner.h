// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Frozen: the frontend's entry into a replay. Slice C implements replay_runner.cpp. The frontend
// calls Run from SDL_AppInit after the window exists and the signal handlers are installed, in
// place of loading a game (hook listed in contracts_0.6.md); Run returns when the replay ended and
// the frontend then finishes the unattended run (report publication, exit code) as it does after a
// game run. Run never starts a guest process and never touches a game file.

#pragma once

#include <atomic>
#include <string>

#include "common/run_outcome.h"
#include "video_core/replay/options.h"

namespace Core {
class System;
namespace Frontend {
class EmuWindow;
}
} // namespace Core

namespace ReplayRunner {

struct Outcome {
    Common::RunOutcome::Reason reason = Common::RunOutcome::Reason::None;
    std::string detail;  // one line, quoted into the report's exit.detail
};

// Opens the capture, applies the pins, replays, writes the pictures and the report sections
// (PublishReplayReport and, when asked, PublishFrameTimingReport). `max_frames` is --run-frames
// (0 = all). `stop` is set by the frontend's signal handling.
[[nodiscard]] Outcome Run(Core::System& system, Core::Frontend::EmuWindow& window,
                          const VideoCore::Replay::Options& options, u64 max_frames,
                          const std::atomic<bool>& stop);

} // namespace ReplayRunner
