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
};

struct QuickMarkResult {
    u64 mark_id{};
    u64 seq{};
    u64 present_id{};
    std::filesystem::path directory;
    std::filesystem::path before_log;
    std::filesystem::path summary_path;
    bool ok{};
};

[[nodiscard]] bool DiagnosticEnabled() noexcept;
[[nodiscard]] bool GpuTraceEnabled() noexcept;
[[nodiscard]] bool AudioSummaryEnabled() noexcept;
[[nodiscard]] bool FsSummaryEnabled() noexcept;

void SetPresentCounter(const std::atomic<u64>* counter) noexcept;

u64 Record(Subsystem subsystem, Severity severity, EventKind kind, u64 arg0 = 0, u64 arg1 = 0,
           u64 arg2 = 0, s64 arg3 = 0, std::string_view detail = {}) noexcept;

[[nodiscard]] SnapshotResult Snapshot(u64 after_seq = 0, std::size_t max_events = 500) noexcept;
[[nodiscard]] RingStatus GetStatus() noexcept;
[[nodiscard]] std::vector<SourceInfo> GetSources() noexcept;

void ResetForTests(std::size_t capacity = 8192) noexcept;

[[nodiscard]] bool WriteIncidentJsonl(const std::filesystem::path& path, std::string_view reason,
                                      u64 marker_id = 0) noexcept;

/// Records a Marker event and saves a local incident bundle (before-snapshot + summary, plus an
/// optional delayed aftermath snapshot) under `incidents_root`.
[[nodiscard]] QuickMarkResult TriggerQuickMark(const std::filesystem::path& incidents_root,
                                               std::string_view label = "visual_glitch",
                                               u32 aftermath_delay_ms = 5000) noexcept;

/// Crash-safe tail dump: uses try_lock and fixed stack buffers with zero heap allocations.
void DumpCrashTail(const std::filesystem::path& path) noexcept;

[[nodiscard]] const char* SubsystemName(Subsystem subsystem) noexcept;
[[nodiscard]] const char* SeverityName(Severity severity) noexcept;
[[nodiscard]] const char* EventKindName(EventKind kind) noexcept;

} // namespace Common::SystemLog
