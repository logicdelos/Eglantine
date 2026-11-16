// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/nx2_bottleneck.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <iterator>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <fmt/format.h>

#include "common/cpu_features.h"
#include "common/logging.h"
#include "common/memory_accounting.h"
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
std::atomic<u64> window_begin{};
std::atomic<u64> last_frame{};
std::atomic<u64> longest_frame_ns{};
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
    /// Share of the window not spent waiting for work; negative for a thread that never ran.
    std::array<double, NumThreads> busy{};
    std::array<std::array<WaitTotals, NumWaits>, NumWaitRows> waits{};
    std::array<double, MaxCores> guest_run{};
    u32 audio_underruns{};

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
};

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
        window.guest_run[i] =
            static_cast<double>(guest_run_ns[i].exchange(0, std::memory_order_relaxed)) / window_ns;
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
}

void ReportLoop() {
    Common::SetCurrentThreadName("Nx2Bottleneck");
    u64 begin = Now();
    u32 run_seen = ~u32{0};
    for (;;) {
        std::this_thread::sleep_for(Period);
        const u64 now = Now();
        const Window window = Collect(begin, now);
        begin = now;
        // A window the guest was paused or started in is not a whole one.
        const u32 run = run_count.load(std::memory_order_relaxed);
        if (guest_running.load(std::memory_order_relaxed) && run == run_seen) {
            Report(window);
            ReportMemory();
        }
        run_seen = run;
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
    if (previous == 0) {
        return;
    }
    const u64 ns = now - previous;
    RaiseTo(longest_frame_ns, ns);
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

void SetGuestRunning(bool running) {
    static std::once_flag started;
    if (running) {
        // The time paused is not a frame.
        last_frame.store(0, std::memory_order_relaxed);
        run_count.fetch_add(1, std::memory_order_relaxed);
        std::call_once(started, [] { std::thread{ReportLoop}.detach(); });
    }
    guest_running.store(running, std::memory_order_relaxed);
}

} // namespace Nx2Bottleneck
