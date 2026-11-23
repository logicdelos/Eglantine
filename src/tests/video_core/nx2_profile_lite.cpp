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
