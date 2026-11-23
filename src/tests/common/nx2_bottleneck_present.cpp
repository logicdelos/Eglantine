// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <chrono>
#include <string>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "common/nx2_bottleneck.h"

TEST_CASE("Nx2Bottleneck: time in the present call is the graphics card, not the thread's work",
          "[common][nx2_bottleneck]") {
    using namespace Nx2Bottleneck;
    BeginRecording(Now());
    SetGuestRunning(true);
    MarkThread(Thread::GpuBack);
    // The back thread ran, with no time idle; far more of the window than it lasted went to the
    // present call, so all of its busy time is that wait.
    BeginIdle(Thread::GpuBack);
    EndIdle(Thread::GpuBack);
    std::this_thread::sleep_for(std::chrono::milliseconds{80});
    AddWait(Wait::HostGpuPresent, 10'000'000'000ULL);
    NoteFrame();
    SetGuestRunning(false);
    const RunRecording recording = EndRecording(false);

    const ThreadSplit& back = recording.summary.threads[static_cast<size_t>(Thread::GpuBack)];
    REQUIRE(back.seen);
    CHECK(back.waits_ms[static_cast<size_t>(Wait::HostGpuPresent)] >= 10'000.0);
    CHECK(back.work_ms == 0.0);
    CHECK(recording.summary.limiter.find("the graphics card (the GPU back thread waits ") !=
          std::string::npos);
    CHECK(recording.summary.limiter.find("of its time in the present call for it)") !=
          std::string::npos);
}
