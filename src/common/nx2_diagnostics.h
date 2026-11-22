// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/common_types.h"

// What the log says on its own when a session goes wrong, next to the 10 s report of
// nx2_bottleneck.h (which drives the detectors below once per window): native shader rejections
// counted by class, scene changes, a warning for host memory that stays nearly full, a game that
// presents frames without getting anywhere, and one block of numbers when the emulator stops.
// Always on, and only ever reads: nothing here changes what the guest or the renderer does. The
// per-draw entry points are a relaxed atomic load or a few relaxed adds, with no lock and no
// allocation.
namespace Nx2Bottleneck {

// ---- Shader rejections ----

enum class RejectionClass : u8 {
    AllZeroCode,          ///< the program's first instruction is all zero: its code is not there
    BranchOutsideCapture, ///< a branch leaves the captured program (the end was not found)
    UnsupportedOpcode,    ///< an instruction the lowering does not know
    Other,                ///< anything else (an unsupported mode, scratch registers, resources)
    Count,
};
inline constexpr size_t RejectionClasses = static_cast<size_t>(RejectionClass::Count);
using RejectionCounts = std::array<u64, RejectionClasses>;

/// Which class the exception text of a rejected native shader belongs to.
[[nodiscard]] RejectionClass ClassifyShaderRejection(std::string_view what) noexcept;
[[nodiscard]] const char* RejectionClassName(RejectionClass rejection) noexcept;

/// A native shader was rejected for a reason not seen before (call it where the rejection is logged
/// once per distinct message). Counted per class; the next 10 s report says how many were new in
/// its window: "Shader rejections: 3 new in the last 10 s (...); 17 this session".
void NoteShaderRejection(std::string_view what) noexcept;

/// "3 new in the last 10 s (all-zero code 2, unsupported opcode 1); 17 this session".
[[nodiscard]] std::string FormatShaderRejections(const RejectionCounts& window, u64 session_total);

// ---- Scene changes ----

/// The game moved to a different kind of frame. Two triggers: the set of colour target sizes and
/// formats the draws render to (the first target of each draw) differs from the one before for
/// SceneConfirmFrames frames in a row, or SceneBurstPipelines or more pipelines were built within
/// SceneBurstSeconds. One "Scene change at t=..." line each, at most one every SceneLogSpacing
/// seconds (the ones in between are counted into the next line) and a SceneChange event in the
/// system event ring, so the Ctrl+Alt+F8 bundle holds it.
inline constexpr u32 SceneConfirmFrames = 5;
inline constexpr u32 SceneBurstPipelines = 10;
inline constexpr double SceneBurstSeconds = 2.0;
inline constexpr double SceneLogSpacing = 5.0;

/// The draw that is about to be recorded renders to colour target 0 of this size and format.
/// Called for every draw by the thread that records them: when the target is the one of the draw
/// before it costs one relaxed load and a compare.
void NoteDrawTarget(u32 width, u32 height, u32 format) noexcept;
/// A pipeline was built that this session had not built before (not one from the disk cache).
void NoteNewPipeline() noexcept;
/// The renderer finished a frame (once per presented frame, by the thread that records draws).
void NoteSceneFrame();

/// Decides when a sequence of frame signatures is a scene change: the signature must differ from
/// the accepted one for `confirm` frames in a row (a frame that returns to the accepted one starts
/// over). The first signature seen is accepted without a change. Pure, for the tests.
class SceneTracker {
public:
    explicit SceneTracker(u32 confirm = SceneConfirmFrames) : confirm_{confirm} {}

    /// The signature (never 0) of a frame with draws. True when it completes a change; the frames
    /// the previous signature lasted are then in StableFrames.
    bool OnFrame(u64 signature) noexcept;

    [[nodiscard]] u64 Accepted() const noexcept {
        return accepted_;
    }
    /// Frames the signature before the last change was seen for.
    [[nodiscard]] u64 PreviousStableFrames() const noexcept {
        return previous_stable_;
    }

private:
    u32 confirm_;
    u64 accepted_{};
    u64 candidate_{};
    u32 candidate_run_{};
    u64 stable_{};
    u64 previous_stable_{};
};

/// Rate limit for log lines: Allow(now) is true when `spacing` seconds passed since the last true
/// one; the refused calls are counted until then. Pure, for the tests.
class LineLimiter {
public:
    explicit LineLimiter(double spacing_seconds) : spacing_{spacing_seconds} {}
    bool Allow(double now_seconds) noexcept;
    /// Refused calls since the last allowed one, then 0.
    u64 TakeSuppressed() noexcept {
        const u64 count = suppressed_;
        suppressed_ = 0;
        return count;
    }

private:
    double spacing_;
    double last_{};
    bool any_{};
    u64 suppressed_{};
};

/// Counts new pipelines over a sliding span: Add(now, n) returns the count in the span when it
/// reaches `threshold`, once per burst (the burst is over when the span holds under half the
/// threshold again). Pure, for the tests.
class BurstDetector {
public:
    BurstDetector(u32 threshold, double span_seconds) : threshold_{threshold}, span_{span_seconds} {}
    /// `count` events at `now_seconds` (0 to just move time on). The number in the span when a
    /// burst starts, else 0.
    u32 Add(double now_seconds, u32 count = 1);

private:
    u32 threshold_;
    double span_;
    bool in_burst_{};
    std::vector<std::pair<double, u32>> events_;
};

// ---- Host memory ----

struct HostMemory {
    bool known{};
    u64 free_bytes{};
    u64 total_bytes{};
    double commit_fraction{}; ///< share of the host's commit limit in use
    u64 process_committed{};  ///< what the emulator committed (private bytes)
    u64 process_working_set{};
};

/// Where the host stands now (Windows only; `known` is false elsewhere).
[[nodiscard]] HostMemory ReadHostMemory() noexcept;

/// Nearly out of memory: under 1 GiB of RAM free, or 95% of the commit limit in use.
inline constexpr u64 LowMemoryFreeBytes = u64{1} << 30;
inline constexpr double LowMemoryCommitFraction = 0.95;
[[nodiscard]] bool IsLowMemory(const HostMemory& memory) noexcept;

/// Warn once when memory has been low for LowMemorySustainedSeconds without a break, and again only
/// after it recovered. Pure, for the tests.
inline constexpr double LowMemorySustainedSeconds = 30.0;
class LowMemoryDetector {
public:
    enum class Action { None, Warn, Recovered };
    Action Update(double now_seconds, bool low) noexcept;
    /// How long memory has been low without a break, at `now_seconds`.
    [[nodiscard]] double LowFor(double now_seconds) const noexcept {
        return low_ ? now_seconds - low_since_ : 0.0;
    }

private:
    bool low_{};
    bool warned_{};
    double low_since_{};
};

// ---- No progress ----

/// A game that presents frames and gets nowhere: one guest core is full the whole time, no file is
/// read and its calls are SleepThread that gave the CPU to nobody. The facts of one whole 10 s
/// window of the report.
struct ProgressWindow {
    bool whole{};              ///< not a start, pause or end of the run
    double seconds{};          ///< length of the window
    u32 frames{};
    double busiest_core_run{}; ///< share of the window the busiest core spent in guest code
    size_t busiest_core{};
    u64 file_reads{};
    u64 empty_yields{};        ///< SleepThread calls that yielded to no other thread
    u64 svc_calls{};           ///< every SVC the guest called
};
inline constexpr double NoProgressCoreShare = 0.95;
inline constexpr u32 NoProgressWindows = 2; ///< 2 windows of 10 s: 20 s
[[nodiscard]] bool WindowLooksStuck(const ProgressWindow& window) noexcept;

/// True once per episode: NoProgressWindows stuck windows in a row. A window that is not stuck (or
/// not whole) ends the episode. Pure, for the tests.
class NoProgressDetector {
public:
    bool Update(const ProgressWindow& window) noexcept;
    [[nodiscard]] u32 Streak() const noexcept {
        return streak_;
    }

private:
    u32 streak_{};
    bool fired_{};
};

/// Where the probe of a stuck core reads the guest from. `read` gives the program counter of the
/// guest code the core runs (and the id of the guest thread on it) and returns false when it
/// cannot; `locate` turns a program counter into "module+0xoffset". Set by the core while a
/// process lives, cleared before it goes; both are only called with the source's lock held.
struct GuestCoreSource {
    bool (*read)(void* user, size_t core, u64& pc, u64& thread_id) = nullptr;
    std::string (*locate)(void* user, u64 pc) = nullptr;
    void* user = nullptr;
};
void SetGuestCoreSource(const GuestCoreSource& source);

// ---- Stop summary ----

/// The 10 s report calls this with the window's first line, so the stop summary can quote the last
/// whole window.
void RememberReport(std::string_view line);

/// One block in the log: why it stopped, the time run, frames presented, the last whole 10 s
/// report, Error/Critical counts, the last 5 distinct error lines, memory, rejections. Written once
/// per process by whichever of the stop paths comes first (a forced stop, a normal stop, the
/// command line's stop).
void WriteStopSummary(const char* reason);

/// The same block for a crashed process, built in fixed buffers without allocation or locks that
/// may be held; each line goes to `emit`. Once per process.
void WriteCrashSummary(void (*emit)(const char* line));

/// Seconds since this process started (the time of the log's first column, give or take startup).
[[nodiscard]] double UptimeSeconds() noexcept;

/// Frames the renderer presented since the process started.
[[nodiscard]] u64 FramesPresented() noexcept;

/// Name of the guest thread with this id (the source of the SleepThread line), empty if unknown.
[[nodiscard]] std::string GuestThreadName(u64 thread_id);

// ---- Driven by the 10 s report (nx2_bottleneck.cpp) ----

/// Whole windows only: the new shader rejections line, and the no-progress detector.
void OnReportWindow(const ProgressWindow& progress);
/// Every tick, whole window or not: host memory.
void OnReportTick();

} // namespace Nx2Bottleneck
