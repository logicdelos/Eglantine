// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu_cmd/run_control.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace RunControl {

namespace {

constexpr std::string_view MarkerResultName(Spikes::MarkerResult result) noexcept {
    switch (result) {
    case Spikes::MarkerResult::Accepted:
        return "accepted";
    case Spikes::MarkerResult::Duplicate:
        return "duplicate";
    case Spikes::MarkerResult::Backdated:
        return "backdated";
    case Spikes::MarkerResult::NoSession:
        return "no_session";
    }
    return "invalid";
}

} // Anonymous namespace

void HandleMarkerLine(std::string_view line) {
    const MarkerClassification classification = DispatchMarkerLine(line, Spikes::NowNs());
    switch (classification.parse) {
    case MarkerParse::NotMarker:
        return;
    case MarkerParse::Valid:
        break;
    case MarkerParse::Malformed:
    case MarkerParse::MissingCredentials:
    case MarkerParse::TitleMismatch:
    case MarkerParse::NonceMismatch:
        LOG_WARNING(Frontend, "Nx2Startup interactive result=rejected reason={}",
                    MarkerParseName(classification.parse));
        return;
    }
    const Spikes::InteractiveMarker& marker = classification.marker;
    const Spikes::MarkerResult result = Spikes::MarkInteractive(marker);
    if (result != Spikes::MarkerResult::Accepted) {
        CountRefusedMarker();
    }
    LOG_INFO(Frontend, "Nx2Startup interactive scene={} check={} origin={} result={}",
             marker.scene_id, marker.scene_check_passed ? "pass" : "fail", marker.origin,
             MarkerResultName(result));
}

std::optional<ProcessCreation> CaptureProcessCreation() {
#ifdef _WIN32
    FILETIME creation{};
    FILETIME exit_time{};
    FILETIME kernel{};
    FILETIME user{};
    // The two clocks are read back to back, bracketed by the monotonic clock: whatever the
    // scheduler does in between is the uncertainty of the mapping.
    const u64 before_ns = Spikes::NowNs();
    FILETIME now{};
    GetSystemTimePreciseAsFileTime(&now);
    const u64 after_ns = Spikes::NowNs();
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit_time, &kernel, &user)) {
        return std::nullopt;
    }
    const auto to_u64 = [](const FILETIME& time) {
        return (static_cast<u64>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    };
    const u64 now_ticks = to_u64(now);
    const u64 created_ticks = to_u64(creation);
    if (created_ticks == 0 || created_ticks > now_ticks) {
        return std::nullopt;
    }
    // FILETIME counts 100 ns steps. The process creation time stamp is kept at the kernel tick
    // (up to 16 ms), which is the larger part of the uncertainty.
    constexpr u64 kernel_tick_ns = 16'000'000;
    const u64 age_ns = (now_ticks - created_ticks) * 100;
    const u64 mid_ns = before_ns + (after_ns - before_ns) / 2;
    if (age_ns > mid_ns) {
        return std::nullopt; // would be before the clock's own origin
    }
    return ProcessCreation{
        .created_ns = mid_ns - age_ns,
        .uncertainty_ns = (after_ns - before_ns) + kernel_tick_ns,
    };
#else
    return std::nullopt;
#endif
}

} // namespace RunControl
