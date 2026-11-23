// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <string>

#include <catch2/catch_test_macros.hpp>

#include "common/common_types.h"
#include "video_core/nx2_profile.h"

namespace {

namespace Profile = VideoCommon::Nx2Profile;

} // Anonymous namespace

TEST_CASE("Nx2Profile callee names: Clang shows the lambda's file and line", "[video_core]") {
    CHECK(Profile::TrimCalleeName(
              "static const char *VideoCommon::GpuSplit::CalleeSignature() [Func = (lambda at "
              "C:/src/video_core/engines/maxwell_3d.cpp:2390:9)]") ==
          "lambda at maxwell_3d.cpp:2390");
    CHECK(Profile::TrimCalleeName(
              "static const char *VideoCommon::GpuSplit::CalleeSignature() [Func = (lambda at "
              "C:\\src\\video_core\\gpu_split.cpp:12:5)]") == "lambda at gpu_split.cpp:12");
}

TEST_CASE("Nx2Profile callee names: GCC keeps the enclosing function", "[video_core]") {
    CHECK(Profile::TrimCalleeName(
              "static const char* VideoCommon::GpuSplit::CalleeSignature() [with Func = "
              "Tegra::Engines::Maxwell3D::FrontDmaRead(VideoCommon::GpuSplit&, int)::<lambda()>]") ==
          "Tegra::Engines::Maxwell3D::FrontDmaRead()::<lambda()>");
}

TEST_CASE("Nx2Profile callee names: MSVC drops the calling convention and scope marks",
          "[video_core]") {
    CHECK(Profile::TrimCalleeName(
              "const char *__cdecl VideoCommon::GpuSplit::CalleeSignature<class `void __cdecl "
              "Tegra::Engines::Maxwell3D::FrontDmaRead(void)'::`2'::<lambda_1>>(void)") ==
          "void Tegra::Engines::Maxwell3D::FrontDmaRead()::<lambda_1>");
}

TEST_CASE("Nx2Profile callee names: long names keep their end", "[video_core]") {
    const std::string tail(200, 'a');
    const std::string name = Profile::TrimCalleeName(
        "static const char* VideoCommon::GpuSplit::CalleeSignature() [with Func = " + tail + "]");
    CHECK(name.size() == 120);
    CHECK(name.substr(0, 3) == "...");
    CHECK(name.substr(3) == tail.substr(tail.size() - 117));
}

TEST_CASE("Nx2Profile callees: 64 slots, then one for the rest", "[video_core]") {
    auto& data = Profile::t_data;
    data = {};
    for (u64 thunk = 1; thunk <= 70; ++thunk) {
        Profile::AddCallee(thunk * 16, 10, 5, 32);
    }
    // A callee that has a slot keeps accumulating there.
    Profile::AddCallee(16, 10, 5, 32);
    u64 slots = 0;
    u64 calls = 0;
    for (const Profile::CalleeStat& callee : data.callees) {
        if (callee.calls != 0) {
            ++slots;
            calls += callee.calls;
        }
    }
    CHECK(slots == u64{64});
    CHECK(data.other_calls.calls == u64{6 * 32});
    CHECK(data.other_calls.exclusive_ns == u64{6 * 5});
    CHECK(calls + data.other_calls.calls == u64{71 * 32});
    data = {};
}

TEST_CASE("Nx2Profile back entries: counts scaled, writes only for state runs", "[video_core]") {
    auto& data = Profile::t_data;
    data = {};
    Profile::AddBackEntry(VideoCommon::BackEntry::StateRun, 10, 10, 32);
    Profile::AddBackEntry(VideoCommon::BackEntry::Call, 4, 3, 32);
    const auto counter = [&data](Profile::Counter which) {
        return data.counters[static_cast<size_t>(which)];
    };
    CHECK(counter(Profile::Counter::BackEntryStateRun) == u64{32});
    CHECK(counter(Profile::Counter::BackWordsStateRun) == u64{320});
    CHECK(counter(Profile::Counter::BackStateWrites) == u64{320});
    CHECK(counter(Profile::Counter::BackEntryCall) == u64{32});
    CHECK(counter(Profile::Counter::BackWordsCall) == u64{128});
    CHECK(counter(Profile::Counter::BackEntryOther) == u64{0});
    data = {};
}

TEST_CASE("Nx2Profile methods: method 0 is a row, the 65th key goes to the others",
          "[video_core]") {
    auto& data = Profile::t_data;
    data = {};
    // Method 0 of the first engine has key 0: it must still take a row of its own.
    for (u32 key = 0; key < 65; ++key) {
        Profile::AddMethod(key, 10, 5, 32);
        Profile::AddMethod(key, 10, 5, 32);
    }
    u64 rows = 0;
    for (const Profile::CalleeStat& row : data.methods) {
        if (row.calls != 0) {
            ++rows;
            CHECK(row.calls == u64{2 * 32});
            CHECK(row.exclusive_ns == u64{2 * 5});
            CHECK((row.key & Profile::MethodKeyTag) != 0);
        }
    }
    CHECK(rows == u64{64});
    CHECK(data.other_methods.calls == u64{2 * 32});
    CHECK(data.other_calls.calls == u64{0});
    data = {};
}

TEST_CASE("Nx2Profile feedback probe: runs, repeats and what rendered in between",
          "[video_core]") {
    Profile::FeedbackProbe probe;
    CHECK(probe.Format().empty());

    constexpr u64 target = 0x1000;
    const auto copy = [&](bool first_in_draw, bool repeat_in_draw) {
        probe.Add({.target = target,
                   .view = 7,
                   .width = 880,
                   .height = 880,
                   .format = 52,
                   .regions = 2,
                   .pixels = 1'000'000,
                   .first_in_draw = first_in_draw,
                   .repeat_in_draw = repeat_in_draw,
                   .scissor_on = true,
                   .scissor_area = 0.5,
                   .viewport_area = 1.0});
    };
    // Three draws of one frame sample the target through the same view. The first has two copies
    // (the same view twice), the third had another draw render to the target after the second.
    probe.NoteDraw(1);
    probe.NoteTargetDraw(target);
    copy(true, false);
    copy(false, true);
    probe.NoteDraw(1);
    probe.NoteTargetDraw(target);
    copy(true, false);
    probe.NoteDraw(1);
    probe.NoteTargetDraw(target);
    probe.NoteTargetDraw(target);
    copy(true, false);

    CHECK(probe.frames == u64{1});
    CHECK(probe.draws == u64{3});
    CHECK(probe.copies == u64{4});
    CHECK(probe.regions_max == u64{2});
    CHECK(probe.repeats_in_draw == u64{1});
    CHECK(probe.distinct_sum == u64{1});
    CHECK(probe.repeat_copies == u64{3});
    CHECK(probe.other_written == u64{1});
    CHECK(probe.runs == u64{1});
    CHECK(probe.run_draws == u64{3});
    CHECK(probe.run_max == u64{3});

    const std::string line = probe.Format();
    CHECK(line.starts_with("Feedback shadow copies per frame: 4.0 copies in 3.0 draws, 4.0 MP "
                           "(1.0 repeat a view the draw already copied)"));
    CHECK(line.find("regions per CopyImage: mean 2.00, max 2") != std::string::npos);
    CHECK(line.find("1.0 distinct targets (top by MP: 880x880 format 52: 4.0 copies, 4.0 MP)") !=
          std::string::npos);
    CHECK(line.find("scissor on for 4.0 copies, mean 0.50 of the target") != std::string::npos);
    CHECK(line.find("viewport mean 1.00 of the target") != std::string::npos);
    CHECK(line.find("runs of mean 3.00, max 3 draws") != std::string::npos);
    CHECK(line.find("rendered to by another draw between copies: 1.0 of 3.0 repeat copies") !=
          std::string::npos);

    // A second frame: the counts are per frame.
    probe.NoteDraw(2);
    probe.NoteTargetDraw(target);
    copy(true, false);
    CHECK(probe.frames == u64{2});
    CHECK(probe.distinct_sum == u64{2});
    CHECK(probe.Format().starts_with("Feedback shadow copies per frame: 2.5 copies in 2.0 draws"));
}
