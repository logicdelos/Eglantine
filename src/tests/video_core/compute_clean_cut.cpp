// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// When the compute engine is at a clean cut of a replay checkpoint. A native (Switch 2) engine
// launches through the QMD control methods and never writes the launch register, so an inline
// upload it makes must not count as one that waits for a launch; a legacy engine keeps that
// refusal until the launch comes, and an upload still being fed is refused in both.

#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "core/core.h"
#include "core/device_memory.h"
#include "tests/core/system_test_context.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/gpu.h"
#include "video_core/macro.h"
#include "video_core/memory_manager.h"
#include "video_core/renderer_null/null_rasterizer.h"

namespace {

using Tegra::Engines::KeplerCompute;

constexpr u32 LineLengthIn = 0x60;
constexpr u32 LineCount = 0x61;
constexpr u32 ExecUpload = 0x6C;
constexpr u32 DataUpload = 0x6D;
constexpr u32 Launch = 0xAF;
static_assert(KEPLER_COMPUTE_REG_INDEX(upload) == LineLengthIn);
static_assert(KEPLER_COMPUTE_REG_INDEX(exec_upload) == ExecUpload);
static_assert(KEPLER_COMPUTE_REG_INDEX(data_upload) == DataUpload);
static_assert(KEPLER_COMPUTE_REG_INDEX(launch) == Launch);

constexpr std::string_view WaitsForLaunch = "a compute upload waits for its launch";
constexpr std::string_view Unfinished = "an inline compute upload is unfinished";

std::string_view Blocker(const KeplerCompute& compute) {
    const char* const blocker = compute.CleanCutBlocker();
    return blocker != nullptr ? std::string_view{blocker} : std::string_view{};
}

struct Fixture {
    ScopedSystemTestContext context;
    Core::System system;
    Core::DeviceMemory physical;
    Tegra::MaxwellDeviceMemoryManager device_memory{physical};
    Tegra::MemoryManager memory{system, device_memory, 32, 1ULL << 31};
    Tegra::GPU gpu{system, false, false};
    Null::RasterizerNull rasterizer{gpu};
    Tegra::NativeMacroContext macros;
    KeplerCompute compute{memory};

    explicit Fixture(bool native) {
        compute.BindRasterizer(&rasterizer);
        if (native) {
            compute.SetNativeMacroMode(&macros.engine, macros.positions, &macros);
        }
    }

    /// A linear one-line upload of `bytes`, started with LAUNCH_DMA.
    void BeginUpload(u32 bytes) {
        compute.CallMethod(system, LineLengthIn, bytes, true);
        compute.CallMethod(system, LineCount, 1, true);
        compute.CallMethod(system, ExecUpload, 1, true);
    }

    /// `count` words of inline data; the last of the run ends the method run when `ends_run`.
    void SendWords(u32 count, bool ends_run) {
        for (u32 i = 0; i < count; ++i) {
            compute.CallMethod(system, DataUpload, 0x01010101U * (i + 1),
                               ends_run && i + 1 == count);
        }
    }
};

} // namespace

TEST_CASE("compute clean cut: a native engine's finished uploads never wait for a launch",
          "[video_core][replay][compute_clean_cut]") {
    Fixture f{true};
    REQUIRE(f.compute.IsNativeMode());
    REQUIRE(f.compute.CleanCutBlocker() == nullptr);
    // Far more uploads than any one frame holds, none followed by a launch: every one of them is
    // finished, so the engine is at a clean cut after each.
    for (u32 upload = 0; upload < 200; ++upload) {
        f.BeginUpload(16);
        f.SendWords(4, true);
        INFO("upload " << upload);
        REQUIRE(Blocker(f.compute).empty());
    }
}

TEST_CASE("compute clean cut: a native upload still being fed is refused until it is complete",
          "[video_core][replay][compute_clean_cut]") {
    Fixture f{true};
    f.BeginUpload(16);
    f.SendWords(2, true);
    CHECK(Blocker(f.compute) == Unfinished);
    f.SendWords(1, true);
    CHECK(Blocker(f.compute) == Unfinished);
    f.SendWords(1, true);
    CHECK(Blocker(f.compute).empty());
    // A new upload that is not finished is refused again, and a stale offset does not hide it.
    f.BeginUpload(8);
    CHECK(Blocker(f.compute) == Unfinished);
    f.SendWords(2, true);
    CHECK(Blocker(f.compute).empty());
}

TEST_CASE("compute clean cut: a legacy engine refuses until the launch that reads the upload",
          "[video_core][replay][compute_clean_cut]") {
    Fixture f{false};
    REQUIRE_FALSE(f.compute.IsNativeMode());
    REQUIRE(f.compute.CleanCutBlocker() == nullptr);
    f.BeginUpload(16);
    f.SendWords(4, true);
    CHECK(Blocker(f.compute) == WaitsForLaunch);
    f.BeginUpload(16);
    f.SendWords(4, true);
    CHECK(Blocker(f.compute) == WaitsForLaunch);
    // The launch reads the recorded uploads and empties the list.
    f.compute.CallMethod(f.system, Launch, 0, true);
    CHECK(Blocker(f.compute).empty());
}
