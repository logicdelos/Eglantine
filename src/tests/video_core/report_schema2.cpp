// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <sstream>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "yuzu_cmd/run_control.h"
#include "yuzu_cmd/run_report.h"

namespace {

namespace Spikes = VideoCommon::Nx2Spikes;
namespace Envelope = VideoCommon::CacheEnvelope;
namespace Schema2 = RunReport::Schema2;
using Json = nlohmann::json;

constexpr size_t Disk = static_cast<size_t>(Spikes::Cause::Disk);
constexpr size_t Shader = static_cast<size_t>(Spikes::Cause::Shader);
constexpr size_t Waiting = static_cast<size_t>(Spikes::Cause::Waiting);
constexpr size_t Unknown = static_cast<size_t>(Spikes::Cause::Unknown);

Spikes::LongFrame MakeFrame(u64 id, u64 duration_ns) {
    Spikes::LongFrame frame;
    frame.id = id;
    frame.start_ns = 1'000'000'000;
    frame.end_ns = frame.start_ns + duration_ns;
    frame.duration_ns = duration_ns;
    frame.cause_ns[Shader] = duration_ns - duration_ns / 3;
    frame.cause_ns[Waiting] = duration_ns / 3;
    frame.activity_ns[Shader] = duration_ns;
    frame.primary_cause = Spikes::Cause::Shader;
    frame.confidence_permille = 1000;
    frame.present_result = Spikes::PresentResult::Success;
    frame.instrumentation_complete = true;
    return frame;
}

Spikes::AttributionSnapshot MakeSnapshot(u64 long_frames) {
    Spikes::AttributionSnapshot snapshot;
    snapshot.enabled = true;
    snapshot.complete = true;
    snapshot.threshold_ns = 100'000'000;
    snapshot.rejected_presents = 3;
    snapshot.frames = 500;
    snapshot.long_frames = long_frames;
    snapshot.unknown_wall_fraction = 0.125;
    snapshot.unknown_primary_frame_fraction = 0.5;
    snapshot.producer_unknown_wait_ns = 7;
    snapshot.process_created_ns = 5'000'000;
    snapshot.process_created_uncertainty_ns = 16'500'000;
    snapshot.first_host_present_ns = 2'500'000'000;
    snapshot.process_to_first_present.status = Spikes::IntervalStatus::Complete;
    snapshot.process_to_first_present.duration_ns = 2'495'000'000;
    snapshot.process_to_first_present.cause_ns[Disk] = 300'000'000;
    snapshot.process_to_first_present.cause_ns[Shader] = 2'000'000'000;
    snapshot.process_to_first_present.cause_ns[Unknown] = 195'000'000;
    snapshot.first_present_to_interactive.status = Spikes::IntervalStatus::Incomplete;
    snapshot.first_present_to_interactive.duration_ns = 6'500'000'000;
    snapshot.first_present_to_interactive.cause_ns[Waiting] = 6'500'000'000;
    snapshot.interactive.scene_id = "reference_scene";
    snapshot.interactive.origin = "lab";
    snapshot.interactive.marker_ns = 9'000'000'000;
    snapshot.interactive.scene_check_passed = true;
    snapshot.interactive_status = Spikes::IntervalStatus::Incomplete;
    snapshot.phases = {
        {Spikes::BootPhase::FrontendLoad, Spikes::PhaseOutcome::Ok, 6'000'000, 900'000'000},
        {Spikes::BootPhase::WarmRefresh, Spikes::PhaseOutcome::Ok, 900'000'000, 910'000'000},
        {Spikes::BootPhase::CacheValidate, Spikes::PhaseOutcome::Ok, 910'000'000, 930'000'000},
        {Spikes::BootPhase::PipelineReplay, Spikes::PhaseOutcome::Cancelled, 930'000'000, 0},
    };
    snapshot.first_120s.anchor_ns = 2'500'000'000;
    snapshot.first_120s.complete = true;
    snapshot.first_120s.elapsed_ns = Spikes::FirstWindowNs;
    snapshot.first_120s.frames = 480;
    snapshot.first_120s.frames_over_threshold = 2;
    snapshot.first_120s.max_ns = 150'000'000;
    snapshot.first_120s.crossing_tail_interval_ns = 16'000'000;
    return snapshot;
}

Envelope::CacheReport MakeAcceptedCache() {
    Envelope::CacheReport report;
    report.identity.shader_format_version = 3;
    report.identity.host.build_id = 0xfedcba9876543210ull;
    report.identity.host.title_id = 0x0100000000001234ull;
    report.identity.host.game_version = 65536;
    report.identity.host.content_stamp = 0x1;
    report.identity.host.settings_hash = 0xffffffffffffffffull;
    report.identity.device.vendor_id = 0x10de;
    report.identity.device.device_id = 0x2684;
    report.identity.device.pipeline_cache_uuid = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
                                                  14, 255};
    Envelope::CacheFileReport file;
    file.kind = Envelope::PayloadKind::Transferable;
    file.load.accepted = true;
    file.load.kind = Envelope::PayloadKind::Transferable;
    file.load.records = 812;
    report.files.push_back(file);
    report.stats.recorded = 5;
    report.stats.prepared = 700;
    report.stats.adopted = 650;
    report.stats.demand_wait_ns = 1'234'567;
    report.stats.write_status = Envelope::WriteStatus::Ok;
    return report;
}

std::string SpoolOf(const std::vector<Spikes::LongFrame>& frames) {
    std::string text;
    for (const Spikes::LongFrame& frame : frames) {
        text += Schema2::LongFrameJson(frame);
        text += '\n';
    }
    return text;
}

// The schema 1 members of a report as the writer hands them over: the object is not closed.
const std::string Schema1Document = R"({
  "schema_version": 2,
  "build": {"revision": "abc"},
  "title_id": "0100000000001234",
  "limits": {"seconds": 120, "frames": null},
  "elapsed_seconds": 12.5,
  "summary": {"frames": 100, "over_100ms": 4},
  "windows": [{"start_seconds": 0.0, "end_seconds": 5.0, "partial": false, "metrics": {}}],
  "logs": {"scope": "filtered_emitted", "error": 0, "critical": 0},
  "exit": {"reason": "run_seconds", "code": 0, "shutdown_complete": true,
           "metrics_complete": true, "detail": ""},
  "tunables": {"EGLANTINE_X": "1"},
  "settings": {"initial": {}, "final": {}}
})";

/// The whole report text the writer produces: schema 1 members, then the schema 2 members.
Json WriteReport(const Schema2::MembersInput& input, bool* stream_ok = nullptr) {
    std::string base = Schema1Document;
    base.resize(base.size() - 2); // "\n}"
    std::ostringstream out;
    out << base;
    const bool ok = Schema2::WriteMembers(out, input);
    out << "\n}\n";
    if (stream_ok != nullptr) {
        *stream_ok = ok;
    }
    return Json::parse(out.str());
}

} // Anonymous namespace

TEST_CASE("schema 2 normal run: golden members", "[report][schema2]") {
    const std::vector<Spikes::LongFrame> frames{MakeFrame(41, 150'000'000),
                                                MakeFrame(42, 100'000'001)};
    std::istringstream spool{SpoolOf(frames)};
    const Spikes::AttributionSnapshot snapshot = MakeSnapshot(2);
    const Envelope::CacheReport cache = MakeAcceptedCache();
    bool ok = false;
    const Json report = WriteReport({snapshot, cache, &spool, true, false}, &ok);
    REQUIRE(ok);

    const Json attribution = report["attribution"];
    CHECK(attribution["source"] == "host_present_submission");
    CHECK(attribution["threshold_ns"] == 100'000'000);
    CHECK(attribution["enabled"] == true);
    CHECK(attribution["complete"] == true);
    CHECK(attribution["lost_events"] == 0);
    CHECK(attribution["sink_write_errors"] == 0);
    CHECK(attribution["rejected_presents"] == 3);
    CHECK(attribution["unknown_wall_fraction"] == 0.125);
    CHECK(attribution["unknown_primary_frame_fraction"] == 0.5);
    CHECK(attribution["producer_unknown_wait_ns"] == 7);
    REQUIRE(attribution["long_frames"].size() == 2);
    CHECK(attribution["long_frames"][0] == Json::parse(Schema2::LongFrameJson(frames[0])));
    CHECK(attribution["long_frames"][1]["id"] == 42);
    CHECK(attribution["long_frames"][1]["duration_ns"] == 100'000'001);

    const Json expected_startup = Json::parse(R"({
      "process_created": {"ns": 5000000, "uncertainty_ns": 16500000},
      "first_host_present_ns": 2500000000,
      "process_to_first_present": {"duration_ns": 2495000000, "status": "complete",
        "cause_ns": {"disk": 300000000, "shader": 2000000000, "pipeline": 0, "texture": 0,
                     "decompression": 0, "waiting": 0, "code_translation": 0,
                     "unknown": 195000000}},
      "first_present_to_interactive": {"duration_ns": 6500000000, "status": "incomplete",
        "cause_ns": {"disk": 0, "shader": 0, "pipeline": 0, "texture": 0,
                     "decompression": 0, "waiting": 6500000000, "code_translation": 0,
                     "unknown": 0}},
      "interactive_scene": {"id": "reference_scene", "marker_ns": 9000000000, "origin": "lab",
                            "scene_check": "passed", "status": "incomplete"},
      "phases": [
        {"phase": "frontend_load", "outcome": "ok", "begin_ns": 6000000, "end_ns": 900000000},
        {"phase": "warm_refresh", "outcome": "ok", "begin_ns": 900000000, "end_ns": 910000000},
        {"phase": "cache_validate", "outcome": "ok", "begin_ns": 910000000,
         "end_ns": 930000000},
        {"phase": "pipeline_replay", "outcome": "cancelled", "begin_ns": 930000000,
         "end_ns": null}]
    })");
    CHECK(report["startup"] == expected_startup);

    const Json expected_first_120s = Json::parse(R"({
      "anchor": "first_real_host_present", "complete": true, "elapsed_ns": 120000000000,
      "frames": 480, "frames_over_100ms": 2, "max_ns": 150000000,
      "crossing_tail_interval_ns": 16000000, "scene_check": "passed"
    })");
    CHECK(report["first_120s"] == expected_first_120s);

    const Json expected_cache = Json::parse(R"({
      "status": "reported",
      "identity": {"envelope_version": 1, "shader_format_version": 3,
                   "build_id": "fedcba9876543210", "title_id": "0100000000001234",
                   "game_version": 65536, "content_stamp": "0000000000000001",
                   "settings_hash": "ffffffffffffffff",
                   "device": {"vendor_id": 4318, "device_id": 9860, "driver_id": 0,
                              "driver_version": 0,
                              "pipeline_cache_uuid": "000102030405060708090a0b0c0d0eff"}},
      "files": [{"kind": "transferable", "status": "accepted", "reason": "none",
                 "expected": null, "found": null, "records": 812}],
      "recorded": 5, "prepared": 700, "adopted": 650, "demand_promoted": 0,
      "demand_wait_ns": 1234567, "prepare_failures": 0, "records_skipped": 0,
      "write_status": "ok", "queue_high_water": 0, "memory_high_water_bytes": 0,
      "budget_pauses": 0
    })");
    Json actual_cache = report["cache"];
    // The envelope version is the header's constant: the golden only pins that it is a number.
    REQUIRE(actual_cache["identity"]["envelope_version"].is_number_unsigned());
    actual_cache["identity"]["envelope_version"] = 1;
    CHECK(actual_cache == expected_cache);
}

TEST_CASE("schema 2 keeps every schema 1 member", "[report][schema2]") {
    static_assert(Schema2::SchemaVersion == 2);
    const Json schema1 = Json::parse(Schema1Document);
    std::istringstream spool{""};
    const Spikes::AttributionSnapshot snapshot = MakeSnapshot(0);
    const Envelope::CacheReport cache = MakeAcceptedCache();
    const Json report = WriteReport({snapshot, cache, &spool, true, false});

    for (const auto& [key, value] : schema1.items()) {
        INFO("schema 1 member " << key);
        REQUIRE(report.contains(key));
        CHECK(report[key] == value);
    }
    // The new members are new names: appending them cannot replace a schema 1 member.
    for (const char* key : {"attribution", "startup", "first_120s", "cache"}) {
        INFO("schema 2 member " << key);
        CHECK_FALSE(schema1.contains(key));
        CHECK(report.contains(key));
    }
    CHECK(report.size() == schema1.size() + 4);
}

TEST_CASE("attribution is complete only when everything was stored", "[report][schema2]") {
    const std::vector<Spikes::LongFrame> frames{MakeFrame(1, 120'000'000),
                                                MakeFrame(2, 130'000'000)};
    const std::string full = SpoolOf(frames);
    const Envelope::CacheReport cache = MakeAcceptedCache();
    const auto complete_of = [&cache](const Spikes::AttributionSnapshot& snapshot,
                                      std::istream* spool, bool run_complete, bool spool_failed,
                                      size_t& lines) {
        const Json report = WriteReport({snapshot, cache, spool, run_complete, spool_failed});
        lines = report["attribution"]["long_frames"].size();
        return report["attribution"]["complete"].get<bool>();
    };
    size_t lines = 0;

    {
        std::istringstream spool{full};
        CHECK(complete_of(MakeSnapshot(2), &spool, true, false, lines));
        CHECK(lines == 2);
    }
    {
        // The shutdown did not finish (a stuck exit published anyway).
        std::istringstream spool{full};
        CHECK_FALSE(complete_of(MakeSnapshot(2), &spool, false, false, lines));
        CHECK(lines == 2);
    }
    {
        // The recorder itself says it lost something.
        std::istringstream spool{full};
        Spikes::AttributionSnapshot snapshot = MakeSnapshot(2);
        snapshot.complete = false;
        CHECK_FALSE(complete_of(snapshot, &spool, true, false, lines));
    }
    {
        // The spool could not be created: the long frames were not stored.
        CHECK_FALSE(complete_of(MakeSnapshot(2), nullptr, true, true, lines));
        CHECK(lines == 0);
    }
    {
        // The recorder counted two long frames, the spool holds one.
        std::istringstream spool{SpoolOf({frames[0]})};
        CHECK_FALSE(complete_of(MakeSnapshot(2), &spool, true, false, lines));
        CHECK(lines == 1);
    }
    {
        // A write cut short: the last line has no newline and is not a long frame.
        std::string cut = full;
        cut.pop_back();
        cut.resize(cut.size() - 10);
        std::istringstream spool{cut};
        CHECK_FALSE(complete_of(MakeSnapshot(2), &spool, true, false, lines));
        CHECK(lines == 1);
    }
    {
        // More lines than the recorder counted (a late write): not complete, and not truncated
        // to what the recorder counted either.
        std::istringstream spool{full};
        CHECK_FALSE(complete_of(MakeSnapshot(1), &spool, true, false, lines));
    }
    {
        // No long frame and no spool is a complete, empty list.
        CHECK(complete_of(MakeSnapshot(0), nullptr, true, false, lines));
        CHECK(lines == 0);
    }
}

TEST_CASE("disabled attribution reports nothing as zero", "[report][schema2]") {
    Spikes::AttributionSnapshot snapshot; // the recorder never ran
    std::istringstream spool{SpoolOf({MakeFrame(9, 200'000'000)})};
    const Envelope::CacheReport cache; // nothing published
    const Json report = WriteReport({snapshot, cache, &spool, true, false});

    const Json attribution = report["attribution"];
    CHECK(attribution["enabled"] == false);
    CHECK(attribution["complete"].is_null());
    CHECK(attribution["threshold_ns"].is_null());
    CHECK(attribution["lost_events"].is_null());
    CHECK(attribution["unknown_wall_fraction"].is_null());
    CHECK(attribution["long_frames"].empty());

    const Json startup = report["startup"];
    CHECK(startup["process_created"].is_null());
    CHECK(startup["first_host_present_ns"].is_null());
    CHECK(startup["phases"].is_null());
    CHECK(startup["process_to_first_present"]["status"] == "not_observed");
    CHECK(startup["process_to_first_present"]["duration_ns"].is_null());
    CHECK(startup["process_to_first_present"]["cause_ns"].is_null());
    CHECK(startup["interactive_scene"]["status"] == "not_observed");
    CHECK(startup["interactive_scene"]["id"].is_null());
    CHECK(startup["interactive_scene"]["scene_check"].is_null());

    const Json first = report["first_120s"];
    CHECK(first["complete"] == false);
    CHECK(first["frames_over_100ms"].is_null());
    CHECK(first["max_ns"].is_null());
    CHECK(first["elapsed_ns"].is_null());

    CHECK(report["cache"]["status"] == "not_reported");
    CHECK(report["cache"]["identity"].is_null());
    CHECK(report["cache"]["files"].empty());
    CHECK(report["cache"]["recorded"].is_null());
    CHECK(report["cache"]["write_status"].is_null());
}

TEST_CASE("first 120 s: before the first present and at the window edge", "[report][schema2]") {
    Spikes::AttributionSnapshot snapshot = MakeSnapshot(0);

    SECTION("no present yet") {
        snapshot.first_host_present_ns = 0;
        snapshot.first_120s = {};
        const Json first = Json::parse(Schema2::First120sJson(snapshot));
        CHECK(first["complete"] == false);
        CHECK(first["frames"].is_null());
        CHECK(first["frames_over_100ms"].is_null());
        CHECK(Json::parse(Schema2::StartupJson(snapshot))["first_host_present_ns"].is_null());
    }
    SECTION("window still open: the crossing interval is not known yet") {
        snapshot.first_120s.complete = false;
        snapshot.first_120s.elapsed_ns = 119'999'999'999;
        const Json first = Json::parse(Schema2::First120sJson(snapshot));
        CHECK(first["complete"] == false);
        CHECK(first["elapsed_ns"] == 119'999'999'999);
        CHECK(first["crossing_tail_interval_ns"].is_null());
        CHECK(first["frames_over_100ms"] == 2);
    }
    SECTION("window closed at exactly 120 s") {
        const Json first = Json::parse(Schema2::First120sJson(snapshot));
        CHECK(first["complete"] == true);
        CHECK(first["elapsed_ns"] == 120'000'000'000);
        CHECK(first["crossing_tail_interval_ns"] == 16'000'000);
    }
    SECTION("a failed scene check is reported as failed, not as missing") {
        snapshot.interactive.scene_check_passed = false;
        CHECK(Json::parse(Schema2::First120sJson(snapshot))["scene_check"] == "failed");
        snapshot.interactive_status = Spikes::IntervalStatus::NotObserved;
        CHECK(Json::parse(Schema2::First120sJson(snapshot))["scene_check"].is_null());
    }
}

TEST_CASE("long frame json: exact integers, clamps and names", "[report][schema2]") {
    Spikes::LongFrame frame = MakeFrame(7, 100'000'000); // exactly the 100 ms threshold
    const Json exact = Json::parse(Schema2::LongFrameJson(frame));
    CHECK(exact["duration_ns"] == 100'000'000);
    CHECK(exact["cause_ns"]["shader"] == 66'666'667);
    CHECK(exact["cause_ns"]["waiting"] == 33'333'333);
    CHECK(exact["primary_cause"] == "shader");
    CHECK(exact["present_result"] == "success");
    CHECK(exact["instrumentation_complete"] == true);
    CHECK(exact["unknown_reasons"].empty());
    CHECK(exact["counter_deltas"].empty());
    CHECK(exact["critical_work_ids"].empty());
    REQUIRE(exact["cause_ns"].size() == Spikes::CauseCount);

    SECTION("values past 2^53 are not rounded") {
        frame.start_ns = 9'007'199'254'740'993ull;
        frame.end_ns = frame.start_ns + 1'000'000'000;
        const Json json = Json::parse(Schema2::LongFrameJson(frame));
        CHECK(json["start_ns"].get<u64>() == 9'007'199'254'740'993ull);
        CHECK(json["end_ns"].get<u64>() == 9'007'199'255'740'993ull);
    }
    SECTION("critical work: only what is counted, never more than the array holds") {
        for (size_t i = 0; i < frame.critical_work_ids.size(); ++i) {
            frame.critical_work_ids[i] = 100 + i;
        }
        frame.critical_work_count = 3;
        CHECK(Json::parse(Schema2::LongFrameJson(frame))["critical_work_ids"] ==
              Json::parse("[100,101,102]"));
        frame.critical_work_count = 1'000'000;
        const Json clamped = Json::parse(Schema2::LongFrameJson(frame));
        CHECK(clamped["critical_work_ids"].size() == Spikes::MaxCriticalWork);
        CHECK(clamped["critical_work_ids"][7] == 107);
    }
    SECTION("unknown reasons: named bits, and a bit with no name by number") {
        frame.unknown_reasons = (u32{1} << 0) | (u32{1} << 7) | (u32{1} << 9);
        CHECK(Json::parse(Schema2::LongFrameJson(frame))["unknown_reasons"] ==
              Json::parse(R"(["pre_instrumentation","producer_unknown_wait","bit_9"])"));
    }
    SECTION("counters: nonzero only, by their stable names") {
        frame.counter_deltas[0] = 4;
        frame.counter_deltas[frame.counter_deltas.size() - 1] = 9;
        const Json counters = Json::parse(Schema2::LongFrameJson(frame))["counter_deltas"];
        REQUIRE(counters.size() == 2);
        CHECK(counters[std::string{Spikes::EventToken(static_cast<Spikes::Event>(0))}] == 4);
        CHECK(counters[std::string{Spikes::EventToken(static_cast<Spikes::Event>(
                  frame.counter_deltas.size() - 1))}] == 9);
    }
    SECTION("results") {
        frame.present_result = Spikes::PresentResult::OutOfDate;
        frame.instrumentation_complete = false;
        const Json json = Json::parse(Schema2::LongFrameJson(frame));
        CHECK(json["present_result"] == "out_of_date");
        CHECK(json["instrumentation_complete"] == false);
    }
}

TEST_CASE("cache block: a rejected cache is in the report with its reason", "[report][schema2]") {
    Envelope::CacheReport report = MakeAcceptedCache();
    report.files.clear();

    Envelope::CacheFileReport stale;
    stale.kind = Envelope::PayloadKind::Transferable;
    stale.load.accepted = false;
    stale.load.reason = Envelope::RejectReason::FormatVersion;
    stale.load.kind = Envelope::PayloadKind::Transferable;
    stale.load.expected = 1;
    stale.load.found = 999;
    report.files.push_back(stale);

    Envelope::CacheFileReport legacy = stale;
    legacy.load.reason = Envelope::RejectReason::LegacyFormat;
    legacy.load.expected = 0;
    legacy.load.found = 0;
    legacy.load.legacy_backed_up = true;
    report.files.push_back(legacy);

    Envelope::CacheFileReport first_launch;
    first_launch.kind = Envelope::PayloadKind::Transferable;
    first_launch.load.accepted = false;
    first_launch.load.reason = Envelope::RejectReason::Missing;
    report.files.push_back(first_launch);

    Envelope::CacheFileReport unlabelled; // a file nobody opened this run
    report.files.push_back(unlabelled);

    const Json json = Json::parse(Schema2::CacheJson(report));
    REQUIRE(json["files"].size() == 4);
    CHECK(json["files"][0] == Json::parse(R"({"kind": "transferable", "status": "rejected",
        "reason": "format_version", "expected": 1, "found": 999, "records": null})"));
    CHECK(json["files"][1]["reason"] == "legacy_format");
    CHECK(json["files"][1]["status"] == "rejected");
    CHECK(json["files"][2] == Json::parse(R"({"kind": "transferable", "status": "missing",
        "reason": "missing", "expected": null, "found": null, "records": null})"));
    CHECK(json["files"][3]["status"] == "missing");
    CHECK(json["files"][3]["reason"] == "none");

    // The reporter only describes the cache. Whatever the cache said, the run's exit code comes
    // from the run outcome (a rejected cache is a normal first launch of a new build): nothing in
    // the cache block is an input to it, and the rest of the report is unchanged by it.
    std::istringstream spool{""};
    const Spikes::AttributionSnapshot snapshot = MakeSnapshot(0);
    const Json full = WriteReport({snapshot, report, &spool, true, false});
    CHECK(full["exit"]["code"] == 0);
    CHECK(full["exit"]["reason"] == "run_seconds");
    CHECK(full["cache"]["files"][0]["reason"] == "format_version");
}

TEST_CASE("crash report members are safe and never complete", "[report][schema2][crash]") {
    const std::vector<Spikes::LongFrame> frames{MakeFrame(1, 120'000'000),
                                                MakeFrame(2, 130'000'000)};
    std::string spool = SpoolOf(frames);
    spool += R"({"id":3,"start_ns":10,"cut)"; // the line the consumer was writing

    std::vector<char> buffer(spool.begin(), spool.end());
    const size_t joined = Schema2::JoinSpoolLines(buffer.data(), buffer.size());
    char head[1024];
    char tail[1024];
    const size_t head_size = Schema2::CrashMembersHead(head, sizeof(head), true, 100'000'000);
    const size_t tail_size = Schema2::CrashMembersTail(tail, sizeof(tail));
    REQUIRE(head_size != 0);
    REQUIRE(tail_size != 0);

    const std::string text = R"({"schema_version":2,"exit":{"reason":"host_crash"})" +
                             std::string{head, head_size} + std::string{buffer.data(), joined} +
                             std::string{tail, tail_size} + R"(,"tunables":{}})";
    const Json report = Json::parse(text);
    const Json attribution = report["attribution"];
    CHECK(attribution["complete"] == false);
    CHECK(attribution["enabled"] == true);
    CHECK(attribution["status"] == "host_crash");
    CHECK(attribution["threshold_ns"] == 100'000'000);
    REQUIRE(attribution["long_frames"].size() == 2);
    CHECK(attribution["long_frames"][1]["id"] == 2);
    CHECK(report["startup"].is_null());
    CHECK(report["first_120s"].is_null());
    CHECK(report["cache"].is_null());
    CHECK(report["tunables"] == Json::object());

    SECTION("attribution was off") {
        const size_t off_size = Schema2::CrashMembersHead(head, sizeof(head), false, 100'000'000);
        const Json off = Json::parse("{" + std::string{head, off_size}.substr(1) + "]}" + "}");
        CHECK(off["attribution"]["enabled"] == false);
        CHECK(off["attribution"]["complete"].is_null());
        CHECK(off["attribution"]["threshold_ns"].is_null());
        CHECK(off["attribution"]["long_frames"].empty());
    }
    SECTION("a buffer that is too small writes nothing rather than half") {
        CHECK(Schema2::CrashMembersHead(head, 20, true, 100'000'000) == 0);
        CHECK(Schema2::CrashMembersHead(head, head_size - 1, true, 100'000'000) == 0);
        CHECK(Schema2::CrashMembersHead(head, head_size, true, 100'000'000) == head_size);
        CHECK(Schema2::CrashMembersTail(tail, tail_size - 1) == 0);
        CHECK(Schema2::CrashMembersTail(tail, tail_size) == tail_size);
    }
}

TEST_CASE("spool excerpt join", "[report][schema2][crash]") {
    const auto join = [](std::string text) {
        const size_t size = Schema2::JoinSpoolLines(text.data(), text.size());
        return text.substr(0, size);
    };
    CHECK(join("") == "");
    CHECK(join("{\"a\":1}") == "");             // no newline: still being written
    CHECK(join("{\"a\":1}\n") == "{\"a\":1}");
    CHECK(join("{\"a\":1}\n{\"b\":2}\n") == "{\"a\":1},{\"b\":2}");
    CHECK(join("{\"a\":1}\n{\"b\"") == "{\"a\":1}");
    CHECK(join("\n\n{\"a\":1}\n\n{\"b\":2}\n") == "{\"a\":1},{\"b\":2}"); // blank lines go
    CHECK(join("\n") == "");
}

TEST_CASE("interactive marker lines", "[report][schema2][marker]") {
    using RunControl::ParseMarkerLine;

    SECTION("a valid line") {
        const auto marker = ParseMarkerLine("interactive menu_1.a-b pass lab2", 123'456);
        REQUIRE(marker.has_value());
        CHECK(marker->scene_id == "menu_1.a-b");
        CHECK(marker->origin == "lab2");
        CHECK(marker->scene_check_passed);
        CHECK(marker->marker_ns == 123'456);
    }
    SECTION("origin defaults, verdict fail, tabs and line ends are tolerated") {
        const auto marker = ParseMarkerLine("  interactive\tscene\tfail \r\n", 5);
        REQUIRE(marker.has_value());
        CHECK(marker->origin == "lab");
        CHECK_FALSE(marker->scene_check_passed);
    }
    SECTION("lengths are checked at their limits") {
        const std::string longest_scene(RunControl::MaxSceneIdLength, 'a');
        const std::string longest_origin(RunControl::MaxOriginLength, 'b');
        CHECK(ParseMarkerLine("interactive " + longest_scene + " pass " + longest_origin, 1)
                  .has_value());
        CHECK_FALSE(ParseMarkerLine("interactive " + longest_scene + "a pass", 1).has_value());
        CHECK_FALSE(
            ParseMarkerLine("interactive scene pass " + longest_origin + "b", 1).has_value());
    }
    SECTION("malformed lines are rejected") {
        const std::string long_scene(RunControl::MaxSceneIdLength + 1, 'a');
        for (const std::string& line : {
                 std::string{},
                 std::string{"   "},
                 std::string{"interactive"},
                 std::string{"interactive scene"},
                 std::string{"interactive scene maybe"},
                 std::string{"interactive scene PASS"},
                 std::string{"interactive scene pass lab extra"},
                 std::string{"interactive scene pass lab extra more"},
                 std::string{"Interactive scene pass"},
                 std::string{"interactivescene pass"},
                 std::string{"interactive_ scene pass"},
                 std::string{"interactive scene/../x pass"},
                 std::string{"interactive sc\"ene pass"},
                 std::string{"interactive scene pass la/b"},
                 std::string{"interactive sc\xc3\xa9ne pass"},
                 std::string{"interactive scene\x01 pass"},
                 std::string{"interactive scene pass\0lab", 24},
                 "interactive " + long_scene + " pass",
                 std::string{"gpu high"},
                 std::string{"12 500"},
             }) {
            INFO("line: " << line);
            CHECK_FALSE(ParseMarkerLine(line, 1).has_value());
        }
    }
}

TEST_CASE("host identity hashing separates fields", "[report][schema2]") {
    RunControl::Fnv64 split_a;
    split_a.Text("ab");
    split_a.Text("c");
    RunControl::Fnv64 split_b;
    split_b.Text("a");
    split_b.Text("bc");
    CHECK(split_a.Value() != split_b.Value());

    RunControl::Fnv64 same_a;
    same_a.U64(0x0100000000001234ull);
    same_a.Text("x");
    RunControl::Fnv64 same_b;
    same_b.U64(0x0100000000001234ull);
    same_b.Text("x");
    CHECK(same_a.Value() == same_b.Value());

    // FNV-1a 64 of the empty input is its offset basis, of "a" a published test vector.
    CHECK(RunControl::Fnv64{}.Value() == 0xcbf29ce484222325ull);
    RunControl::Fnv64 a;
    a.Bytes("a", 1);
    CHECK(a.Value() == 0xaf63dc4c8601ec8cull);
}
