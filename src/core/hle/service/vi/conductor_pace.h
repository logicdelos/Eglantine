// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <string>

#include <fmt/format.h>

#include "common/common_types.h"
#include "common/settings.h"

// The pacing of the display conductor as one pure computation, so the two places that need it
// (the composition event and the frame period hint) cannot drift apart and a test can pin the
// results without a System.
//
// What is held by allow_speed_up is the user's speed limit: Settings::SpeedLimit() and
// Settings::SpeedLimited() already carry that. The composition multiplier is the game's own
// request (a buffer queued with swap interval 0 is composed at twice the rate, as upstream does)
// and is applied as given.
namespace Service::VI::Pace {

struct Input {
    bool multicore = false;
    bool speed_limited = true; // Settings::SpeedLimited()
    u16 speed_limit = 100;     // Settings::SpeedLimit()
    f32 compose_scale = 1.0f;  // multiplier requested by the game while composing
    bool video_playback = false; // video is decoding and use_video_framerate is on
    s32 swap_interval = 1;
};

/// Reads the settings the conductor depends on. The only place the pure functions below get
/// their inputs from.
inline Input FromSettings(bool video_playback, f32 compose_scale, s32 swap_interval) {
    Input input;
    input.multicore = Settings::values.use_multi_core.GetValue();
    input.speed_limited = Settings::SpeedLimited();
    input.speed_limit = Settings::SpeedLimit();
    input.compose_scale = compose_scale;
    input.video_playback = video_playback && Settings::values.use_video_framerate.GetValue();
    input.swap_interval = swap_interval;
    return input;
}

constexpr f32 kUnlockedScale = 0.01f;
constexpr s64 kUnlockedTargetDivisor = 4;
constexpr s64 kMinPeriodNs = 1'000'000;
constexpr s64 kMaxPeriodNs = 100'000'000;

/// The base of a display tick's time scale, before the game's own multiplier.
struct Scale {
    f32 value = 1.0f;
    bool unlocked = false;
};

/// The time scale of a display tick: 1 is real time at the game's swap interval, below 1 is
/// faster, above 1 is slower. Single-core runs are paced by SpeedLimiter::DoSpeedLimiting and
/// only the game's composition multiplier applies. With the limit off on multicore the scale is
/// 1 and `unlocked` is set: only the time to the next composition event replaces it with
/// kUnlockedScale (NextTickNs); the period hint keeps 1 and divides the period instead.
constexpr Scale ComputeScale(const Input& input) {
    Scale scale;
    if (input.multicore) {
        if (input.speed_limited) {
            scale.value = 100.f / input.speed_limit;
        } else {
            scale.unlocked = true;
        }
    }
    return scale;
}

constexpr f32 FrameLengthNs(const Input& input) {
    const f32 effective_fps = 60.f / static_cast<f32>(input.swap_interval);
    return 1000000000.f / effective_fps;
}

/// Time to the next composition event.
constexpr s64 NextTickNs(const Input& input) {
    const Scale base = ComputeScale(input);
    f32 scale = base.unlocked ? kUnlockedScale : base.value;
    // Adjust by the speed the game asked for during composition.
    scale /= input.compose_scale;
    if (input.video_playback) {
        // Run at the intended presentation rate during video playback.
        scale = 1.f;
    }
    return static_cast<s64>(scale * FrameLengthNs(input));
}

/// Frame period handed to the platform as a target work duration (a hint, not pacing).
constexpr s64 FramePeriodNs(const Input& input) {
    const Scale base = ComputeScale(input);
    const f32 scale = base.value / input.compose_scale;
    s64 period = static_cast<s64>(scale * FrameLengthNs(input));
    if (base.unlocked) {
        period /= kUnlockedTargetDivisor;
    }
    return std::clamp<s64>(period, kMinPeriodNs, kMaxPeriodNs);
}

/// The text of the log line written when the composition multiplier changes: the swap interval the
/// game queued (not the normalised one: queued 0 is interval 1 with a multiplier of 2) and the
/// multiplier that follows from it.
inline std::string ComposeMultiplierChangeText(s32 queued_swap_interval, f32 old_multiplier,
                                               f32 new_multiplier) {
    return fmt::format("Compose speed multiplier changed: queued interval {} -> multiplier {} (was {})",
                       queued_swap_interval, new_multiplier, old_multiplier);
}

} // namespace Service::VI::Pace
