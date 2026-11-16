// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>
#include <string>
#include <unordered_set>
#include <utility>

#include <fmt/format.h>

#include "common/common_types.h"
#include "common/logging.h"
#include "video_core/nx2_spikes.h"
#include "video_core/renderer_vulkan/vk_driver_build_report.h"

namespace Vulkan::DriverBuildReport {
namespace {

using Clock = std::chrono::steady_clock;
using Duration = Clock::duration;

// Over this, the driver compiled the pipeline instead of taking it from its cache.
constexpr std::chrono::milliseconds CompiledThreshold{10};

// Builds are summarised after this much build activity.
constexpr std::chrono::seconds ReportInterval{10};

thread_local bool precache_thread{};
thread_local bool disk_thread{};
thread_local u32 scope_depth{};
thread_local Clock::time_point front_end_begin{};

struct Bucket {
    u64 count{};
    u64 compiled{};
    std::chrono::steady_clock::duration total{};
    std::chrono::steady_clock::duration max{};

    void Add(std::chrono::steady_clock::duration time) {
        ++count;
        compiled += time > CompiledThreshold ? 1 : 0;
        total += time;
        max = std::max(max, time);
    }

    std::string Format(const char* what) const {
        const auto ms = [](auto d) { return std::chrono::duration<double, std::milli>(d).count(); };
        if (count == 0) {
            return fmt::format("0 {}", what);
        }
        return fmt::format("{} {}, {} over {} ms, {:.1f} ms mean, {:.1f} ms max", count, what,
                           compiled, CompiledThreshold.count(),
                           ms(total) / static_cast<double>(count), ms(max));
    }
};

struct Totals {
    Bucket graphics;
    Bucket compute;
    Bucket precache;

    void Add(Kind kind, std::chrono::steady_clock::duration time) {
        (precache_thread ? precache : kind == Kind::Graphics ? graphics : compute).Add(time);
    }

    void Log(const char* what) const {
        // Over the threshold: compiled, not from the driver's cache.
        LOG_INFO(Render_Vulkan, "Driver pipeline builds {}: {}; {}{}", what,
                 graphics.Format("graphics"), compute.Format("compute"),
                 precache.count != 0 ? "; shader precache: " + precache.Format("compute")
                                     : std::string{});
    }
};

// What the builds since the last "Shader builds:" line did.
struct Period {
    u64 pipelines{};
    u64 graphics{};
    u64 compute{};
    u64 from_disk{};
    u64 duplicates{};
    Duration translate{};
    Duration spirv{};
    Duration other{};
    Duration driver{};
    u64 queued_count{};
    Duration queued{};
    Duration queued_max{};
    Duration longest{};
    u64 gpu_waits{};
    Duration gpu_wait{};
    u64 gpu_builds{};
    Duration gpu_build{};
    u64 submission_waits{};
    Duration submission_wait{};
};

std::mutex mutex;
Totals window;
Totals summary;
std::chrono::steady_clock::time_point window_start{std::chrono::steady_clock::now()};

// Shader build report state, guarded by the mutex.
Period period;
bool started{};            ///< The period has build activity: `first` is set
Clock::time_point first;   ///< Start of the period's first build segment
Clock::time_point last_end; ///< When the builders last went idle
Clock::time_point last_change;
u32 active{};              ///< Builder threads in a build now
double busy_ns{};          ///< Builders busy, integrated over time
size_t builder_threads{1};
bool boot_done{};
bool drain_logged{};
std::unordered_set<u64> built_keys;

// Read by Tick without the mutex.
std::atomic<bool> period_pending{};
std::atomic<bool> drain_armed{};
// Counted per skipped draw, so not under the mutex. `skips_period` is what no line reported yet,
// `skips_session` the whole session.
constexpr size_t SkipReasonCount = static_cast<size_t>(SkipReason::Count);
using SkipCounts = std::array<u64, SkipReasonCount>;
std::array<std::atomic<u64>, SkipReasonCount> skips_period{};
std::array<std::atomic<u64>, SkipReasonCount> skips_session{};
std::atomic<u64> skips_pending{};
// Guarded by the mutex: when Tick first saw skips nothing reported.
bool skips_window_open{};
Clock::time_point skips_window_start;

SkipCounts TakeSkips() {
    SkipCounts taken{};
    for (size_t i = 0; i < SkipReasonCount; ++i) {
        taken[i] = skips_period[i].exchange(0, std::memory_order_relaxed);
    }
    skips_pending.store(0, std::memory_order_relaxed);
    skips_window_open = false;
    return taken;
}

// "14 draws skipped (async-translation 10, geometry-consistency 4; last 10 s)", then, when there
// were any, ", 2 dispatches skipped (compute-no-pipeline 2)". The reasons are in enum order.
std::string FormatSkips(const SkipCounts& counts, const char* when) {
    u64 draws{};
    u64 dispatches{};
    std::string draw_reasons;
    std::string dispatch_reasons;
    for (size_t i = 0; i < SkipReasonCount; ++i) {
        if (counts[i] == 0) {
            continue;
        }
        const bool compute = i >= static_cast<size_t>(SkipReason::ComputeNoShader);
        (compute ? dispatches : draws) += counts[i];
        auto& reasons = compute ? dispatch_reasons : draw_reasons;
        reasons += fmt::format("{}{} {}", reasons.empty() ? "" : ", ",
                               SkipReasonName(static_cast<SkipReason>(i)), counts[i]);
    }
    std::string text = fmt::format("{} draws skipped ({}{}{})", draws, draw_reasons,
                                   draw_reasons.empty() ? "" : "; ", when);
    if (dispatches != 0) {
        text += fmt::format(", {} dispatches skipped ({})", dispatches, dispatch_reasons);
    }
    return text;
}

double Ms(Duration d) {
    return std::chrono::duration<double, std::milli>(d).count();
}

void Start(Clock::time_point now) {
    if (!started) {
        started = true;
        first = now;
        last_change = now;
        busy_ns = 0.0;
    }
}

void Advance(Clock::time_point now) {
    busy_ns += static_cast<double>(active) *
               static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                       now - last_change)
                                       .count());
    last_change = now;
}

void EmitPeriod(Clock::time_point now, const char* when) {
    Advance(now);
    const Clock::time_point end = active != 0 || last_end < first ? now : last_end;
    const double wall_ns = static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - first).count());
    const Period& p = period;
    LOG_INFO(Render_Vulkan,
             "Shader builds: {} pipelines in {:.1f} s ({} graphics, {} compute, {} from disk "
             "cache), translation {:.0f} ms, SPIR-V {:.0f} ms, driver {:.0f} ms, queued {:.1f} ms "
             "avg / {:.1f} ms longest, workers busy {:.1f} of {}, longest single build {:.1f} ms, "
             "GPU thread waited {} times {:.0f} ms, duplicates {}; other {:.0f} ms, GPU thread "
             "built {} itself in {:.0f} ms, submission thread waited {} times {:.0f} ms, {}",
             p.pipelines, wall_ns / 1e9, p.graphics, p.compute, p.from_disk, Ms(p.translate),
             Ms(p.spirv), Ms(p.driver),
             p.queued_count != 0 ? Ms(p.queued) / static_cast<double>(p.queued_count) : 0.0,
             Ms(p.queued_max), wall_ns > 0.0 ? busy_ns / wall_ns : 0.0, builder_threads,
             Ms(p.longest), p.gpu_waits, Ms(p.gpu_wait), p.duplicates, Ms(p.other), p.gpu_builds,
             Ms(p.gpu_build), p.submission_waits, Ms(p.submission_wait),
             FormatSkips(TakeSkips(), when));
    period = {};
    busy_ns = 0.0;
    last_change = now;
    started = active != 0;
    first = now;
    period_pending.store(false, std::memory_order_relaxed);
}

void EmitIfDue(Clock::time_point now) {
    if (period.pipelines != 0 && started && now - first >= ReportInterval) {
        EmitPeriod(now, "last 10 s");
    }
}

// A build happened: the queue drain line is owed once the builders idle after boot.
void NoteBuildActivity() {
    period_pending.store(true, std::memory_order_relaxed);
    if (boot_done && !drain_logged) {
        drain_armed.store(true, std::memory_order_relaxed);
    }
}

} // Anonymous namespace

PrecacheScope::PrecacheScope() {
    precache_thread = true;
}

PrecacheScope::~PrecacheScope() {
    precache_thread = false;
}

DiskScope::DiskScope() {
    disk_thread = true;
}

DiskScope::~DiskScope() {
    disk_thread = false;
}

void Record(Kind kind, std::chrono::steady_clock::duration time) {
    if (!precache_thread) {
        using VideoCommon::Nx2Spikes::Event;
        VideoCommon::Nx2Spikes::Note(Event::DriverBuild, time);
        if (time > CompiledThreshold) {
            VideoCommon::Nx2Spikes::Note(Event::DriverCompile, time);
        }
    }
    std::scoped_lock lock{mutex};
    window.Add(kind, time);
    summary.Add(kind, time);
    const auto now = std::chrono::steady_clock::now();
    if (now - window_start >= std::chrono::seconds{10}) {
        window.Log("in the last 10 s");
        window = {};
        window_start = now;
    }
    if (!precache_thread) {
        Start(now);
        ++period.pipelines;
        ++(kind == Kind::Graphics ? period.graphics : period.compute);
        period.driver += time;
        NoteBuildActivity();
    }
}

void LogSummary(const char* when) {
    std::scoped_lock lock{mutex};
    summary.Log(when);
    summary = {};
    if (period.pipelines != 0) {
        EmitPeriod(Clock::now(), "at boot");
    }
    boot_done = true;
    period_pending.store(false, std::memory_order_relaxed);
}

void AddPhase(Phase phase, std::chrono::steady_clock::duration time) {
    if (precache_thread) {
        return;
    }
    std::scoped_lock lock{mutex};
    Start(Clock::now());
    (phase == Phase::Translate ? period.translate
     : phase == Phase::Spirv   ? period.spirv
                               : period.other) += time;
}

BuildScope::BuildScope() : counted{!precache_thread && scope_depth == 0} {
    ++scope_depth;
    if (!counted) {
        return;
    }
    const auto now = Clock::now();
    std::scoped_lock lock{mutex};
    Start(now);
    Advance(now);
    ++active;
}

BuildScope::~BuildScope() {
    --scope_depth;
    if (!counted) {
        return;
    }
    const auto now = Clock::now();
    std::scoped_lock lock{mutex};
    Advance(now);
    if (--active == 0) {
        last_end = now;
    }
}

void AddQueued(std::chrono::steady_clock::duration time) {
    if (precache_thread) {
        return;
    }
    std::scoped_lock lock{mutex};
    Start(Clock::now());
    ++period.queued_count;
    period.queued += time;
    period.queued_max = std::max(period.queued_max, time);
}

void AddQueuedSince(std::chrono::steady_clock::time_point queued) {
    AddQueued(Clock::now() - queued);
}

void NoteBuilt(std::chrono::steady_clock::duration build_time) {
    if (precache_thread) {
        return;
    }
    std::scoped_lock lock{mutex};
    period.longest = std::max(period.longest, build_time);
}

bool NoteStart(u64 hash) {
    if (precache_thread) {
        return false;
    }
    std::scoped_lock lock{mutex};
    Start(Clock::now());
    const bool duplicate = !built_keys.insert(hash).second;
    period.duplicates += duplicate ? 1 : 0;
    period.from_disk += disk_thread ? 1 : 0;
    return duplicate;
}

void NoteGpuWait(std::chrono::steady_clock::duration time) {
    std::scoped_lock lock{mutex};
    Start(Clock::now());
    ++period.gpu_waits;
    period.gpu_wait += time;
}

void NoteGpuBuild(std::chrono::steady_clock::duration time) {
    std::scoped_lock lock{mutex};
    Start(Clock::now());
    ++period.gpu_builds;
    period.gpu_build += time;
}

void NoteSubmissionWait(std::chrono::steady_clock::duration time) {
    std::scoped_lock lock{mutex};
    Start(Clock::now());
    ++period.submission_waits;
    period.submission_wait += time;
}

const char* SkipReasonName(SkipReason reason) noexcept {
    switch (reason) {
    case SkipReason::StageRejected:
        return "stage-rejected";
    case SkipReason::AsyncTranslation:
        return "async-translation";
    case SkipReason::BuildDeferred:
        return "build-deferred";
    case SkipReason::AsyncBuild:
        return "async-build";
    case SkipReason::AsyncBuildStale:
        return "async-build-stale";
    case SkipReason::TranslationMissing:
        return "translation-missing";
    case SkipReason::CreationFailed:
        return "creation-failed";
    case SkipReason::GeometryConsistency:
        return "geometry-consistency";
    case SkipReason::ConfigureFailed:
        return "configure-failed";
    case SkipReason::MeshDisabled:
        return "mesh-disabled";
    case SkipReason::ComputeNoShader:
        return "compute-no-shader";
    case SkipReason::ComputeNoPipeline:
        return "compute-no-pipeline";
    case SkipReason::ComputeConfigureFailed:
        return "compute-configure-failed";
    case SkipReason::Unknown:
    case SkipReason::Count:
        break;
    }
    return "no-pipeline";
}

void NoteDrawSkipped(SkipReason reason) {
    const size_t index = static_cast<size_t>(reason);
    skips_period[index].fetch_add(1, std::memory_order_relaxed);
    skips_session[index].fetch_add(1, std::memory_order_relaxed);
    skips_pending.fetch_add(1, std::memory_order_relaxed);
}

void LogSkipSummary(const char* when) {
    SkipCounts counts{};
    for (size_t i = 0; i < SkipReasonCount; ++i) {
        counts[i] = skips_session[i].load(std::memory_order_relaxed);
    }
    LOG_INFO(Render_Vulkan, "Skipped work: {}", FormatSkips(counts, when));
}

void SetFrontEnd(std::chrono::steady_clock::time_point begin) {
    front_end_begin = begin;
}

std::chrono::steady_clock::duration TakeFrontEnd() {
    const auto begin = std::exchange(front_end_begin, Clock::time_point{});
    return begin == Clock::time_point{} ? Duration{} : Clock::now() - begin;
}

void BeginSession(size_t count) {
    std::scoped_lock lock{mutex};
    builder_threads = count;
    built_keys.clear();
    period = {};
    started = false;
    busy_ns = 0.0;
    boot_done = false;
    drain_logged = false;
    drain_armed.store(false, std::memory_order_relaxed);
    period_pending.store(false, std::memory_order_relaxed);
    for (size_t i = 0; i < SkipReasonCount; ++i) {
        skips_period[i].store(0, std::memory_order_relaxed);
        skips_session[i].store(0, std::memory_order_relaxed);
    }
    skips_pending.store(0, std::memory_order_relaxed);
    skips_window_open = false;
}

void SetBuilderThreads(size_t count) {
    std::scoped_lock lock{mutex};
    builder_threads = count;
}

bool DrainArmed() {
    return drain_armed.load(std::memory_order_relaxed);
}

void Tick(bool queues_idle) {
    const bool skips = skips_pending.load(std::memory_order_relaxed) != 0;
    if (!skips && !period_pending.load(std::memory_order_relaxed) &&
        !drain_armed.load(std::memory_order_relaxed)) {
        return;
    }
    std::scoped_lock lock{mutex};
    const auto now = Clock::now();
    EmitIfDue(now);
    if (drain_armed.load(std::memory_order_relaxed) && queues_idle && active == 0) {
        if (period.pipelines != 0) {
            EmitPeriod(now, "queues drained");
        }
        drain_logged = true;
        drain_armed.store(false, std::memory_order_relaxed);
    }
    // Skips that no "Shader builds:" line took: a build-free stretch still reports them.
    if (skips_pending.load(std::memory_order_relaxed) == 0) {
        return;
    }
    if (!skips_window_open) {
        skips_window_open = true;
        skips_window_start = now;
    } else if (now - skips_window_start >= ReportInterval) {
        LOG_INFO(Render_Vulkan, "Skipped work: {}", FormatSkips(TakeSkips(), "last 10 s"));
    }
}

} // namespace Vulkan::DriverBuildReport
