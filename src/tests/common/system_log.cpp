// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "common/logging.h"
#include "common/nx2_bottleneck.h"
#include "common/system_log.h"

namespace {

std::vector<nlohmann::json> ReadJsonLines(const std::filesystem::path& path) {
    std::vector<nlohmann::json> rows;
    std::ifstream in{path};
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty()) {
            rows.push_back(nlohmann::json::parse(line));
        }
    }
    return rows;
}

} // Anonymous namespace

TEST_CASE("SystemLog ring wrap-around, eviction, and present_id", "[common][system_log]") {
    Common::SystemLog::ResetForTests(4);
    std::atomic<u64> presents{42};
    Common::SystemLog::SetPresentCounter(&presents);

    for (u64 i = 1; i <= 6; ++i) {
        const u64 seq = Common::SystemLog::Record(
            Common::SystemLog::Subsystem::Gpu, Common::SystemLog::Severity::Warning,
            Common::SystemLog::EventKind::GpuFenceWait, i, i * 1000, 0, -1, "wait");
        REQUIRE(seq == i);
    }

    const auto snap = Common::SystemLog::Snapshot(0, 100);
    REQUIRE(snap.status.capacity == 4);
    REQUIRE(snap.status.newest == 6);
    REQUIRE(snap.status.oldest == 3);
    REQUIRE(snap.status.evicted == 2);
    REQUIRE(snap.status.dropped == 0);
    REQUIRE(snap.events.size() == 4);
    REQUIRE(snap.events.front().seq == 3);
    REQUIRE(snap.events.back().seq == 6);
    REQUIRE(snap.events.back().present_id == 42);
    REQUIRE(std::string{snap.events.back().detail} == "wait");

    const auto tail = Common::SystemLog::Snapshot(4, 100);
    REQUIRE(tail.events.size() == 2);
    REQUIRE(tail.events[0].seq == 5);
    REQUIRE(tail.events[1].seq == 6);

    Common::SystemLog::SetPresentCounter(nullptr);
    Common::SystemLog::ResetForTests(8192);
}

TEST_CASE("SystemLog concurrent writers stay monotonic and bounded", "[common][system_log]") {
    Common::SystemLog::ResetForTests(64);
    constexpr int kThreads = 4;
    constexpr int kPerThread = 200;

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([t] {
            for (int i = 0; i < kPerThread; ++i) {
                Common::SystemLog::Record(Common::SystemLog::Subsystem::Audio,
                                          Common::SystemLog::Severity::Warning,
                                          Common::SystemLog::EventKind::AudioUnderrun,
                                          static_cast<u64>(t), static_cast<u64>(i));
            }
        });
    }
    for (auto& th : threads) {
        th.join();
    }

    const auto snap = Common::SystemLog::Snapshot(0, 100);
    REQUIRE(snap.status.newest == static_cast<u64>(kThreads * kPerThread));
    REQUIRE(snap.status.evicted == static_cast<u64>(kThreads * kPerThread - 64));
    REQUIRE(snap.events.size() == 64);
    for (std::size_t i = 1; i < snap.events.size(); ++i) {
        REQUIRE(snap.events[i].seq == snap.events[i - 1].seq + 1);
    }

    Common::SystemLog::ResetForTests(8192);
}

TEST_CASE("SystemLog sources report missing diagnostic feeds explicitly", "[common][system_log]") {
    Common::SystemLog::ResetForTests(16);
    const auto sources = Common::SystemLog::GetSources();
    REQUIRE_FALSE(sources.empty());
    bool saw_native = false;
    bool saw_gpu_trace = false;
    for (const auto& s : sources) {
        if (std::string_view{s.name} == "native_log_warnings_errors") {
            saw_native = true;
            REQUIRE(s.enabled);
        }
        if (std::string_view{s.name} == "gpu_trace") {
            saw_gpu_trace = true;
            if (!s.enabled) {
                REQUIRE(std::string_view{s.missing_reason} == "diagnostic_off");
            }
        }
    }
    REQUIRE(saw_native);
    REQUIRE(saw_gpu_trace);
    Common::SystemLog::ResetForTests(8192);
}

TEST_CASE("SystemLog WriteIncidentJsonl and DumpCrashTail produce valid JSONL",
          "[common][system_log]") {
    Common::SystemLog::ResetForTests(32);
    Common::SystemLog::Record(Common::SystemLog::Subsystem::Graphics,
                              Common::SystemLog::Severity::Critical,
                              Common::SystemLog::EventKind::GpuDeviceLost, 1, 2, 3, -4,
                              "device \"lost\"\nline");

    const auto dir = std::filesystem::temp_directory_path() / "eglantine_system_log_test";
    std::filesystem::create_directories(dir);
    const auto incident_path = dir / "incident.jsonl";
    const auto crash_path = dir / "crash_tail.jsonl";

    REQUIRE(Common::SystemLog::WriteIncidentJsonl(incident_path, "unit_test", 7));
    Common::SystemLog::DumpCrashTail(crash_path);

    const auto incident_rows = ReadJsonLines(incident_path);
    REQUIRE(incident_rows.size() == 2);
    REQUIRE(incident_rows[0]["type"] == "header");
    REQUIRE(incident_rows[0]["reason"] == "unit_test");
    REQUIRE(incident_rows[0]["marker_id"] == 7);
    REQUIRE(incident_rows[0].contains("missing_sources"));
    REQUIRE(incident_rows[1]["type"] == "event");
    REQUIRE(incident_rows[1]["kind"] == "GpuDeviceLost");

    const auto crash_rows = ReadJsonLines(crash_path);
    REQUIRE(crash_rows.size() == 2);
    REQUIRE(crash_rows[0]["type"] == "crash_tail");
    REQUIRE(crash_rows[1]["type"] == "event");
    REQUIRE(crash_rows[1]["kind"] == "GpuDeviceLost");

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    Common::SystemLog::ResetForTests(8192);
}

TEST_CASE("SystemLog taps LOG_WARNING and LOG_ERROR automatically", "[common][system_log]") {
    Common::Log::Initialize();
    Common::Log::Filter filter{Common::Log::Level::Info};
    Common::Log::SetGlobalFilter(filter);
    Common::SystemLog::ResetForTests(64);

    LOG_WARNING(Common, "synthetic warning {}", 123);
    LOG_ERROR(Common, "synthetic error {}", 456);

    const auto snap = Common::SystemLog::Snapshot(0, 64);
    bool found_warn = false;
    bool found_err = false;
    for (const auto& ev : snap.events) {
        if (ev.kind == Common::SystemLog::EventKind::LogWarning &&
            std::string_view{ev.detail}.find("synthetic warning 123") != std::string_view::npos) {
            found_warn = true;
        }
        if (ev.kind == Common::SystemLog::EventKind::LogError &&
            std::string_view{ev.detail}.find("synthetic error 456") != std::string_view::npos) {
            found_err = true;
        }
    }
    REQUIRE(found_warn);
    REQUIRE(found_err);
    Common::SystemLog::ResetForTests(8192);
}

TEST_CASE("SystemLog TriggerQuickMark writes before, summary, and aftermath bundles",
          "[common][system_log]") {
    Common::SystemLog::ResetForTests(64);
    Common::SystemLog::Record(Common::SystemLog::Subsystem::Gpu,
                              Common::SystemLog::Severity::Warning,
                              Common::SystemLog::EventKind::GpuFenceWait, 10, 20000, 0, 0,
                              "before_mark");

    const auto root = std::filesystem::temp_directory_path() / "eglantine_quick_mark_test";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);

    const auto res = Common::SystemLog::TriggerQuickMark(root, "glitch_check", 50);
    REQUIRE(res.ok);
    REQUIRE(std::filesystem::is_regular_file(res.before_log));
    REQUIRE(std::filesystem::is_regular_file(res.summary_path));
    {
        const auto summary_rows = ReadJsonLines(res.summary_path);
        REQUIRE(summary_rows.size() == 1);
        const auto& summary = summary_rows[0];
        REQUIRE(summary.contains("observed"));
        REQUIRE(summary["observed"]["mark_id"] == res.mark_id);
        REQUIRE(summary["observed"]["events_in_ring"].get<u64>() >= 1);
        REQUIRE(summary.contains("suspected_causes"));
        REQUIRE(summary["suspected_causes"].is_array());
        REQUIRE_FALSE(summary["suspected_causes"].empty());
        REQUIRE(summary["suspected_causes"][0]["confidence"] == "unknown");
    }

    Common::SystemLog::Record(Common::SystemLog::Subsystem::Audio,
                              Common::SystemLog::Severity::Warning,
                              Common::SystemLog::EventKind::AudioUnderrun, 3, 0, 0, 0,
                              "after_mark");

    const auto after_path = res.directory / "system_events_after.jsonl";
    for (int i = 0; i < 40 && !std::filesystem::is_regular_file(after_path); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds{25});
    }
    REQUIRE(std::filesystem::is_regular_file(after_path));
    const auto after_rows = ReadJsonLines(after_path);
    REQUIRE(after_rows.size() >= 3);
    bool saw_after = false;
    for (const auto& row : after_rows) {
        if (row["type"] == "event" && row["kind"] == "AudioUnderrun") {
            saw_after = true;
        }
    }
    REQUIRE(saw_after);

    std::filesystem::remove_all(root, ec);
    Common::SystemLog::ResetForTests(8192);
}

TEST_CASE("Nx2Bottleneck::NoteFrame emits FrameHitch with dominant wait on micro-stutter",
          "[common][system_log]") {
    Common::SystemLog::ResetForTests(64);
    Nx2Bottleneck::SetGuestRunning(true);

    for (int i = 0; i < 18; ++i) {
        Nx2Bottleneck::NoteFrame();
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
    }
    Nx2Bottleneck::AddWait(Nx2Bottleneck::Wait::ShaderBuild, 45'000'000ULL);
    std::this_thread::sleep_for(std::chrono::milliseconds{60});
    Nx2Bottleneck::NoteFrame();

    const auto snap = Common::SystemLog::Snapshot(0, 64);
    bool saw_hitch = false;
    for (const auto& ev : snap.events) {
        if (ev.kind == Common::SystemLog::EventKind::FrameHitch) {
            saw_hitch = true;
            REQUIRE(ev.arg0 >= 33000);
            REQUIRE(std::string_view{ev.detail} == "ShaderBuild");
        }
    }
    REQUIRE(saw_hitch);
    Common::SystemLog::ResetForTests(8192);
}
