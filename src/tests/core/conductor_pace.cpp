// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Pins the display conductor's pacing for the speed settings: the interval to the next
// composition event, from the settings through Settings::SpeedLimited() and
// Settings::SpeedLimit(), for the cases that matter once allow_speed_up exists. The composition
// multiplier is the game's own request and is applied as given.

#include <cstdlib>
#include <algorithm>

#include <catch2/catch_test_macros.hpp>

#include "common/settings.h"
#include "core/hle/service/vi/conductor_pace.h"

namespace {

namespace Pace = Service::VI::Pace;

constexpr s64 kRealTimeNs = 16'666'667;
constexpr s64 kDoubleRateNs = 8'333'333;
constexpr s64 kHalfRateNs = 33'333'333;
constexpr s64 kToleranceNs = 1'000;

bool Near(s64 actual, s64 expected) {
    return std::llabs(actual - expected) <= kToleranceNs;
}

class PaceSettingsGuard {
public:
    PaceSettingsGuard()
        : multicore{Settings::values.use_multi_core.GetValue()},
          use_limit{Settings::values.use_speed_limit.GetValue()},
          limit{Settings::values.speed_limit.GetValue()},
          mode{Settings::values.current_speed_mode.GetValue()},
          allow{Settings::values.allow_speed_up.GetValue()},
          video{Settings::values.use_video_framerate.GetValue()} {
        Settings::values.use_multi_core.SetValue(true);
        Settings::values.current_speed_mode.SetValue(Settings::SpeedMode::Standard);
        Settings::values.use_video_framerate.SetValue(false);
    }

    ~PaceSettingsGuard() {
        Settings::values.use_multi_core.SetValue(multicore);
        Settings::values.use_speed_limit.SetValue(use_limit);
        Settings::values.speed_limit.SetValue(limit);
        Settings::values.current_speed_mode.SetValue(mode);
        Settings::values.allow_speed_up.SetValue(allow);
        Settings::values.use_video_framerate.SetValue(video);
    }

private:
    bool multicore;
    bool use_limit;
    u16 limit;
    Settings::SpeedMode mode;
    bool allow;
    bool video;
};

void Set(bool allow_speed_up, bool use_limit, u16 limit) {
    Settings::values.allow_speed_up.SetValue(allow_speed_up);
    Settings::values.use_speed_limit.SetValue(use_limit);
    Settings::values.speed_limit.SetValue(limit);
}

s64 NextTick(f32 compose_scale = 1.0f, s32 swap_interval = 1, bool video = false) {
    return Pace::NextTickNs(Pace::FromSettings(video, compose_scale, swap_interval));
}

s64 Period(f32 compose_scale = 1.0f, s32 swap_interval = 1) {
    return Pace::FramePeriodNs(Pace::FromSettings(false, compose_scale, swap_interval));
}

} // namespace

TEST_CASE("Conductor pace: without allow_speed_up the user's limit cannot speed the ticks up",
          "[core][conductor_pace]") {
    const PaceSettingsGuard guard;

    Set(false, false, 100);
    CHECK(Near(NextTick(), kRealTimeNs));
    CHECK(Near(Period(), kRealTimeNs));

    Set(false, true, 200);
    CHECK(Near(NextTick(), kRealTimeNs));
    CHECK(Near(Period(), kRealTimeNs));

    Set(false, true, 0);
    CHECK(Near(NextTick(), kRealTimeNs));

    // A limit below 100 % still slows the game down.
    Set(false, true, 50);
    CHECK(Near(NextTick(), kHalfRateNs));
    CHECK(Near(Period(), kHalfRateNs));
}

TEST_CASE("Conductor pace: a game that asks for presentation without waiting gets the doubled "
          "composition rate, which is its own request and not capped",
          "[core][conductor_pace]") {
    const PaceSettingsGuard guard;

    Set(false, false, 100);
    CHECK(Near(NextTick(2.0f), kDoubleRateNs));
    CHECK(Near(Period(2.0f), kDoubleRateNs));
    Set(false, true, 200);
    CHECK(Near(NextTick(2.0f), kDoubleRateNs));
    // The user's own slower limit is applied on top of the game's request.
    Set(false, true, 50);
    CHECK(Near(NextTick(2.0f), kRealTimeNs));
}

TEST_CASE("Conductor pace: with allow_speed_up the unlocked and fast values are as before",
          "[core][conductor_pace]") {
    const PaceSettingsGuard guard;

    // Limit off: the unlocked scale of 0.01 of real time for the next composition event; the
    // period hint keeps the scale of 1 and divides the period by four (about 4.167 ms), as before.
    Set(true, false, 100);
    CHECK(Near(NextTick(), 166'667));
    CHECK(Near(Period(), 4'166'667));
    // Limit above 100 %: scaled by 100 / limit.
    Set(true, true, 200);
    CHECK(Near(NextTick(), kDoubleRateNs));
    CHECK(Near(Period(), kDoubleRateNs));
    Set(true, true, 100);
    CHECK(Near(NextTick(), kRealTimeNs));
}

TEST_CASE("Conductor pace: video playback with use_video_framerate runs at the presentation rate",
          "[core][conductor_pace]") {
    const PaceSettingsGuard guard;
    Settings::values.use_video_framerate.SetValue(true);

    Set(false, true, 50);
    CHECK(Near(NextTick(1.0f, 1, true), kRealTimeNs));
    Set(true, false, 100);
    CHECK(Near(NextTick(2.0f, 1, true), kRealTimeNs));
    // Without video playback the same settings give the settings' value.
    CHECK(Near(NextTick(2.0f, 1, false), 83'333));
    // The setting off means video playback is not special.
    Settings::values.use_video_framerate.SetValue(false);
    CHECK(Near(NextTick(1.0f, 1, true), 166'667));
}

TEST_CASE("Conductor pace: the swap interval and single-core runs", "[core][conductor_pace]") {
    const PaceSettingsGuard guard;

    Set(false, true, 100);
    CHECK(Near(NextTick(1.0f, 2), 2 * kRealTimeNs));

    // Single core: the limiter paces, the conductor only applies the game's multiplier.
    Settings::values.use_multi_core.SetValue(false);
    Set(true, false, 50);
    CHECK(Near(NextTick(), kRealTimeNs));
    CHECK(Near(NextTick(2.0f), kDoubleRateNs));
}

namespace {

// A literal copy of the arithmetic of Conductor::GetNextTicks and Conductor::GetFramePeriodNs as
// it was at commit 3558ded5 (src/core/hle/service/vi/conductor.cpp), with the settings and the
// conductor's members turned into parameters. Nothing here is derived from the new code. The
// speed limit is Settings::SpeedLimit() with allow_speed_up on, which is the value of the
// speed_limit setting, so `limit` stands for both.
s64 LegacyNextTicks(bool multicore, bool use_speed_limit, u16 limit, f32 compose_speed_scale,
                    bool video_playback, s32 swap_interval) {
    auto speed_scale = 1.f;
    if (multicore) {
        if (use_speed_limit) {
            speed_scale = 100.f / limit;
        } else {
            // Run at unlocked framerate.
            speed_scale = 0.01f;
        }
    }
    speed_scale /= compose_speed_scale;
    if (video_playback) {
        speed_scale = 1.f;
    }
    const f32 effective_fps = 60.f / static_cast<f32>(swap_interval);
    return static_cast<s64>(speed_scale * (1000000000.f / effective_fps));
}

s64 LegacyFramePeriodNs(bool multicore, bool use_speed_limit, u16 limit, f32 compose_speed_scale,
                        s32 swap_interval) {
    f32 speed_scale = 1.f;
    bool unlocked = false;
    if (multicore) {
        if (use_speed_limit) {
            speed_scale = 100.f / limit;
        } else {
            unlocked = true;
        }
    }
    speed_scale /= compose_speed_scale;
    const f32 effective_fps = 60.f / static_cast<f32>(swap_interval);
    s64 period = static_cast<s64>(speed_scale * (1000000000.f / effective_fps));
    if (unlocked) {
        period /= 4;
    }
    return std::clamp<s64>(period, 1'000'000, 100'000'000);
}

Pace::Input MakeInput(bool multicore, bool limited, u16 limit, f32 compose, bool video, s32 interval) {
    Pace::Input input;
    input.multicore = multicore;
    input.speed_limited = limited;
    input.speed_limit = limit;
    input.compose_scale = compose;
    input.video_playback = video;
    input.swap_interval = interval;
    return input;
}

} // namespace

TEST_CASE("Conductor pace: every combination equals the arithmetic before the extraction",
          "[core][conductor_pace]") {
    // The frame-period hint equals the legacy value in each of them.
    for (const bool multicore : {false, true}) {
        for (const bool limited : {false, true}) {
            for (const u16 limit : {u16{50}, u16{100}, u16{200}}) {
                for (const f32 compose : {1.0f, 2.0f}) {
                    for (const s32 interval : {1, 2}) {
                        for (const bool video : {false, true}) {
                            const Pace::Input input =
                                MakeInput(multicore, limited, limit, compose, video, interval);
                            INFO("multicore " << multicore << " limited " << limited << " limit "
                                              << limit << " compose " << compose << " interval "
                                              << interval << " video " << video);
                            CHECK(Pace::NextTickNs(input) ==
                                  LegacyNextTicks(multicore, limited, limit, compose, video,
                                                  interval));
                            CHECK(Pace::FramePeriodNs(input) ==
                                  LegacyFramePeriodNs(multicore, limited, limit, compose, interval));
                        }
                    }
                }
            }
        }
    }
}

TEST_CASE("Conductor pace: the frame-period hint equals the legacy value at the clamp boundaries",
          "[core][conductor_pace]") {
    // Below the minimum, at the limits and above the maximum, limited and unlocked.
    for (const bool limited : {false, true}) {
        for (const u16 limit : {u16{1}, u16{50}, u16{100}, u16{1000}, u16{2000}}) {
            for (const f32 compose : {1.0f, 2.0f, 4.0f, 8.0f, 16.0f, 0.5f}) {
                for (const s32 interval : {1, 2, 6}) {
                    const Pace::Input input = MakeInput(true, limited, limit, compose, false, interval);
                    CHECK(Pace::FramePeriodNs(input) ==
                          LegacyFramePeriodNs(true, limited, limit, compose, interval));
                }
            }
        }
    }
    // The hint stays inside its bounds, and the values that matter are as the old code gave them.
    CHECK(Pace::FramePeriodNs(MakeInput(true, true, 1, 1.0f, false, 1)) == 100'000'000);
    CHECK(Pace::FramePeriodNs(MakeInput(true, true, 2000, 4.0f, false, 1)) == 1'000'000);
    CHECK(Pace::FramePeriodNs(MakeInput(true, false, 100, 8.0f, false, 1)) == 1'000'000);
    CHECK(Pace::FramePeriodNs(MakeInput(true, false, 100, 1.0f, false, 6)) == 25'000'000);
    CHECK(Near(Pace::FramePeriodNs(MakeInput(true, false, 100, 1.0f, false, 1)), 4'166'667));
}

TEST_CASE("Conductor pace: the log line names the queued swap interval and the multiplier",
          "[core][conductor_pace]") {
    // Queued interval 0 is normalised to interval 1 with a multiplier of 2; the log says 0.
    CHECK(Pace::ComposeMultiplierChangeText(0, 1.0f, 2.0f) ==
          "Compose speed multiplier changed: queued interval 0 -> multiplier 2 (was 1)");
    CHECK(Pace::ComposeMultiplierChangeText(-1, 1.0f, 4.0f) ==
          "Compose speed multiplier changed: queued interval -1 -> multiplier 4 (was 1)");
    // A high queued interval is a precise multiplier (150 gives 1.5).
    CHECK(Pace::ComposeMultiplierChangeText(150, 2.0f, 1.5f) ==
          "Compose speed multiplier changed: queued interval 150 -> multiplier 1.5 (was 2)");
    CHECK(Pace::ComposeMultiplierChangeText(1, 2.0f, 1.0f) ==
          "Compose speed multiplier changed: queued interval 1 -> multiplier 1 (was 2)");
}
