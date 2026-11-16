// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "common/key_script.h"

using namespace Common::KeyScript;

namespace {

Observation At(double seconds, u64 frames = 0, std::optional<int> shaders = std::nullopt,
               u64 present_count = 0) {
    Observation now;
    now.seconds = seconds;
    now.frames = frames;
    now.present_count = present_count;
    now.shaders_building = shaders;
    return now;
}

Runner MakeRunner(std::string_view text) {
    ParseResult parsed = Parse(text);
    REQUIRE(parsed.error.empty());
    return Runner{std::move(parsed.steps)};
}

} // Anonymous namespace

TEST_CASE("KeyScript parses the timed and present-count syntax as before", "[common][keyscript]") {
    const ParseResult parsed = Parse("36:4,36.6:-4, 41 : 4,f600:-4,\n50.5:79,");
    REQUIRE(parsed.error.empty());
    REQUIRE(parsed.steps.size() == 5);
    CHECK(parsed.steps[0].trigger == Trigger::Seconds);
    CHECK(parsed.steps[0].seconds == 36.0);
    CHECK(parsed.steps[0].code == 4);
    CHECK(parsed.steps[1].code == -4);
    CHECK(parsed.steps[2].seconds == 41.0);
    CHECK(parsed.steps[3].trigger == Trigger::PresentCount);
    CHECK(parsed.steps[3].count == 600);
    CHECK_FALSE(parsed.steps[3].timeout.has_value());
    CHECK(parsed.steps[4].seconds == 50.5);
    CHECK(parsed.steps[4].code == 79);
    CHECK(Parse("").steps.empty());
    CHECK(Parse("").error.empty());
}

TEST_CASE("KeyScript keeps accepting a leading plus in f<N>", "[common][keyscript]") {
    const ParseResult plus = Parse("f+600:40");
    const ParseResult plain = Parse("f600:40");
    REQUIRE(plus.error.empty());
    REQUIRE(plus.steps.size() == 1);
    CHECK(plus.steps[0].trigger == plain.steps[0].trigger);
    CHECK(plus.steps[0].count == plain.steps[0].count);
    CHECK(plus.steps[0].code == plain.steps[0].code);
    CHECK_FALSE(Parse("f++600:40").error.empty());
    CHECK_FALSE(Parse("f+:40").error.empty());
}

TEST_CASE("KeyScript parses the event steps", "[common][keyscript]") {
    const ParseResult parsed = Parse("frame>=600:4,after first_frame+2.5@60:-4,"
                                     "after shaders_idle+3:4,after prev+0.5:-4,after prev:4,"
                                     "f10@30:4");
    REQUIRE(parsed.error.empty());
    REQUIRE(parsed.steps.size() == 6);
    CHECK(parsed.steps[0].trigger == Trigger::Frames);
    CHECK(parsed.steps[0].count == 600);
    CHECK(parsed.steps[0].timeout == DefaultEventTimeoutSeconds);
    CHECK(parsed.steps[1].trigger == Trigger::AfterFirstFrame);
    CHECK(parsed.steps[1].seconds == 2.5);
    CHECK(parsed.steps[1].timeout == 60.0);
    CHECK(parsed.steps[1].when == "after first_frame+2.5@60");
    CHECK(parsed.steps[2].trigger == Trigger::AfterShadersIdle);
    CHECK(parsed.steps[3].trigger == Trigger::AfterPrevious);
    CHECK(parsed.steps[4].seconds == 0.0);
    CHECK(parsed.steps[5].trigger == Trigger::PresentCount);
    CHECK(parsed.steps[5].timeout == 30.0);
}

TEST_CASE("KeyScript refuses a script with any bad step and keeps none of it", "[common][keyscript]") {
    for (const char* text :
         {"10:4,oops", "10:4,after sometime+1:4", "frame>=abc:4", "10@5:4", "10:x", "5:4,after first_frame+z:4",
          "frame>=10@0:4", "frame>=10@-3:4", "-1:4", "f:4", "after first_frame+1"}) {
        const ParseResult parsed = Parse(text);
        INFO(text);
        CHECK_FALSE(parsed.error.empty());
        CHECK(parsed.steps.empty());
    }
}

TEST_CASE("KeyScript fires timed steps in order at their time", "[common][keyscript]") {
    Runner runner = MakeRunner("1:4,1.5:-4,5:79");
    CHECK(runner.Poll(At(0.0)).fired.empty());
    CHECK(runner.Poll(At(1.0)).fired == std::vector<size_t>{0});
    const PollResult late = runner.Poll(At(9.0));
    CHECK(late.fired == std::vector<size_t>{1, 2});
    CHECK_FALSE(late.timed_out.has_value());
    CHECK(runner.GetProgress().finished);
    const auto records = runner.Records();
    CHECK(records[1].status == Status::Fired);
    CHECK(records[1].fired_seconds == 9.0);
}

TEST_CASE("KeyScript waits for the present counter and for frames", "[common][keyscript]") {
    Runner runner = MakeRunner("f100:4,frame>=50:-4");
    CHECK(runner.Poll(At(1.0, 40, std::nullopt, 99)).fired.empty());
    CHECK(runner.Poll(At(2.0, 40, std::nullopt, 100)).fired == std::vector<size_t>{0});
    CHECK(runner.Poll(At(3.0, 49)).fired.empty());
    CHECK(runner.Poll(At(4.0, 50)).fired == std::vector<size_t>{1});
}

TEST_CASE("KeyScript steps wait for the step before them", "[common][keyscript]") {
    Runner runner = MakeRunner("frame>=100:4,1:-4");
    // The second step's time has passed, but the first is still waiting.
    CHECK(runner.Poll(At(10.0, 99)).fired.empty());
    CHECK(runner.Poll(At(11.0, 100)).fired == std::vector<size_t>{0, 1});
}

TEST_CASE("KeyScript after first_frame counts from the first frame", "[common][keyscript]") {
    Runner runner = MakeRunner("after first_frame+2:4");
    CHECK(runner.Poll(At(5.0, 0)).fired.empty());
    CHECK(runner.Poll(At(10.0, 1)).fired.empty());
    CHECK(runner.Poll(At(11.9, 5)).fired.empty());
    CHECK(runner.Poll(At(12.0, 6)).fired == std::vector<size_t>{0});
}

TEST_CASE("KeyScript after prev counts from the previous step", "[common][keyscript]") {
    Runner runner = MakeRunner("after prev+1:4,after prev+2:-4");
    CHECK(runner.Poll(At(100.0)).fired.empty()); // the first poll is the start
    CHECK(runner.Poll(At(101.0)).fired == std::vector<size_t>{0});
    CHECK(runner.Poll(At(102.9)).fired.empty());
    CHECK(runner.Poll(At(103.0)).fired == std::vector<size_t>{1});
}

TEST_CASE("KeyScript shaders_idle needs a quiet queue after the first frame", "[common][keyscript]") {
    Runner runner = MakeRunner("after shaders_idle+3:4");
    CHECK(runner.Poll(At(0.0, 0, 0)).fired.empty());   // quiet, but nothing presented yet
    CHECK(runner.Poll(At(4.0, 1, 0)).fired.empty());   // quiet counts from the first frame
    CHECK(runner.Poll(At(5.0, 2, 2)).fired.empty());   // a build starts: the quiet restarts
    CHECK(runner.Poll(At(6.0, 3, 0)).fired.empty());
    CHECK(runner.Poll(At(8.9, 4, 0)).fired.empty());
    CHECK(runner.Poll(At(9.0, 5, 0)).fired == std::vector<size_t>{0});
}

TEST_CASE("KeyScript shaders_idle does not count an unknown queue as quiet", "[common][keyscript]") {
    Runner runner = MakeRunner("after shaders_idle+1:4");
    CHECK(runner.Poll(At(0.0, 1, std::nullopt)).fired.empty());
    CHECK(runner.Poll(At(50.0, 100, std::nullopt)).fired.empty());
    CHECK(runner.Poll(At(51.0, 101, 0)).fired.empty());
    CHECK(runner.Poll(At(52.0, 102, 0)).fired == std::vector<size_t>{0});
}

TEST_CASE("KeyScript gives up when an event step times out", "[common][keyscript]") {
    Runner runner = MakeRunner("1:4,frame>=1000@10:-4,20:79");
    CHECK(runner.Poll(At(1.0, 0)).fired == std::vector<size_t>{0});
    CHECK(runner.Poll(At(10.9, 5)).fired.empty());
    const PollResult result = runner.Poll(At(11.0, 5));
    REQUIRE(result.timed_out.has_value());
    CHECK(*result.timed_out == 1);
    CHECK(runner.GetProgress().finished);
    const auto records = runner.Records();
    CHECK(records[0].status == Status::Fired);
    CHECK(records[1].status == Status::TimedOut);
    CHECK(records[2].status == Status::Skipped);
    // Nothing fires after a step gave up, whatever the clock says.
    const PollResult after = runner.Poll(At(30.0, 5000));
    CHECK(after.fired.empty());
    CHECK_FALSE(after.timed_out.has_value());
}

TEST_CASE("KeyScript timeout counts from when the step became the next one", "[common][keyscript]") {
    Runner runner = MakeRunner("frame>=10@5:4,frame>=20@5:-4");
    CHECK(runner.Poll(At(0.0, 0)).fired.empty());
    CHECK(runner.Poll(At(4.0, 10)).fired == std::vector<size_t>{0});
    CHECK(runner.Poll(At(8.9, 11)).fired.empty());
    CHECK(runner.Poll(At(9.0, 12)).timed_out == std::optional<size_t>{1});
}

TEST_CASE("KeyScript a step that is due at its timeout fires", "[common][keyscript]") {
    Runner runner = MakeRunner("frame>=10@5:4");
    CHECK(runner.Poll(At(0.0, 0)).fired.empty());
    const PollResult result = runner.Poll(At(5.0, 10));
    CHECK(result.fired == std::vector<size_t>{0});
    CHECK_FALSE(result.timed_out.has_value());
}

TEST_CASE("KeyScript an event step has a default timeout, a timed step none", "[common][keyscript]") {
    Runner event = MakeRunner("frame>=10:4");
    CHECK(event.Poll(At(0.0, 0)).fired.empty());
    CHECK(event.Poll(At(DefaultEventTimeoutSeconds - 1.0, 0)).fired.empty());
    CHECK(event.Poll(At(DefaultEventTimeoutSeconds, 0)).timed_out.has_value());

    Runner timed = MakeRunner("100000:4");
    CHECK(timed.Poll(At(0.0)).fired.empty());
    CHECK_FALSE(timed.Poll(At(99999.0)).timed_out.has_value());
}

TEST_CASE("KeyScript records when each step fired", "[common][keyscript]") {
    Runner runner = MakeRunner("frame>=3:4");
    Observation now = At(2.5, 3);
    now.run_seconds = 12.25;
    CHECK(runner.Poll(now).fired == std::vector<size_t>{0});
    const auto records = runner.Records();
    REQUIRE(records.size() == 1);
    CHECK(records[0].when == "frame>=3");
    CHECK(records[0].fired_seconds == 2.5);
    CHECK(records[0].fired_run_seconds == 12.25);
    CHECK(records[0].fired_frames == 3);
    CHECK(StatusName(records[0].status) == "fired");
}
