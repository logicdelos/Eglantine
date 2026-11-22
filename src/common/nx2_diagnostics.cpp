// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/nx2_diagnostics.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/nx2_sampler.h"
#include "common/system_log.h"
#include "common/thread.h"

#ifdef _WIN32
#ifndef PSAPI_VERSION
#define PSAPI_VERSION 2
#endif
#include <windows.h>

#include <psapi.h>
#endif

namespace Nx2Bottleneck {

namespace {

constexpr double GiB = 1024.0 * 1024.0 * 1024.0;
const auto process_start = std::chrono::steady_clock::now();

// ---- Shader rejections ----

constexpr std::array<const char*, RejectionClasses> RejectionNames{
    "all-zero code",
    "branch outside capture",
    "unsupported opcode",
    "other",
};
std::array<std::atomic<u64>, RejectionClasses> rejections_window{};
std::array<std::atomic<u64>, RejectionClasses> rejections_session{};

std::string FormatClasses(const RejectionCounts& counts) {
    std::string text;
    for (size_t i = 0; i < RejectionClasses; ++i) {
        if (counts[i] != 0) {
            text += fmt::format("{}{} {}", text.empty() ? "" : ", ", RejectionNames[i], counts[i]);
        }
    }
    return text;
}

RejectionCounts Load(const std::array<std::atomic<u64>, RejectionClasses>& counters) {
    RejectionCounts counts{};
    for (size_t i = 0; i < RejectionClasses; ++i) {
        counts[i] = counters[i].load(std::memory_order_relaxed);
    }
    return counts;
}

u64 Sum(const RejectionCounts& counts) {
    u64 total = 0;
    for (const u64 count : counts) {
        total += count;
    }
    return total;
}

// ---- Scene changes ----

constexpr size_t SceneTargetSlots = 16;
/// The colour target sizes and formats of the draws since the last frame boundary, and the one of
/// the latest draw. A key is never 0 (bit 63), so 0 is an empty slot.
std::atomic<u64> draw_last_key{};
std::array<std::atomic<u64>, SceneTargetSlots> draw_targets{};
std::atomic<u32> new_pipelines{};

[[nodiscard]] u64 PackTarget(u32 width, u32 height, u32 format) noexcept {
    return (u64{1} << 63) | (u64{width & 0xffffu} << 24) | (u64{height & 0xffffu} << 8) |
           (format & 0xffu);
}

[[nodiscard]] u64 TargetSignature(const u64* keys, size_t count) noexcept {
    u64 hash = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < count; ++i) {
        hash = (hash ^ keys[i]) * 0x100000001b3ULL;
        hash ^= hash >> 29;
    }
    return hash != 0 ? hash : 1;
}

/// "1920x1080 fmt 0xd5 + 960x540 fmt 0xc3" (4 at most, then "+2 more").
std::string DescribeTargets(const u64* keys, size_t count) {
    if (count == 0) {
        return "none";
    }
    std::string text;
    for (size_t i = 0; i < (std::min<size_t>)(count, 4); ++i) {
        text += fmt::format("{}{}x{} fmt {:#x}", i == 0 ? "" : " + ", (keys[i] >> 24) & 0xffff,
                            (keys[i] >> 8) & 0xffff, keys[i] & 0xff);
    }
    if (count > 4) {
        text += fmt::format(" +{} more", count - 4);
    }
    return text;
}

struct SceneState {
    std::mutex mutex;
    SceneTracker tracker;
    LineLimiter limiter{SceneLogSpacing};
    BurstDetector burst{SceneBurstPipelines, SceneBurstSeconds};
    std::array<u64, SceneTargetSlots> accepted{};
    size_t accepted_count{};
};

// Leaked: the renderer can still end a frame while the process shuts down.
SceneState& Scene() {
    static auto* state = new SceneState;
    return *state;
}

// ---- Host memory, no progress ----

std::mutex guest_source_mutex;
GuestCoreSource guest_source;
std::atomic<u32> probes_started{};
std::atomic_bool probe_running{};
constexpr u32 MaxProbes = 3;
constexpr double ProbeSeconds = 1.5;

LowMemoryDetector memory_detector;
NoProgressDetector progress_detector;

// ---- Stop summary ----

constexpr size_t ReportCapacity = 1400;
std::mutex report_mutex;
char last_report[ReportCapacity]{};
double last_report_at{};
std::atomic_bool stop_written{};
std::atomic_bool crash_written{};

/// A copy of the last remembered report line; without the lock when it is not free, for a crash.
void CopyLastReport(char (&line)[ReportCapacity], double& at, bool crash) {
    std::unique_lock lock{report_mutex, std::defer_lock};
    if (crash) {
        (void)lock.try_lock();
    } else {
        lock.lock();
    }
    std::memcpy(line, last_report, ReportCapacity);
    line[ReportCapacity - 1] = '\0';
    at = last_report_at;
}

/// One block of lines, in fixed buffers: it also runs in a crashed process.
template <typename Emit>
void BuildSummary(const char* reason, bool crash, Emit&& emit) {
    char line[2048];
    const auto counts = Common::Log::GetLogCounts();
    std::snprintf(line, sizeof(line),
                  "Stop summary (%s): ran %.1f s, %llu frames presented, %llu errors and %llu "
                  "critical messages logged this session",
                  reason, UptimeSeconds(), static_cast<unsigned long long>(FramesPresented()),
                  static_cast<unsigned long long>(counts.error),
                  static_cast<unsigned long long>(counts.critical));
    emit(line);

    char report[ReportCapacity];
    double report_at = 0.0;
    CopyLastReport(report, report_at, crash);
    if (report[0] != '\0') {
        std::snprintf(line, sizeof(line), "Stop summary: last full 10 s report, written at %.0f s: %s",
                      report_at, report);
    } else {
        std::snprintf(line, sizeof(line), "Stop summary: no full 10 s report was written yet");
    }
    emit(line);

    const HostMemory memory = ReadHostMemory();
    if (memory.known) {
        std::snprintf(line, sizeof(line),
                      "Stop summary: memory: host RAM free %.1f of %.1f GiB, host commit %.0f%% of "
                      "its limit; emulator %.1f GiB committed, %.1f GiB in RAM",
                      static_cast<double>(memory.free_bytes) / GiB,
                      static_cast<double>(memory.total_bytes) / GiB, memory.commit_fraction * 100.0,
                      static_cast<double>(memory.process_committed) / GiB,
                      static_cast<double>(memory.process_working_set) / GiB);
        emit(line);
    }

    const RejectionCounts rejected = Load(rejections_session);
    if (const u64 total = Sum(rejected); total != 0) {
        std::snprintf(line, sizeof(line),
                      "Stop summary: native shaders rejected this session: %llu (%s)",
                      static_cast<unsigned long long>(total), FormatClasses(rejected).c_str());
        emit(line);
    }

    Common::Log::RecentError errors[Common::Log::RecentErrorSlots];
    const size_t error_count =
        Common::Log::GetRecentErrors(errors, Common::Log::RecentErrorSlots, !crash);
    if (error_count == 0) {
        std::snprintf(line, sizeof(line), "Stop summary: no error was logged");
        emit(line);
    }
    for (size_t i = 0; i < error_count; ++i) {
        char text[sizeof(errors[i].message)];
        std::memcpy(text, errors[i].message, sizeof(text));
        text[sizeof(text) - 1] = '\0';
        for (char* c = text; *c != '\0'; ++c) {
            if (*c == '\n' || *c == '\r') {
                *c = ' ';
            }
        }
        std::snprintf(line, sizeof(line),
                      "Stop summary: error %zu of %zu (the last distinct ones, newest last): "
                      "[%.1f] %s%s x%llu: %s",
                      i + 1, error_count, errors[i].last_seconds,
                      errors[i].critical ? "CRITICAL " : "", errors[i].site,
                      static_cast<unsigned long long>(errors[i].count), text);
        emit(line);
    }
}

} // Anonymous namespace

double UptimeSeconds() noexcept {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - process_start).count();
}

// ---- Shader rejections ----

RejectionClass ClassifyShaderRejection(std::string_view what) noexcept {
    if (what.find("[op 000 00000000 00000000 00000000 00000000]") != std::string_view::npos) {
        return RejectionClass::AllZeroCode;
    }
    if (what.find("branch outside capture") != std::string_view::npos) {
        return RejectionClass::BranchOutsideCapture;
    }
    if (what.find("unsupported native opcode") != std::string_view::npos) {
        return RejectionClass::UnsupportedOpcode;
    }
    return RejectionClass::Other;
}

const char* RejectionClassName(RejectionClass rejection) noexcept {
    return RejectionNames[static_cast<size_t>(rejection)];
}

void NoteShaderRejection(std::string_view what) noexcept {
    const size_t index = static_cast<size_t>(ClassifyShaderRejection(what));
    rejections_window[index].fetch_add(1, std::memory_order_relaxed);
    rejections_session[index].fetch_add(1, std::memory_order_relaxed);
}

std::string FormatShaderRejections(const RejectionCounts& window, u64 session_total) {
    const u64 total = Sum(window);
    if (total == 0) {
        return fmt::format("0 new in the last 10 s; {} this session", session_total);
    }
    return fmt::format("{} new in the last 10 s ({}); {} this session", total, FormatClasses(window),
                       session_total);
}

// ---- Scene changes ----

bool SceneTracker::OnFrame(u64 signature) noexcept {
    if (accepted_ == 0) {
        accepted_ = signature;
        stable_ = 1;
        return false;
    }
    if (signature == accepted_) {
        ++stable_;
        candidate_ = 0;
        candidate_run_ = 0;
        return false;
    }
    if (signature != candidate_) {
        candidate_ = signature;
        candidate_run_ = 1;
    } else {
        ++candidate_run_;
    }
    if (candidate_run_ < confirm_) {
        return false;
    }
    previous_stable_ = stable_;
    accepted_ = signature;
    stable_ = candidate_run_;
    candidate_ = 0;
    candidate_run_ = 0;
    return true;
}

bool LineLimiter::Allow(double now_seconds) noexcept {
    if (any_ && now_seconds - last_ < spacing_) {
        ++suppressed_;
        return false;
    }
    any_ = true;
    last_ = now_seconds;
    return true;
}

u32 BurstDetector::Add(double now_seconds, u32 count) {
    if (count != 0) {
        events_.emplace_back(now_seconds, count);
    }
    const auto old = std::find_if(events_.begin(), events_.end(), [&](const auto& event) {
        return event.first >= now_seconds - span_;
    });
    events_.erase(events_.begin(), old);
    u32 in_span = 0;
    for (const auto& event : events_) {
        in_span += event.second;
    }
    if (in_burst_) {
        // Over when the span holds under half the threshold again.
        in_burst_ = in_span * 2 >= threshold_;
        return 0;
    }
    if (in_span >= threshold_) {
        in_burst_ = true;
        return in_span;
    }
    return 0;
}

void NoteDrawTarget(u32 width, u32 height, u32 format) noexcept {
    const u64 key = PackTarget(width, height, format);
    if (draw_last_key.load(std::memory_order_relaxed) == key) {
        return;
    }
    draw_last_key.store(key, std::memory_order_relaxed);
    for (auto& slot : draw_targets) {
        u64 seen = slot.load(std::memory_order_relaxed);
        if (seen == key) {
            return;
        }
        if (seen == 0 && (slot.compare_exchange_strong(seen, key, std::memory_order_relaxed) ||
                          seen == key)) {
            return;
        }
    }
}

void NoteNewPipeline() noexcept {
    new_pipelines.fetch_add(1, std::memory_order_relaxed);
}

void NoteSceneFrame() {
    std::array<u64, SceneTargetSlots> keys{};
    size_t count = 0;
    for (auto& slot : draw_targets) {
        if (const u64 key = slot.exchange(0, std::memory_order_relaxed); key != 0) {
            keys[count++] = key;
        }
    }
    draw_last_key.store(0, std::memory_order_relaxed);
    const u32 built = new_pipelines.exchange(0, std::memory_order_relaxed);
    const double now = UptimeSeconds();
    std::sort(keys.begin(), keys.begin() + static_cast<std::ptrdiff_t>(count));

    SceneState& scene = Scene();
    std::scoped_lock lock{scene.mutex};
    const u32 burst = scene.burst.Add(now, built);
    bool targets_changed = false;
    if (count != 0) {
        targets_changed = scene.tracker.OnFrame(TargetSignature(keys.data(), count));
    }
    const auto previous = scene.accepted;
    const size_t previous_count = scene.accepted_count;
    if (count != 0 && (targets_changed || scene.accepted_count == 0)) {
        scene.accepted = keys;
        scene.accepted_count = count;
    }
    if (!targets_changed && burst == 0) {
        return;
    }
    if (!scene.limiter.Allow(now)) {
        return;
    }
    std::string what;
    if (targets_changed) {
        what = fmt::format("render targets {} -> {} (the old set lasted {} frames)",
                           DescribeTargets(previous.data(), previous_count),
                           DescribeTargets(keys.data(), count), scene.tracker.PreviousStableFrames());
    }
    if (burst != 0) {
        what += fmt::format("{}{} pipelines built in the last {:.0f} s", what.empty() ? "" : "; ",
                            burst, SceneBurstSeconds);
        if (!targets_changed) {
            what += fmt::format(" (render targets {})", DescribeTargets(keys.data(), count));
        }
    }
    if (const u64 suppressed = scene.limiter.TakeSuppressed(); suppressed != 0) {
        what += fmt::format(" (+{} more within {:.0f} s, not logged)", suppressed, SceneLogSpacing);
    }
    LOG_INFO(Render_Vulkan, "Scene change at t={:.1f} s: {}", now, what);
    const std::string brief =
        targets_changed ? fmt::format("{} -> {}", DescribeTargets(previous.data(), previous_count),
                                      DescribeTargets(keys.data(), count))
                        : fmt::format("{} pipelines in {:.0f} s", burst, SceneBurstSeconds);
    Common::SystemLog::Record(Common::SystemLog::Subsystem::Gpu, Common::SystemLog::Severity::Info,
                              Common::SystemLog::EventKind::SceneChange,
                              scene.tracker.Accepted(), burst, scene.tracker.PreviousStableFrames(),
                              targets_changed ? 0 : 1, brief);
}

// ---- Host memory ----

HostMemory ReadHostMemory() noexcept {
    HostMemory memory;
#ifdef _WIN32
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status)) {
        memory.known = true;
        memory.free_bytes = status.ullAvailPhys;
        memory.total_bytes = status.ullTotalPhys;
        if (status.ullTotalPageFile != 0) {
            memory.commit_fraction = 1.0 - static_cast<double>(status.ullAvailPageFile) /
                                               static_cast<double>(status.ullTotalPageFile);
        }
    }
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                             sizeof(counters))) {
        memory.process_committed = counters.PrivateUsage;
        memory.process_working_set = counters.WorkingSetSize;
    }
#endif
    return memory;
}

bool IsLowMemory(const HostMemory& memory) noexcept {
    return memory.known &&
           (memory.free_bytes < LowMemoryFreeBytes || memory.commit_fraction >= LowMemoryCommitFraction);
}

LowMemoryDetector::Action LowMemoryDetector::Update(double now_seconds, bool low) noexcept {
    if (!low) {
        const bool was_warned = warned_;
        low_ = false;
        warned_ = false;
        return was_warned ? Action::Recovered : Action::None;
    }
    if (!low_) {
        low_ = true;
        low_since_ = now_seconds;
    }
    if (!warned_ && now_seconds - low_since_ >= LowMemorySustainedSeconds) {
        warned_ = true;
        return Action::Warn;
    }
    return Action::None;
}

// ---- No progress ----

bool WindowLooksStuck(const ProgressWindow& window) noexcept {
    return window.whole && window.frames > 0 && window.busiest_core_run >= NoProgressCoreShare &&
           window.file_reads == 0 && window.svc_calls > 0 &&
           window.empty_yields * 2 >= window.svc_calls;
}

bool NoProgressDetector::Update(const ProgressWindow& window) noexcept {
    if (!WindowLooksStuck(window)) {
        streak_ = 0;
        fired_ = false;
        return false;
    }
    ++streak_;
    if (streak_ >= NoProgressWindows && !fired_) {
        fired_ = true;
        return true;
    }
    return false;
}

void SetGuestCoreSource(const GuestCoreSource& source) {
    std::scoped_lock lock{guest_source_mutex};
    guest_source = source;
}

namespace {

/// What a stuck core runs, sampled for ProbeSeconds: the guest program counter polled through the
/// core's source (a top 3 of module+offset) and, at the same time, the host thread's call stacks
/// (Nx2Sampler), whose report goes to the log directory. A thread of its own: the core is not
/// held up, and nothing here may block the report loop.
void RunProbe(size_t core) {
    Common::SetCurrentThreadName("Nx2NoProgress");
    struct Polled {
        u64 pc;
        u64 thread_id;
    };
    std::array<Polled, 512> polled;
    std::atomic<size_t> polled_count{0};
    std::atomic_bool done{false};
    // The poller reads and nothing else: it runs while the sampler pauses the core's host thread
    // about 1000 times a second, so it takes no lock a core could hold and logs nothing.
    std::thread poller{[&] {
        while (!done.load(std::memory_order_relaxed) &&
               polled_count.load(std::memory_order_relaxed) < polled.size()) {
            Polled next{};
            {
                std::scoped_lock lock{guest_source_mutex};
                if (guest_source.read == nullptr ||
                    !guest_source.read(guest_source.user, core, next.pc, next.thread_id)) {
                    return;
                }
            }
            const size_t index = polled_count.load(std::memory_order_relaxed);
            polled[index] = next;
            polled_count.store(index + 1, std::memory_order_relaxed);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }};

    char stamp[32]{};
    const std::time_t now = std::time(nullptr);
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H-%M-%S", std::localtime(&now));
    const std::string path = fmt::format(
        "{}/no_progress_{}.txt", Common::FS::GetEdenPathString(Common::FS::EdenPath::LogDir), stamp);
    const std::string host =
        Nx2Sampler::SampleBrief({fmt::format("CPUCore_{}", core), "CPUThread"}, ProbeSeconds, path);
    done.store(true, std::memory_order_relaxed);
    poller.join();

    const size_t total = polled_count.load(std::memory_order_relaxed);
    std::map<u64, u32> by_pc;
    std::map<u64, u32> by_thread;
    for (size_t i = 0; i < total; ++i) {
        ++by_pc[polled[i].pc];
        ++by_thread[polled[i].thread_id];
    }
    std::vector<std::pair<u32, u64>> top_pc;
    for (const auto& [pc, count] : by_pc) {
        top_pc.emplace_back(count, pc);
    }
    std::sort(top_pc.rbegin(), top_pc.rend());
    std::string guest;
    if (total == 0) {
        guest = "the guest program counter could not be read";
    } else {
        const auto thread = std::max_element(by_thread.begin(), by_thread.end(),
                                             [](const auto& a, const auto& b) {
                                                 return a.second < b.second;
                                             });
        const std::string name = GuestThreadName(thread->first);
        guest = fmt::format("guest thread {:#x} {:?} ran at", thread->first,
                            name.empty() ? std::string{"unnamed"} : name);
        std::scoped_lock lock{guest_source_mutex};
        for (size_t i = 0; i < (std::min<size_t>)(3, top_pc.size()); ++i) {
            std::string where;
            if (guest_source.locate != nullptr) {
                where = guest_source.locate(guest_source.user, top_pc[i].second);
            }
            if (where.empty()) {
                where = fmt::format("{:#x}", top_pc[i].second);
            }
            guest += fmt::format("{} {} {:.0f}%", i == 0 ? "" : ",", where,
                                 100.0 * top_pc[i].first / static_cast<double>(total));
        }
        guest += fmt::format(" ({} polls)", total);
    }
    LOG_WARNING(Core, "No progress sample of core {} ({:.1f} s): {}; host: {}; full report: {}", core,
                ProbeSeconds, guest, host.empty() ? std::string{"not available"} : host, path);
    probe_running.store(false, std::memory_order_release);
}

void CheckProgress(const ProgressWindow& window) {
    if (!progress_detector.Update(window)) {
        return;
    }
    const double yield_share =
        window.svc_calls != 0
            ? 100.0 * static_cast<double>(window.empty_yields) / static_cast<double>(window.svc_calls)
            : 0.0;
    const double seconds = window.seconds * progress_detector.Streak();
    LOG_WARNING(Core,
                "No progress (suspected): core {} ran game code {:.0f}% for {:.0f} s with {} file "
                "reads while {} frames kept presenting in the last window; SleepThread calls that "
                "yielded to nobody were {:.0f}% of the {} SVCs. The game looks stuck in a wait loop "
                "(the frame-stall watchdog does not fire while frames present).",
                window.busiest_core, window.busiest_core_run * 100.0, seconds, window.file_reads,
                window.frames, yield_share, window.svc_calls);
    Common::SystemLog::Record(
        Common::SystemLog::Subsystem::Core, Common::SystemLog::Severity::Warning,
        Common::SystemLog::EventKind::NoProgress, window.busiest_core,
        static_cast<u64>(window.busiest_core_run * 100.0), window.frames,
        static_cast<s64>(yield_share),
        fmt::format("core {} {:.0f}% for {:.0f} s, {} reads", window.busiest_core,
                    window.busiest_core_run * 100.0, seconds, window.file_reads));
    bool expected = false;
    if (probes_started.load(std::memory_order_relaxed) < MaxProbes &&
        probe_running.compare_exchange_strong(expected, true)) {
        probes_started.fetch_add(1, std::memory_order_relaxed);
        std::thread{RunProbe, window.busiest_core}.detach();
    }
}

void CheckMemory(double now) {
    const HostMemory memory = ReadHostMemory();
    const bool low = IsLowMemory(memory);
    // (the warning's duration comes from the detector)
    switch (memory_detector.Update(now, low)) {
    case LowMemoryDetector::Action::Warn: {
        const bool ram = memory.free_bytes < LowMemoryFreeBytes;
        const bool commit = memory.commit_fraction >= LowMemoryCommitFraction;
        LOG_WARNING(Core,
                    "LOW MEMORY: {}{}{} for {:.0f} s (host RAM free {:.1f} of {:.1f} GiB, host "
                    "commit {:.0f}% of its limit); emulator {:.1f} GiB committed, {:.1f} GiB in "
                    "RAM. Expect stutter, long loads or a crash.",
                    ram ? "host RAM free below 1.0 GiB" : "", ram && commit ? " and " : "",
                    commit ? "host commit above 95%" : "", memory_detector.LowFor(now),
                    static_cast<double>(memory.free_bytes) / GiB,
                    static_cast<double>(memory.total_bytes) / GiB, memory.commit_fraction * 100.0,
                    static_cast<double>(memory.process_committed) / GiB,
                    static_cast<double>(memory.process_working_set) / GiB);
        Common::SystemLog::Record(
            Common::SystemLog::Subsystem::Memory, Common::SystemLog::Severity::Warning,
            Common::SystemLog::EventKind::LowMemory, memory.free_bytes, memory.total_bytes,
            memory.process_committed, static_cast<s64>(memory.commit_fraction * 100.0),
            ram ? "host RAM free below 1 GiB" : "host commit above 95%");
        break;
    }
    case LowMemoryDetector::Action::Recovered:
        LOG_INFO(Core, "Memory recovered: host RAM free {:.1f} of {:.1f} GiB, host commit {:.0f}% of "
                       "its limit",
                 static_cast<double>(memory.free_bytes) / GiB,
                 static_cast<double>(memory.total_bytes) / GiB, memory.commit_fraction * 100.0);
        break;
    case LowMemoryDetector::Action::None:
        break;
    }
}

} // Anonymous namespace

void OnReportWindow(const ProgressWindow& progress) {
    const RejectionCounts fresh = [] {
        RejectionCounts counts{};
        for (size_t i = 0; i < RejectionClasses; ++i) {
            counts[i] = rejections_window[i].exchange(0, std::memory_order_relaxed);
        }
        return counts;
    }();
    if (Sum(fresh) != 0) {
        LOG_WARNING(HW_GPU, "Shader rejections: {}",
                    FormatShaderRejections(fresh, Sum(Load(rejections_session))));
    }
    CheckProgress(progress);
}

void OnReportTick() {
    CheckMemory(UptimeSeconds());
}

// ---- Stop summary ----

void RememberReport(std::string_view line) {
    const double at = UptimeSeconds();
    std::scoped_lock lock{report_mutex};
    const size_t length = (std::min)(line.size(), ReportCapacity - 1);
    std::memcpy(last_report, line.data(), length);
    last_report[length] = '\0';
    last_report_at = at;
}

void WriteStopSummary(const char* reason) {
    if (stop_written.exchange(true)) {
        return;
    }
    BuildSummary(reason, false, [](const char* line) { LOG_INFO(Core, "{}", line); });
}

void WriteCrashSummary(void (*emit)(const char* line)) {
    if (crash_written.exchange(true)) {
        return;
    }
    BuildSummary("crash", true, [emit](const char* line) { emit(line); });
}

} // namespace Nx2Bottleneck
