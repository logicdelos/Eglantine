// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <initializer_list>
#include <optional>

#include <catch2/catch_test_macros.hpp>

#include "common/test_frame_delay.h"
#include "core/core_timing.h"
#include "core/hle/service/present_admission.h"

namespace {
using Common::TestHooks::FrameDelay;
using Service::PresentAdmission::HasMappedGsvImage;
using Service::PresentAdmission::HasNewLegacyComposition;

TEST_CASE("CPU present delay: unset or zero leaves the presenting thread alone", "[common][delay]") {
    for (const char* ns : {static_cast<const char*>(nullptr), "0", "000"}) {
        FrameDelay delay{ns, ns ? "cpu-present" : nullptr};
        CHECK(delay.Apply() == 0);
        const auto snapshot = delay.Snapshot();
        CHECK(snapshot.configured == (ns != nullptr));
        CHECK(snapshot.requested_ns == 0);
        CHECK(snapshot.error.empty());
        CHECK(snapshot.frames_delayed == 0);
        CHECK(snapshot.added_ns_total == 0);
        CHECK(snapshot.max_added_ns == 0);
    }
    FrameDelay zero_without_target{"0", nullptr};
    CHECK(zero_without_target.Apply() == 0);
    CHECK(zero_without_target.Snapshot().error.empty());
}

TEST_CASE("CPU present delay: only a bounded decimal and the CPU target opt in", "[common][delay]") {
    const auto invalid = [](const char* ns, const char* target) {
        FrameDelay delay{ns, target};
        CHECK(delay.Apply() == 0);
        CHECK(delay.Snapshot().configured);
        CHECK_FALSE(delay.Snapshot().error.empty());
        CHECK(delay.Snapshot().requested_ns == 0);
        CHECK(delay.Snapshot().frames_delayed == 0);
    };
    for (const char* ns : {"", "-1", "+1", "1.5", " 1", "1ns", "1000000001", "18446744073709551616"}) {
        invalid(ns, "cpu-present");
    }
    invalid(nullptr, "cpu-present");
    for (const char* target : {static_cast<const char*>(nullptr), "", "gpu-frame", "CPU-PRESENT"}) {
        invalid("1", target);
    }
    CHECK(FrameDelay{"0001", "cpu-present"}.Snapshot().requested_ns == 1);
    CHECK(FrameDelay{"1000000000", "cpu-present"}.Snapshot().requested_ns == 1'000'000'000);
}

TEST_CASE("CPU present delay: only submitted frames wait; a reused address is not a duplicate",
          "[common][delay]") {
    FrameDelay delay{"250000", "cpu-present"};
    Core::Timing::CoreTiming guest;
    guest.SetMulticore(false);
    guest.Initialize([] {});
    guest.AddTicks(4096);
    const auto before = guest.GetGlobalTimeNs();
    u64 sum{}, longest{};
    const auto submit = [&] {
        const auto ns = delay.Apply();
        CHECK(ns >= 250'000);
        sum += ns;
        longest = std::max(longest, ns);
    };
    const auto legacy = [&](bool acquired, bool empty) {
        if (HasNewLegacyComposition(acquired, empty)) {
            submit();
        }
    };
    const auto gsv = [&](std::optional<u64> address) {
        if (HasMappedGsvImage(address)) {
            submit();
        }
    };

    legacy(false, false); // Unchanged visible layers on a compositor refresh.
    legacy(true, true);   // Acquired buffer, but no visible composition.
    gsv(std::nullopt);    // No device mapping: GSV returns before RequestComposite.
    CHECK(delay.Snapshot().frames_delayed == 0);
    legacy(true, false);
    CHECK(delay.Snapshot().frames_delayed == 1);
    gsv(0x1000);
    legacy(false, false);
    gsv(std::nullopt);
    CHECK(delay.Snapshot().frames_delayed == 2);
    gsv(0x1000); // New submission, same image.
    const auto snapshot = delay.Snapshot();
    CHECK(snapshot.frames_delayed == 3);
    CHECK(snapshot.requested_ns == 250'000);
    CHECK(snapshot.added_ns_total == sum);
    CHECK(snapshot.max_added_ns == longest);
    CHECK(guest.GetGlobalTimeNs() == before);
}
} // Anonymous namespace
