// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// allow_speed_up decides whether the game may run faster than real time. With it off the pacing
// readers (Settings::SpeedLimited and Settings::SpeedLimit) always see a limit that is on and at
// most 100; with it on they see what use_speed_limit and the limits say.

#include <memory>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/baseline_settings.h"
#include "common/settings.h"

namespace {

/// Puts the settings the speed limit reads back as they were when the test ends.
class SpeedSettingsGuard {
public:
    SpeedSettingsGuard()
        : use_limit{Settings::values.use_speed_limit.GetValue()},
          limit{Settings::values.speed_limit.GetValue()},
          slow{Settings::values.slow_speed_limit.GetValue()},
          turbo{Settings::values.turbo_speed_limit.GetValue()},
          mode{Settings::values.current_speed_mode.GetValue()},
          allow{Settings::values.allow_speed_up.GetValue()} {}

    ~SpeedSettingsGuard() {
        Settings::values.use_speed_limit.SetValue(use_limit);
        Settings::values.speed_limit.SetValue(limit);
        Settings::values.slow_speed_limit.SetValue(slow);
        Settings::values.turbo_speed_limit.SetValue(turbo);
        Settings::values.current_speed_mode.SetValue(mode);
        Settings::values.allow_speed_up.SetValue(allow);
    }

private:
    bool use_limit;
    u16 limit;
    u16 slow;
    u16 turbo;
    Settings::SpeedMode mode;
    bool allow;
};

void Set(bool allow_speed_up, bool use_limit, u16 limit) {
    Settings::values.allow_speed_up.SetValue(allow_speed_up);
    Settings::values.use_speed_limit.SetValue(use_limit);
    Settings::values.speed_limit.SetValue(limit);
    Settings::values.current_speed_mode.SetValue(Settings::SpeedMode::Standard);
}

} // namespace

TEST_CASE("Without allow_speed_up the game is paced to at most real time",
          "[common][speed_limit]") {
    const SpeedSettingsGuard guard;

    // The limit off, or above 100 %, or 0: limited, at 100.
    Set(false, false, 200);
    CHECK(Settings::SpeedLimited());
    CHECK(Settings::SpeedLimit() == 100);
    Set(false, false, 50);
    CHECK(Settings::SpeedLimited());
    CHECK(Settings::SpeedLimit() == 100);
    Set(false, true, 200);
    CHECK(Settings::SpeedLimited());
    CHECK(Settings::SpeedLimit() == 100);
    Set(false, true, 0);
    CHECK(Settings::SpeedLimited());
    CHECK(Settings::SpeedLimit() == 100);
    // A limit below 100 % still slows the game down.
    Set(false, true, 50);
    CHECK(Settings::SpeedLimited());
    CHECK(Settings::SpeedLimit() == 50);
    Set(false, true, 100);
    CHECK(Settings::SpeedLimit() == 100);
}

TEST_CASE("Without allow_speed_up turbo mode stays at real time and slow mode still slows",
          "[common][speed_limit]") {
    const SpeedSettingsGuard guard;
    Set(false, true, 100);
    Settings::values.turbo_speed_limit.SetValue(200);
    Settings::values.slow_speed_limit.SetValue(50);

    Settings::values.current_speed_mode.SetValue(Settings::SpeedMode::Turbo);
    CHECK(Settings::SpeedLimited());
    CHECK(Settings::SpeedLimit() == 100);
    // Turbo with the limit off is limited as well.
    Settings::values.use_speed_limit.SetValue(false);
    CHECK(Settings::SpeedLimited());
    CHECK(Settings::SpeedLimit() == 100);

    Settings::values.use_speed_limit.SetValue(true);
    Settings::values.current_speed_mode.SetValue(Settings::SpeedMode::Slow);
    CHECK(Settings::SpeedLimited());
    CHECK(Settings::SpeedLimit() == 50);
}

TEST_CASE("With allow_speed_up the limits are what they were", "[common][speed_limit]") {
    const SpeedSettingsGuard guard;

    Set(true, false, 100);
    CHECK_FALSE(Settings::SpeedLimited());
    Set(true, true, 200);
    CHECK(Settings::SpeedLimited());
    CHECK(Settings::SpeedLimit() == 200);
    Set(true, true, 50);
    CHECK(Settings::SpeedLimited());
    CHECK(Settings::SpeedLimit() == 50);
    Set(true, true, 0);
    CHECK(Settings::SpeedLimit() == 0);

    Set(true, true, 100);
    Settings::values.turbo_speed_limit.SetValue(300);
    Settings::values.slow_speed_limit.SetValue(25);
    Settings::values.current_speed_mode.SetValue(Settings::SpeedMode::Turbo);
    CHECK(Settings::SpeedLimited());
    CHECK(Settings::SpeedLimit() == 300);
    Settings::values.current_speed_mode.SetValue(Settings::SpeedMode::Slow);
    CHECK(Settings::SpeedLimit() == 25);
}

TEST_CASE("allow_speed_up off is the baseline, on is a deviation unless declared",
          "[common][speed_limit]") {
    Settings::Baseline::ClearNotedSources();
    const auto values = std::make_unique<Settings::Values>();
    CHECK_FALSE(values->allow_speed_up.GetValue());

    const auto entries = Settings::Baseline::Entries(*values);
    bool listed = false;
    for (const auto& entry : entries) {
        if (entry.key == "allow_speed_up") {
            listed = true;
            CHECK(entry.value == "false");
        }
    }
    CHECK(listed);
    CHECK(Settings::Baseline::Deviations(*values, {}).empty());

    values->allow_speed_up.SetValue(true);
    const auto deviations = Settings::Baseline::Deviations(*values, {});
    REQUIRE(deviations.size() == 1);
    CHECK(deviations[0].key == "allow_speed_up");
    CHECK(deviations[0].baseline == "false");
    CHECK(deviations[0].effective == "true");
    CHECK_FALSE(Settings::Baseline::OnBaseline(deviations, {}));
    CHECK(Settings::Baseline::OnBaseline(deviations, {"allow_speed_up"}));

    // --baseline-allow use_speed_limit=false,allow_speed_up=true is accepted and reported.
    const auto declared_values = std::make_unique<Settings::Values>();
    Settings::Baseline::ResetToBaseline(*declared_values);
    const std::vector<std::string> items{"use_speed_limit=false", "allow_speed_up=true"};
    CHECK(Settings::Baseline::ApplyAllowedValues(*declared_values, items).empty());
    const auto declared = Settings::Baseline::Deviations(*declared_values, {});
    REQUIRE(declared.size() == 2);
    CHECK(declared[0].key == "allow_speed_up");
    CHECK(declared[0].effective == "true");
    CHECK(declared[0].source == "command line");
    CHECK(declared[1].key == "use_speed_limit");
    CHECK(declared[1].effective == "false");
    CHECK(declared[1].source == "command line");
    CHECK(Settings::Baseline::OnBaseline(declared, Settings::Baseline::AllowedKeys(items)));
    Settings::Baseline::ClearNotedSources();
}
