// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>

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
    Count,
};

inline constexpr size_t MaxCores = 8;

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
