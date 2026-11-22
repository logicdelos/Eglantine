// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "common/common_types.h"

namespace Common::SystemLog {

enum class Subsystem : u8 {
    Graphics,
    Gpu,
    Audio,
    Kernel,
    Service,
    Filesystem,
    Input,
    Core,
    Memory,
    Crash,
    Marker,
    Log,
};

enum class Severity : u8 {
    Trace,
    Info,
    Warning,
    Error,
    Critical,
};

enum class EventKind : u16 {
    GpuSubmit,
    GpuRenderPass,
    GpuFenceWait,
    GpuPresent,
    GpuError,
    GpuDeviceLost,
    GpuSwapchainRecreate,
    FrameHitch,
    AudioUnderrun,
    AudioError,
    AudioSummary,
    FsShortRead,
    FsSlowRead,
    FsError,
    SchedSummary,
    KernelStall,
    IpcSummary,
    IpcError,
    InputSummary,
    LogWarning,
    LogError,
    LogCritical,
    Marker,
    HostCrash,
};

struct Event {
    u64 seq{};
    u64 mono_ns{};
    u64 present_id{};
    Subsystem subsystem{};
    Severity severity{};
    EventKind kind{};
    u32 thread_id{};
    u64 arg0{};
    u64 arg1{};
    u64 arg2{};
    s64 arg3{};
    char detail[96]{};
};

struct SourceInfo {
    const char* name{};
    bool enabled{};
    u64 recorded{};
    u64 dropped{};
    const char* missing_reason{};
};

struct RingStatus {
    std::size_t capacity{};
    u64 newest{};
    u64 oldest{};
    u64 evicted{};
    u64 dropped{};
    bool diagnostic_mode{};
};

struct SnapshotResult {
    std::vector<Event> events;
    RingStatus status{};
    std::vector<SourceInfo> sources;
    /// Retained rows that matched the selection but were left out by the `max_events` bound.
    u64 omitted{};
    /// Rows after `after_seq` that the ring had already evicted (`Snapshot` only).
    u64 lost{};
};

struct EventQuery {
    u64 after_seq{};
    std::size_t limit{200};
    bool filter_subsystem{};
    Subsystem subsystem{};
    Severity min_severity{Severity::Trace};
};

/// One page of a filtered read. `next` is the cursor to pass as `after_seq` for the next page: the
/// last raw row the page is complete up to, whether or not it matched, so a match behind any
/// number of non-matching rows is still reached.
struct EventPage {
    std::vector<Event> events;
    RingStatus status{};
    std::vector<SourceInfo> sources;
    u64 next{};
    bool more{};
    /// Rows after `after_seq` evicted before this read.
    u64 lost{};
};

struct QuickMarkResult {
    u64 mark_id{};
    u64 seq{};
    u64 present_id{};
    std::filesystem::path directory;
    std::filesystem::path before_log;
    std::filesystem::path summary_path;
    bool ok{};
    /// False when the delayed aftermath file could not be queued (the queue is bounded).
    bool aftermath_scheduled{};
};

/// What the crash handler knows without looking anything up; written as the tail's last row.
struct CrashInfo {
    u64 code{};
    u64 address{};
    u32 thread_id{};
    const char* module{};
};

[[nodiscard]] bool DiagnosticEnabled() noexcept;
[[nodiscard]] bool GpuTraceEnabled() noexcept;
[[nodiscard]] bool AudioSummaryEnabled() noexcept;
[[nodiscard]] bool FsSummaryEnabled() noexcept;

void SetPresentCounter(const std::atomic<u64>* counter) noexcept;

u64 Record(Subsystem subsystem, Severity severity, EventKind kind, u64 arg0 = 0, u64 arg1 = 0,
           u64 arg2 = 0, s64 arg3 = 0, std::string_view detail = {}) noexcept;

/// The oldest `max_events` retained rows with seq > `after_seq`; `omitted` counts the newer ones
/// left out and `lost` the rows after `after_seq` the ring evicted first.
[[nodiscard]] SnapshotResult Snapshot(u64 after_seq = 0, std::size_t max_events = 500) noexcept;
/// The newest `max_events` retained rows with seq <= `through_seq`, oldest first; `omitted` counts
/// the older retained rows left out. With the seq of a mark, the window ends at the mark.
[[nodiscard]] SnapshotResult SnapshotThrough(u64 through_seq, std::size_t max_events) noexcept;
/// Rows matching the query, oldest first, with a cursor that advances past non-matching rows.
[[nodiscard]] EventPage QueryEvents(const EventQuery& query) noexcept;
[[nodiscard]] RingStatus GetStatus() noexcept;
[[nodiscard]] std::vector<SourceInfo> GetSources() noexcept;

void ResetForTests(std::size_t capacity = 8192) noexcept;

/// Writes the newest 4096 retained rows (the window ends at the newest row).
[[nodiscard]] bool WriteIncidentJsonl(const std::filesystem::path& path, std::string_view reason,
                                      u64 marker_id = 0) noexcept;

/// Records a Marker event and saves a local incident bundle (before-snapshot + summary, plus an
/// optional delayed aftermath snapshot) under `incidents_root`.
[[nodiscard]] QuickMarkResult TriggerQuickMark(const std::filesystem::path& incidents_root,
                                               std::string_view label = "visual_glitch",
                                               u32 aftermath_delay_ms = 5000) noexcept;

/// Crash-safe tail dump for a crashed process: the log's last rows from fixed stack buffers and the
/// raw file API, with no heap allocation and no C runtime formatting. The ring is read only when
/// its mutex is free at once (try_lock); otherwise the file says so. `crash`, when given, is
/// appended as a HostCrash row whether or not the ring could be read. Best effort: the caller
/// writes its own report and the minidump first.
void DumpCrashTail(const std::filesystem::path::value_type* path,
                   const CrashInfo* crash = nullptr) noexcept;

[[nodiscard]] const char* SubsystemName(Subsystem subsystem) noexcept;
[[nodiscard]] const char* SeverityName(Severity severity) noexcept;
[[nodiscard]] const char* EventKindName(EventKind kind) noexcept;

} // namespace Common::SystemLog
