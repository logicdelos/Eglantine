// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <string>
#include <string_view>

#include "common/run_outcome.h"
#include "video_core/replay/format.h"
#include "video_core/replay/replay_internal.h"
#include "video_core/replay/replay_session.h"
#include "video_core/replay/result.h"
#include "yuzu_cmd/replay_runner.h"

namespace ReplayRunner {

namespace {

namespace Replay = VideoCore::Replay;
using Reason = Common::RunOutcome::Reason;

// A capture that never reached the GPU still leaves a replay section behind, so the report says
// why nothing ran.
void PublishRefusal(const Replay::ReplayConfig& config, std::string_view message,
                    Replay::ReadError kind) {
    Replay::ReplayReport report;
    report.status = Replay::SectionStatus::Failed;
    report.benchmark = config.benchmark;
    report.error = std::string(message);
    report.error_kind = Replay::ErrorKindText(kind);
    Replay::PublishReplayReport(report);
}

[[nodiscard]] std::string Describe(const Replay::ReplayReport& report) {
    if (report.run_complete) {
        return "replayed " + std::to_string(report.frames_replayed) + " of " +
               std::to_string(report.frames_expected) + " frames";
    }
    if (report.error.empty()) {
        return "the replay stopped early";
    }
    return report.error_kind.empty() ? report.error
                                     : report.error + " (" + report.error_kind + ")";
}

} // namespace

Outcome Run(Core::System& system, Core::Frontend::EmuWindow& window,
            const VideoCore::Replay::Options& options, u64 max_frames,
            const std::atomic<bool>& stop) {
    const Replay::ReplayConfig config = Replay::ReplayConfigFromOptions(options, max_frames);
    Replay::ReplaySession session;

    std::string detail;
    const Replay::ReadError opened = session.Open(config, detail);
    if (opened != Replay::ReadError::None) {
        const std::string message = std::string("capture refused (") +
                                    std::string(Replay::ReadErrorName(opened)) + "): " + detail;
        PublishRefusal(config, message, opened);
        return {Reason::BadArguments, message};
    }
    if (!session.ApplyPins(detail)) {
        // A pin the build cannot honour is the format's UnsupportedState.
        const std::string message = Replay::DescribeStop("capture pin refused: " + detail,
                                                         Replay::ReadError::UnsupportedState);
        PublishRefusal(config, message, Replay::ReadError::UnsupportedState);
        return {Reason::BadArguments, message};
    }

    const Replay::ReplayReport report = session.Run(system, window, stop);

    Replay::OutcomeInput input;
    input.ran = true;
    input.frames_replayed = report.frames_replayed;
    input.frames_asked = report.frames_expected;
    input.failed = report.status == Replay::SectionStatus::Failed;
    input.interrupted = report.status == Replay::SectionStatus::Incomplete &&
                        report.error == Replay::InterruptedText;
    input.startup_failed = Replay::StartsWith(report.error, Replay::StartupPrefix);
    input.report_io = Replay::StartsWith(report.error, Replay::ReportIoPrefix);
    const Reason current = Common::RunOutcome::CurrentReason();
    input.device_lost = current == Reason::DeviceLost;
    input.shader_failure = current == Reason::ShaderFailure;
    return {Replay::JudgeReplay(input), Describe(report)};
}

} // namespace ReplayRunner
