// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <nlohmann/json.hpp>

#include "common/frame_timing_log.h"
#include "video_core/replay/result.h"

namespace VideoCore::Replay {

void AddReportSections(nlohmann::json& document) {
    if (const std::optional<CaptureReport> capture = PublishedCaptureReport()) {
        document["capture"] = ToJson(*capture);
    }
    if (const std::optional<ReplayReport> replay = PublishedReplayReport()) {
        document["replay"] = ToJson(*replay);
    }
    if (const std::optional<CensusReport> census = PublishedCensusReport()) {
        document["census"] = ToJson(*census);
    }

    const Common::FrameTiming::Summary timing = Common::FrameTiming::GetSummary();
    if (timing.started) {
        FrameTimingReport report;
        report.status = timing.complete ? SectionStatus::Complete : SectionStatus::Incomplete;
        report.source = timing.source == Common::FrameTiming::Source::HostPresent
                            ? "host_present"
                            : "replay_frame";
        report.frames = timing.recorded + timing.lost;
        report.recorded = timing.recorded;
        report.lost = timing.lost;
        report.first_ns = timing.first_ns;
        report.last_ns = timing.last_ns;
        report.raw_file_written = timing.file_written;
        document["frame_timing"] = ToJson(report);
    }
}

} // namespace VideoCore::Replay
