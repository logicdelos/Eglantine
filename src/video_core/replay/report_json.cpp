// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <mutex>
#include <optional>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "video_core/replay/result.h"

namespace VideoCore::Replay {

namespace {

using Json = nlohmann::json;

std::string Hex(u64 value) {
    return fmt::format("{:016x}", value);
}

Json Share(u64 numerator, u64 denominator) {
    if (denominator == 0) {
        return Json(nullptr);
    }
    return Json(static_cast<double>(numerator) / static_cast<double>(denominator));
}

Json Text(const std::string& text) {
    return text.empty() ? Json(nullptr) : Json(text);
}

struct Published {
    std::mutex mutex;
    std::optional<CaptureReport> capture;
    std::optional<ReplayReport> replay;
    std::optional<CensusReport> census;
};

Published& Store() {
    static Published published;
    return published;
}

} // namespace

void EvaluateCoverage(CensusReport& report) noexcept {
    const CensusCounters& c = report.counters;
    const u64 presents = c.nx2_presents + c.legacy_presents;
    report.criteria[0] = report.bootstrap_found && c.resolver_calls > 0;
    report.criteria[1] = c.discovered_targets > 0 && c.resolved_targets == c.discovered_targets;
    report.criteria[2] =
        c.discovered_targets > 0 && c.instrumented_targets == c.discovered_targets;
    report.criteria[3] = report.audit_enabled && c.unknown_dispatch_targets == 0 &&
                         c.unknown_dispatch_calls == 0;
    report.criteria[4] = report.audit_enabled &&
                         c.startup_calls + c.frame_calls + c.open_tail_calls == c.total_calls &&
                         c.audit_mismatched_frames == 0 && c.function_sum_mismatches == 0;
    report.criteria[5] = c.role_conflicts == 0 && c.dropped_records == 0 &&
                         c.table_overflows == 0 && c.missed_resolver_returns == 0 &&
                         c.unresolved_pending_calls == 0;
    report.criteria[6] = presents > 0 && c.frames_closed == presents &&
                         (c.nx2_presents == 0 || c.nx2_frames_closed > 0) &&
                         (c.legacy_presents == 0 || c.legacy_frames_closed > 0) &&
                         c.output_id_gaps == 0 && c.output_id_duplicates == 0;
    bool all = true;
    for (const bool criterion : report.criteria) {
        all = all && criterion;
    }
    report.coverage_complete = report.run_complete && all;
    report.library_replay_ready =
        report.coverage_complete && c.unsupported_argument_schemas == 0 && report.closure_ok;
}

Json ToJson(const CaptureReport& report) {
    Json json = Json::object();
    json["version"] = ReportSectionVersion;
    json["status"] = std::string{SectionStatusName(report.status)};
    json["run_complete"] = report.run_complete;
    json["dependency_complete"] = report.dependency_complete;
    json["library_replay_ready"] = report.library_replay_ready;
    json["library_section"] = report.library_section;
    json["format_major"] = report.format_major;
    json["format_minor"] = report.format_minor;
    json["first_frame"] = report.first_frame;
    json["requested_frames"] = report.requested_frames;
    json["finalized_frames"] = report.finalized_frames;
    json["preroll_frames"] = report.preroll_frames;
    json["file_bytes"] = report.file_bytes;
    json["chunks"] = report.chunks;
    json["gpu_words"] = report.gpu_words;
    json["library_calls"] = report.library_calls;
    json["initial_backing_bytes"] = report.initial_backing_bytes;
    json["delta_bytes"] = report.delta_bytes;
    json["dedup_bytes"] = report.dedup_bytes;
    json["incomplete_reasons"] = report.incomplete_reasons;
    json["dropped_events"] = report.dropped_events;
    json["unresolved_resources"] = report.unresolved_resources;
    json["outputs_recorded"] = report.outputs_recorded;
    json["readbacks_recorded"] = report.readbacks_recorded;
    json["outputs_not_comparable"] = report.outputs_not_comparable;
    json["syncpoints_skipped"] = report.syncpoints_skipped;
    json["outputs_overflowed"] = report.outputs_overflowed;
    json["pins_unknown"] = report.pins_unknown;
    json["error"] = Text(report.error);
    return json;
}

Json ToJson(const ReplayReport& report) {
    Json json = Json::object();
    json["version"] = ReportSectionVersion;
    json["status"] = std::string{SectionStatusName(report.status)};
    json["run_complete"] = report.run_complete;
    json["dependency_complete"] = report.dependency_complete;
    json["benchmark"] = report.benchmark;
    json["format_major"] = report.format_major;
    json["format_minor"] = report.format_minor;
    json["capture_session_id"] = Hex(report.capture_session_id);
    json["capture_build_id"] = Hex(report.capture_build_id);
    json["capture_chain_digest"] = Hex(report.capture_chain_digest);
    json["replay_guest_call_count"] = report.replay_guest_call_count ? Json(*report.replay_guest_call_count) : Json(nullptr);
    json["settings_hash"] = Hex(report.settings_hash);
    json["frames_expected"] = report.frames_expected;
    json["frames_replayed"] = report.frames_replayed;
    json["preroll_frames_executed"] = report.preroll_frames_executed;
    json["pictures_written"] = report.pictures_written;
    json["identical_frames"] = report.identical_frames;
    json["semantic_mismatches"] = report.semantic_mismatches;
    json["replay_digest"] = Hex(report.replay_digest);
    json["setup_ms"] = report.setup_ms;
    json["replay_ms"] = report.replay_ms;
    json["error"] = Text(report.error);
    if (!report.error_kind.empty()) {
        json["error_kind"] = Text(report.error_kind);
    }
    return json;
}

Json ToJson(const CensusReport& report) {
    const CensusCounters& c = report.counters;
    Json criteria = Json::object();
    for (size_t i = 0; i < report.criteria.size(); ++i) {
        criteria[std::string{CensusCriterionNames[i]}] = report.criteria[i];
    }
    Json counters = Json::object();
    counters["resolver_calls"] = c.resolver_calls;
    counters["discovered_targets"] = c.discovered_targets;
    counters["resolved_targets"] = c.resolved_targets;
    counters["instrumented_targets"] = c.instrumented_targets;
    counters["unknown_dispatch_targets"] = c.unknown_dispatch_targets;
    counters["unknown_dispatch_calls"] = c.unknown_dispatch_calls;
    counters["startup_calls"] = c.startup_calls;
    counters["frame_calls"] = c.frame_calls;
    counters["open_tail_calls"] = c.open_tail_calls;
    counters["total_calls"] = c.total_calls;
    counters["audited_calls"] = c.audited_calls;
    counters["audit_mismatched_frames"] = c.audit_mismatched_frames;
    counters["function_sum_mismatches"] = c.function_sum_mismatches;
    counters["role_conflicts"] = c.role_conflicts;
    counters["dropped_records"] = c.dropped_records;
    counters["table_overflows"] = c.table_overflows;
    counters["missed_resolver_returns"] = c.missed_resolver_returns;
    counters["unresolved_pending_calls"] = c.unresolved_pending_calls;
    counters["unsupported_argument_schemas"] = c.unsupported_argument_schemas;
    counters["null_results"] = c.null_results;
    counters["nx2_presents"] = c.nx2_presents;
    counters["legacy_presents"] = c.legacy_presents;
    counters["nx2_frames_closed"] = c.nx2_frames_closed;
    counters["legacy_frames_closed"] = c.legacy_frames_closed;
    counters["frames_closed"] = c.frames_closed;
    counters["output_id_gaps"] = c.output_id_gaps;
    counters["output_id_duplicates"] = c.output_id_duplicates;

    Json json = Json::object();
    json["version"] = ReportSectionVersion;
    json["schema"] = CensusSchemaVersion;
    json["run_complete"] = report.run_complete;
    json["coverage_complete"] = report.coverage_complete;
    json["library_replay_ready"] = report.library_replay_ready;
    json["bootstrap_found"] = report.bootstrap_found;
    json["audit_enabled"] = report.audit_enabled;
    json["closure_ok"] = report.closure_ok;
    json["sidecar_written"] = report.sidecar_written;
    json["resolved_target_share"] = Share(c.resolved_targets, c.discovered_targets);
    json["instrumented_target_share"] = Share(c.instrumented_targets, c.discovered_targets);
    json["criteria"] = std::move(criteria);
    json["counters"] = std::move(counters);
    return json;
}

Json ToJson(const FrameTimingReport& report) {
    Json json = Json::object();
    json["version"] = ReportSectionVersion;
    json["status"] = std::string{SectionStatusName(report.status)};
    json["source"] = Text(report.source);
    json["frames"] = report.frames;
    json["recorded"] = report.recorded;
    json["lost"] = report.lost;
    json["first_ns"] = report.first_ns;
    json["last_ns"] = report.last_ns;
    json["raw_file_written"] = report.raw_file_written;
    return json;
}

Json ToJson(const TestHookReport& report) {
    Json json = Json::object();
    json["version"] = ReportSectionVersion;
    json["frame_delay_enabled"] = report.frame_delay_enabled;
    json["target"] = Text(report.target);
    json["requested_ns"] = report.requested_ns;
    json["frames_delayed"] = report.frames_delayed;
    json["added_ns_total"] = report.added_ns_total;
    json["max_added_ns"] = report.max_added_ns;
    json["env_error"] = Text(report.env_error);
    return json;
}

std::string LivePictureName(u64 frame_id) {
    return fmt::format("live_{:08}.png", frame_id);
}

std::string ReplayPictureName(u64 frame_id) {
    return fmt::format("replay_{:08}.png", frame_id);
}

std::string ReplayFrameLine(const ReplayFrameRecord& record) {
    return fmt::format(
        "{{\"schema\":{},\"frame_id\":{},\"width\":{},\"height\":{},\"live_checksum\":\"{}\","
        "\"replay_checksum\":\"{}\",\"identical\":{},\"semantic_mismatches\":{},"
        "\"replay_ns\":{},\"call_count\":{},\"effect_count\":{},\"replay_guest_call_count\":{},\"live_png\":\"{}\","
        "\"replay_png\":\"{}\"}}",
        ReplayFrameLineSchema, record.frame_id, record.width, record.height,
        Hex(record.live_checksum), Hex(record.replay_checksum),
        record.live_checksum == record.replay_checksum ? "true" : "false",
        record.semantic_mismatches, record.replay_ns, record.call_count, record.effect_count,
        record.replay_guest_call_count, LivePictureName(record.frame_id), ReplayPictureName(record.frame_id));
}

// Preroll has a composed replay picture but no expected live picture or equivalence verdict.
std::string ReplayPrerollFrameLine(const ReplayFrameRecord& record, u64 output_id) {
    return Json{{"schema", 1}, {"frame_id", record.frame_id}, {"output_id", output_id},
                {"width", record.width}, {"height", record.height},
                {"has_live_picture", false}, {"replay_checksum", Hex(record.replay_checksum)},
                {"call_count", record.call_count}, {"effect_count", record.effect_count},
                {"replay_guest_call_count", record.replay_guest_call_count}}.dump();
}

void PublishCaptureReport(const CaptureReport& report) {
    Published& store = Store();
    const std::scoped_lock lock{store.mutex};
    store.capture = report;
}

void PublishReplayReport(const ReplayReport& report) {
    Published& store = Store();
    const std::scoped_lock lock{store.mutex};
    store.replay = report;
}

void PublishCensusReport(const CensusReport& report) {
    Published& store = Store();
    const std::scoped_lock lock{store.mutex};
    store.census = report;
}

std::optional<CaptureReport> PublishedCaptureReport() {
    Published& store = Store();
    const std::scoped_lock lock{store.mutex};
    return store.capture;
}

std::optional<ReplayReport> PublishedReplayReport() {
    Published& store = Store();
    const std::scoped_lock lock{store.mutex};
    return store.replay;
}

std::optional<CensusReport> PublishedCensusReport() {
    Published& store = Store();
    const std::scoped_lock lock{store.mutex};
    return store.census;
}

void ClearPublishedReports() {
    Published& store = Store();
    const std::scoped_lock lock{store.mutex};
    store.capture.reset();
    store.replay.reset();
    store.census.reset();
}

} // namespace VideoCore::Replay
