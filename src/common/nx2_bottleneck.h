// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <string>

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

} // namespace Nx2Bottleneck
