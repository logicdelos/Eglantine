// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/nx2_bottleneck.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "common/cpu_features.h"
#include "common/logging.h"
#include "common/memory_accounting.h"
#include "common/run_outcome.h"
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

constexpr auto Period = std::chrono::seconds{10};
constexpr size_t NumThreads = static_cast<size_t>(Thread::Count);
constexpr size_t NumWaits = static_cast<size_t>(Wait::Count);
/// Waits are kept per thread; the last row is every thread that is not one of `Thread`.
constexpr size_t NumWaitRows = NumThreads + 1;
/// The waits that come out of a thread's working time: the first ones of `Wait`.
constexpr size_t NumThreadWaits = static_cast<size_t>(Wait::GuestOnGpu);
constexpr std::array<std::string_view, NumThreads> ThreadNames{
    "GPU thread",
    "GPU back thread",
    "Vulkan worker",
};
constexpr std::array<std::string_view, NumThreadWaits> WaitNames{
    "the graphics card", "shader builds",       "the Vulkan worker",
    "a present frame",   "the GPU back thread",
};
/// A thread working this much of the time, or guest code filling a core this much, limits.
constexpr double Saturated = 0.85;

struct IdleMeter {
    std::atomic<u64> ns{};
    /// When the wait in progress began, 0 while working: a wait can outlast a report.
    std::atomic<u64> since{};
    std::atomic<bool> seen{};
};

struct WaitMeter {
    std::atomic<u64> ns{};
    std::atomic<u64> longest_ns{};
    std::atomic<u32> count{};
};

std::array<IdleMeter, NumThreads> idle_meters;
std::array<std::array<WaitMeter, NumWaits>, NumWaitRows> wait_meters;
std::array<std::atomic<u64>, MaxCores> guest_run_ns;
/// Per CPU core, so cores calling SVCs at once do not share a cache line.
alignas(64) std::array<std::array<std::atomic<u32>, NumSvcIds>, MaxCores> svc_counts;
std::array<std::array<u32, NumSvcIds>, MaxCores> svc_previous{};
struct alignas(64) SleepMeter {
    std::atomic<u64> backoffs{};
    std::atomic<u64> wait_ns{};
    std::atomic<u64> longest_ns{};
    u64 previous_backoffs{};
    u64 previous_wait_ns{};
};
std::array<SleepMeter, MaxCores> sleep_meters;

struct SleepRegistry {
    std::mutex mutex;
    SleepThreadCounter* first{};
    u64 retired_id{};
    u64 retired_calls{};
    std::array<u64, 8> retired_bins{};
    u64 retired_empty{};
};
// Threads can be destroyed during system teardown, after other static destructors.
SleepRegistry& SleepThreads() {
    static auto* registry = new SleepRegistry;
    return *registry;
}
std::mutex sleep_name_mutex;
std::string (*sleep_name_source)(void*, u64){};
void* sleep_name_user{};
std::atomic<JitTotals (*)()> jit_source{};
/// The JIT's totals at the last report; only the reporter thread reads or writes them.
JitTotals jit_previous;
bool jit_previous_valid = false;
std::atomic<u64> window_begin{};
std::atomic<u64> last_frame{};
std::atomic<u64> longest_frame_ns{};
/// The last present intervals in microseconds. Only the presenting thread writes (`NoteFrame`),
/// only the report thread reads; a slot rewritten during the read just shifts one sample.
constexpr size_t IntervalRing = 1024;
std::array<std::atomic<u32>, IntervalRing> interval_us{};
std::atomic<u32> intervals_written{};
u32 intervals_read{};
std::atomic<u32> frames{};
std::atomic<u32> frames_over_25ms{};
std::atomic<u32> frames_over_50ms{};
std::atomic<u32> frames_over_100ms{};
std::atomic<u64> over_25ms_ns{};
std::atomic<u64> over_50ms_ns{};
std::atomic<u32> audio_underruns{};
std::atomic<bool> guest_running{};
std::atomic<u32> run_count{};
thread_local size_t wait_row = NumThreads;

/// Present intervals as a histogram, kept only while a report records. Microseconds below 64 have
/// a bucket each; above, every power of two is split into 64 equal buckets, so a bucket is at most
/// 1/64 of its octave wide. The 58 octaves from 2^6 to 2^63 cover every u64 microsecond count.
constexpr size_t HistogramBuckets = 64 + 64 * 58;
std::array<std::atomic<u64>, HistogramBuckets> interval_histogram{};
std::atomic<u64> histogram_sum_us{};
std::atomic<u64> histogram_max_us{};
std::atomic<bool> histogram_enabled{};
/// The histogram of the window Collect took last (guarded by collect_mutex).
std::array<u64, HistogramBuckets> window_histogram{};
/// Every Collect runs under this, and `collect_begin` is where the window being filled started:
/// Collect resets the counters, so there is one collector, and whatever wants a window of them
/// (the log, the report) takes it from the loop below.
std::mutex& collect_mutex = *new std::mutex;
u64 collect_begin = 0;

void RaiseTo(std::atomic<u64>& longest, u64 value) {
    u64 seen = longest.load(std::memory_order_relaxed);
    while (value > seen && !longest.compare_exchange_weak(seen, value, std::memory_order_relaxed)) {
    }
}

struct WaitTotals {
    double ms{};
    double longest_ms{};
    u32 count{};
};

struct SvcCount {
    u32 id{};
    u32 count{};
};

/// One report's worth of the counters.
struct Window {
    double seconds{};
    u32 frames{};
    u32 over_25ms{};
    u32 over_50ms{};
    u32 over_100ms{};
    /// Time taken by the frames over 25 ms and over 50 ms.
    double over_25ms_ms{};
    double over_50ms_ms{};
    double longest_frame_ms{};
    /// Present intervals of the window (the last 1024 at most) over 1.5x their median.
    u32 late{};
    double median_ms{};
    double p99_ms{};
    /// Share of the window not spent waiting for work; negative for a thread that never ran.
    std::array<double, NumThreads> busy{};
    std::array<std::array<WaitTotals, NumWaits>, NumWaitRows> waits{};
    std::array<double, MaxCores> guest_run{};
    u32 audio_underruns{};
    u64 svc_total{};
    /// The most called SVCs, most first; an id with no calls has count 0.
    std::array<SvcCount, 3> svc_top{};
    /// What the JIT did in the window; unknown until the JIT has been read twice.
    bool jit_known = false;
    JitTotals jit;

    [[nodiscard]] double Busy(Thread thread) const {
        return busy[static_cast<size_t>(thread)];
    }
    /// On every thread together.
    [[nodiscard]] WaitTotals Waited(Wait wait) const {
        WaitTotals total;
        for (const auto& row : waits) {
            const WaitTotals& part = row[static_cast<size_t>(wait)];
            total.ms += part.ms;
            total.longest_ms = (std::max)(total.longest_ms, part.longest_ms);
            total.count += part.count;
        }
        return total;
    }
    /// Share of the window `thread` spent in the wait at `wait`.
    [[nodiscard]] double Share(Thread thread, size_t wait) const {
        return waits[static_cast<size_t>(thread)][wait].ms / (seconds * 1000.0);
    }
    /// Busy less the waits: the thread's own work.
    [[nodiscard]] double Working(Thread thread) const {
        double working = Busy(thread);
        for (size_t wait = 0; wait < NumThreadWaits; ++wait) {
            working -= Share(thread, wait);
        }
        return (std::max)(working, 0.0);
    }
    [[nodiscard]] Thread Renderer() const {
        return Busy(Thread::GpuBack) >= 0.0 ? Thread::GpuBack : Thread::Gpu;
    }
    std::array<u64, 8> sleep_bins{};
    u64 sleep_top_id{};
    u64 sleep_top_calls{};
    u64 empty_yields{};
    u64 yield_backoffs{};
    u64 yield_wait_ns{};
    u64 yield_longest_ns{};
    /// Raw time not spent working, and raw guest time per core, for the report.
    std::array<double, NumThreads> idle_ms{};
    std::array<double, MaxCores> guest_run_ms{};
    /// Exact sum and longest of the present intervals of the window, with the histogram.
    u64 interval_sum_us{};
    u64 interval_max_us{};
};

/// Fills in the interval statistics from the ring.
void SummarizeIntervals(Window& window) {
    const u32 written = intervals_written.load(std::memory_order_acquire);
    const u32 count = (std::min)(written - intervals_read, static_cast<u32>(IntervalRing));
    intervals_read = written;
    if (count == 0) {
        return;
    }
    std::array<u32, IntervalRing> sorted;
    for (u32 i = 0; i < count; ++i) {
        sorted[i] =
            interval_us[(written - count + i) % IntervalRing].load(std::memory_order_relaxed);
    }
    const auto end = sorted.begin() + count;
    std::sort(sorted.begin(), end);
    const size_t middle = count / 2;
    const double median = count % 2 != 0
                              ? static_cast<double>(sorted[middle])
                              : (static_cast<double>(sorted[middle - 1]) + sorted[middle]) / 2.0;
    window.median_ms = median / 1e3;
    window.p99_ms = static_cast<double>(sorted[(size_t{count} * 99 + 99) / 100 - 1]) / 1e3;
    window.late = static_cast<u32>(std::count_if(
        sorted.begin(), end, [median](u32 us) { return static_cast<double>(us) > median * 1.5; }));
}

/// Takes the counters since `begin` and starts the next window at `now`.
Window Collect(u64 begin, u64 now) {
    // First, so a wait that ends from here on counts only its part after `now`.
    window_begin.store(now, std::memory_order_relaxed);
    const double window_ns = static_cast<double>(now - begin);
    Window window;
    window.seconds = window_ns / 1e9;
    window.frames = frames.exchange(0, std::memory_order_relaxed);
    window.over_25ms = frames_over_25ms.exchange(0, std::memory_order_relaxed);
    window.over_50ms = frames_over_50ms.exchange(0, std::memory_order_relaxed);
    window.over_100ms = frames_over_100ms.exchange(0, std::memory_order_relaxed);
    window.over_25ms_ms =
        static_cast<double>(over_25ms_ns.exchange(0, std::memory_order_relaxed)) / 1e6;
    window.over_50ms_ms =
        static_cast<double>(over_50ms_ns.exchange(0, std::memory_order_relaxed)) / 1e6;
    window.longest_frame_ms =
        static_cast<double>(longest_frame_ns.exchange(0, std::memory_order_relaxed)) / 1e6;
    SummarizeIntervals(window);
    if (histogram_enabled.load(std::memory_order_relaxed)) {
        for (size_t i = 0; i < HistogramBuckets; ++i) {
            window_histogram[i] = interval_histogram[i].exchange(0, std::memory_order_acquire);
        }
        window.interval_sum_us = histogram_sum_us.exchange(0, std::memory_order_relaxed);
        window.interval_max_us = histogram_max_us.exchange(0, std::memory_order_relaxed);
    }
    window.audio_underruns = audio_underruns.exchange(0, std::memory_order_relaxed);
    for (size_t i = 0; i < NumThreads; ++i) {
        IdleMeter& meter = idle_meters[i];
        u64 idle = meter.ns.exchange(0, std::memory_order_relaxed);
        if (const u64 since = meter.since.load(std::memory_order_relaxed); since != 0) {
            // Still waiting: the part inside this window.
            idle += now - (std::min)((std::max)(since, begin), now);
        }
        window.busy[i] = meter.seen.load(std::memory_order_relaxed)
                             ? std::clamp(1.0 - static_cast<double>(idle) / window_ns, 0.0, 1.0)
                             : -1.0;
        window.idle_ms[i] = static_cast<double>((std::min)(idle, now - begin)) / 1e6;
    }
    for (size_t row = 0; row < NumWaitRows; ++row) {
        for (size_t i = 0; i < NumWaits; ++i) {
            WaitMeter& meter = wait_meters[row][i];
            window.waits[row][i] = {
                .ms = static_cast<double>(meter.ns.exchange(0, std::memory_order_relaxed)) / 1e6,
                .longest_ms =
                    static_cast<double>(meter.longest_ns.exchange(0, std::memory_order_relaxed)) /
                    1e6,
                .count = meter.count.exchange(0, std::memory_order_relaxed),
            };
        }
    }
    for (size_t i = 0; i < MaxCores; ++i) {
        const u64 run_ns = guest_run_ns[i].exchange(0, std::memory_order_relaxed);
        window.guest_run_ms[i] = static_cast<double>(run_ns) / 1e6;
        window.guest_run[i] = static_cast<double>(run_ns) / window_ns;
    }
    std::array<u32, NumSvcIds> svc{};
    for (size_t core = 0; core < MaxCores; ++core) {
        for (size_t id = 0; id < NumSvcIds; ++id) {
            const u32 count = svc_counts[core][id].load(std::memory_order_relaxed);
            svc[id] += count - svc_previous[core][id];
            svc_previous[core][id] = count;
        }
    }
    for (size_t id = 0; id < NumSvcIds; ++id) {
        const u32 count = svc[id];
        window.svc_total += count;
        if (count <= window.svc_top.back().count) {
            continue;
        }
        size_t slot = window.svc_top.size() - 1;
        for (; slot > 0 && window.svc_top[slot - 1].count < count; --slot) {
            window.svc_top[slot] = window.svc_top[slot - 1];
        }
        window.svc_top[slot] = {.id = static_cast<u32>(id), .count = count};
    }
    {
        auto& registry = SleepThreads();
        std::scoped_lock lock{registry.mutex};
        for (auto& meter : sleep_meters) {
            const u64 backoffs = meter.backoffs.load(std::memory_order_relaxed);
            const u64 wait_ns = meter.wait_ns.load(std::memory_order_relaxed);
            window.yield_backoffs += backoffs - meter.previous_backoffs;
            window.yield_wait_ns += wait_ns - meter.previous_wait_ns;
            meter.previous_backoffs = backoffs;
            meter.previous_wait_ns = wait_ns;
            // A per-window maximum, unlike the monotonic counters above.
            window.yield_longest_ns = (std::max)(window.yield_longest_ns,
                meter.longest_ns.exchange(0, std::memory_order_relaxed));
        }
        window.sleep_top_id = registry.retired_id;
        window.sleep_top_calls = registry.retired_calls;
        registry.retired_id = registry.retired_calls = 0;
        window.sleep_bins = registry.retired_bins;
        window.empty_yields = registry.retired_empty;
        registry.retired_bins.fill(0);
        registry.retired_empty = 0;
        for (auto* counter = registry.first; counter != nullptr; counter = counter->next) {
            const u64 calls = counter->Collect(window.sleep_bins, window.empty_yields);
            if (calls > window.sleep_top_calls) {
                window.sleep_top_calls = calls;
                window.sleep_top_id = counter->id;
            }
        }
    }
    if (const auto source = jit_source.load(std::memory_order_relaxed); source != nullptr) {
        const JitTotals totals = source();
        if (jit_previous_valid) {
            window.jit_known = true;
            window.jit = {
                .blocks_compiled = totals.blocks_compiled - jit_previous.blocks_compiled,
                .compile_ns = totals.compile_ns - jit_previous.compile_ns,
                .fastmem_faults = totals.fastmem_faults - jit_previous.fastmem_faults,
            };
        }
        jit_previous = totals;
        jit_previous_valid = true;
    }
    return window;
}

std::string Dominant(const Window& window, Thread thread);

/// One thing a thread's time goes to: the wait at `part`, or its own work (NumThreadWaits).
std::string Describe(const Window& window, Thread thread, size_t part) {
    const std::string_view name = ThreadNames[static_cast<size_t>(thread)];
    switch (static_cast<Wait>(part)) {
    case Wait::HostGpu:
        return fmt::format("the graphics card (the {} waits for it)", name);
    case Wait::ShaderBuild:
        return fmt::format("shader builds (the {} waits for them)", name);
    case Wait::VulkanWorker:
        return Dominant(window, Thread::VulkanWorker);
    case Wait::Present:
        return "presentation (the display's refresh rate)";
    case Wait::GpuBack:
        return Dominant(window, Thread::GpuBack);
    default:
        break;
    }
    if (thread == Thread::VulkanWorker) {
        return "the emulator's Vulkan worker (recording commands and handing them to the driver)";
    }
    return fmt::format("the emulator's {} (its own work on the CPU)", name);
}

/// What a thread spends most of its time on: two things when neither clearly leads.
std::string Dominant(const Window& window, Thread thread) {
    size_t first = NumThreadWaits;
    size_t second = NumThreadWaits;
    double most = window.Working(thread);
    double next = 0.0;
    for (size_t i = 0; i < NumThreadWaits; ++i) {
        const double share = window.Share(thread, i);
        if (share > most) {
            next = std::exchange(most, share);
            second = std::exchange(first, i);
        } else if (share > next) {
            next = share;
            second = i;
        }
    }
    if (next * 1.5 < most || next == 0.0) {
        return Describe(window, thread, first);
    }
    return fmt::format("{} and {}", Describe(window, thread, first),
                       Describe(window, thread, second));
}

/// The part with no time to spare, empty when nothing measured is saturated.
std::string Saturation(const Window& window) {
    const Thread renderer = window.Renderer();
    for (const Thread thread : {renderer, Thread::VulkanWorker, Thread::Gpu}) {
        if (window.Busy(thread) >= Saturated) {
            return Dominant(window, thread);
        }
    }
    const auto busiest = std::ranges::max_element(window.guest_run);
    if (*busiest >= Saturated) {
        return fmt::format("the game's own code on CPU core {}",
                           std::distance(window.guest_run.begin(), busiest));
    }
    return {};
}

std::string Limiter(const Window& window) {
    if (window.frames == 0) {
        return "no frame was presented";
    }
    struct Cadence {
        double frame_ms;
        /// Frames too long for it, and the time they took.
        u32 late;
        double late_ms;
    };
    const std::array cadences{
        Cadence{1000.0 / 60.0, window.over_25ms, window.over_25ms_ms},
        Cadence{1000.0 / 30.0, window.over_50ms, window.over_50ms_ms},
    };
    const auto on_cadence = [](double frame_ms, const Cadence& cadence, double tolerance) {
        return std::abs(frame_ms - cadence.frame_ms) < cadence.frame_ms * tolerance;
    };
    const std::string saturated = Saturation(window);
    const double window_ms = window.seconds * 1000.0;
    for (const Cadence& cadence : cadences) {
        if (!on_cadence(window_ms / window.frames, cadence, 0.02) ||
            cadence.late * 50 > window.frames) {
            continue;
        }
        // A steady 60 or 30 fps is the game's own rate, unless it wanted the higher one.
        const double fps = 1000.0 / cadence.frame_ms;
        if (saturated.empty()) {
            return fmt::format("nothing, a steady {:.0f} fps", fps);
        }
        return fmt::format("nothing if the game runs at {:.0f} fps, which it holds; no time to "
                           "spare in {}",
                           fps, saturated);
    }
    if (!saturated.empty()) {
        return saturated;
    }
    for (const Cadence& cadence : cadences) {
        // Most frames on time and a few long ones: not a sustained limit. The frames after a
        // long one come early, hence the wider margin.
        if (cadence.late != 0 && cadence.late * 2 < window.frames &&
            on_cadence((window_ms - cadence.late_ms) / (window.frames - cadence.late), cadence,
                       0.05)) {
            return fmt::format("hitches: {} of {} frames ran long (longest {:.0f} ms), the others "
                               "held {:.0f} fps",
                               cadence.late, window.frames, window.longest_frame_ms,
                               1000.0 / cadence.frame_ms);
        }
    }
    if (window.Waited(Wait::GuestOnGpu).ms >= window_ms * 0.5) {
        return fmt::format("the GPU side, though no thread is saturated; most of its time goes "
                           "to {}",
                           Dominant(window, window.Renderer()));
    }
    return "unclear, nothing measured is saturated";
}

/// Host memory, empty where it is not measured (anything but Windows). `low` is set when the
/// host has nearly none left.
std::string Memory(bool& low) {
#ifdef _WIN32
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (!GlobalMemoryStatusEx(&status) ||
        !GetProcessMemoryInfo(GetCurrentProcess(),
                              reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                              sizeof(counters))) {
        return {};
    }
    constexpr double GiB = 1024.0 * 1024.0 * 1024.0;
    const double commit_used = 1.0 - static_cast<double>(status.ullAvailPageFile) /
                                         static_cast<double>(status.ullTotalPageFile);
    low = status.ullAvailPhys * 20 < status.ullTotalPhys || commit_used >= 0.95;
    return fmt::format("; memory: emulator {:.1f} GiB committed and {:.1f} GiB in RAM, host RAM "
                       "free {:.1f} of {:.1f} GiB, host commit {:.0f}% of its limit",
                       static_cast<double>(counters.PrivateUsage) / GiB,
                       static_cast<double>(counters.WorkingSetSize) / GiB,
                       static_cast<double>(status.ullAvailPhys) / GiB,
                       static_cast<double>(status.ullTotalPhys) / GiB, commit_used * 100.0);
#else
    low = false;
    return {};
#endif
}

/// One line with the process's memory and what each owner reports (Common::MemoryAccounting).
/// An owner that never reported is left out.
void ReportMemory() {
    namespace Accounting = Common::MemoryAccounting;
    using Accounting::Owner;
    constexpr u64 MiB = u64{1} << 20;
    std::string text;
    const auto item = [&text]<typename... Args>(fmt::format_string<Args...> format, Args&&... args) {
        if (!text.empty()) {
            text += ", ";
        }
        fmt::format_to(std::back_inserter(text), format, std::forward<Args>(args)...);
    };
    const auto owner = [&item](const char* name, Owner which) {
        if (Accounting::Peak(which) != 0) {
            item("{} {} MiB (peak {})", name, Accounting::Bytes(which) / MiB,
                 Accounting::Peak(which) / MiB);
        }
    };
    u64 private_bytes = 0;
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                             reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                             sizeof(counters))) {
        private_bytes = counters.PrivateUsage;
        item("private {} MiB, working set {} MiB", private_bytes / MiB,
             static_cast<u64>(counters.WorkingSetSize) / MiB);
    }
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status)) {
        item("system commit free {} MiB", status.ullAvailPageFile / MiB);
    }
#endif
    owner("guest RAM committed", Owner::GuestRam);
    owner("JIT code", Owner::JitCode);
    if (Accounting::Peak(Owner::GpuLocalBudget) != 0 || Accounting::Peak(Owner::GpuHostBudget) != 0) {
        item("GPU heap {} MiB (peak {}) of {} MiB (device-local) + {} MiB (peak {}) of {} MiB "
             "(host-visible)",
             Accounting::Bytes(Owner::GpuLocalHeap) / MiB, Accounting::Peak(Owner::GpuLocalHeap) / MiB,
             Accounting::Bytes(Owner::GpuLocalBudget) / MiB,
             Accounting::Bytes(Owner::GpuHostHeap) / MiB, Accounting::Peak(Owner::GpuHostHeap) / MiB,
             Accounting::Bytes(Owner::GpuHostBudget) / MiB);
    }
    owner("staging", Owner::Staging);
    owner("buffer cache scratch", Owner::BufferScratch);
    owner("texture scratch", Owner::TextureScratch);
    if (private_bytes != 0) {
        // The owners that sit in private memory. Guest RAM is a mapped section, the GPU heaps are
        // driver memory and staging may be either, so they are not taken off.
        const u64 listed = Accounting::Bytes(Owner::JitCode) +
                           Accounting::Bytes(Owner::BufferScratch) +
                           Accounting::Bytes(Owner::TextureScratch);
        item("other {} MiB", (static_cast<s64>(private_bytes) - static_cast<s64>(listed)) /
                                 static_cast<s64>(MiB));
    }
    if (!text.empty()) {
        LOG_INFO(Core, "{}", text);
    }
}

/// EDEN_NX2_MEMORY_MAP=1 turns on the region census, ReportMemoryMap.
bool MemoryMapEnabled() {
    static const bool enabled = [] {
        const char* const value = std::getenv("EDEN_NX2_MEMORY_MAP");
        return value != nullptr && value[0] != '\0' && std::string_view{value} != "0";
    }();
    return enabled;
}

/// Wakes the report thread for a census (guarded by wake_mutex) and tells the waiter it is done.
/// Process-lifetime, like the report thread that waits on them: never destroyed at exit.
std::mutex& wake_mutex = *new std::mutex;
std::condition_variable& wake = *new std::condition_variable;
bool census_requested = false;
bool census_done = false;

/// One line from a VirtualQuery walk of the whole process: committed bytes by kind, the process's
/// private usage beside the MEM_PRIVATE bytes the walk sees, and the 30 biggest allocations with
/// how much of each is in the working set (one page in 16 is sampled). Ranges owners registered
/// with Common::MemoryAccounting::Register are summed under their name, wherever their views lie.
void ReportMemoryMap() {
#ifdef _WIN32
    namespace Accounting = Common::MemoryAccounting;
    constexpr u64 MiB = u64{1} << 20;
    constexpr u64 SampleStride = 16;
    constexpr size_t BatchSize = 4096;
    constexpr size_t Shown = 30;
    const auto start = std::chrono::steady_clock::now();

    struct Group {
        const char* name; ///< Null: an allocation nobody registered
        uintptr_t base;
        u64 reserved;
        u64 committed;
        u64 sampled; ///< Committed bytes the working-set samples found resident
        DWORD type;  ///< Of the committed regions
    };
    std::vector<Group> groups;
    for (const Accounting::Range& range : Accounting::ranges) {
        // A slot read between two equal live states was not unregistered or reused meanwhile.
        const u64 state = range.state.load();
        if ((state & 3) != 2) {
            continue;
        }
        const char* const name = range.name.load();
        const uintptr_t range_base = range.base.load();
        const size_t range_size = range.size.load();
        if (range.state.load() == state) {
            groups.push_back({name, range_base, range_size, 0, 0, 0});
        }
    }
    std::sort(groups.begin(), groups.end(), [](const Group& left, const Group& right) {
        return left.base < right.base;
    });
    const size_t registered = groups.size();

    SYSTEM_INFO system_info{};
    GetSystemInfo(&system_info);
    const u64 page_size = system_info.dwPageSize;
    const auto limit = reinterpret_cast<uintptr_t>(system_info.lpMaximumApplicationAddress);
    HANDLE const process = GetCurrentProcess();
    std::vector<PSAPI_WORKING_SET_EX_INFORMATION> batch;
    std::vector<size_t> batch_group;
    /// The bytes each sample stands for: a stride, or less at the end of a region.
    std::vector<u64> batch_bytes;
    batch.reserve(BatchSize);
    batch_group.reserve(BatchSize);
    batch_bytes.reserve(BatchSize);
    const auto flush = [&] {
        if (!batch.empty() &&
            QueryWorkingSetEx(process, batch.data(),
                              static_cast<DWORD>(batch.size() * sizeof(batch.front())))) {
            for (size_t i = 0; i < batch.size(); ++i) {
                groups[batch_group[i]].sampled +=
                    (batch[i].VirtualAttributes.Flags & 1) != 0 ? batch_bytes[i] : u64{0};
            }
        }
        batch.clear();
        batch_group.clear();
        batch_bytes.clear();
    };

    MEMORY_BASIC_INFORMATION region{};
    size_t current = ~size_t{0}; ///< The unregistered group the walk is in, none yet
    uintptr_t address = 0;
    while (address <= limit &&
           VirtualQuery(reinterpret_cast<const void*>(address), &region, sizeof(region)) != 0) {
        const auto begin = reinterpret_cast<uintptr_t>(region.BaseAddress);
        const u64 size = region.RegionSize;
        const uintptr_t next = begin + static_cast<uintptr_t>(size);
        if (region.State != MEM_FREE) {
            // Heap allocations usually start after the region's allocator header. Split at
            // registered ranges instead of assigning the whole region from its first byte.
            uintptr_t segment = begin;
            while (segment < next) {
                const auto registered_end =
                    groups.begin() + static_cast<std::ptrdiff_t>(registered);
                const auto after = std::upper_bound(
                    groups.begin(), registered_end, segment,
                    [](uintptr_t addr, const Group& group) { return addr < group.base; });
                size_t index = registered;
                uintptr_t segment_end = next;
                if (after != groups.begin()) {
                    const auto before = std::prev(after);
                    if (segment - before->base < before->reserved) {
                        index = static_cast<size_t>(before - groups.begin());
                        segment_end = (std::min)(
                            segment_end, before->base + static_cast<uintptr_t>(before->reserved));
                    }
                }
                if (after != registered_end) {
                    segment_end = (std::min)(segment_end, after->base);
                }
                if (index == registered) {
                    const auto allocation = reinterpret_cast<uintptr_t>(region.AllocationBase);
                    if (current == ~size_t{0} || groups[current].base != allocation) {
                        groups.push_back({nullptr, allocation, 0, 0, 0, 0});
                        current = groups.size() - 1;
                    }
                    index = current;
                    groups[index].reserved += segment_end - segment;
                }
                if (region.State == MEM_COMMIT) {
                    groups[index].type = region.Type;
                    groups[index].committed += segment_end - segment;
                    for (uintptr_t page = segment; page < segment_end;
                         page += page_size * SampleStride) {
                        PSAPI_WORKING_SET_EX_INFORMATION entry{};
                        entry.VirtualAddress = reinterpret_cast<PVOID>(page);
                        batch.push_back(entry);
                        batch_group.push_back(index);
                        batch_bytes.push_back((std::min)(
                            page_size * SampleStride, static_cast<u64>(segment_end - page)));
                        if (batch.size() == BatchSize) {
                            flush();
                        }
                    }
                }
                segment = segment_end;
            }
        }
        if (next <= begin) {
            break;
        }
        address = next;
    }
    flush();

    // Private, mapped and image
    std::array<u64, 3> committed_total{};
    std::array<u64, 3> resident_total{};
    // Private committed bytes in allocations below 1 MiB, 1-16 MiB, 16-64 MiB and 64 MiB or more
    std::array<u64, 4> by_size{};
    std::vector<const Group*> top;
    const auto resident = [](const Group& group) {
        return (std::min)(group.sampled, group.committed);
    };
    const auto kind_of = [](DWORD type) -> size_t {
        return type == MEM_MAPPED ? 1U : type == MEM_IMAGE ? 2U : 0U;
    };
    for (const Group& group : groups) {
        if (group.committed == 0) {
            continue;
        }
        const size_t kind = kind_of(group.type);
        committed_total[kind] += group.committed;
        resident_total[kind] += resident(group);
        if (kind == 0) {
            by_size[group.committed < MiB        ? 0U
                    : group.committed < 16 * MiB ? 1U
                    : group.committed < 64 * MiB ? 2U
                                                 : 3U] += group.committed;
        }
        top.push_back(&group);
    }
    const size_t shown = (std::min)(top.size(), Shown);
    std::partial_sort(top.begin(), top.begin() + static_cast<std::ptrdiff_t>(shown), top.end(),
                      [](const Group* a, const Group* b) { return a->committed > b->committed; });

    std::string text = fmt::format("private committed {} MiB, mapped {} MiB, image {} MiB",
                                   committed_total[0] / MiB, committed_total[1] / MiB,
                                   committed_total[2] / MiB);
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(process, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                             sizeof(counters))) {
        fmt::format_to(std::back_inserter(text),
                       ", process private usage {} MiB (private usage minus MEM_PRIVATE {} MiB; "
                       "GPU attribution unknown)",
                       static_cast<u64>(counters.PrivateUsage) / MiB,
                       (static_cast<s64>(counters.PrivateUsage) -
                        static_cast<s64>(committed_total[0])) /
                           static_cast<s64>(MiB));
    }
    fmt::format_to(std::back_inserter(text),
                   "; in the working set (sampled, views of guest RAM counted twice): private ~{} "
                   "MiB, mapped ~{} MiB, image ~{} MiB",
                   resident_total[0] / MiB, resident_total[1] / MiB, resident_total[2] / MiB);
    fmt::format_to(std::back_inserter(text),
                   "; private by allocation size: under 1 MiB {} MiB, 1-16 MiB {} MiB, 16-64 MiB "
                   "{} MiB, 64 MiB and more {} MiB",
                   by_size[0] / MiB, by_size[1] / MiB, by_size[2] / MiB, by_size[3] / MiB);
    text += "; largest:";
    constexpr std::array<const char*, 3> KindNames{"private", "mapped", "image"};
    for (size_t i = 0; i < shown; ++i) {
        const Group& group = *top[i];
        fmt::format_to(std::back_inserter(text),
                       " {}@{:#x} committed {} of {} MiB {} (~{} MiB in the working set);",
                       group.name != nullptr ? group.name : "?", group.base, group.committed / MiB,
                       group.reserved / MiB, KindNames[kind_of(group.type)], resident(group) / MiB);
    }
    fmt::format_to(
        std::back_inserter(text), " census took {} ms",
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                              start)
            .count());
    LOG_INFO(Core, "{}", text);
#endif
}

/// The SVCs a game calls most often by name, any other by number.
std::string SvcName(u32 id) {
    static constexpr std::array<std::pair<u32, std::string_view>, 27> Names{{
        {0x06, "QueryMemory"},
        {0x0B, "SleepThread"},
        {0x10, "GetCurrentProcessorNumber"},
        {0x11, "SignalEvent"},
        {0x12, "ClearEvent"},
        {0x16, "CloseHandle"},
        {0x17, "ResetSignal"},
        {0x18, "WaitSynchronization"},
        {0x1A, "ArbitrateLock"},
        {0x1B, "ArbitrateUnlock"},
        {0x1C, "WaitProcessWideKeyAtomic"},
        {0x1D, "SignalProcessWideKey"},
        {0x1E, "GetSystemTick"},
        {0x21, "SendSyncRequest"},
        {0x22, "SendSyncRequestWithUserBuffer"},
        {0x34, "WaitForAddress"},
        {0x35, "SignalToAddress"},
        {0x36, "SynchronizePreemptionState"},
        {0x43, "ReplyAndReceive"},
        {0x44, "ReplyAndReceiveWithUserBuffer"},
        {0x80, "SetGpuMemoryAttribute"},
        {0x81, "LockGpuSharableMemory"},
        {0x82, "UnlockGpuSharableMemory"},
        {0x84, "MapRemoteMemory"},
        {0x85, "UnmapRemoteMemory"},
        {0x87, "(unnamed 0x87)"},
        {0x88, "(unnamed 0x88)"},
    }};
    for (const auto& [number, name] : Names) {
        if (number == id) {
            return std::string{name};
        }
    }
    return fmt::format("{:#04x}", id);
}

/// What the guest's CPU side did in the window: SVCs, fastmem faults, JIT compiling and waits
/// for the kernel scheduler lock.
std::string CpuLine(const Window& window) {
    std::string text;
    const auto add = [&text]<typename... Args>(fmt::format_string<Args...> format, Args&&... args) {
        fmt::format_to(std::back_inserter(text), format, std::forward<Args>(args)...);
    };
    add("svc {:.0f}/s", static_cast<double>(window.svc_total) / window.seconds);
    const char* separator = " (top: ";
    for (const SvcCount& svc : window.svc_top) {
        if (svc.count == 0) {
            break;
        }
        add("{}{} {}", separator, SvcName(svc.id), svc.count);
        separator = ", ";
    }
    if (window.svc_top.front().count != 0) {
        add(")");
    }
    if (window.jit_known) {
        add(", fastmem faults {:.1f}/s, JIT compiled {} blocks / {:.1f} ms",
            static_cast<double>(window.jit.fastmem_faults) / window.seconds,
            window.jit.blocks_compiled, static_cast<double>(window.jit.compile_ns) / 1e6);
    }
    const WaitTotals lock = window.Waited(Wait::KernelLock);
    add(", scheduler lock wait {:.1f} ms in {} contended takes (longest {:.2f} ms)", lock.ms,
        lock.count, lock.longest_ms);
    return text;
}

void Report(const Window& window) {
    std::string text;
    const auto add = [&text]<typename... Args>(fmt::format_string<Args...> format, Args&&... args) {
        fmt::format_to(std::back_inserter(text), format, std::forward<Args>(args)...);
    };
    add("Last {:.0f} s: {:.1f} fps", window.seconds, window.frames / window.seconds);
    if (window.frames != 0) {
        add(" (frame {:.1f} ms, longest {:.0f} ms, {} over 100 ms)",
            window.seconds * 1000.0 / window.frames, window.longest_frame_ms, window.over_100ms);
    }
    add(". Limited by: {}", Limiter(window));
    const WaitTotals shaders = window.Waited(Wait::ShaderBuild);
    const WaitTotals reads = window.Waited(Wait::FileRead);
    if (reads.longest_ms >= 50.0) {
        add("; a file read took {:.0f} ms", reads.longest_ms);
    }
    if (shaders.ms >= 20.0) {
        add("; shader builds held rendering for {:.0f} ms", shaders.ms);
    }
    bool memory_low = false;
    const std::string memory = Memory(memory_low);
    if (memory_low) {
        add("; host memory is nearly full");
    }

    for (size_t i = 0; i < NumThreads; ++i) {
        const Thread thread = static_cast<Thread>(i);
        const double busy = window.Busy(thread);
        if (busy < 0.0) {
            // Never ran: the back thread without the GPU thread split.
            continue;
        }
        add("{} {}: {:.0f}% working", i == 0 ? "." : ";", ThreadNames[i],
            window.Working(thread) * 100.0);
        for (size_t wait = 0; wait < NumThreadWaits; ++wait) {
            if (const double share = window.Share(thread, wait); share >= 0.005) {
                add(", {:.0f}% waiting for {}", share * 100.0, WaitNames[wait]);
            }
        }
        add(", {:.0f}% idle", (1.0 - busy) * 100.0);
    }
    add("; game code per CPU core:");
    for (const double share : window.guest_run) {
        // Cores the guest never ran on are left out.
        if (share > 0.0) {
            add(" {:.0f}%", share * 100.0);
        }
    }
    if (const WaitTotals fence = window.Waited(Wait::GuestOnGpu); fence.count != 0) {
        add("; game waiting for its frames on the GPU: {:.0f}% of the time (longest {:.0f} ms)",
            fence.ms / (window.seconds * 10.0), fence.longest_ms);
    }
    add("; shader build waits: {} ({:.0f} ms, longest {:.0f} ms)", shaders.count, shaders.ms,
        shaders.longest_ms);
    add("; file reads: {} ({:.0f} ms, longest {:.0f} ms)", reads.count, reads.ms, reads.longest_ms);
    add("; audio underruns: {}{}", window.audio_underruns, memory);
    LOG_INFO(Core, "{}", text);
    LOG_INFO(Core,
             "Frames: {} shown | late (> 1.5x the median interval) {} | longest {:.1f} ms | "
             "median {:.1f} ms | 99th {:.1f} ms",
             window.frames, window.late, window.longest_frame_ms, window.median_ms,
             window.p99_ms);
    LOG_INFO(Core, "CPU: {}", CpuLine(window));
    std::string name;
    {
        std::scoped_lock lock{sleep_name_mutex};
        if (sleep_name_source != nullptr && window.sleep_top_calls != 0) {
            name = sleep_name_source(sleep_name_user, window.sleep_top_id);
        }
    }
    LOG_INFO(Core,
             "SleepThread: ns=0 {} | ns=-1 {} | ns=-2 {} | 1..999ns {} | "
             "1..99us {} | 100..999us {} | >=1ms {} | other-negative {} | "
             "top tid={:#x} name={:?} calls={} | empty-yields {} | "
             "backoffs {} waited {:.3f} ms longest {:.3f} ms",
             window.sleep_bins[0], window.sleep_bins[1], window.sleep_bins[2],
             window.sleep_bins[3], window.sleep_bins[4], window.sleep_bins[5],
             window.sleep_bins[6], window.sleep_bins[7], window.sleep_top_id,
             name.empty() ? std::string{"unavailable"} : name,
             window.sleep_top_calls, window.empty_yields, window.yield_backoffs,
             static_cast<double>(window.yield_wait_ns) / 1e6,
             static_cast<double>(window.yield_longest_ns) / 1e6);
}

// The run report: the windows of the log, kept as numbers.

static_assert(NumThreads == 3);

[[nodiscard]] size_t BucketOf(u64 us) {
    if (us < 64) {
        return static_cast<size_t>(us);
    }
    const unsigned octave = static_cast<unsigned>(std::bit_width(us)) - 1;
    return 64 + 64 * (octave - 6) + static_cast<size_t>((us - (u64{1} << octave)) >> (octave - 6));
}

/// The largest microsecond count in bucket `index`.
[[nodiscard]] u64 BucketUpper(size_t index) {
    if (index < 64) {
        return index;
    }
    const size_t step = index - 64;
    const unsigned octave = 6 + static_cast<unsigned>(step / 64);
    const u64 width = u64{1} << (octave - 6);
    return (u64{1} << octave) + static_cast<u64>(step % 64) * width + (width - 1);
}

/// The sample at nearest rank ceil(percent / 100 * samples), as the upper end of its bucket
/// (microseconds) capped at the exact longest, in milliseconds.
[[nodiscard]] std::optional<double> Quantile(const std::array<u64, HistogramBuckets>& histogram,
                                             u64 samples, u64 percent, u64 max_us) {
    if (samples == 0) {
        return std::nullopt;
    }
    const u64 rank = (std::max)(u64{1}, (samples * percent + 99) / 100);
    u64 seen = 0;
    for (size_t i = 0; i < histogram.size(); ++i) {
        seen += histogram[i];
        if (seen >= rank) {
            return static_cast<double>((std::min)(BucketUpper(i), max_us)) / 1e3;
        }
    }
    return static_cast<double>(max_us) / 1e3;
}

void FillIntervals(RunMetrics& metrics, const std::array<u64, HistogramBuckets>& histogram,
                   u64 sum_us, u64 max_us) {
    u64 samples = 0;
    for (const u64 count : histogram) {
        samples += count;
    }
    metrics.samples = samples;
    if (samples == 0) {
        metrics.mean_ms.reset();
        metrics.p50_ms.reset();
        metrics.p95_ms.reset();
        metrics.p99_ms.reset();
        metrics.max_ms.reset();
        return;
    }
    metrics.mean_ms = static_cast<double>(sum_us) / static_cast<double>(samples) / 1e3;
    metrics.p50_ms = Quantile(histogram, samples, 50, max_us);
    metrics.p95_ms = Quantile(histogram, samples, 95, max_us);
    metrics.p99_ms = Quantile(histogram, samples, 99, max_us);
    metrics.max_ms = static_cast<double>(max_us) / 1e3;
}

/// One window as numbers. A thread's work is what its time leaves after idling and its waits.
RunMetrics MetricsOf(const Window& window) {
    RunMetrics metrics;
    metrics.active_seconds = window.seconds;
    metrics.frames = window.frames;
    if (window.seconds > 0.0) {
        metrics.fps = static_cast<double>(window.frames) / window.seconds;
    }
    FillIntervals(metrics, window_histogram, window.interval_sum_us, window.interval_max_us);
    metrics.over_25ms = window.over_25ms;
    metrics.over_50ms = window.over_50ms;
    metrics.over_100ms = window.over_100ms;
    for (size_t i = 0; i < NumThreads; ++i) {
        if (window.busy[i] < 0.0) {
            // Never ran: the back thread without the GPU thread split.
            continue;
        }
        ThreadSplit& split = metrics.threads[i];
        split.seen = true;
        split.observed_seconds = window.seconds;
        split.idle_ms = window.idle_ms[i];
        double waited_ms = 0.0;
        for (size_t wait = 0; wait < NumThreadWaits; ++wait) {
            split.waits_ms[wait] = window.waits[i][wait].ms;
            waited_ms += window.waits[i][wait].ms;
        }
        split.work_ms = (std::max)(window.seconds * 1000.0 - split.idle_ms - waited_ms, 0.0);
    }
    const WaitTotals fence = window.Waited(Wait::GuestOnGpu);
    metrics.guest_gpu_wait = {.total_ms = fence.ms, .longest_ms = fence.longest_ms,
                              .count = fence.count};
    if (window.seconds > 0.0) {
        // Summed over threads, so it can pass 1.
        metrics.guest_gpu_fraction = fence.ms / (window.seconds * 1000.0);
    }
    metrics.guest_cpu_core_busy_ms = window.guest_run_ms;
    const WaitTotals shaders = window.Waited(Wait::ShaderBuild);
    metrics.shader_wait = {.total_ms = shaders.ms, .longest_ms = shaders.longest_ms,
                           .count = shaders.count};
    metrics.limiter = Limiter(window);
    return metrics;
}

/// Adds the counters of `part` to `total`; the interval statistics and the limiter of a total
/// come from the cumulative histogram and the aggregate window instead.
void Accumulate(RunMetrics& total, const RunMetrics& part) {
    total.active_seconds += part.active_seconds;
    total.frames += part.frames;
    total.over_25ms += part.over_25ms;
    total.over_50ms += part.over_50ms;
    total.over_100ms += part.over_100ms;
    for (size_t i = 0; i < NumThreads; ++i) {
        const ThreadSplit& from = part.threads[i];
        if (!from.seen) {
            continue;
        }
        ThreadSplit& to = total.threads[i];
        to.seen = true;
        to.observed_seconds += from.observed_seconds;
        to.work_ms += from.work_ms;
        to.idle_ms += from.idle_ms;
        for (size_t wait = 0; wait < NumThreadWaits; ++wait) {
            to.waits_ms[wait] += from.waits_ms[wait];
        }
    }
    const auto add_wait = [](WaitSummary& to, const WaitSummary& from) {
        to.total_ms += from.total_ms;
        to.longest_ms = (std::max)(to.longest_ms, from.longest_ms);
        to.count += from.count;
    };
    add_wait(total.guest_gpu_wait, part.guest_gpu_wait);
    add_wait(total.shader_wait, part.shader_wait);
    for (size_t core = 0; core < MaxCores; ++core) {
        total.guest_cpu_core_busy_ms[core] += part.guest_cpu_core_busy_ms[core];
    }
}

struct Recorder {
    bool enabled = false;
    u64 origin_ns = 0;
    RunMetrics sum;
    /// Raw sums of the windows, for the limiter of the whole run.
    Window aggregate;
    std::array<double, NumThreads> busy_weighted{};
    std::array<double, NumThreads> seen_seconds{};
    std::array<u64, HistogramBuckets> histogram{};
    u64 sum_us = 0;
    u64 max_us = 0;
    std::vector<RunWindow> windows;
};

/// Guarded by collect_mutex. Never destroyed: the report loop can outlive static destructors.
Recorder& GetRecorder() {
    static auto* recorder = new Recorder;
    return *recorder;
}

/// Seconds from the start of the recording to `ns`, on Now()'s clock.
double RecordingSeconds(u64 ns) {
    const u64 origin = GetRecorder().origin_ns;
    return ns > origin ? static_cast<double>(ns - origin) / 1e9 : 0.0;
}

/// Keeps `window`, which covers `begin` to `now`, in the recording. With collect_mutex held, and
/// right after the Collect that produced it (it reads window_histogram).
void RecordWindowLocked(const Window& window, u64 begin, u64 now, bool partial) {
    Recorder& recorder = GetRecorder();
    if (!recorder.enabled || !(window.seconds > 0.0)) {
        return;
    }
    RunMetrics metrics = MetricsOf(window);
    Accumulate(recorder.sum, metrics);
    Window& aggregate = recorder.aggregate;
    aggregate.seconds += window.seconds;
    aggregate.frames += window.frames;
    aggregate.over_25ms += window.over_25ms;
    aggregate.over_50ms += window.over_50ms;
    aggregate.over_100ms += window.over_100ms;
    aggregate.over_25ms_ms += window.over_25ms_ms;
    aggregate.over_50ms_ms += window.over_50ms_ms;
    aggregate.longest_frame_ms = (std::max)(aggregate.longest_frame_ms, window.longest_frame_ms);
    for (size_t i = 0; i < NumThreads; ++i) {
        if (window.busy[i] >= 0.0) {
            recorder.busy_weighted[i] += window.busy[i] * window.seconds;
            recorder.seen_seconds[i] += window.seconds;
        }
    }
    for (size_t row = 0; row < NumWaitRows; ++row) {
        for (size_t i = 0; i < NumWaits; ++i) {
            WaitTotals& to = aggregate.waits[row][i];
            const WaitTotals& from = window.waits[row][i];
            to.ms += from.ms;
            to.longest_ms = (std::max)(to.longest_ms, from.longest_ms);
            to.count += from.count;
        }
    }
    for (size_t i = 0; i < HistogramBuckets; ++i) {
        recorder.histogram[i] += window_histogram[i];
    }
    recorder.sum_us += window.interval_sum_us;
    recorder.max_us = (std::max)(recorder.max_us, window.interval_max_us);
    recorder.windows.push_back({.start_seconds = RecordingSeconds(begin),
                                .end_seconds = RecordingSeconds(now),
                                .partial = partial,
                                .metrics = std::move(metrics)});
}

/// The counters since the window began, as a window that ends now (guest paused or run over).
void CloseSegmentLocked(u64 now) {
    const u64 begin = std::exchange(collect_begin, now);
    const Window window = Collect(begin, now);
    RecordWindowLocked(window, begin, now, true);
}

/// Drops what the counters gathered while the guest did not run; its windows start here.
void StartSegmentLocked(u64 now) {
    const u64 begin = collect_begin != 0 ? collect_begin : now;
    static_cast<void>(Collect(begin, now));
    collect_begin = now;
}

/// The whole run: the windows added up, with the frame statistics of the cumulative histogram and
/// the limiter judged on the raw sums.
RunMetrics SummarizeLocked() {
    Recorder& recorder = GetRecorder();
    RunMetrics summary = recorder.sum;
    const double active = summary.active_seconds;
    if (!(active > 0.0)) {
        return RunMetrics{};
    }
    FillIntervals(summary, recorder.histogram, recorder.sum_us, recorder.max_us);
    summary.fps = static_cast<double>(summary.frames) / active;
    summary.guest_gpu_fraction = summary.guest_gpu_wait.total_ms / (active * 1000.0);
    Window aggregate = recorder.aggregate;
    for (size_t i = 0; i < NumThreads; ++i) {
        aggregate.busy[i] =
            recorder.seen_seconds[i] > 0.0 ? recorder.busy_weighted[i] / recorder.seen_seconds[i]
                                           : -1.0;
    }
    for (size_t core = 0; core < MaxCores; ++core) {
        aggregate.guest_run[core] = summary.guest_cpu_core_busy_ms[core] / (active * 1000.0);
    }
    summary.limiter = Limiter(aggregate);
    return summary;
}

void ReportLoop() {
    Common::SetCurrentThreadName("Nx2Bottleneck");
    {
        std::scoped_lock lock{collect_mutex};
        if (collect_begin == 0) {
            collect_begin = Now();
        }
    }
    u32 run_seen = ~u32{0};
    u32 memory_reports = 0;
    for (;;) {
        bool census_now = false;
        {
            std::unique_lock lock{wake_mutex};
            census_now = wake.wait_for(lock, Period, [] { return census_requested; });
            census_requested = false;
        }
        if (census_now) {
            ReportMemoryMap();
            {
                std::lock_guard lock{wake_mutex};
                census_done = true;
            }
            wake.notify_all();
            continue;
        }
        const u64 now = Now();
        Window window;
        bool whole = false;
        {
            std::scoped_lock lock{collect_mutex};
            const u64 begin = std::exchange(collect_begin, now);
            window = Collect(begin, now);
            // A window the guest was paused or started in is not a whole one.
            const u32 run = run_count.load(std::memory_order_relaxed);
            const bool running = guest_running.load(std::memory_order_relaxed);
            whole = running && run == run_seen;
            run_seen = run;
            if (running) {
                // The report keeps those too, marked partial, as it keeps the end of the run.
                RecordWindowLocked(window, begin, now,
                                   !whole || window.seconds < static_cast<double>(Period.count()) * 0.99);
            }
        }
        if (whole) {
            Report(window);
            ReportMemory();
            if (MemoryMapEnabled() && ++memory_reports % 6 == 0) {
                ReportMemoryMap();
            }
        }
    }
}

} // Anonymous namespace

u64 Now() {
    return static_cast<u64>(Common::g_wall_clock.GetTimeNS().count());
}

void MarkThread(Thread thread) {
    wait_row = static_cast<size_t>(thread);
}

void BeginIdle(Thread thread) {
    IdleMeter& meter = idle_meters[static_cast<size_t>(thread)];
    meter.since.store(Now(), std::memory_order_relaxed);
    meter.seen.store(true, std::memory_order_relaxed);
}

void EndIdle(Thread thread) {
    IdleMeter& meter = idle_meters[static_cast<size_t>(thread)];
    const u64 now = Now();
    const u64 since = (std::max)(meter.since.exchange(0, std::memory_order_relaxed),
                                 window_begin.load(std::memory_order_relaxed));
    if (now > since) {
        meter.ns.fetch_add(now - since, std::memory_order_relaxed);
    }
}

void AddWait(Wait wait, u64 ns) {
    WaitMeter& meter = wait_meters[wait_row][static_cast<size_t>(wait)];
    meter.ns.fetch_add(ns, std::memory_order_relaxed);
    meter.count.fetch_add(1, std::memory_order_relaxed);
    RaiseTo(meter.longest_ns, ns);
}

void AddGuestRun(size_t core, u64 ns) {
    if (core < MaxCores) {
        guest_run_ns[core].fetch_add(ns, std::memory_order_relaxed);
    }
}

void NoteFrame() {
    const u64 now = Now();
    const u64 previous = last_frame.exchange(now, std::memory_order_relaxed);
    frames.fetch_add(1, std::memory_order_relaxed);
    Common::RunOutcome::NoteFrame();
    if (previous == 0) {
        return;
    }
    const u64 ns = now - previous;
    RaiseTo(longest_frame_ns, ns);
    if (histogram_enabled.load(std::memory_order_relaxed)) {
        const u64 us = ns / 1000;
        RaiseTo(histogram_max_us, us);
        histogram_sum_us.fetch_add(us, std::memory_order_relaxed);
        interval_histogram[BucketOf(us)].fetch_add(1, std::memory_order_release);
    }
    const u32 written = intervals_written.load(std::memory_order_relaxed);
    interval_us[written % IntervalRing].store(
        static_cast<u32>((std::min)(ns / 1000, u64{(std::numeric_limits<u32>::max)()})),
        std::memory_order_relaxed);
    intervals_written.store(written + 1, std::memory_order_release);
    if (ns > 25'000'000) {
        frames_over_25ms.fetch_add(1, std::memory_order_relaxed);
        over_25ms_ns.fetch_add(ns, std::memory_order_relaxed);
    }
    if (ns > 50'000'000) {
        frames_over_50ms.fetch_add(1, std::memory_order_relaxed);
        over_50ms_ns.fetch_add(ns, std::memory_order_relaxed);
    }
    if (ns > 100'000'000) {
        frames_over_100ms.fetch_add(1, std::memory_order_relaxed);
    }
}

void NoteAudioUnderrun() {
    audio_underruns.fetch_add(1, std::memory_order_relaxed);
}

void NoteSvc(size_t core, u32 id) {
    if (core < MaxCores && id < NumSvcIds) {
        auto& count = svc_counts[core][id];
        count.store(count.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    }
}

void SleepThreadCounter::Register(u64 thread_id) {
    auto& registry = SleepThreads();
    std::scoped_lock lock{registry.mutex};
    id = thread_id;
    next = registry.first;
    registry.first = this;
}

u64 SleepThreadCounter::Collect(std::array<u64, 8>& totals, u64& empty_total) {
    u64 calls = 0;
    for (size_t bin = 0; bin < bins.size(); ++bin) {
        const u64 count = bins[bin].load(std::memory_order_relaxed);
        const u64 delta = count - previous_bins[bin];
        previous_bins[bin] = count;
        totals[bin] += delta;
        calls += delta;
    }
    const u64 count = empty.load(std::memory_order_relaxed);
    empty_total += count - previous_empty;
    previous_empty = count;
    return calls;
}

SleepThreadCounter::~SleepThreadCounter() {
    Reset();
}

void SleepThreadCounter::Reset() {
    auto& registry = SleepThreads();
    std::scoped_lock lock{registry.mutex};
    if (id == 0) {
        return;
    }
    const u64 count = Collect(registry.retired_bins, registry.retired_empty);
    if (count > registry.retired_calls) {
        registry.retired_calls = count;
        registry.retired_id = id;
    }
    auto** link = &registry.first;
    while (*link != this) {
        link = &(*link)->next;
    }
    *link = next;
    id = 0;
    next = nullptr;
}

void AddYieldBackoff(size_t core, u64 ns) {
    if (core < MaxCores) {
        auto& meter = sleep_meters[core];
        meter.backoffs.store(meter.backoffs.load(std::memory_order_relaxed) + 1,
                             std::memory_order_relaxed);
        meter.wait_ns.store(meter.wait_ns.load(std::memory_order_relaxed) + ns,
                            std::memory_order_relaxed);
        RaiseTo(meter.longest_ns, ns);
    }
}

void SetSleepThreadNameSource(std::string (*source)(void*, u64), void* user) {
    std::scoped_lock lock{sleep_name_mutex};
    sleep_name_source = source;
    sleep_name_user = user;
}

void ClearSleepThreadCounters() {
    auto& registry = SleepThreads();
    std::scoped_lock lock{registry.mutex};
    while (registry.first != nullptr) {
        auto* counter = registry.first;
        registry.first = counter->next;
        counter->id = 0;
        counter->next = nullptr;
    }
    registry.retired_id = 0;
    registry.retired_calls = 0;
    registry.retired_bins.fill(0);
    registry.retired_empty = 0;
    for (auto& meter : sleep_meters) {
        meter.backoffs.store(0, std::memory_order_relaxed);
        meter.wait_ns.store(0, std::memory_order_relaxed);
        meter.longest_ns.store(0, std::memory_order_relaxed);
        meter.previous_backoffs = 0;
        meter.previous_wait_ns = 0;
    }
}

void SetJitSource(JitTotals (*source)()) {
    jit_source.store(source, std::memory_order_relaxed);
}

void SetGuestRunning(bool running) {
    static std::once_flag started;
    if (running) {
        // The time paused is not a frame.
        last_frame.store(0, std::memory_order_relaxed);
        run_count.fetch_add(1, std::memory_order_relaxed);
        {
            std::scoped_lock lock{collect_mutex};
            if (GetRecorder().enabled) {
                StartSegmentLocked(Now());
            }
        }
        std::call_once(started, [] { std::thread{ReportLoop}.detach(); });
        guest_running.store(true, std::memory_order_relaxed);
        return;
    }
    if (MemoryMapEnabled() && guest_running.load(std::memory_order_relaxed)) {
        // The region census at pause or shutdown runs on the report thread, before the guest's
        // memory goes away; this waits for it.
        std::unique_lock lock{wake_mutex};
        census_requested = true;
        census_done = false;
        wake.notify_all();
        wake.wait_for(lock, std::chrono::seconds{5}, [] { return census_done; });
    }
    // The window the pause interrupts goes to the report as a partial one.
    std::scoped_lock lock{collect_mutex};
    if (GetRecorder().enabled && guest_running.load(std::memory_order_relaxed)) {
        CloseSegmentLocked(Now());
    }
    guest_running.store(false, std::memory_order_relaxed);
}

void BeginRecording(u64 origin_ns) {
    std::scoped_lock lock{collect_mutex};
    Recorder& recorder = GetRecorder();
    recorder = Recorder{};
    recorder.enabled = true;
    recorder.origin_ns = origin_ns;
    histogram_enabled.store(true, std::memory_order_relaxed);
    if (guest_running.load(std::memory_order_relaxed)) {
        StartSegmentLocked(Now());
    }
}

RunRecording EndRecording(bool forced) {
    std::scoped_lock lock{collect_mutex};
    Recorder& recorder = GetRecorder();
    RunRecording result;
    if (!recorder.enabled) {
        return result;
    }
    const bool still_running = guest_running.load(std::memory_order_relaxed);
    if (still_running) {
        CloseSegmentLocked(Now());
    }
    result.summary = SummarizeLocked();
    result.windows = std::move(recorder.windows);
    result.complete = !forced && !still_running;
    recorder.enabled = false;
    histogram_enabled.store(false, std::memory_order_relaxed);
    return result;
}

} // namespace Nx2Bottleneck
