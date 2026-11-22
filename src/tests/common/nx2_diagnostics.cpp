// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <string>

#include <catch2/catch_test_macros.hpp>

#include "common/logging.h"
#include "common/nx2_diagnostics.h"

using namespace Nx2Bottleneck;

TEST_CASE("Shader rejection classes follow the exception text", "[common][nx2_diagnostics]") {
    // The texts of the rejections in a real log.
    REQUIRE(ClassifyShaderRejection(
                "Native shader at 0x23e6f92000, instruction 0x0: unsupported native opcode [op 000 "
                "00000000 00000000 00000000 00000000]") == RejectionClass::AllZeroCode);
    REQUIRE(ClassifyShaderRejection(
                "Native shader branch outside capture bounds at 0x23a2427080 +0xe60 [29242944 "
                "555d5f5e 19042924 00a0a0aa]") == RejectionClass::BranchOutsideCapture);
    REQUIRE(ClassifyShaderRejection(
                "Native shader at 0x1000, instruction 0x40: unsupported native opcode [op 1a4 "
                "00000001 00000000 00000000 00000000]") == RejectionClass::UnsupportedOpcode);
    REQUIRE(ClassifyShaderRejection("Native shader at 0x1000, instruction 0x8: native program "
                                    "uses lowering scratch registers") == RejectionClass::Other);
    REQUIRE(ClassifyShaderRejection("") == RejectionClass::Other);
}

TEST_CASE("Shader rejections are counted per class in the window and the session",
          "[common][nx2_diagnostics]") {
    RejectionCounts window{};
    window[static_cast<size_t>(RejectionClass::AllZeroCode)] = 2;
    window[static_cast<size_t>(RejectionClass::UnsupportedOpcode)] = 1;
    REQUIRE(FormatShaderRejections(window, 17) ==
            "3 new in the last 10 s (all-zero code 2, unsupported opcode 1); 17 this session");
    REQUIRE(FormatShaderRejections({}, 17) == "0 new in the last 10 s; 17 this session");
}

TEST_CASE("A scene change needs the new target set for several frames", "[common][nx2_diagnostics]") {
    SceneTracker tracker{3};
    // The first set is accepted without a change.
    REQUIRE_FALSE(tracker.OnFrame(11));
    for (int i = 0; i < 9; ++i) {
        REQUIRE_FALSE(tracker.OnFrame(11));
    }
    // A pass that shows for 2 frames only (a menu, a fade) is no change.
    REQUIRE_FALSE(tracker.OnFrame(22));
    REQUIRE_FALSE(tracker.OnFrame(22));
    REQUIRE_FALSE(tracker.OnFrame(11));
    // Neither is a set that flips between two others.
    REQUIRE_FALSE(tracker.OnFrame(22));
    REQUIRE_FALSE(tracker.OnFrame(33));
    REQUIRE_FALSE(tracker.OnFrame(22));
    REQUIRE_FALSE(tracker.OnFrame(33));
    REQUIRE_FALSE(tracker.OnFrame(11));
    // 3 frames in a row are.
    REQUIRE_FALSE(tracker.OnFrame(44));
    REQUIRE_FALSE(tracker.OnFrame(44));
    REQUIRE(tracker.OnFrame(44));
    REQUIRE(tracker.Accepted() == 44);
    // The old set was seen 12 frames: the first and nine more, then 2 more that came back to it.
    REQUIRE(tracker.PreviousStableFrames() == 12);
    // Once, not on every later frame of the new set.
    REQUIRE_FALSE(tracker.OnFrame(44));
    REQUIRE_FALSE(tracker.OnFrame(44));
}

TEST_CASE("Scene change lines are rate limited and counted", "[common][nx2_diagnostics]") {
    LineLimiter limiter{5.0};
    REQUIRE(limiter.Allow(100.0));
    REQUIRE_FALSE(limiter.Allow(101.0));
    REQUIRE_FALSE(limiter.Allow(104.9));
    REQUIRE(limiter.TakeSuppressed() == 2);
    REQUIRE(limiter.TakeSuppressed() == 0);
    REQUIRE(limiter.Allow(105.0));
}

TEST_CASE("A pipeline burst fires once and re-arms when it is over", "[common][nx2_diagnostics]") {
    BurstDetector burst{10, 2.0};
    REQUIRE(burst.Add(0.0, 4) == 0);
    REQUIRE(burst.Add(0.5, 3) == 0);
    // 4 + 3 + 3 within 2 s.
    REQUIRE(burst.Add(1.0, 3) == 10);
    // The same burst goes on without another report.
    REQUIRE(burst.Add(1.5, 5) == 0);
    REQUIRE(burst.Add(1.9, 5) == 0);
    // Everything older than 2 s is dropped: the span is almost empty, the burst is over.
    REQUIRE(burst.Add(5.0, 0) == 0);
    REQUIRE(burst.Add(5.1, 4) == 0);
    REQUIRE(burst.Add(5.2, 6) == 10);
    // Spread over more than the span it is no burst.
    BurstDetector slow{10, 2.0};
    for (int i = 0; i < 10; ++i) {
        REQUIRE(slow.Add(i * 1.0, 1) == 0);
    }
}

TEST_CASE("Low memory warns after 30 s, once, and again only after recovering",
          "[common][nx2_diagnostics]") {
    using Action = LowMemoryDetector::Action;
    LowMemoryDetector detector;
    REQUIRE(detector.Update(0.0, true) == Action::None);
    REQUIRE(detector.Update(10.0, true) == Action::None);
    REQUIRE(detector.Update(20.0, true) == Action::None);
    REQUIRE(detector.Update(30.0, true) == Action::Warn);
    REQUIRE(detector.LowFor(30.0) == 30.0);
    REQUIRE(detector.Update(40.0, true) == Action::None);
    REQUIRE(detector.Update(50.0, true) == Action::None);
    REQUIRE(detector.Update(60.0, false) == Action::Recovered);
    REQUIRE(detector.Update(70.0, false) == Action::None);
    // A new episode counts its 30 s from its own start.
    REQUIRE(detector.Update(80.0, true) == Action::None);
    REQUIRE(detector.Update(100.0, true) == Action::None);
    REQUIRE(detector.Update(110.0, true) == Action::Warn);
}

TEST_CASE("A low spell that ends before 30 s warns of nothing", "[common][nx2_diagnostics]") {
    using Action = LowMemoryDetector::Action;
    LowMemoryDetector detector;
    REQUIRE(detector.Update(0.0, true) == Action::None);
    REQUIRE(detector.Update(10.0, true) == Action::None);
    REQUIRE(detector.Update(20.0, false) == Action::None);
    // The 10 s of the first spell do not count towards the second.
    REQUIRE(detector.Update(30.0, true) == Action::None);
    REQUIRE(detector.Update(50.0, true) == Action::None);
}

TEST_CASE("Memory is low under 1 GiB free or at 95% commit", "[common][nx2_diagnostics]") {
    HostMemory memory;
    REQUIRE_FALSE(IsLowMemory(memory)); // unknown
    memory.known = true;
    memory.free_bytes = u64{4} << 30;
    memory.commit_fraction = 0.6;
    REQUIRE_FALSE(IsLowMemory(memory));
    memory.free_bytes = (u64{1} << 30) - 1;
    REQUIRE(IsLowMemory(memory));
    memory.free_bytes = u64{4} << 30;
    memory.commit_fraction = 0.95;
    REQUIRE(IsLowMemory(memory));
}

namespace {
ProgressWindow StuckWindow() {
    ProgressWindow window;
    window.whole = true;
    window.seconds = 10.0;
    window.frames = 600;
    window.busiest_core_run = 0.98;
    window.busiest_core = 0;
    window.file_reads = 0;
    window.empty_yields = 10'400'000;
    window.svc_calls = 11'000'000;
    return window;
}
} // namespace

TEST_CASE("A window of one full core, no reads and idle yields looks stuck",
          "[common][nx2_diagnostics]") {
    REQUIRE(WindowLooksStuck(StuckWindow()));

    auto window = StuckWindow();
    window.file_reads = 3;
    REQUIRE_FALSE(WindowLooksStuck(window));
    window = StuckWindow();
    window.busiest_core_run = 0.9;
    REQUIRE_FALSE(WindowLooksStuck(window));
    window = StuckWindow();
    window.frames = 0; // the frame-stall watchdog's case
    REQUIRE_FALSE(WindowLooksStuck(window));
    window = StuckWindow();
    window.empty_yields = 1'000'000; // the calls are not mostly SleepThread that yielded to nobody
    REQUIRE_FALSE(WindowLooksStuck(window));
    window = StuckWindow();
    window.whole = false;
    REQUIRE_FALSE(WindowLooksStuck(window));
}

TEST_CASE("No progress fires after 2 windows, once per episode", "[common][nx2_diagnostics]") {
    NoProgressDetector detector;
    REQUIRE_FALSE(detector.Update(StuckWindow()));
    REQUIRE(detector.Update(StuckWindow()));
    REQUIRE(detector.Streak() == 2);
    REQUIRE_FALSE(detector.Update(StuckWindow()));
    REQUIRE_FALSE(detector.Update(StuckWindow()));

    // A window with reads ends the episode, and the next one needs 2 windows again.
    auto reading = StuckWindow();
    reading.file_reads = 40;
    REQUIRE_FALSE(detector.Update(reading));
    REQUIRE(detector.Streak() == 0);
    REQUIRE_FALSE(detector.Update(StuckWindow()));
    REQUIRE(detector.Update(StuckWindow()));

    // So does a window the guest was paused in.
    REQUIRE_FALSE(detector.Update(ProgressWindow{}));
    REQUIRE_FALSE(detector.Update(StuckWindow()));
    REQUIRE(detector.Update(StuckWindow()));
}

TEST_CASE("Distinct recent errors keep one slot per source line", "[common][nx2_diagnostics]") {
    // Own lines: the log of the whole test run feeds the same slots.
    for (int i = 0; i < 3; ++i) {
        LOG_ERROR(Common, "nx2_diagnostics test error {}", i);
    }
    LOG_ERROR(Common, "nx2_diagnostics test other site");
    Common::Log::RecentError errors[Common::Log::RecentErrorSlots];
    const size_t count = Common::Log::GetRecentErrors(errors, Common::Log::RecentErrorSlots);
    // Without a log backend nothing is tapped: the filter drops the messages first.
    if (count == 0) {
        SUCCEED("no log backend in this process");
        return;
    }
    REQUIRE(std::string{errors[count - 1].message} == "nx2_diagnostics test other site");
    REQUIRE(errors[count - 1].count == 1);
    REQUIRE(count >= 2);
    REQUIRE(std::string{errors[count - 2].message} == "nx2_diagnostics test error 2");
    REQUIRE(errors[count - 2].count == 3);
}
