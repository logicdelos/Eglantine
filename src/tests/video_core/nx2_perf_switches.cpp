// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <memory>
#include <random>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "common/settings.h"
#include "video_core/nx2_diag.h"
#include "video_core/nx2_perf.h"

namespace {

using namespace VideoCommon::Nx2Perf;

constexpr size_t Index(Opt opt) {
    return static_cast<size_t>(opt);
}

} // namespace

TEST_CASE("Nx2Perf pending_wake is on by default and off under SAFE", "[nx2_perf]") {
    REQUIRE(opt_names[Index(Opt::PendingWake)] == "pending_wake");
    REQUIRE(IsOff(DefaultOffMask(), Index(Opt::PendingWake)));
    REQUIRE(!IsOff(ParseOffMask(false, "", ""), Index(Opt::PendingWake)));
    REQUIRE(IsOff(ParseOffMask(true, "", ""), Index(Opt::PendingWake)));
    REQUIRE(!IsOff(ParseOffMask(true, "pending_wake", ""), Index(Opt::PendingWake)));
    REQUIRE(ParseOffMask(false, "", "pending_wake") == (OptInMask() | Bit(Opt::PendingWake)));
}

TEST_CASE("Nx2Perf diag_draw_lite is on by default and off under SAFE", "[nx2_perf]") {
    REQUIRE(opt_names[Index(Opt::DiagDrawLite)] == "diag_draw_lite");
    REQUIRE(IsOff(DefaultOffMask(), Index(Opt::DiagDrawLite)));
    REQUIRE(!IsOff(ParseOffMask(false, "", ""), Index(Opt::DiagDrawLite)));
    REQUIRE(IsOff(ParseOffMask(true, "", ""), Index(Opt::DiagDrawLite)));
    REQUIRE(!IsOff(ParseOffMask(true, "diag_draw_lite", ""), Index(Opt::DiagDrawLite)));
    REQUIRE(ParseOffMask(false, "", "diag_draw_lite") == (OptInMask() | Bit(Opt::DiagDrawLite)));
}

TEST_CASE("Nx2Perf opt-in switches stay off until EDEN_NX2_PERF_ON names them", "[nx2_perf]") {
    for (const Opt opt : {Opt::SemaphoreFenceLean, Opt::DescriptorRunReuse}) {
        REQUIRE(IsOff(ParseOffMask(false, "", ""), Index(opt)));
        REQUIRE(IsOff(ParseOffMask(true, "", ""), Index(opt)));
        REQUIRE(!IsOff(ParseOffMask(false, opt_names[Index(opt)], ""), Index(opt)));
        REQUIRE(!IsOff(ParseOffMask(true, opt_names[Index(opt)], ""), Index(opt)));
    }
}

TEST_CASE("Nx2Perf skip_unneeded_target_copies setting gates the three copy cuts", "[nx2_perf]") {
    REQUIRE(TargetCopyCutsMask() == (Bit(Opt::FeedbackShadowShare) |
                                     Bit(Opt::FeedbackSkipUnwritten) |
                                     Bit(Opt::MaskedTargetUnmodified)));
    for (const bool safe : {false, true}) {
        for (const std::string_view on : {std::string_view{}, std::string_view{"feedback_skip_unwritten"}}) {
            const u64 env_mask = ParseOffMask(safe, on, "");
            // Setting on: the environment decides as it did without the setting.
            REQUIRE(WithTargetCopySetting(env_mask, true) == env_mask);
            // Setting off: the three are off whatever the environment says, nothing else changes.
            const u64 off_mask = WithTargetCopySetting(env_mask, false);
            REQUIRE((off_mask & TargetCopyCutsMask()) == TargetCopyCutsMask());
            REQUIRE((off_mask & ~TargetCopyCutsMask()) == (env_mask & ~TargetCopyCutsMask()));
        }
    }
    // On by default, SAFE turns them off, EDEN_NX2_PERF_ON turns one back on under SAFE.
    REQUIRE((WithTargetCopySetting(ParseOffMask(false, "", ""), true) & TargetCopyCutsMask()) == 0);
    REQUIRE((WithTargetCopySetting(ParseOffMask(true, "", ""), true) & TargetCopyCutsMask()) ==
            TargetCopyCutsMask());
    const u64 safe_on = WithTargetCopySetting(ParseOffMask(true, "feedback_shadow_share", ""), true);
    REQUIRE(!IsOff(safe_on, Index(Opt::FeedbackShadowShare)));
    REQUIRE(IsOff(safe_on, Index(Opt::FeedbackSkipUnwritten)));
    REQUIRE(IsOff(WithTargetCopySetting(ParseOffMask(true, "feedback_shadow_share", ""), false),
                  Index(Opt::FeedbackShadowShare)));
}

TEST_CASE("Nx2Perf ApplySettings follows skip_unneeded_target_copies", "[nx2_perf]") {
    constexpr std::array cuts{Opt::FeedbackShadowShare, Opt::FeedbackSkipUnwritten,
                              Opt::MaskedTargetUnmodified};
    auto& setting = Settings::values.skip_unneeded_target_copies;
    const bool saved = setting.GetValue();
    setting.SetValue(true);
    ApplySettings();
    std::array<bool, cuts.size()> with_setting{};
    for (size_t i = 0; i < cuts.size(); ++i) {
        with_setting[i] = On(cuts[i]);
    }
    const bool other = On(Opt::Bounds);
    setting.SetValue(false);
    ApplySettings();
    for (const Opt opt : cuts) {
        REQUIRE(!On(opt));
    }
    REQUIRE(On(Opt::Bounds) == other);
    setting.SetValue(true);
    ApplySettings();
    for (size_t i = 0; i < cuts.size(); ++i) {
        REQUIRE(On(cuts[i]) == with_setting[i]);
    }
    setting.SetValue(saved);
    ApplySettings();
}

TEST_CASE("diag_draw_lite: folded draws equal the synchronous Touch and Note calls", "[nx2_perf]") {
    using namespace VideoCommon::Nx2Diag;
    Diagnostics sync{Diagnostics::Quiet{}, true};
    Diagnostics lite{Diagnostics::Quiet{}, true};
    for (Diagnostics* diag : {&sync, &lite}) {
        diag->RegisterProgram(0x1000, 0xabc);
        diag->RegisterProgram(0x3000, 0xabc);
    }
    const auto table = std::make_unique<LiteTable>();
    static constexpr std::array<const char*, 3> outcomes{"drawn", "async-build", "ablated"};
    std::mt19937_64 rng(7);
    for (int i = 0; i < 6000; ++i) {
        if (i == 3000) {
            // A key registered mid-run: the pending draws keep the keys of their generation.
            sync.RegisterProgram(0x2000, 0xdef);
            lite.RegisterProgram(0x2000, 0xdef);
        }
        const u64 vs = 0x1000 + (rng() % 4) * 0x1000;
        const u64 ps = 0x8000 + (rng() % 300) * 0x100;
        const char* const outcome = outcomes[rng() % outcomes.size()];
        const u64 instances = 1 + rng() % 3;
        const u64 vertices = rng() % 100;
        const u32 width = 640 + static_cast<u32>(rng() % 2);
        sync.Touch('v', vs, outcome, instances, vertices, width, 360);
        sync.Touch('p', ps, outcome, instances, vertices, width, 360);
        sync.Note(false, vs, ps, outcome, instances, vertices);
        const LiteAnswer answer = lite.LiteLookup(*table, vs, ps);
        const u64 expected_key = vs == 0x1000 || vs == 0x3000   ? u64{0xabc}
                                 : vs == 0x2000 && i >= 3000 ? u64{0xdef}
                                                             : u64{0};
        REQUIRE(answer.vkey == expected_key);
        lite.LiteAdd(*table, answer, outcome, instances, vertices, width, 360);
    }
    lite.FoldLite(*table);
    REQUIRE(table->used == 0);
    u32 lines = 0;
    REQUIRE(lite.DrawDifferences(sync, lines) == 0);
    sync.Touch('v', 0x5000, "drawn", 1, 3, 640, 360);
    REQUIRE(lite.DrawDifferences(sync, lines) != 0);
}
