// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <nlohmann/json.hpp>

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
}

} // namespace VideoCore::Replay
