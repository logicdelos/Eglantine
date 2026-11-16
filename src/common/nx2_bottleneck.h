// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "common/common_types.h"

// What limits the frame rate, said in the log every 10 seconds while a game runs: the frame rate,
// one verdict, and the numbers behind it. Always on. Each number is wall time measured where a
// thread waits, not CPU time from the system: a thread waiting in Common::Event (MWAITX on AMD)
// counts as running there.
namespace Nx2Bottleneck {

/// Host threads whose time divides into working and waiting for work.
enum class Thread : u32 {
    Gpu,
    GpuBack, // exists only with the GPU thread split
    VulkanWorker,
    Count,
};

/// What a thread stops for, kept per thread: the ones before GuestOnGpu come out of the working
/// time of the `Thread` they happen on.
enum class Wait : u32 {
    HostGpu,      // the graphics card, to finish work already submitted
    ShaderBuild,  // a pipeline the current draw cannot go without
    VulkanWorker, // the Vulkan worker, to empty its queue
    Present,      // a free presentation frame
    GpuBack,      // the GPU thread, for the back thread (GPU thread split)
    GuestOnGpu,   // the game, for the frame it presented to finish on the GPU
    FileRead,     // any thread, inside a read of a host file
    KernelLock,   // any thread, to take the kernel scheduler lock while another one holds it
    Count,
};

inline constexpr size_t MaxCores = 8;

/// SVC ids counted by NoteSvc.
inline constexpr size_t NumSvcIds = 256;

/// Totals since the process started that the JIT keeps itself.
struct JitTotals {
    u64 blocks_compiled{};
    u64 compile_ns{};
    u64 fastmem_faults{};
};

/// Nanoseconds on the clock every figure here uses.
[[nodiscard]] u64 Now();

/// The calling thread is `thread`: the waits it reports from here on are its own.
void MarkThread(Thread thread);

void BeginIdle(Thread thread);
void EndIdle(Thread thread);
void AddWait(Wait wait, u64 ns);
/// Time a CPU core thread spent inside guest code.
void AddGuestRun(size_t core, u64 ns);
void NoteFrame();
void NoteAudioUnderrun();
/// The guest on CPU core `core` called SVC `id`; the CPU line of the report counts them.
void NoteSvc(size_t core, u32 id);
/// Where the CPU line of the report reads the JIT's totals; its JIT part shows once this is set.
void SetJitSource(JitTotals (*source)());

// One writer (the guest thread), isolated from other threads' hot counters.
struct alignas(64) SleepThreadCounter {
    ~SleepThreadCounter();
    void Register(u64 thread_id);
    void Reset();
    u64 Collect(std::array<u64, 8>& totals, u64& empty_total);

    void Note(s64 ns) {
        const size_t bin = ns == 0 ? 0U : ns == -1 ? 1U : ns == -2 ? 2U : ns < 0 ? 7U :
                           ns < 1'000 ? 3U : ns < 100'000 ? 4U : ns < 1'000'000 ? 5U : 6U;
        auto& counter = bins[bin];
        counter.store(counter.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    }

    void NoteEmptyYield(bool yielded) {
        empty.store(empty.load(std::memory_order_relaxed) + static_cast<u64>(yielded),
                    std::memory_order_relaxed);
    }

    std::array<std::atomic<u64>, 8> bins{};
    std::atomic<u64> empty{};
    std::array<u64, 8> previous_bins{};
    u64 previous_empty{};
    u64 id{};
    SleepThreadCounter* next{};
};

void AddYieldBackoff(size_t core, u64 ns);
void SetSleepThreadNameSource(std::string (*source)(void*, u64), void* user);
// Called after stopping guest cores, before their slab storage can be recycled.
void ClearSleepThreadCounters();

/// Reports run only while the guest does: not paused, not stopped.
void SetGuestRunning(bool running);

class IdleScope {
public:
    explicit IdleScope(Thread thread_) : thread{thread_} {
        BeginIdle(thread);
    }
    ~IdleScope() {
        EndIdle(thread);
    }
    IdleScope(const IdleScope&) = delete;
    IdleScope& operator=(const IdleScope&) = delete;

private:
    Thread thread;
};

class WaitScope {
public:
    explicit WaitScope(Wait wait_) : wait{wait_}, begin{Now()} {}
    ~WaitScope() {
        AddWait(wait, Now() - begin);
    }
    WaitScope(const WaitScope&) = delete;
    WaitScope& operator=(const WaitScope&) = delete;

private:
    Wait wait;
    u64 begin;
};

// The numbers of a run for the command line's --report. The collector that writes the log every 10
// seconds keeps them too, window by window; nothing here collects on its own.

/// A thread's time: observed = work + idle + the waits, within the timer's rounding (a wait that
/// runs past the thread's working time leaves work at 0 instead of negative).
struct ThreadSplit {
    bool seen{}; // false: the thread never ran (the back thread without the GPU thread split)
    double observed_seconds{};
    double work_ms{};
    double idle_ms{};
    /// Indexed by `Wait`, HostGpu to GpuBack.
    std::array<double, static_cast<size_t>(Wait::GuestOnGpu)> waits_ms{};
};

struct WaitSummary {
    double total_ms{};
    double longest_ms{};
    u64 count{};
};

/// The statistics of a window or of the whole run.
struct RunMetrics {
    double active_seconds{}; // time the guest ran; a paused guest does not count
    u64 frames{};
    std::optional<double> fps; // empty without active time
    /// Present intervals: every frame but the first after a start or resume has one. Percentiles
    /// are nearest rank on a log histogram of microseconds (a bucket is at most 1/64 of its
    /// octave wide), capped at the exact longest.
    u64 samples{};
    std::optional<double> mean_ms;
    std::optional<double> p50_ms;
    std::optional<double> p95_ms;
    std::optional<double> p99_ms;
    std::optional<double> max_ms;
    u64 over_25ms{};
    u64 over_50ms{};
    u64 over_100ms{};
    std::array<ThreadSplit, static_cast<size_t>(Thread::Count)> threads{};
    /// The game waiting for its frames on the GPU, summed over threads: it can exceed the time.
    WaitSummary guest_gpu_wait{};
    std::optional<double> guest_gpu_fraction;
    std::array<double, MaxCores> guest_cpu_core_busy_ms{};
    WaitSummary shader_wait{};
    /// The verdict of the log's "Limited by", judged on the same numbers.
    std::string limiter{"not_running"};
};

struct RunWindow {
    double start_seconds{}; // from the start of the recording
    double end_seconds{};
    bool partial{}; // shorter than the reporting period: a start, a pause or the end of the run
    RunMetrics metrics;
};

struct RunRecording {
    RunMetrics summary;
    std::vector<RunWindow> windows;
    /// False when the recording was taken while the guest still ran or by a forced shutdown.
    bool complete = true;
};

/// Starts keeping every window the guest runs, with a histogram of the present intervals.
/// `origin_ns`, on the clock of Now(), is the zero of the window times. Call before the guest runs.
void BeginRecording(u64 origin_ns);

/// Stops recording and returns it. The window in progress is closed as a partial one. `forced`
/// marks a recording taken while the shutdown was not finished.
[[nodiscard]] RunRecording EndRecording(bool forced);

} // namespace Nx2Bottleneck
