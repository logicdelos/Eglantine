// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>
#include <string_view>

#include "common/common_types.h"
#include "common/run_outcome.h"
#include "video_core/cache_envelope.h"
#include "video_core/nx2_spikes.h"

// The JSON file of --report: what the run did, written once when it ends. The numbers come from
// the Nx2Bottleneck collector and the exit reason from Common::RunOutcome; this only checks the
// destination, gathers the rest and writes the file (to a temporary name first, then renamed).
namespace RunReport {

enum class PrepareResult {
    Ready,
    /// The destination exists, or its directory does not: the caller's mistake (exit code 64).
    BadDestination,
    /// The directory refused a temporary file (exit code 74).
    IoError,
};

/// Checks `destination` (UTF-8) before anything is launched and creates the empty temporary file
/// next to it, so a destination that cannot be written fails now and not after a long run. An
/// existing destination is refused rather than overwritten. `message` says what is wrong.
[[nodiscard]] PrepareResult Prepare(const std::string& destination, std::string& message);

/// Records what is only known at the start: the environment variables that tune the emulator and
/// the settings as they are (after the config file and the command line were applied).
void CaptureStart();

/// The program id of the loaded game, for the report's title_id.
void SetTitleId(u64 program_id);

/// Writes the report for a run that ended for `reason` with `code`. `shutdown_complete` is false
/// when the process is being ended without the teardown having finished, which also marks the
/// numbers incomplete. Safe to call from two threads (the second call does nothing and returns
/// the first one's result); returns false, after saying why on stderr, when nothing was written.
/// Does nothing and returns true when Prepare was never called.
[[nodiscard]] bool Publish(Common::RunOutcome::Reason reason, int code, bool shutdown_complete,
                           std::string_view detail);

/// Ends the process after a failure that cannot return to the frontend's own shutdown (a window
/// constructor, a network callback). In an unattended run the failure is recorded as a startup
/// error and the report is written first, and the exit code is the one of the reason on record;
/// otherwise the code is 1.
[[noreturn]] void ExitWithFailure(std::string_view detail);

// Schema 2 (milestone 0.5, plan_0.5.md section 2.5): the normal report grows four members, the
// crash report a safe scalar status of the same. Schema 1 members keep their names and meanings
// (summary, windows, identity, settings, exit completeness). All timestamps are nanoseconds of
// the NowNs clock, all durations integer nanoseconds; a value that is not available is null next
// to a status, never zero. Slice F implements this: OpenLongFrameSpool in run_report.cpp, the
// formatters in report_schema2.cpp, which the unit tests compile on their own and which must not
// use run_report.cpp. Slice B feeds it through the LongFrameSink and the snapshot, slice D
// through the cache report.
//
//   "schema_version": 2
//   "attribution": { "source": "host_present_submission", "threshold_ns", "enabled", "complete",
//       "lost_events", "sink_write_errors", "rejected_presents", "unknown_wall_fraction",
//       "unknown_primary_frame_fraction", "producer_unknown_wait_ns",
//       "long_frames": [LongFrame...] }
//   LongFrame: { "id", "start_ns", "end_ns", "duration_ns", "cause_ns": {<cause>: ns...},
//       "activity_ns": {<cause>: ns...}, "primary_cause", "confidence_permille",
//       "unknown_reasons": [<name>...], "counter_deltas": {<event>: count, nonzero only},
//       "critical_work_ids": [...], "present_result", "instrumentation_complete" }
//   "startup": { "process_created": {"ns", "uncertainty_ns"} | null, "first_host_present_ns",
//       "process_to_first_present": {"duration_ns", "cause_ns", "status"},
//       "first_present_to_interactive": {"duration_ns", "cause_ns", "status"},
//       "interactive_scene": {"id", "marker_ns", "origin", "scene_check", "status"},
//       "phases": [{"phase", "outcome", "begin_ns", "end_ns"}...] }
//   "first_120s": { "anchor": "first_real_host_present", "complete", "elapsed_ns", "frames",
//       "frames_over_100ms", "max_ns", "crossing_tail_interval_ns", "scene_check" }
//   "cache": { "identity": {...}, "files": [{"kind", "status": "accepted"|"rejected"|"missing",
//       "reason", "expected", "found", "records"}...], "recorded", "prepared", "adopted",
//       "demand_promoted", "demand_wait_ns", "prepare_failures", "records_skipped",
//       "write_status", "queue_high_water", "memory_high_water_bytes", "budget_pauses" }
// <cause> is a Nx2Spikes::CauseName, <event> an Event name, "reason" a RejectReasonName.
namespace Schema2 {

inline constexpr int SchemaVersion = 2;

/// Creates the long-frame spool (a JSONL temporary file next to the report destination given to
/// Prepare) and installs the Nx2Spikes long frame sink that appends LongFrameJson lines to it.
/// False, with the reason in `message`, when it cannot: attribution then reports itself
/// incomplete. Call after Prepare and before Nx2Spikes::BeginSession; does nothing and returns
/// true when Prepare was never called.
[[nodiscard]] bool OpenLongFrameSpool(std::string& message);

/// One compact JSON object (no newline) for a long frame.
[[nodiscard]] std::string LongFrameJson(const VideoCommon::Nx2Spikes::LongFrame& frame);

/// The "attribution" object up to and including `"long_frames":[`. The writer appends the
/// spooled lines separated by commas, then `]}`, so that no long frame list is held in memory.
[[nodiscard]] std::string AttributionJsonHead(const VideoCommon::Nx2Spikes::AttributionSnapshot& snapshot);

/// The "startup" and "first_120s" objects.
[[nodiscard]] std::string StartupJson(const VideoCommon::Nx2Spikes::AttributionSnapshot& snapshot);
[[nodiscard]] std::string First120sJson(const VideoCommon::Nx2Spikes::AttributionSnapshot& snapshot);

/// The "cache" object.
[[nodiscard]] std::string CacheJson(const VideoCommon::CacheEnvelope::CacheReport& report);

} // namespace Schema2

} // namespace RunReport
