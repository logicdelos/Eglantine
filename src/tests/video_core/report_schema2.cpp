// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
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
    snapshot.dropped_boundaries = 2;
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
    report.stats.evicted = 3;
    report.stats.checkpoints = 2;
    report.stats.bytes_committed = 4096;
    report.stats.write_status = Envelope::WriteStatus::Ok;
    return report;
}

const RunControl::SessionInfo NoSession{};

RunControl::SessionInfo MakeSession() {
    RunControl::SessionInfo session;
    session.title_id = 0x0100000000001234ull;
    session.nonce = 0xa1b2c3d4e5f60718ull;
    session.rejected = {1, 2, 3, 4, 5};
    session.manifest = {42, true, 1'500'000};
    return session;
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
    const Json report = WriteReport({snapshot, cache, MakeSession(), &spool, true, false}, &ok);
    REQUIRE(ok);

    const Json attribution = report["attribution"];
    CHECK(attribution["source"] == "host_present_submission");
    CHECK(attribution["threshold_ns"] == 100'000'000);
    CHECK(attribution["enabled"] == true);
    CHECK(attribution["complete"] == true);
    CHECK(attribution["lost_events"] == 0);
    CHECK(attribution["dropped_boundaries"] == 2);
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
         "end_ns": null}],
      "session": {"title_id": "0100000000001234", "nonce": "a1b2c3d4e5f60718",
        "markers_rejected": {"malformed": 1, "missing_credentials": 2, "title_mismatch": 3,
                             "nonce_mismatch": 4, "refused": 5},
        "content_manifest": {"entries": 42, "over_cap": true, "duration_ns": 1500000}}
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
      "budget_pauses": 0, "evicted": 3, "checkpoints": 2, "bytes_committed": 4096,
      "translate_cost": "not_separately_measured"
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
    const Json report = WriteReport({snapshot, cache, NoSession, &spool, true, false});

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
        const Json report = WriteReport({snapshot, cache, NoSession, spool, run_complete, spool_failed});
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
    const Json report = WriteReport({snapshot, cache, NoSession, &spool, true, false});

    const Json attribution = report["attribution"];
    CHECK(attribution["enabled"] == false);
    CHECK(attribution["complete"].is_null());
    CHECK(attribution["threshold_ns"].is_null());
    CHECK(attribution["lost_events"].is_null());
    CHECK(attribution["dropped_boundaries"].is_null());
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
    CHECK(startup["session"]["nonce"].is_null()); // no session: nothing a marker could match
    CHECK(startup["session"]["markers_rejected"]["malformed"] == 0);

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
    CHECK(report["cache"]["evicted"].is_null());
    CHECK(report["cache"]["translate_cost"].is_null());
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
    const Json full = WriteReport({snapshot, report, NoSession, &spool, true, false});
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

namespace {

constexpr u64 TestTitle = 0x0100000000001234ull;
constexpr u64 TestNonce = 0xa1b2c3d4e5f60718ull;
constexpr std::string_view TestCredentials = "0100000000001234 a1b2c3d4e5f60718";

std::string MarkerWith(std::string_view body, std::string_view credentials = TestCredentials) {
    return "interactive " + std::string{body} + " " + std::string{credentials};
}

RunControl::MarkerParse Classify(std::string_view line) {
    return RunControl::ClassifyMarkerLine(line, TestTitle, TestNonce, 1).parse;
}

} // Anonymous namespace

TEST_CASE("interactive marker lines", "[report][schema2][marker]") {
    using RunControl::ClassifyMarkerLine;
    using RunControl::MarkerParse;

    SECTION("a valid line") {
        const auto marker = ClassifyMarkerLine(
            "interactive menu_1.a-b pass 0100000000001234 a1b2c3d4e5f60718 lab2", TestTitle,
            TestNonce, 123'456);
        REQUIRE(marker.parse == MarkerParse::Valid);
        CHECK(marker.marker.scene_id == "menu_1.a-b");
        CHECK(marker.marker.origin == "lab2");
        CHECK(marker.marker.scene_check_passed);
        CHECK(marker.marker.marker_ns == 123'456);
    }
    SECTION("origin defaults, verdict fail, tabs and line ends are tolerated") {
        const auto marker = ClassifyMarkerLine(
            "  interactive\tscene\tfail 0100000000001234\ta1b2c3d4e5f60718 \r\n", TestTitle,
            TestNonce, 5);
        REQUIRE(marker.parse == MarkerParse::Valid);
        CHECK(marker.marker.origin == "lab");
        CHECK_FALSE(marker.marker.scene_check_passed);
    }
    SECTION("lengths are checked at their limits") {
        const std::string longest_scene(RunControl::MaxSceneIdLength, 'a');
        const std::string longest_origin(RunControl::MaxOriginLength, 'b');
        CHECK(Classify(MarkerWith(longest_scene + " pass") + " " + longest_origin) ==
              MarkerParse::Valid);
        CHECK(Classify(MarkerWith(longest_scene + "a pass")) == MarkerParse::Malformed);
        CHECK(Classify(MarkerWith("scene pass") + " " + longest_origin + "b") ==
              MarkerParse::Malformed);
    }
    SECTION("lines that are not markers") {
        for (const std::string& line : {
                 std::string{},
                 std::string{"   "},
                 std::string{"gpu high"},
                 std::string{"12 500"},
                 std::string{"Interactive scene pass"},
                 std::string{"interactivescene pass"},
                 std::string{"interactive_ scene pass"},
                 std::string{"x interactive scene pass"},
             }) {
            INFO("line: " << line);
            CHECK(Classify(line) == MarkerParse::NotMarker);
            CHECK_FALSE(RunControl::IsMarkerLine(line));
        }
    }
    SECTION("malformed lines are rejected") {
        const std::string long_scene(RunControl::MaxSceneIdLength + 1, 'a');
        for (const std::string& line : {
                 std::string{"interactive"},
                 std::string{"interactive scene"},
                 std::string{"interactive scene maybe"},
                 MarkerWith("scene maybe"),
                 MarkerWith("scene PASS"),
                 MarkerWith("scene pass") + " lab extra",
                 MarkerWith("scene/../x pass"),
                 MarkerWith("sc\"ene pass"),
                 MarkerWith("scene pass") + " la/b",
                 MarkerWith("sc\xc3\xa9ne pass"),
                 MarkerWith("scene\x01 pass"),
                 MarkerWith(long_scene + " pass"),
                 // the credentials are exactly 16 lowercase hex digits each
                 MarkerWith("scene pass", "0100000000001234 A1B2C3D4E5F60718"),
                 MarkerWith("scene pass", "010000000000123 a1b2c3d4e5f60718"),
                 MarkerWith("scene pass", "0100000000001234 a1b2c3d4e5f607180"),
                 MarkerWith("scene pass", "0x00000000001234 a1b2c3d4e5f60718"),
                 MarkerWith("scene pass", "0100000000001234 zzzzzzzzzzzzzzzz"),
             }) {
            INFO("line: " << line);
            CHECK(Classify(line) == MarkerParse::Malformed);
            CHECK(RunControl::IsMarkerLine(line));
        }
    }
    SECTION("a marker without title and nonce is refused, not read as the old grammar") {
        CHECK(Classify("interactive scene pass") == MarkerParse::MissingCredentials);
        CHECK(Classify("interactive scene fail lab") == MarkerParse::MissingCredentials);
    }
    SECTION("title and nonce must be this run's") {
        // a marker left over from an earlier run of the same title: another nonce
        CHECK(Classify(MarkerWith("scene pass", "0100000000001234 0000000000000001")) ==
              MarkerParse::NonceMismatch);
        // a marker meant for another title
        CHECK(Classify(MarkerWith("scene pass", "0100000000005678 a1b2c3d4e5f60718")) ==
              MarkerParse::TitleMismatch);
        // both wrong: the title is what is said first
        CHECK(Classify(MarkerWith("scene pass", "0100000000005678 0000000000000001")) ==
              MarkerParse::TitleMismatch);
        CHECK(Classify(MarkerWith("scene pass")) == MarkerParse::Valid);
        // no session (nonce 0) accepts nothing, not even a line that says 0
        CHECK(ClassifyMarkerLine(MarkerWith("scene pass", "0100000000001234 0000000000000000"),
                                 TestTitle, 0, 1)
                  .parse == MarkerParse::NonceMismatch);
    }
}

TEST_CASE("marker dispatch: the line reaches the parser, whatever its blanks", "[report][marker]") {
    // yuzu_cmd's RunLiveKeys routes a line by IsMarkerLine to HandleMarkerLine, which classifies
    // it with DispatchMarkerLine: the same functions, so that nothing the parser accepts is lost
    // on the way.
    const std::string fixture =
        "  interactive\tscene\tfail 0100000000001234\ta1b2c3d4e5f60718\r\n";
    CHECK(RunControl::IsMarkerLine(fixture));
    CHECK(RunControl::IsMarkerLine("\tinteractive scene pass"));
    CHECK(RunControl::IsMarkerLine("interactive\r"));
    CHECK_FALSE(RunControl::IsMarkerLine("quit"));

    RunControl::SetSession(TestTitle, TestNonce);
    const RunControl::MarkerClassification accepted = RunControl::DispatchMarkerLine(fixture, 77);
    REQUIRE(accepted.parse == RunControl::MarkerParse::Valid);
    CHECK_FALSE(accepted.marker.scene_check_passed);
    CHECK(accepted.marker.marker_ns == 77);
    RunControl::SetSession(0, 0);
}

TEST_CASE("marker dispatch binds a marker to this run and counts what it refuses",
          "[report][marker]") {
    using RunControl::MarkerParse;
    RunControl::SetSession(TestTitle, TestNonce);
    const RunControl::MarkerRejections before = RunControl::ReadSession().rejected;
    const auto dispatch = [](std::string_view line) {
        return RunControl::DispatchMarkerLine(line, 1).parse;
    };

    // a line from an earlier run, a line for another title, and one without credentials
    CHECK(dispatch(MarkerWith("scene pass", "0100000000001234 1111111111111111")) ==
          MarkerParse::NonceMismatch);
    CHECK(dispatch(MarkerWith("scene pass", "0100000000009999 a1b2c3d4e5f60718")) ==
          MarkerParse::TitleMismatch);
    CHECK(dispatch("interactive scene pass") == MarkerParse::MissingCredentials);
    CHECK(dispatch("interactive scene") == MarkerParse::Malformed);
    const RunControl::MarkerRejections refused = RunControl::ReadSession().rejected;
    CHECK(refused.nonce_mismatch == before.nonce_mismatch + 1);
    CHECK(refused.title_mismatch == before.title_mismatch + 1);
    CHECK(refused.missing_credentials == before.missing_credentials + 1);
    CHECK(refused.malformed == before.malformed + 1);

    // the right line, and a line that is no marker, count nothing
    CHECK(dispatch(MarkerWith("scene pass")) == MarkerParse::Valid);
    CHECK(dispatch("gpu high") == MarkerParse::NotMarker);
    const RunControl::MarkerRejections after = RunControl::ReadSession().rejected;
    CHECK(after.nonce_mismatch == refused.nonce_mismatch);
    CHECK(after.title_mismatch == refused.title_mismatch);
    CHECK(after.missing_credentials == refused.missing_credentials);
    CHECK(after.malformed == refused.malformed);
    CHECK(after.refused == refused.refused);

    // the report says it
    const Json json = Json::parse(Schema2::SessionJson(RunControl::ReadSession()));
    CHECK(json["title_id"] == "0100000000001234");
    CHECK(json["nonce"] == "a1b2c3d4e5f60718");
    CHECK(json["markers_rejected"]["nonce_mismatch"] == after.nonce_mismatch);
    RunControl::SetSession(0, 0);

    // session.txt is exactly these two lines
    CHECK(RunControl::SessionFileText(TestTitle, TestNonce) ==
          "title 0100000000001234\nnonce a1b2c3d4e5f60718\n");
    CHECK(RunControl::NewNonce() != 0);
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

namespace {

/// A directory of files that is removed with it. Write times are fixed, so that a stamp changes
/// only where a test changes a file.
class TempTree {
public:
    TempTree() {
        static u64 counter = 0;
        root = std::filesystem::temp_directory_path() /
               fmt::format("eglantine_manifest_test_{}_{}", Spikes::NowNs(), counter++);
        std::filesystem::create_directories(root);
    }
    ~TempTree() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
    TempTree(const TempTree&) = delete;
    TempTree& operator=(const TempTree&) = delete;

    std::filesystem::path Write(std::string_view relative, std::string_view content) const {
        const std::filesystem::path path = root / std::filesystem::path{std::string{relative}};
        std::filesystem::create_directories(path.parent_path());
        {
            std::ofstream out{path, std::ios::binary | std::ios::trunc};
            out << content;
        }
        Stamp(path, 0);
        return path;
    }

    static void Stamp(const std::filesystem::path& path, int minutes) {
        static const auto base = std::filesystem::file_time_type::clock::now();
        std::filesystem::last_write_time(path, base + std::chrono::minutes{minutes});
    }

    std::filesystem::path root;
};

u64 StampOf(const RunControl::HostIdentitySource& source,
            RunControl::ManifestSummary* summary = nullptr) {
    return RunControl::AssembleHostIdentity(source, summary).content_stamp;
}

/// Sets or clears an environment variable for the life of the object.
class EnvGuard {
public:
    EnvGuard(std::string name_, const std::optional<std::string>& value) : name{std::move(name_)} {
        if (const char* const old = std::getenv(name.c_str())) {
            saved = old;
        }
        Set(value);
    }
    ~EnvGuard() {
        Set(saved);
    }
    EnvGuard(const EnvGuard&) = delete;
    EnvGuard& operator=(const EnvGuard&) = delete;

    void Set(const std::optional<std::string>& value) const {
#ifdef _WIN32
        _putenv_s(name.c_str(), value ? value->c_str() : "");
#else
        if (value) {
            setenv(name.c_str(), value->c_str(), 1);
        } else {
            unsetenv(name.c_str());
        }
#endif
    }

private:
    std::string name;
    std::optional<std::string> saved;
};

} // Anonymous namespace

TEST_CASE("content stamp covers every file of an enabled mod", "[report][identity]") {
    TempTree tree;
    tree.Write("game.nsp", "game");
    tree.Write("mods/Same/romfs/a.bin", "aaaa");
    const std::filesystem::path deep = tree.Write("mods/Same/romfs/deep/b.bin", "bbbb");
    tree.Write("mods/Other/romfs/c.bin", "cccc");

    RunControl::HostIdentitySource source;
    source.title_id = 0x0100000000001234ull;
    source.game_path = tree.root / "game.nsp";
    source.labels = {"2|0100000000001234|0|Same|1.0"};
    source.content_trees = {tree.root / "mods" / "Same"};
    const u64 base = StampOf(source);
    CHECK(StampOf(source) == base);

    SECTION("one file of the mod changes size, the name and version stay") {
        tree.Write("mods/Same/romfs/deep/b.bin", "bbbbb");
        CHECK(StampOf(source) != base);
    }
    SECTION("one file of the mod changes only its write time") {
        TempTree::Stamp(deep, 90);
        CHECK(StampOf(source) != base);
    }
    SECTION("a file is added") {
        tree.Write("mods/Same/romfs/new.bin", "n");
        CHECK(StampOf(source) != base);
    }
    SECTION("a file is removed") {
        std::filesystem::remove(deep);
        CHECK(StampOf(source) != base);
    }
    SECTION("a mod that is not enabled is not in the stamp") {
        tree.Write("mods/Other/romfs/c.bin", "much longer");
        CHECK(StampOf(source) == base);
    }
    SECTION("the order of the lists does not matter") {
        tree.Write("update.nsp", "u");
        source.content_files = {tree.root / "update.nsp", tree.root / "game.nsp"};
        source.labels.push_back("1|0100000000001234|65536|Update|1.0.1");
        const u64 first = StampOf(source);
        std::ranges::reverse(source.content_files);
        std::ranges::reverse(source.labels);
        source.content_trees.push_back(tree.root / "mods" / "Other");
        const u64 with_other = StampOf(source);
        CHECK(with_other != first);
        std::ranges::reverse(source.content_trees);
        CHECK(StampOf(source) == with_other);
    }
}

TEST_CASE("content stamp covers the game, the update and a loose RomFS", "[report][identity]") {
    TempTree tree;
    tree.Write("dump/exefs/main", "MAIN");
    tree.Write("dump/romfs/data/x.bin", "xxxx");
    tree.Write("game.nsp", "game");
    tree.Write("update.nsp", "update");

    RunControl::HostIdentitySource loose;
    loose.title_id = 0x0100000000001234ull;
    loose.game_path = tree.root / "dump";
    const u64 loose_base = StampOf(loose);

    SECTION("a RomFS file changes while main is untouched") {
        tree.Write("dump/romfs/data/x.bin", "xxxxxx");
        CHECK(StampOf(loose) != loose_base);
    }
    SECTION("a RomFS file is added") {
        tree.Write("dump/romfs/data/y.bin", "y");
        CHECK(StampOf(loose) != loose_base);
    }
    SECTION("main changes") {
        tree.Write("dump/exefs/main", "MAIN2");
        CHECK(StampOf(loose) != loose_base);
    }

    RunControl::HostIdentitySource packaged;
    packaged.title_id = loose.title_id;
    packaged.game_path = tree.root / "game.nsp";
    packaged.content_files = {tree.root / "update.nsp"};
    const u64 packaged_base = StampOf(packaged);
    CHECK(packaged_base != loose_base);

    SECTION("the game file changes size") {
        tree.Write("game.nsp", "game!");
        CHECK(StampOf(packaged) != packaged_base);
    }
    SECTION("the update file changes size") {
        tree.Write("update.nsp", "update!");
        CHECK(StampOf(packaged) != packaged_base);
    }
    SECTION("the update file is gone") {
        std::filesystem::remove(tree.root / "update.nsp");
        CHECK(StampOf(packaged) != packaged_base);
    }
    SECTION("another title") {
        packaged.title_id += 1;
        CHECK(StampOf(packaged) != packaged_base);
    }
}

TEST_CASE("content manifest cap is deterministic and part of the stamp", "[report][identity]") {
    TempTree tree;
    for (int i = 0; i < 6; ++i) {
        tree.Write(fmt::format("mod/f{}.bin", i), "x");
    }
    RunControl::HostIdentitySource source;
    source.title_id = 0x0100000000001234ull;
    source.content_trees = {tree.root / "mod"};

    RunControl::ManifestSummary exact;
    RunControl::ManifestSummary roomy;
    RunControl::ManifestSummary capped;
    source.max_manifest_entries = 6; // exactly the number of files: not over the cap
    const u64 stamp_exact = StampOf(source, &exact);
    source.max_manifest_entries = 100;
    const u64 stamp_roomy = StampOf(source, &roomy);
    source.max_manifest_entries = 5; // one file too many
    const u64 stamp_capped = StampOf(source, &capped);

    CHECK(exact.entries == 6);
    CHECK_FALSE(exact.over_cap);
    CHECK(stamp_exact == stamp_roomy);
    CHECK(roomy.entries == 6);
    CHECK(capped.entries == 5);
    CHECK(capped.over_cap);
    CHECK(stamp_capped != stamp_exact);

    // The walk stops in the same place every time (name order): a file past the cap is not
    // seen, a file before it is.
    tree.Write("mod/f5.bin", "changed past the cap");
    CHECK(StampOf(source) == stamp_capped);
    tree.Write("mod/f0.bin", "changed before the cap");
    CHECK(StampOf(source) != stamp_capped);
}

TEST_CASE("settings hash covers every switch that changes generated shaders",
          "[report][identity]") {
    const u64 base = RunControl::HostSettingsHash();
    CHECK(RunControl::HostSettingsHash() == base);

    for (const std::string_view name : RunControl::ShaderGenerationSwitches) {
        INFO("switch " << name);
        EnvGuard guard{std::string{name}, std::nullopt};
        const u64 unset = RunControl::HostSettingsHash();
        guard.Set("1");
        const u64 one = RunControl::HostSettingsHash();
        guard.Set("0");
        const u64 zero = RunControl::HostSettingsHash();
        CHECK(unset != one);
        CHECK(one != zero);
        CHECK(zero != unset);
    }
    CHECK(RunControl::HostSettingsHash() == base);

    const auto changes_hash = [](const auto& flip) {
        const u64 before = RunControl::HostSettingsHash();
        flip();
        const u64 after = RunControl::HostSettingsHash();
        flip();
        return after != before && RunControl::HostSettingsHash() == before;
    };
    CHECK(changes_hash([] {
        Settings::values.fix_bloom_effects.SetValue(!Settings::values.fix_bloom_effects.GetValue());
    }));
    CHECK(changes_hash([] {
        Settings::values.disable_shader_loop_safety_checks.SetValue(
            !Settings::values.disable_shader_loop_safety_checks.GetValue());
    }));
    CHECK(changes_hash([] {
        Settings::values.renderer_debug.SetValue(!Settings::values.renderer_debug.GetValue());
    }));
    CHECK(changes_hash([] {
        Settings::values.vertex_input_dynamic_state.SetValue(
            !Settings::values.vertex_input_dynamic_state.GetValue());
    }));
    CHECK(changes_hash([] {
        Settings::values.use_squashed_iterated_blend = !Settings::values.use_squashed_iterated_blend;
    }));
    CHECK(changes_hash([] {
        Settings::values.resolution_info.active = !Settings::values.resolution_info.active;
    }));
    for (u32* const field : {&Settings::values.resolution_info.up_scale,
                             &Settings::values.resolution_info.down_shift}) {
        const u32 original = *field;
        ++*field;
        const u64 changed = RunControl::HostSettingsHash();
        *field = original;
        CHECK(changed != base);
        CHECK(RunControl::HostSettingsHash() == base);
    }
}

TEST_CASE("a disabled boot phase scope does nothing", "[report][schema2][startup]") {
    REQUIRE_FALSE(Spikes::AttributionEnabled());
    const size_t phases_before = Spikes::ReadAttributionSnapshot().phases.size();

    RunControl::BootPhaseScope scope{Spikes::BootPhase::GameRun};
    CHECK_FALSE(scope.Active()); // so End returns before it reads a clock or writes a log line
    scope.End(Spikes::PhaseOutcome::Ok);
    CHECK_FALSE(scope.Active());

    RunControl::DiskLoadPhases load;
    load.OnStage(VideoCore::LoadCallbackStage::Build);
    load.Finish(Spikes::PhaseOutcome::Ok);

    CHECK(Spikes::ReadAttributionSnapshot().phases.size() == phases_before);
}
