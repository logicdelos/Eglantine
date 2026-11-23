// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>

#include "core/arm/arm_interface.h"
#include "core/arm/guest_sample.h"

namespace Kernel {
class KernelCore;
} // namespace Kernel

namespace Core {
class ExclusiveMonitor;
class System;
} // namespace Core

namespace Kernel {

extern const std::chrono::nanoseconds YieldIdleDuration;
extern const bool YieldIdleEnabled;

class PhysicalCore {
public:
    PhysicalCore(KernelCore& kernel, std::size_t core_index);
    ~PhysicalCore();

    YUZU_NON_COPYABLE(PhysicalCore);
    YUZU_NON_MOVEABLE(PhysicalCore);

    // Execute guest code running on the given thread.
    void RunThread(KernelCore& kernel, KThread* thread);

    // Copy context from thread to current core.
    void LoadContext(const KThread* thread);
    void LoadSvcArguments(const KProcess& process, std::span<const uint64_t, 8> args);

    // Copy context from current core to thread.
    void SaveContext(KThread* thread) const;
    void SaveSvcArguments(KProcess& process, std::span<uint64_t, 8> args) const;

    // Copy floating point status registers to the target thread.
    void CloneFpuStatus(KThread* dst) const;

    // Log backtrace of current processor state.
    void LogBacktrace(KernelCore& kernel);

    // Wait for an interrupt.
    void Idle(KernelCore& kernel);

    // Opt-in yield-loop backoff, sharing Idle's interrupt condition variable.
    void YieldIdle(KernelCore& kernel, std::chrono::nanoseconds limit);
    void WakeYieldIdle();

    // Interrupt this core.
    void Interrupt();

    // Clear this core's interrupt.
    void ClearInterrupt();

    // Check if this core is interrupted.
    bool IsInterrupted() const;

    std::size_t CoreIndex() const {
        return m_core_index;
    }

    // Where this core's guest code last stopped, and the guest thread it ran (the no-progress
    // probe's source). Safe from any host thread at any time: the core publishes it itself when
    // RunThread's CPU slice ends, so it lags a slice that is still running.
    bool ReadGuestSample(u64& pc, u64& thread_id) const noexcept {
        return m_guest_sample.Read(pc, thread_id);
    }

private:
    const std::size_t m_core_index;
    std::mutex m_guard;
    std::condition_variable m_on_interrupt;
    Core::ArmInterface* m_arm_interface{};
    KThread* m_current_thread{};
    Core::GuestSample m_guest_sample;
    std::atomic<bool> m_is_interrupted{};
    std::atomic<bool> m_yield_idle{};
    bool m_yield_woken{};
    bool m_is_single_core{};
};

} // namespace Kernel
