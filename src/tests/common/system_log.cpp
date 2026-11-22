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
    const Common::SystemLog::CrashInfo crash{
        .code = 0xC0000005ULL, .address = 0x1234, .thread_id = 77, .module = "eglantine.exe"};
    Common::SystemLog::DumpCrashTail(crash_path.c_str(), &crash);

    const auto incident_rows = ReadJsonLines(incident_path);
    REQUIRE(incident_rows.size() == 2);
    REQUIRE(incident_rows[0]["type"] == "header");
    REQUIRE(incident_rows[0]["reason"] == "unit_test");
    REQUIRE(incident_rows[0]["marker_id"] == 7);
    REQUIRE(incident_rows[0].contains("missing_sources"));
    REQUIRE(incident_rows[1]["type"] == "event");
    REQUIRE(incident_rows[1]["kind"] == "GpuDeviceLost");

    const auto crash_rows = ReadJsonLines(crash_path);
    REQUIRE(crash_rows.size() == 3);
    REQUIRE(crash_rows[0]["type"] == "crash_tail");
    REQUIRE(crash_rows[1]["type"] == "event");
    REQUIRE(crash_rows[1]["kind"] == "GpuDeviceLost");
    REQUIRE(crash_rows[2]["type"] == "event");
    REQUIRE(crash_rows[2]["kind"] == "HostCrash");
    REQUIRE(crash_rows[2]["arg0"] == 0xC0000005ULL);
    REQUIRE(crash_rows[2]["thread_id"] == 77);
    REQUIRE(crash_rows[2]["detail"] == "eglantine.exe");

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
        const std::string_view detail{ev.detail};
        // The class and the source line, never the message text.
        REQUIRE(detail.find("synthetic") == std::string_view::npos);
        REQUIRE(detail.find("123") == std::string_view::npos);
        REQUIRE(detail.find("456") == std::string_view::npos);
        if (ev.kind == Common::SystemLog::EventKind::LogWarning &&
            detail.starts_with("Common:system_log.cpp:")) {
            found_warn = true;
        }
        if (ev.kind == Common::SystemLog::EventKind::LogError &&
            detail.starts_with("Common:system_log.cpp:")) {
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
        REQUIRE(summary["before_window"]["contains_mark"] == true);
        REQUIRE(summary["aftermath"]["scheduled"] == true);
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
    REQUIRE(after_rows.size() >= 2);
    REQUIRE(after_rows[0]["type"] == "header");
    REQUIRE(after_rows[0]["window"]["mode"] == "after_seq");
    REQUIRE(after_rows[0]["window"]["anchor_seq"] == res.seq);
    bool saw_after = false;
    for (const auto& row : after_rows) {
        if (row["type"] != "event") {
            continue;
        }
        // Only what came after the mark: not the event recorded before it, not the mark itself.
        REQUIRE(row["seq"].get<u64>() > res.seq);
        REQUIRE(row["detail"] != "before_mark");
        if (row["kind"] == "AudioUnderrun") {
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

TEST_CASE("SystemLog tap of a long message records where it came from and none of its text",
          "[common][system_log]") {
    Common::Log::Initialize();
    Common::Log::Filter filter{Common::Log::Level::Info};
    Common::Log::SetGlobalFilter(filter);
    Common::SystemLog::ResetForTests(64);

    // Longer than the logger's stack buffer: the formatted text lives in a temporary that is gone
    // before the tap runs, and it must not be what the tap reads.
    const std::string secret = "SECRET-guest-value-" + std::string(2 * BUFSIZ, 'x');
    LOG_WARNING(Common, "dump {}", secret);
    const u32 expected_line = __LINE__ - 1;

    const auto snap = Common::SystemLog::Snapshot(0, 64);
    bool found = false;
    for (const auto& ev : snap.events) {
        if (ev.kind != Common::SystemLog::EventKind::LogWarning) {
            continue;
        }
        found = true;
        const std::string_view detail{ev.detail};
        REQUIRE(detail.starts_with("Common:system_log.cpp:"));
        REQUIRE(detail.find("SECRET") == std::string_view::npos);
        REQUIRE(detail.find("dump") == std::string_view::npos);
        REQUIRE(detail.find("xxxx") == std::string_view::npos);
        REQUIRE(ev.arg0 == static_cast<u64>(Common::Log::Class::Common));
        REQUIRE(ev.arg1 == expected_line);
    }
    REQUIRE(found);
    Common::SystemLog::ResetForTests(8192);
}

TEST_CASE("SystemLog quick mark window ends at the mark however much the ring has seen",
          "[common][system_log]") {
    Common::SystemLog::ResetForTests(32);
    for (u64 i = 1; i <= 100; ++i) {
        Common::SystemLog::Record(Common::SystemLog::Subsystem::Gpu,
                                  Common::SystemLog::Severity::Warning,
                                  Common::SystemLog::EventKind::GpuFenceWait, i, 0, 0, 0, "pre");
    }
    const auto root = std::filesystem::temp_directory_path() / "eglantine_quick_mark_window_test";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);

    const auto res = Common::SystemLog::TriggerQuickMark(root, "window", 0);
    REQUIRE(res.ok);
    REQUIRE(res.seq == 101);
    // Events recorded after the mark must not push the mark out of its own before file.
    for (u64 i = 1; i <= 50; ++i) {
        Common::SystemLog::Record(Common::SystemLog::Subsystem::Gpu,
                                  Common::SystemLog::Severity::Warning,
                                  Common::SystemLog::EventKind::GpuFenceWait, i, 0, 0, 0, "post");
    }

    const auto before = ReadJsonLines(res.before_log);
    REQUIRE(before.size() == 1 + 32);
    REQUIRE(before[0]["window"]["mode"] == "newest_through_seq");
    REQUIRE(before[0]["window"]["anchor_seq"] == res.seq);
    REQUIRE(before.back()["type"] == "event");
    REQUIRE(before.back()["seq"] == res.seq);
    REQUIRE(before.back()["kind"] == "Marker");
    for (std::size_t i = 1; i < before.size(); ++i) {
        REQUIRE(before[i]["seq"].get<u64>() <= res.seq);
    }

    const auto summary = ReadJsonLines(res.summary_path);
    REQUIRE(summary.size() == 1);
    REQUIRE(summary[0]["before_window"]["contains_mark"] == true);
    REQUIRE(summary[0]["before_window"]["through_seq"] == res.seq);
    REQUIRE(summary[0]["observed"]["events_in_ring"].get<u64>() == 32);
    REQUIRE(summary[0]["aftermath"]["requested"] == false);

    std::filesystem::remove_all(root, ec);
    Common::SystemLog::ResetForTests(8192);
}

TEST_CASE("SystemLog snapshots report what the bound left out", "[common][system_log]") {
    Common::SystemLog::ResetForTests(16);
    for (u64 i = 1; i <= 40; ++i) {
        Common::SystemLog::Record(Common::SystemLog::Subsystem::Audio,
                                  Common::SystemLog::Severity::Info,
                                  Common::SystemLog::EventKind::AudioUnderrun, i);
    }
    // The ring holds seq 25..40.
    const auto through = Common::SystemLog::SnapshotThrough(30, 4);
    REQUIRE(through.events.size() == 4);
    REQUIRE(through.events.front().seq == 27);
    REQUIRE(through.events.back().seq == 30);
    REQUIRE(through.omitted == 2);

    const auto after = Common::SystemLog::Snapshot(10, 5);
    REQUIRE(after.events.size() == 5);
    REQUIRE(after.events.front().seq == 25);
    REQUIRE(after.events.back().seq == 29);
    REQUIRE(after.omitted == 11);
    REQUIRE(after.lost == 14);
    Common::SystemLog::ResetForTests(8192);
}

TEST_CASE("SystemLog QueryEvents reaches a match behind more than 4096 non-matches",
          "[common][system_log]") {
    Common::SystemLog::ResetForTests(16384);
    for (u64 i = 1; i <= 5000; ++i) {
        Common::SystemLog::Record(Common::SystemLog::Subsystem::Audio,
                                  Common::SystemLog::Severity::Info,
                                  Common::SystemLog::EventKind::AudioUnderrun, i);
    }
    for (u64 i = 0; i < 3; ++i) {
        Common::SystemLog::Record(Common::SystemLog::Subsystem::Gpu,
                                  Common::SystemLog::Severity::Error,
                                  Common::SystemLog::EventKind::GpuError, i);
    }

    Common::SystemLog::EventQuery query;
    query.filter_subsystem = true;
    query.subsystem = Common::SystemLog::Subsystem::Gpu;
    query.limit = 2;
    const auto first = Common::SystemLog::QueryEvents(query);
    REQUIRE(first.events.size() == 2);
    REQUIRE(first.events[0].seq == 5001);
    REQUIRE(first.events[1].seq == 5002);
    REQUIRE(first.more);
    REQUIRE(first.lost == 0);

    query.after_seq = first.next;
    const auto second = Common::SystemLog::QueryEvents(query);
    REQUIRE(second.events.size() == 1);
    REQUIRE(second.events[0].seq == 5003);
    REQUIRE_FALSE(second.more);
    REQUIRE(second.next == 5003);

    // A severity filter that matches nothing still advances the cursor to the newest row.
    Common::SystemLog::EventQuery none;
    none.min_severity = Common::SystemLog::Severity::Critical;
    const auto empty = Common::SystemLog::QueryEvents(none);
    REQUIRE(empty.events.empty());
    REQUIRE_FALSE(empty.more);
    REQUIRE(empty.next == 5003);
    Common::SystemLog::ResetForTests(8192);
}

TEST_CASE("SystemLog detail truncation never splits or emits invalid UTF-8",
          "[common][system_log]") {
    Common::SystemLog::ResetForTests(16);
    // 94 bytes, then a two-byte character that would straddle the 95-byte limit.
    const std::string straddle = std::string(94, 'a') + "\xC3\xA9" + "tail";
    Common::SystemLog::Record(Common::SystemLog::Subsystem::Filesystem,
                              Common::SystemLog::Severity::Info,
                              Common::SystemLog::EventKind::FsError, 0, 0, 0, 0, straddle);
    // A stray continuation byte, a truncated sequence, and an overlong one.
    Common::SystemLog::Record(Common::SystemLog::Subsystem::Filesystem,
                              Common::SystemLog::Severity::Info,
                              Common::SystemLog::EventKind::FsError, 0, 0, 0, 0,
                              "ab\x80" "cd\xE2\x82" "ef\xC0\xAF");

    const auto snap = Common::SystemLog::Snapshot(0, 16);
    REQUIRE(snap.events.size() == 2);
    REQUIRE(std::string_view{snap.events[0].detail} == std::string(94, 'a'));
    REQUIRE(std::string_view{snap.events[1].detail}.starts_with("ab\xEF\xBF\xBD" "cd"));

    // Whatever the bytes were, the exported rows parse as JSON (the parser rejects bad UTF-8).
    const auto path = std::filesystem::temp_directory_path() / "eglantine_utf8_test.jsonl";
    REQUIRE(Common::SystemLog::WriteIncidentJsonl(path, "utf8", 1));
    const auto rows = ReadJsonLines(path);
    REQUIRE(rows.size() == 3);
    REQUIRE(rows[1]["detail"] == std::string(94, 'a'));
    std::error_code ec;
    std::filesystem::remove(path, ec);
    Common::SystemLog::ResetForTests(8192);
}
