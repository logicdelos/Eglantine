// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Frozen: the result structures of the 0.6 modes and their JSON in the run report. Slice A
// implements report_json.cpp (formatters, the census coverage evaluation, the sidecar line
// writer); slices B, C, D and E fill their structure and publish it; nothing else changes the
// schema.
//
// Run report integration. The run report stays schema 2 (run_report.h). Each mode adds ONE
// optional member, present only when that mode ran, each with its own "version" (1):
//   "capture"       the capture of slice B
//   "replay"        the replay of slice C
//   "census"        the call census of slice D
//   "frame_timing"  the frame-timing log of slice E
//   "test_hooks"    the calibrated delay injection of slice E
// Consumers must ignore members they do not know. A mode never writes a path: sidecars and
// captures are named by the operator on the command line and stay out of the report.
//
// Keys of every member (all always present once the member is present, null where a value does not
// apply): see report_json.cpp, which is the single place that spells them; the keys are pinned by
// the replay_report test.
//
// Replay image directory (named by --replay-images; slice C writes it, the lab's frame_compare.py
// reads it): for every compared frame N, with N zero-padded to 8 digits,
//   live_N.png      the live end picture recorded in the capture (decoded from END_PICTURE)
//   replay_N.png    the picture the replay produced, same size, same pixel format
//   frames.jsonl    one ReplayFrameRecord line per frame (ReplayFrameLine)
// PNG files are 8 bit per channel RGBA sRGB. File names are bare names with no directory part.

#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "common/common_types.h"

namespace VideoCore::Replay {

inline constexpr u32 ReportSectionVersion = 1;
inline constexpr u32 CensusSchemaVersion = 2;
inline constexpr u32 ReplayFrameLineSchema = 2;
inline constexpr u32 FrameTimingLineSchema = 1;

enum class SectionStatus : u32 { Off = 0, Complete = 1, Incomplete = 2, Failed = 3 };

[[nodiscard]] constexpr std::string_view SectionStatusName(SectionStatus status) noexcept {
    switch (status) {
    case SectionStatus::Off: return "off";
    case SectionStatus::Complete: return "complete";
    case SectionStatus::Incomplete: return "incomplete";
    case SectionStatus::Failed: return "failed";
    }
    return "unknown";
}

struct CaptureReport {
    SectionStatus status = SectionStatus::Off;
    bool run_complete = false;
    bool dependency_complete = false;
    bool library_replay_ready = false;
    bool library_section = false;
    u32 format_major = 0;
    u32 format_minor = 0;
    u64 first_frame = 0;
    u32 requested_frames = 0;
    u32 finalized_frames = 0;
    u32 preroll_frames = 0;
    u64 file_bytes = 0;
    u64 chunks = 0;
    u64 gpu_words = 0;
    u64 library_calls = 0;
    u64 initial_backing_bytes = 0;
    u64 delta_bytes = 0;
    u64 dedup_bytes = 0;
    u32 incomplete_reasons = 0;  // IncompleteReason bits
    u32 dropped_events = 0;
    u32 unresolved_resources = 0;
    // The semantic-output census (FRAME_END outputs); see CaptureOutputStats in memory_capture.h.
    u64 outputs_recorded = 0;
    u64 readbacks_recorded = 0;
    u64 outputs_not_comparable = 0;
    u64 syncpoints_skipped = 0;
    u64 outputs_overflowed = 0;
    u64 pins_unknown = 0;
    std::string error;           // short reason when status != Complete, empty otherwise
};

struct ReplayReport {
    SectionStatus status = SectionStatus::Off;
    bool run_complete = false;
    bool dependency_complete = false;
    bool benchmark = false;
    u32 format_major = 0;
    u32 format_minor = 0;
    u64 capture_session_id = 0;
    u64 capture_build_id = 0;
    u64 capture_chain_digest = 0;
    std::optional<u64> replay_guest_call_count; // unknown until the no-process check succeeds
    u64 settings_hash = 0;
    u64 frames_expected = 0;
    u64 frames_replayed = 0;
    u64 preroll_frames_executed = 0;
    u64 pictures_written = 0;
    u64 identical_frames = 0;       // replay checksum equals the live checksum (informational)
    u64 semantic_mismatches = 0;
    u64 replay_digest = 0;          // FNV-1a chain over the replay checksum of every frame
    double setup_ms = 0.0;
    double replay_ms = 0.0;
    std::string error;
    std::string error_kind;  // ReadErrorName of the refusal when the stop has one, else empty
};

// One line of <replay-images>/frames.jsonl.
struct ReplayFrameRecord {
    u64 frame_id = 0;
    u32 width = 0;
    u32 height = 0;
    u64 live_checksum = 0;
    u64 replay_checksum = 0;
    u32 semantic_mismatches = 0;
    u64 replay_ns = 0;
    // The FRAME_END declarations of the captured frame.
    u64 call_count = 0;
    u32 effect_count = 0;
    u64 replay_guest_call_count = 0; // independent of the capture's call_count
};

// The counters of the call census (plan 5.1). Slice D fills them; the criteria below are pure
// functions of the counters, so the same evaluation runs in the emulator, in the audit and in the
// lab's census report.
struct CensusCounters {
    u64 resolver_calls = 0;
    u64 discovered_targets = 0;
    u64 resolved_targets = 0;
    u64 instrumented_targets = 0;
    u64 unknown_dispatch_targets = 0;
    u64 unknown_dispatch_calls = 0;
    u64 startup_calls = 0;
    u64 frame_calls = 0;
    u64 open_tail_calls = 0;
    u64 total_calls = 0;
    u64 audited_calls = 0;
    u64 audit_mismatched_frames = 0;
    u64 function_sum_mismatches = 0;
    u64 role_conflicts = 0;
    u64 dropped_records = 0;
    u64 table_overflows = 0;
    u64 missed_resolver_returns = 0;
    u64 unresolved_pending_calls = 0;
    u64 unsupported_argument_schemas = 0;
    u64 null_results = 0;  // informational, never a failure
    u64 nx2_presents = 0;
    u64 legacy_presents = 0;
    u64 nx2_frames_closed = 0;
    u64 legacy_frames_closed = 0;
    u64 frames_closed = 0;
    u64 output_id_gaps = 0;
    u64 output_id_duplicates = 0;
};

inline constexpr u32 CensusCriterionCount = 7;

// The seven coverage criteria of plan 5.2, in order.
inline constexpr std::array<std::string_view, CensusCriterionCount> CensusCriterionNames{
    "bootstrap_and_resolver",
    "all_targets_resolved",
    "all_targets_instrumented",
    "no_unknown_dispatch",
    "per_frame_sums_close",
    "no_loss",
    "both_present_paths_close_frames",
};

struct CensusReport {
    CensusCounters counters;
    bool audit_enabled = false;
    bool bootstrap_found = false;
    bool run_complete = false;    // the run reached its normal end and the census stopped cleanly
    bool closure_ok = false;      // the capture closure check passed (false without a capture)
    bool sidecar_written = false;
    // Outputs of EvaluateCoverage:
    std::array<bool, CensusCriterionCount> criteria{};
    bool coverage_complete = false;
    bool library_replay_ready = false;
};

// Sets criteria, coverage_complete and library_replay_ready from the counters and flags:
//   1 bootstrap_found && resolver_calls > 0
//   2 discovered_targets > 0 && resolved_targets == discovered_targets
//   3 discovered_targets > 0 && instrumented_targets == discovered_targets
//   4 audit_enabled && unknown_dispatch_targets == 0 && unknown_dispatch_calls == 0
//   5 audit_enabled && startup_calls + frame_calls + open_tail_calls == total_calls &&
//     audit_mismatched_frames == 0 && function_sum_mismatches == 0
//   6 role_conflicts, dropped_records, table_overflows, missed_resolver_returns and
//     unresolved_pending_calls are all zero
//   7 at least one present, frames_closed == nx2_presents + legacy_presents, every path that
//     presented closed at least one frame, output_id_gaps == 0 and output_id_duplicates == 0
// coverage_complete = run_complete && all seven; library_replay_ready = coverage_complete &&
// unsupported_argument_schemas == 0 && closure_ok.
void EvaluateCoverage(CensusReport& report) noexcept;

struct FrameTimingReport {
    SectionStatus status = SectionStatus::Off;
    std::string source;          // "host_present" or "replay_frame"
    u64 frames = 0;
    u64 recorded = 0;
    u64 lost = 0;                // frames that did not fit the preallocated log: never silent
    u64 first_ns = 0;
    u64 last_ns = 0;
    bool raw_file_written = false;
};

struct TestHookReport {
    bool frame_delay_enabled = false;
    std::string target;          // "cpu-present", "gpu-frame" or "" when disabled
    u64 requested_ns = 0;
    u64 frames_delayed = 0;
    u64 added_ns_total = 0;
    u64 max_added_ns = 0;
    std::string env_error;       // set when an environment value was present but invalid
};

[[nodiscard]] nlohmann::json ToJson(const CaptureReport& report);
[[nodiscard]] nlohmann::json ToJson(const ReplayReport& report);
[[nodiscard]] nlohmann::json ToJson(const CensusReport& report);
[[nodiscard]] nlohmann::json ToJson(const FrameTimingReport& report);
[[nodiscard]] nlohmann::json ToJson(const TestHookReport& report);

// One JSONL line (no trailing newline) of frames.jsonl, keys in a fixed order.
[[nodiscard]] std::string ReplayFrameLine(const ReplayFrameRecord& record);
[[nodiscard]] std::string ReplayPrerollFrameLine(const ReplayFrameRecord& record, u64 output_id);
// File names inside the replay image directory.
[[nodiscard]] std::string LivePictureName(u64 frame_id);
[[nodiscard]] std::string ReplayPictureName(u64 frame_id);

// The capture, replay and census modes publish their result once, when their mode ends;
// thread-safe, last write wins.
void PublishCaptureReport(const CaptureReport& report);
void PublishReplayReport(const ReplayReport& report);
void PublishCensusReport(const CensusReport& report);
[[nodiscard]] std::optional<CaptureReport> PublishedCaptureReport();
[[nodiscard]] std::optional<ReplayReport> PublishedReplayReport();
[[nodiscard]] std::optional<CensusReport> PublishedCensusReport();
// Forgets every published result (tests only).
void ClearPublishedReports();

// The one-line hook of the run report (report_sections.cpp): adds "capture", "replay" and "census"
// for every mode that published to `document`, and nothing for a mode that did not run. The run
// report writes "test_hooks" itself (common/test_frame_delay.h).
void AddReportSections(nlohmann::json& document);

} // namespace VideoCore::Replay
