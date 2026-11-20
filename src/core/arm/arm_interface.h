// SPDX-FileCopyrightText: 2014 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <span>
#include <string>
#include <vector>

#include "common/common_funcs.h"
#include "common/common_types.h"
#include "core/hardware_properties.h"

#include "core/hle/kernel/svc_types.h"

namespace Common {
struct PageTable;
}

namespace Kernel {
enum class DebugWatchpointType : u8;
struct DebugWatchpoint;
class KThread;
class KProcess;
} // namespace Kernel

namespace Core {
using WatchpointArray = std::array<Kernel::DebugWatchpoint, Core::Hardware::NUM_WATCHPOINTS>;

// NOTE: these values match the HaltReason enum in Dynarmic
enum class HaltReason : u64 {
    StepThread = 0x00000001,
    DataAbort = 0x00000004,
    BreakLoop = 0x02000000,
    SupervisorCall = 0x04000000,
    InstructionBreakpoint = 0x08000000,
    PrefetchAbort = 0x20000000,
};
DECLARE_ENUM_FLAG_OPERATORS(HaltReason);

enum class Architecture {
    AArch64,
    AArch32,
};

/// Whether a core is executing guest code, kept so another core can tell when code it has
/// asked to invalidate can no longer be running on this one.
struct CoreRunState {
    std::atomic<u64> entered{0}; ///< Run slices started.
    std::atomic<u64> left{0};    ///< Run slices finished; equal to `entered` when idle.
    std::atomic<bool> parked{false}; ///< Waiting in a host call: runs no guest code meanwhile.
};

/// Marks one run slice on the calling host thread: which core it is and which guest thread it
/// runs. A host thread runs one guest thread per slice.
class ScopedRun {
public:
    explicit ScopedRun(CoreRunState& state, u64 guest_thread_id) noexcept;
    ~ScopedRun();
    YUZU_NON_COPYABLE(ScopedRun);
    YUZU_NON_MOVEABLE(ScopedRun);

private:
    CoreRunState& m_state;
    CoreRunState* m_previous;
    u64 m_previous_thread;
};

/// The run state of the core the calling host thread is currently running guest code on.
CoreRunState* CurrentRun() noexcept;

/// The id of the guest thread of that slice; 0 outside one.
u64 CurrentGuestThreadId() noexcept;

/// What a core's interface wants of its idle time (ArmInterface::PrewarmIdle).
enum class PrewarmState : u8 {
    Finished, ///< Nothing, now or later: the core just waits
    Poll,     ///< Something may come up: ask again after a short wait
};

/// Generic ARMv8 CPU interface
class ArmInterface {
public:
    YUZU_NON_COPYABLE(ArmInterface);
    YUZU_NON_MOVEABLE(ArmInterface);

    explicit ArmInterface(bool uses_wall_clock) : m_uses_wall_clock{uses_wall_clock} {}
    virtual ~ArmInterface() = default;

    // Perform any backend-specific initialization.
    virtual void Initialize() {}

    // The core this interface belongs to has nothing to run (PhysicalCore::Idle, on the core's
    // host thread, no guest code executing): translate code ahead of the guest until `stop` is set
    // or nothing can be done now. Returns at once when the backend has nothing to do.
    virtual PrewarmState PrewarmIdle(const std::atomic<bool>& stop) {
        return PrewarmState::Finished;
    }
    /// Controlled shutdown only, after the core's host thread has joined.
    virtual void LogPrewarmOutcomes() const {}

    // Runs the CPU until an event happens.
    virtual HaltReason RunThread(Kernel::KThread* thread) = 0;

    // Runs the CPU for one instruction or until an event happens.
    virtual HaltReason StepThread(Kernel::KThread* thread) = 0;

    // Admits a backend-specific mechanism to lock the thread context.
    virtual void LockThread(Kernel::KThread* thread) {}
    virtual void UnlockThread(Kernel::KThread* thread) {}

    // Clear the entire instruction cache for this CPU.
    virtual void ClearInstructionCache() = 0;

    // Clear a range of the instruction cache for this CPU.
    virtual void InvalidateCacheRange(u64 addr, std::size_t size) = 0;

    // Get the current architecture.
    // This returns AArch64 when PSTATE.nRW == 0 and AArch32 when PSTATE.nRW == 1.
    virtual Architecture GetArchitecture() const = 0;

    // Context accessors.
    // These should not be called if the CPU is running.
    virtual void GetContext(Kernel::Svc::ThreadContext& ctx) const = 0;
    virtual void SetContext(const Kernel::Svc::ThreadContext& ctx) = 0;
    virtual void SetTpidrroEl0(u64 value) = 0;

    virtual void GetSvcArguments(std::span<uint64_t, 8> args) const = 0;
    virtual void SetSvcArguments(std::span<const uint64_t, 8> args) = 0;
    virtual u32 GetSvcNumber() const = 0;

    void SetWatchpointArray(const WatchpointArray* watchpoints) {
        m_watchpoints = watchpoints;
    }

    // Signal an interrupt for execution to halt as soon as possible.
    // It is safe to call this if the CPU is not running.
    virtual void SignalInterrupt(Kernel::KThread* thread) = 0;

    // Stack trace generation.
    void LogBacktrace(Kernel::KProcess* process) const;

    // Debug functionality.
    virtual const Kernel::DebugWatchpoint* HaltedWatchpoint() const = 0;
    virtual void RewindBreakpointInstruction() = 0;

    // Run slices are recorded only by a JIT that has instruction probes (the NVN census); see
    // Core::InvalidateInstructionCacheRangeAndWait.
    CoreRunState& RunState() noexcept {
        return m_run_state;
    }
    bool TracksRuns() const noexcept {
        return m_tracks_runs;
    }

protected:
    const Kernel::DebugWatchpoint* MatchingWatchpoint(
        u64 addr, u64 size, Kernel::DebugWatchpointType access_type) const;

protected:
    const WatchpointArray* m_watchpoints{};
    bool m_uses_wall_clock{};
    bool m_tracks_runs{};

private:
    CoreRunState m_run_state;
};

} // namespace Core
