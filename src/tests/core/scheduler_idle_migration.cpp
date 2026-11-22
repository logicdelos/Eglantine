// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Regression for the scheduler's idle-core selection (KScheduler::UpdateHighestPriorityThreadsImpl)
// on a real multicore kernel. Core 5 is idle in every scenario and the outcome is read from each
// thread's active core, which the selection changes synchronously while the waking thread's
// scheduler lock is released; there is no sleep, deadline or race between the check and the
// selection. The only host waits are for setup/teardown of the guest cores.
//
// Threads:
//   * Case 1 holds cores 0 and 1 with ownerless single-core service threads (T0 p0 and T1 p10).
//     They spin on a flag that the fixture releases before it tears the kernel down.
//   * Threads that need an affinity wider than one core are process threads, because
//     KThread::SetCoreMask needs an owner. A process thread asked for priority 0 is created at
//     the lowest number a capability can grant and lowered to 0 while it is still stopped (process
//     capabilities cannot grant 0..3), so the queues never see the change.
//   * Process threads that never run are terminated by normal kernel shutdown.
//
// Cases:
//   1. A (p20 on {0,5}) is runnable behind the protected top T0 (p0 on {0}); B (p21 on {1,5}) wakes
//      behind the pinned top T1 (p10 on {1}). B is eligible and must be migrated to core 5. A's
//      core has a protected top, so A must stay.
//   2. Negative control: the protected top P (p0 on {0,5}) is itself the only suggested thread for
//      core 5 and its core has a pinned replacement R. Nothing else is allowed to move, so core 5
//      must stay idle, P must keep core 0 and R must not move.
//   3. Positive control for 2: with the same shape but an unprotected top S (p20 on {0,5}) the
//      fallback scan still hands S to the idle core and lets R run on core 0.

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <exception>
#include <functional>
#include <initializer_list>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/nx2_bottleneck.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/cpu_manager.h"
#include "core/hle/kernel/code_set.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_resource_limit.h"
#include "core/hle/kernel/k_scoped_resource_reservation.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/svc_types.h"
#include "tests/core/multicore_kernel_runtime.h"

namespace {

constexpr u64 kCodeAddr = 0x80000000ULL;
constexpr u64 kDataAddr = 0x80001000ULL;
constexpr u64 kUserEntry = kCodeAddr + 0x100;
constexpr u64 kUserStackTop = kDataAddr + 0xA00;

constexpr s32 kMainPriority = 20;
// The lowest priority number a process capability can grant.
constexpr s32 kLowestUserNumber = 16;
constexpr s32 kBackgroundCore = 4;
constexpr s32 kIdleCore = 5;

constexpr u32 kBranchToSelf = 0x14000000; // b .

constexpr u64 Mask(std::initializer_list<s32> cores) {
    u64 mask = 0;
    for (const s32 core : cores) {
        mask |= 1ULL << core;
    }
    return mask;
}

Kernel::CodeSet BuildCodeSet() {
    Kernel::CodeSet code_set;
    code_set.memory.resize(0x2000, 0);
    // Every guest thread, including the process main thread, spins in place; none of them ever
    // makes a decision that the scenarios observe.
    std::memcpy(code_set.memory.data(), &kBranchToSelf, sizeof(kBranchToSelf));
    std::memcpy(code_set.memory.data() + (kUserEntry - kCodeAddr), &kBranchToSelf,
                sizeof(kBranchToSelf));

    code_set.CodeSegment().addr = 0;
    code_set.CodeSegment().offset = 0;
    code_set.CodeSegment().size = 0x1000;
    code_set.RODataSegment().addr = 0x1000;
    code_set.RODataSegment().offset = 0x1000;
    code_set.RODataSegment().size = 0;
    code_set.DataSegment().addr = 0x1000;
    code_set.DataSegment().offset = 0x1000;
    code_set.DataSegment().size = 0x1000;
    return code_set;
}

constexpr std::array<u32, 6> BuildProcessCapabilities() {
    // CorePriority: cores 0..5, priorities 16..63.
    const u32 core_prio = 0x7U | (63U << 4) | (16U << 10) | (0U << 16) | (5U << 24);
    // Syscall masks for indices 0, 1, 2 (SVCs 0x00..0x47).
    const u32 svc0 = 0xFU | (0xFFFFFFU << 5) | (0U << 29);
    const u32 svc1 = 0xFU | (0xFFFFFFU << 5) | (1U << 29);
    const u32 svc2 = 0xFU | (0xFFFFFFU << 5) | (2U << 29);
    const u32 kver = 0x3FFFU | (0U << 15) | (16U << 19); // KernelVersion 16.0
    const u32 htab = 0x7FFFU | (64U << 16);              // HandleTable size 64
    return {core_prio, svc0, svc1, svc2, kver, htab};
}

template <typename Fn>
void RunInFreshKernelThread(Fn&& fn) {
    std::exception_ptr err;
    std::jthread worker([&] {
        try {
            fn();
        } catch (...) {
            err = std::current_exception();
        }
    });
    worker.join();
    if (err) {
        std::rethrow_exception(err);
    }
}

template <typename Pred>
bool WaitUntil(Pred&& pred, std::chrono::milliseconds timeout = std::chrono::milliseconds(3000)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!pred()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    return true;
}

// One multicore kernel with a process whose main thread is parked on core 4. Core 5 starts idle.
// Threads are created stopped; a scenario starts them with Start() in the order it needs.
class Scene {
public:
    Scene() = default;

    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;

    // Kept apart from the constructor so that a failed REQUIRE still runs the destructor, which
    // releases the spinning service threads before the kernel is torn down.
    void Bringup() {
        auto& system = runtime.system;
        auto& kernel = system.Kernel();
        REQUIRE(system.IsMulticore());
        REQUIRE(kernel.IsMulticore());

        process = Kernel::KProcess::Create(kernel);
        REQUIRE(process != nullptr);
        Kernel::KProcess::Register(kernel, process);
        kernel.AppendNewProcess(process);

        const auto phys_mem_size =
            kernel.MemoryManager().GetSize(Kernel::KMemoryManager::Pool::Application);
        res_limit = Kernel::CreateResourceLimitForProcess(system, phys_mem_size);
        REQUIRE(res_limit != nullptr);

        Kernel::Svc::CreateProcessParameter params{};
        constexpr std::array<char, 12> kName{'i', 'd', 'l', 'e', '_', 'm', 'i', 'g', 0, 0, 0, 0};
        params.name = kName;
        params.program_id = 0x1002;
        params.code_address = kCodeAddr;
        params.code_num_pages = 2;
        params.flags = Kernel::Svc::CreateProcessFlag::AddressSpace64Bit |
                       Kernel::Svc::CreateProcessFlag::Is64Bit |
                       Kernel::Svc::CreateProcessFlag::IsApplication;
        const auto caps = BuildProcessCapabilities();
        const auto init_res = process->Initialize(kernel, params, caps, res_limit,
                                                  Kernel::KMemoryManager::Pool::Application, 0);
        CAPTURE(init_res.GetInnerValue());
        REQUIRE(init_res.IsSuccess());
        process->LoadModule(kernel, BuildCodeSet(), kCodeAddr);
        process->InitializeInterfaces(kernel);
        kernel.SetApplicationProcess(process);

        // The main thread only exists because a process cannot run without one. Park it on core 4
        // so that it takes no part in the core 0/1/5 scenarios.
        REQUIRE(process->Run(kernel, kMainPriority, 0x1000).IsSuccess());
        REQUIRE_FALSE(process->GetThreadList().empty());
        auto& main_thread = process->GetThreadList().front();
        REQUIRE(main_thread.SetCoreMask(kernel, kBackgroundCore, Mask({kBackgroundCore}))
                    .IsSuccess());
        REQUIRE(main_thread.GetActiveCore() == kBackgroundCore);
    }

    ~Scene() {
        // Let the ownerless spinners exit; a core cannot reschedule away from one that never
        // returns to the scheduler.
        release.store(true, std::memory_order_release);
        for (auto* thread : service_threads) {
            WaitUntil([thread] {
                const auto state = thread->GetState();
                return state == Kernel::ThreadState::Initialized ||
                       state == Kernel::ThreadState::Terminated;
            });
        }
        auto& kernel = runtime.system.Kernel();
        for (auto* thread : threads) {
            thread->Close(kernel);
        }
        if (res_limit != nullptr) {
            res_limit->Close(kernel);
        }
        if (process != nullptr) {
            process->Close(kernel);
        }
    }

    // A process thread. A priority below the lowest a capability can grant (the protected 0..3
    // range) is applied while the thread is still stopped, so the queues never see it change.
    Kernel::KThread* MakeProcessThread(s32 priority, s32 ideal_core, u64 mask) {
        auto& system = runtime.system;
        auto& kernel = system.Kernel();
        Kernel::KScopedResourceReservation reservation{
            kernel, process, Kernel::LimitableResource::ThreadCountMax};
        REQUIRE(reservation.Succeeded());
        auto* thread = Kernel::KThread::Create(kernel);
        REQUIRE(thread != nullptr);
        const s32 initial_priority = priority < kLowestUserNumber ? kLowestUserNumber : priority;
        REQUIRE(Kernel::KThread::InitializeUserThread(system, thread, kUserEntry, 0,
                                                      kUserStackTop, initial_priority, ideal_core,
                                                      process)
                    .IsSuccess());
        reservation.Commit();
        Kernel::KThread::Register(kernel, thread);
        threads.push_back(thread);
        if (priority != initial_priority) {
            thread->SetPriority(priority);
        }
        REQUIRE(thread->SetCoreMask(kernel, ideal_core, mask).IsSuccess());
        REQUIRE(thread->GetPriority() == priority);
        REQUIRE(thread->GetActiveCore() == ideal_core);
        return thread;
    }

    // An ownerless service thread: it has one core (a pinned thread) and spins until teardown.
    Kernel::KThread* MakePinnedServiceThread(s32 priority, s32 core) {
        auto& system = runtime.system;
        auto& kernel = system.Kernel();
        auto* thread = Kernel::KThread::Create(kernel);
        REQUIRE(thread != nullptr);
        REQUIRE(Kernel::KThread::InitializeServiceThread(
                    system, thread,
                    [this] {
                        while (!release.load(std::memory_order_acquire)) {
                            std::this_thread::yield();
                        }
                    },
                    priority, core, nullptr)
                    .IsSuccess());
        Kernel::KThread::Register(kernel, thread);
        threads.push_back(thread);
        service_threads.push_back(thread);
        REQUIRE(thread->GetOwnerProcess() == nullptr);
        REQUIRE(thread->GetPriority() == priority);
        REQUIRE(thread->GetActiveCore() == core);
        return thread;
    }

    // Makes the thread runnable. The scheduler selection for the new state has been performed
    // by the time this returns.
    void Start(Kernel::KThread* thread) {
        REQUIRE(thread->Run(runtime.system.Kernel()).IsSuccess());
    }

    s32 BackgroundCore() {
        return process->GetThreadList().front().GetActiveCore();
    }

private:
    std::atomic<bool> release{false};
    MulticoreKernelRuntime runtime;
    Kernel::KProcess* process{};
    Kernel::KResourceLimit* res_limit{};
    std::vector<Kernel::KThread*> threads;
    std::vector<Kernel::KThread*> service_threads;
};

struct IdleCoreResult {
    s32 background{-1};
    // Active core of each named thread right before and right after B starts.
    s32 first_before{-1};
    s32 woken_before{-1};
    s32 first{-1};
    s32 woken{-1};
    s32 top{-1};
    s32 pinned_top{-1};
};

// Case 1.
IdleCoreResult RunEligibleWakeBehindRestrictedSibling() {
    IdleCoreResult out;
    RunInFreshKernelThread([&] {
        Scene scene;
        scene.Bringup();

        auto* protected_top = scene.MakePinnedServiceThread(0, 0);          // T0: p0 on {0}
        auto* pinned_top = scene.MakePinnedServiceThread(10, 1);            // T1: p10 on {1}
        auto* restricted = scene.MakeProcessThread(20, 0, Mask({0, 5}));    // A: p20 on {0,5}
        auto* waking = scene.MakeProcessThread(21, 1, Mask({1, 5}));        // B: p21 on {1,5}

        scene.Start(protected_top);
        scene.Start(pinned_top);
        scene.Start(restricted);

        // Before the wake: A is runnable behind the protected top and nothing sits on core 5.
        out.first_before = restricted->GetActiveCore();
        out.woken_before = waking->GetActiveCore();

        scene.Start(waking);

        out.background = scene.BackgroundCore();
        out.first = restricted->GetActiveCore();
        out.woken = waking->GetActiveCore();
        out.top = protected_top->GetActiveCore();
        out.pinned_top = pinned_top->GetActiveCore();
    });
    return out;
}

struct FallbackResult {
    s32 background{-1};
    s32 top_before{-1};
    s32 top{-1};
    s32 replacement{-1};
};

// Cases 2 and 3: a top thread with affinity {0,5} and a pinned replacement behind it on core 0,
// the destination being the idle core 5. `top_priority` below 2 is the protected top.
FallbackResult RunTopWithPinnedReplacement(s32 top_priority) {
    FallbackResult out;
    RunInFreshKernelThread([&] {
        Scene scene;
        scene.Bringup();

        auto* top = scene.MakeProcessThread(top_priority, 0, Mask({0, 5}));
        // R: p30 on {0}. A process thread, so that process teardown reaps it even though the top
        // thread keeps core 0 and R never gets to run.
        auto* replacement = scene.MakeProcessThread(30, 0, Mask({0}));

        scene.Start(top);
        out.top_before = top->GetActiveCore();
        scene.Start(replacement);

        out.background = scene.BackgroundCore();
        out.top = top->GetActiveCore();
        out.replacement = replacement->GetActiveCore();
    });
    return out;
}

} // namespace

TEST_CASE("an eligible waking thread migrates to the idle core past a restricted sibling",
          "[core][sched_idle_migration]") {
    const auto result = RunEligibleWakeBehindRestrictedSibling();
    CAPTURE(result.background, result.first_before, result.woken_before, result.first,
            result.woken, result.top, result.pinned_top);

    // Control: before B wakes, A is behind the protected top and B is not scheduled anywhere
    // but its own core 1.
    CHECK(result.first_before == 0);
    CHECK(result.woken_before == 1);

    // B was runnable and allowed on idle core 5; its core 1 is held by an unprotected pinned top.
    CHECK(result.woken == 5);
    // A's core 0 is held by a protected priority 0 thread, so A is not migrated.
    CHECK(result.first == 0);
    // Nothing else moved.
    CHECK(result.top == 0);
    CHECK(result.pinned_top == 1);
    CHECK(result.background == 4);
}

TEST_CASE("the fallback scan never moves a protected top thread to the idle core",
          "[core][sched_idle_migration]") {
    // The protected top is the only suggested thread for core 5 and core 0 has a replacement
    // that is pinned there. No other candidate is allowed, so core 5 stays idle.
    const auto result = RunTopWithPinnedReplacement(0);
    CAPTURE(result.background, result.top_before, result.top, result.replacement);

    CHECK(result.top_before == 0);
    CHECK(result.top == 0);
    CHECK(result.replacement == 0);
    CHECK(result.background == 4);
}

TEST_CASE("the fallback scan still moves an unprotected top thread to the idle core",
          "[core][sched_idle_migration]") {
    // Same shape as the protected case with an ordinary priority: the migration is allowed, so
    // the top thread leaves core 0 to the idle core and the pinned replacement runs there.
    const auto result = RunTopWithPinnedReplacement(20);
    CAPTURE(result.background, result.top_before, result.top, result.replacement);

    CHECK(result.top_before == 0);
    CHECK(result.top == 5);
    CHECK(result.replacement == 0);
    CHECK(result.background == 4);
}
