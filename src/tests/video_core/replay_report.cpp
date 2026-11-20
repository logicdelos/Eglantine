// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "video_core/replay/result.h"

namespace R = VideoCore::Replay;

namespace {

R::CensusReport CompleteCensus() {
    R::CensusReport report;
    report.audit_enabled = true;
    report.bootstrap_found = true;
    report.run_complete = true;
    report.closure_ok = true;
    R::CensusCounters& c = report.counters;
    c.resolver_calls = 12;
    c.discovered_targets = 300;
    c.resolved_targets = 300;
    c.instrumented_targets = 300;
    c.startup_calls = 100;
    c.frame_calls = 900;
    c.open_tail_calls = 0;
    c.total_calls = 1000;
    c.audited_calls = 1000;
    c.nx2_presents = 4;
    c.nx2_frames_closed = 4;
    c.frames_closed = 4;
    R::EvaluateCoverage(report);
    return report;
}

} // namespace

TEST_CASE("Replay report: a complete census passes every criterion", "[video_core][replay]") {
    const R::CensusReport report = CompleteCensus();
    for (const bool criterion : report.criteria) {
        REQUIRE(criterion);
    }
    REQUIRE(report.coverage_complete);
    REQUIRE(report.library_replay_ready);
}

TEST_CASE("Replay report: each criterion fails on its own counter", "[video_core][replay]") {
    const auto only_failing = [](const R::CensusReport& report, size_t index) {
        for (size_t i = 0; i < report.criteria.size(); ++i) {
            if (report.criteria[i] == (i == index)) {
                return false;
            }
        }
        return !report.coverage_complete;
    };

    R::CensusReport report = CompleteCensus();
    report.bootstrap_found = false;
    R::EvaluateCoverage(report);
    REQUIRE(only_failing(report, 0));

    report = CompleteCensus();
    report.counters.resolved_targets = 299;
    R::EvaluateCoverage(report);
    REQUIRE_FALSE(report.criteria[1]);
    REQUIRE_FALSE(report.coverage_complete);

    report = CompleteCensus();
    report.counters.instrumented_targets = 299;
    R::EvaluateCoverage(report);
    REQUIRE(only_failing(report, 2));

    report = CompleteCensus();
    report.counters.unknown_dispatch_calls = 1;
    R::EvaluateCoverage(report);
    REQUIRE(only_failing(report, 3));

    report = CompleteCensus();
    report.counters.open_tail_calls = 1;  // the sum no longer closes
    R::EvaluateCoverage(report);
    REQUIRE(only_failing(report, 4));

    report = CompleteCensus();
    report.counters.dropped_records = 1;
    R::EvaluateCoverage(report);
    REQUIRE(only_failing(report, 5));

    report = CompleteCensus();
    report.counters.legacy_presents = 1;  // a path that presented but never closed a frame
    R::EvaluateCoverage(report);
    REQUIRE(only_failing(report, 6));
}

TEST_CASE("Replay report: readiness needs a finished run and a closed library",
          "[video_core][replay]") {
    R::CensusReport report = CompleteCensus();
    report.run_complete = false;
    R::EvaluateCoverage(report);
    REQUIRE_FALSE(report.coverage_complete);
    REQUIRE_FALSE(report.library_replay_ready);

    report = CompleteCensus();
    report.counters.unsupported_argument_schemas = 1;
    R::EvaluateCoverage(report);
    REQUIRE(report.coverage_complete);
    REQUIRE_FALSE(report.library_replay_ready);

    report = CompleteCensus();
    report.closure_ok = false;
    R::EvaluateCoverage(report);
    REQUIRE(report.coverage_complete);
    REQUIRE_FALSE(report.library_replay_ready);

    // Without the independent audit the census never claims complete coverage.
    report = CompleteCensus();
    report.audit_enabled = false;
    R::EvaluateCoverage(report);
    REQUIRE_FALSE(report.coverage_complete);
}

TEST_CASE("Replay report: section documents", "[video_core][replay]") {
    const nlohmann::json census = R::ToJson(CompleteCensus());
    REQUIRE(census["version"] == 1);
    REQUIRE(census["schema"] == 2);
    REQUIRE(census["coverage_complete"] == true);
    REQUIRE(census["resolved_target_share"] == 1.0);
    REQUIRE(census["criteria"].size() == R::CensusCriterionCount);
    REQUIRE(census["criteria"]["no_unknown_dispatch"] == true);
    REQUIRE(census["counters"]["total_calls"] == 1000);

    R::CensusReport empty;
    REQUIRE(R::ToJson(empty)["resolved_target_share"].is_null());

    R::CaptureReport capture;
    capture.status = R::SectionStatus::Incomplete;
    capture.error = "frame budget exceeded";
    const nlohmann::json capture_json = R::ToJson(capture);
    REQUIRE(capture_json["status"] == "incomplete");
    REQUIRE(capture_json["run_complete"] == false);
    REQUIRE(capture_json["error"] == "frame budget exceeded");
    REQUIRE(R::ToJson(R::CaptureReport{})["error"].is_null());

    R::ReplayReport replay;
    replay.status = R::SectionStatus::Complete;
    replay.capture_session_id = 0xABCDULL;
    const nlohmann::json replay_json = R::ToJson(replay);
    REQUIRE(replay_json["capture_session_id"] == "000000000000abcd");

    R::FrameTimingReport timing;
    timing.source = "host_present";
    timing.lost = 2;
    REQUIRE(R::ToJson(timing)["lost"] == 2);

    R::TestHookReport hooks;
    hooks.env_error = "target missing";
    REQUIRE(R::ToJson(hooks)["env_error"] == "target missing");
    REQUIRE(R::ToJson(R::TestHookReport{})["target"].is_null());
}

TEST_CASE("Replay report: frame line and picture names", "[video_core][replay]") {
    R::ReplayFrameRecord record;
    record.frame_id = 42;
    record.width = 1280;
    record.height = 720;
    record.live_checksum = 0x1234;
    record.replay_checksum = 0x1234;
    record.replay_ns = 5000;
    record.call_count = (u64{1} << 40) + 3;
    record.effect_count = 2;
    const std::string line = R::ReplayFrameLine(record);
    REQUIRE(line.find('\n') == std::string::npos);

    const nlohmann::json parsed = nlohmann::json::parse(line);
    REQUIRE(parsed["schema"] == 2);
    REQUIRE(parsed["call_count"] == (u64{1} << 40) + 3);
    REQUIRE(parsed["effect_count"] == 2);
    REQUIRE(parsed["frame_id"] == 42);
    REQUIRE(parsed["identical"] == true);
    REQUIRE(parsed["live_checksum"] == "0000000000001234");
    REQUIRE(parsed["live_png"] == "live_00000042.png");
    REQUIRE(parsed["replay_png"] == "replay_00000042.png");
    REQUIRE(R::LivePictureName(42) == "live_00000042.png");
    REQUIRE(R::ReplayPictureName(7) == "replay_00000007.png");

    record.replay_checksum = 1;
    REQUIRE(nlohmann::json::parse(R::ReplayFrameLine(record))["identical"] == false);
}

TEST_CASE("Replay report: published sections", "[video_core][replay]") {
    R::ClearPublishedReports();
    REQUIRE_FALSE(R::PublishedCaptureReport().has_value());
    R::CaptureReport capture;
    capture.status = R::SectionStatus::Complete;
    R::PublishCaptureReport(capture);
    REQUIRE(R::PublishedCaptureReport().has_value());
    REQUIRE(R::PublishedCaptureReport()->status == R::SectionStatus::Complete);
    REQUIRE_FALSE(R::PublishedReplayReport().has_value());
    R::ClearPublishedReports();
    REQUIRE_FALSE(R::PublishedCaptureReport().has_value());
}
