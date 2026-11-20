// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Frozen: the raw per-frame timing log the regression check reads (--frame-times). Slice E
// implements frame_timing_log.cpp. The log records one steady-clock time per frame; the lab
// derives frame times, the median and the low end from it, so nothing here summarizes.
//
// A sample is taken where a frame completes from the player's point of view: on the accepted
// present of a live run (Source::HostPresent, the id is the logical accepted-present id of
// replay/run_state.h) and at FRAME_END of a replayed frame (Source::ReplayFrame, the id is the
// captured frame id). The clock is std::chrono::steady_clock in nanoseconds since an arbitrary
// epoch; only differences mean anything.
//
// Loss is never silent. The log is preallocated at Start (capacity * 16 bytes, allocation failure
// fails Start); Record is wait-free (one relaxed fetch_add, two stores) and never allocates; a
// sample that does not fit is counted in `lost` and the log is then incomplete. Stop writes the
// file once, off the hot path, as JSON Lines, to "<path>.partial" and renames it without
// replacing an existing file; a run that did not end normally, or lost a sample, still writes the
// file but marks it incomplete in the trailer. The file never holds paths or titles.
//
// File format (UTF-8, one JSON object per line):
//   {"schema":1,"kind":"frame_times","source":"host_present","clock":"steady_ns","capacity":N}
//   {"id":F,"t":T}                                  repeated, in record order
//   {"end":true,"recorded":R,"lost":L,"complete":B} the last line; a file without it is invalid
// `complete` is true only when lost == 0 and the run ended normally.

#pragma once

#include <filesystem>
#include <string>

#include "common/common_types.h"

namespace Common::FrameTiming {

enum class Source : u32 { HostPresent = 1, ReplayFrame = 2 };

inline constexpr u64 DefaultCapacity = 1ULL << 20;  // 1,048,576 frames, 16 MiB
inline constexpr u64 MaxCapacity = 1ULL << 24;      // 16,777,216 frames, 256 MiB

struct Config {
    std::string path;          // must end in ".jsonl"; the caller already checked it is private
    Source source = Source::HostPresent;
    u64 capacity = DefaultCapacity;
};

namespace detail {
bool Active() noexcept;
void RecordSlow(u64 frame_id) noexcept;
} // namespace detail

// Preallocates and arms the log. False with a message on a bad config or an allocation failure;
// nothing is left behind. One log per process.
[[nodiscard]] bool Start(const Config& config, std::string& error);

// Records the completion of frame `frame_id` at the current steady-clock time.
inline void Record(u64 frame_id) noexcept {
    if (detail::Active()) {
        detail::RecordSlow(frame_id);
    }
}

struct Summary {
    bool started = false;
    bool stopped = false;
    Source source = Source::HostPresent;
    u64 recorded = 0;
    u64 lost = 0;
    u64 first_ns = 0;
    u64 last_ns = 0;
    bool file_written = false;
    bool complete = false;
};

// Ends recording and writes the file. Returns the summary (also kept for GetSummary).
Summary Stop(bool run_ended_normally, std::string& error);

// Writes the file the way Stop(false) does, for a forced exit: the trailer says `complete:false`.
// Safe to call from any thread and more than once: it does nothing, and returns false with the
// reason in `error`, when the log was never started, was already stopped, or another thread is
// inside Stop (it never waits). Returns true when this call wrote the file.
bool StopIncomplete(std::string& error);

[[nodiscard]] Summary GetSummary();

namespace detail {

enum class PublishResult { Published, Exists, Failed };

// Renames `partial` to `final_path` and never replaces an existing `final_path` (Exists, nothing
// touched). Failed is any other error.
[[nodiscard]] PublishResult PublishNoReplace(const std::filesystem::path& partial,
                                             const std::filesystem::path& final_path) noexcept;

} // namespace detail

} // namespace Common::FrameTiming
