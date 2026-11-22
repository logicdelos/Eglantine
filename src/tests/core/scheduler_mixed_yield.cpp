// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Guest-level regression for SleepThread yield modes on one core. A priority 20 caller and a
// strictly lower priority peer (30) are both pinned to core 0. The caller issues one yield mode
// and then SleepThread(-2) (YieldType::ToAnyThread), and the guest itself records whether the peer
// has run by the time each SVC returns:
//   * SleepThread(0) / SleepThread(-1) never run a strictly lower priority thread -> peer == 0.
//   * SleepThread(-2) moves the caller to core -1 so the lower priority peer runs on core 0 and,
//     the caller being pinned to core 0 too, finishes before the caller resumes -> peer == 1.
// A yield mode must not make a later, different mode a no-op. Nothing here depends on timing: both
// threads are confined to one core, the peer cannot run before the caller yields, and the peer
// only exits after publishing its flag.

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>
#include <thread>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "common/nx2_bottleneck.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/cpu_manager.h"
#include "core/hle/kernel/code_set.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_resource_limit.h"
#include "core/hle/kernel/k_scoped_resource_reservation.h"
#include "core/hle/kernel/k_shared_memory.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/svc_types.h"
#include "core/memory.h"
#include "tests/core/multicore_kernel_runtime.h"

namespace {

constexpr u64 kCodeAddr = 0x80000000ULL;
constexpr u64 kDataAddr = 0x80001000ULL;

constexpr u64 kPeerEntry = kCodeAddr + 0x100;

// Data block, addressed by the guest through x19 = kDataAddr.
constexpr u32 kGoOff = 0x00;       // host -> caller: start yielding
constexpr u32 kPeerRanOff = 0x08;  // peer -> everyone: peer has executed
constexpr u32 kResultOff = 0x10;   // caller: peer flag observed after yield i, at +4*i
constexpr u32 kDoneOff = 0x20;     // caller: all yields done
constexpr u32 kArmedOff = 0x28;    // caller: running and spinning on go
constexpr u64 kPeerStackTop = kDataAddr + 0xA00;

constexpr s32 kCallerPriority = 20;
constexpr s32 kPeerPriority = 30;

// Original AArch64 encodings.
constexpr u32 kSvcExitThread = 0xD4000141;  // svc #0xa
constexpr u32 kSvcSleepThread = 0xD4000161; // svc #0xb
constexpr u32 kMovX19CodeBase = 0xD2B00013; // movz x19, #0x8000, lsl #16
constexpr u32 kAddX19DataPage = 0x91400673; // add x19, x19, #1, lsl #12
constexpr u32 kLdrW5X19 = 0xB9400265;       // ldr w5, [x19]
constexpr u32 kCbzW5Back1 = 0x34FFFFE5;     // cbz w5, #-4
constexpr u32 kMovW4One = 0x52800024;       // movz w4, #1

constexpr u32 StrW4X19(u32 off) { // str w4, [x19, #off]
    return 0xB9000264U | ((off / 4) << 10);
}
constexpr u32 StrW6X19(u32 off) { // str w6, [x19, #off]
    return 0xB9000266U | ((off / 4) << 10);
}
constexpr u32 LdrW6X19(u32 off) { // ldr w6, [x19, #off]
    return 0xB9400266U | ((off / 4) << 10);
}
constexpr u32 MovX0Yield(s64 mode) { // mode is 0, -1 or -2
    return mode == 0 ? 0xD2800000U                              // movz x0, #0
                     : 0x92800000U | (static_cast<u32>(-mode - 1) << 5); // movn x0, #(-mode - 1)
}

Kernel::CodeSet BuildCodeSet() {
    Kernel::CodeSet code_set;
    code_set.memory.resize(0x2000, 0);
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

Kernel::CodeSet BuildCodeSet(s64 first_mode) {
    auto code_set = BuildCodeSet();

    // The process main thread is the caller. Keep it alive after reporting so that the host
    // can suspend the process before tearing it down, including on a failed assertion.
    const std::array caller{
        kMovX19CodeBase, kAddX19DataPage, kMovW4One, StrW4X19(kArmedOff),
        kLdrW5X19, kCbzW5Back1,
        MovX0Yield(first_mode), kSvcSleepThread, LdrW6X19(kPeerRanOff), StrW6X19(kResultOff),
        MovX0Yield(-2), kSvcSleepThread, LdrW6X19(kPeerRanOff), StrW6X19(kResultOff + 4),
        kMovW4One, StrW4X19(kDoneOff), 0x14000000U, // b .
    };
    constexpr std::array peer{kMovW4One, StrW4X19(kPeerRanOff), kSvcExitThread};
    std::memcpy(code_set.memory.data(), caller.data(), sizeof(caller));
    std::memcpy(code_set.memory.data() + (kPeerEntry - kCodeAddr), peer.data(), sizeof(peer));

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

Kernel::KProcess* CreateYieldProcess(Core::System& system, Kernel::CodeSet code,
                                     bool application = true) {
    auto& kernel = system.Kernel();
    auto* process = Kernel::KProcess::Create(kernel);
    REQUIRE(process != nullptr);
    Kernel::KProcess::Register(kernel, process);
    auto close_process = SCOPE_GUARD {
        process->Close(kernel);
    };

    const auto phys_mem_size =
        kernel.MemoryManager().GetSize(Kernel::KMemoryManager::Pool::Application);
    auto* res_limit = Kernel::CreateResourceLimitForProcess(system, phys_mem_size);
    REQUIRE(res_limit != nullptr);
    SCOPE_EXIT {
        res_limit->Close(kernel);
    };

    Kernel::Svc::CreateProcessParameter params{};
    params.name = {'y', 'i', 'e', 'l', 'd', '_', 'm', 'i', 'x', 0, 0, 0};
    params.program_id = application ? 0x1001 : 0x1002;
    params.code_address = kCodeAddr;
    params.code_num_pages = 2;
    params.flags = Kernel::Svc::CreateProcessFlag::AddressSpace64Bit |
                   Kernel::Svc::CreateProcessFlag::Is64Bit |
                   (application ? Kernel::Svc::CreateProcessFlag::IsApplication
                                : Kernel::Svc::CreateProcessFlag{});
    const auto caps = BuildProcessCapabilities();
    const auto result = process->Initialize(kernel, params, caps, res_limit,
                                            Kernel::KMemoryManager::Pool::Application, 0);
    CAPTURE(result.GetInnerValue());
    REQUIRE(result.IsSuccess());
    process->LoadModule(kernel, std::move(code), kCodeAddr);
    process->InitializeInterfaces(kernel);
    kernel.AppendNewProcess(process);
    if (application) {
        kernel.SetApplicationProcess(process);
    }
    close_process.Cancel();
    return process;
}

void StartCoreZeroMain(Kernel::KernelCore& kernel, Kernel::KProcess& process) {
    REQUIRE(process.Run(kernel, kCallerPriority, 0x1000).IsSuccess());
    REQUIRE_FALSE(process.GetThreadList().empty());
    REQUIRE(process.GetThreadList().front().SetCoreMask(kernel, 0, 1).IsSuccess());
}

void StartYieldWorker(Core::System& system, Kernel::KProcess& process, s32 core, u32 slot) {
    auto& kernel = system.Kernel();
    Kernel::KScopedResourceReservation reservation{
        kernel, &process, Kernel::LimitableResource::ThreadCountMax};
    REQUIRE(reservation.Succeeded());
    auto* worker = Kernel::KThread::Create(kernel);
    REQUIRE(worker != nullptr);
    SCOPE_EXIT {
        worker->Close(kernel);
    };
    const auto stack_top = kPeerStackTop + 0x100 * core;
    REQUIRE(Kernel::KThread::InitializeUserThread(system, worker, kCodeAddr, slot, stack_top,
                                                  kCallerPriority, core, &process).IsSuccess());
    reservation.Commit();
    Kernel::KThread::Register(kernel, worker);
    REQUIRE(worker->SetCoreMask(kernel, core, u64{1} << core).IsSuccess());
    REQUIRE(worker->Run(kernel).IsSuccess());
}

constexpr u32 kWarmYields = 4096;
constexpr u32 kHotYields = 65535;
constexpr u32 kSlotStride = 64;

constexpr u32 MovW22Count(u32 count) {
    return 0x52800016U | (count << 5); // movz w22, #count
}

Kernel::CodeSet BuildBenchmarkCode() {
    auto code = BuildCodeSet();
    // x20 points to the common phase gate. x19 points to this worker's status slot.
    // Warmup and measurement execute the very same SVC loop, including its guest PC.
    constexpr std::array worker{
        kMovX19CodeBase, kAddX19DataPage,
        0xAA1303F4U,                  // mov x20, x19
        0x8B000273U,                  // add x19, x19, x0 (slot offset)
        0x52800035U,                  // mov w21, #1 (local phase)
        0xB9000275U | ((kArmedOff / 4) << 10), // str w21, [x19, #armed]
        0xB9400285U,                  // ldr w5, [x20] (phase gate)
        0x6B1500BFU,                  // cmp w5, w21
        0x54FFFFC1U,                  // b.ne back to ldr
        MovW22Count(kWarmYields),
        0x710006BFU,                  // cmp w21, #1
        0x54000040U,                  // b.eq to loop
        MovW22Count(kHotYields),
        MovX0Yield(0), kSvcSleepThread,
        0x710006D6U,                  // subs w22, w22, #1
        0x54FFFFA1U,                  // b.ne back to mov x0
        0x71000ABFU,                  // cmp w21, #2
        0x54000060U,                  // b.eq to done store
        0x110006B5U,                  // add w21, w21, #1
        0x17FFFFF1U,                  // b back to armed store
        kMovW4One, StrW4X19(kDoneOff),
        0x14000000U,                  // b .
    };
    std::memcpy(code.memory.data(), worker.data(), sizeof(worker));
    return code;
}

void RunYieldBenchmark(s32 cores) {
    CAPTURE(cores);
    RunInFreshKernelThread([=] {
        MulticoreKernelRuntime runtime;
        auto& kernel = runtime.system.Kernel();
        auto* process = CreateYieldProcess(runtime.system, BuildBenchmarkCode());
        SCOPE_EXIT {
            process->Close(kernel);
        };
        StartCoreZeroMain(kernel, *process);
        for (s32 core = 1; core < cores; ++core) {
            StartYieldWorker(runtime.system, *process, core, core * kSlotStride);
        }

        auto& mem = process->GetMemory();
        const auto all_workers = [&](u32 offset, u32 value) {
            for (s32 core = 0; core < cores; ++core) {
                if (mem.Read32(kDataAddr + core * kSlotStride + offset) != value) {
                    return false;
                }
            }
            return true;
        };
        REQUIRE(WaitUntil([&] { return all_workers(kArmedOff, 1); }));
        mem.Write32(kDataAddr + kGoOff, 1);
        REQUIRE(WaitUntil([&] { return all_workers(kArmedOff, 2); }, std::chrono::seconds{15}));

        using Clock = std::chrono::steady_clock;
        const auto start = Clock::now();
        mem.Write32(kDataAddr + kGoOff, 2);
        // Do not quantize the hot-phase measurement with the setup poll's timed sleep.
        bool completed;
        while (!(completed = all_workers(kDoneOff, 1)) &&
               Clock::now() - start < std::chrono::seconds{30}) {
            std::this_thread::yield();
        }
        const auto elapsed = Clock::now() - start;
        REQUIRE(completed);
        const auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
        const auto calls = cores * kHotYields;
        std::printf("sched_yield_bench mode=0 cores=%d warmup_per_core=%u yields_per_core=%u "
                    "total_yields=%u elapsed_ns=%lld aggregate_yields_per_second=%.3f\n",
                    cores, kWarmYields, kHotYields, calls, static_cast<long long>(elapsed_ns),
                    static_cast<double>(calls) / std::chrono::duration<double>(elapsed).count());
    });
}

std::array<u32, 2> RunYieldSequence(s64 first_mode) {
    std::array<u32, 2> observed{};
    RunInFreshKernelThread([&] {
        MulticoreKernelRuntime runtime;
        Core::System& system = runtime.system;
        auto& kernel = system.Kernel();

        auto* process = CreateYieldProcess(system, BuildCodeSet(first_mode));
        SCOPE_EXIT {
            process->Close(kernel);
        };

        auto& mem = process->GetMemory();
        StartCoreZeroMain(kernel, *process);
        REQUIRE(WaitUntil([&] { return mem.Read32(kDataAddr + kArmedOff) == 1; }));

        Kernel::KScopedResourceReservation reservation{
            kernel, process, Kernel::LimitableResource::ThreadCountMax};
        REQUIRE(reservation.Succeeded());
        auto* peer = Kernel::KThread::Create(kernel);
        REQUIRE(peer != nullptr);
        REQUIRE(Kernel::KThread::InitializeUserThread(system, peer, kPeerEntry, 0, kPeerStackTop,
                                                      kPeerPriority, 0, process).IsSuccess());
        reservation.Commit();
        Kernel::KThread::Register(kernel, peer);
        SCOPE_EXIT {
            peer->Close(kernel);
        };
        REQUIRE(peer->SetCoreMask(kernel, 0, 1).IsSuccess());
        peer->GetContext().r[19] = kDataAddr;
        REQUIRE(peer->Run(kernel).IsSuccess());

        // Control: the peer is runnable but starved while the caller holds core 0.
        REQUIRE(mem.Read32(kDataAddr + kPeerRanOff) == 0);

        mem.Write32(kDataAddr + kGoOff, 1);
        REQUIRE(WaitUntil([&] { return mem.Read32(kDataAddr + kDoneOff) == 1; }));
        observed = {mem.Read32(kDataAddr + kResultOff), mem.Read32(kDataAddr + kResultOff + 4)};
    });
    return observed;
}

void CheckNewCrossProcessPeer(s64 mode) {
    RunInFreshKernelThread([mode] {
        MulticoreKernelRuntime runtime;
        auto& system = runtime.system;
        auto& kernel = system.Kernel();
        constexpr u64 shared_addr = kDataAddr + 0x1000;
        constexpr u32 add_shared_page = 0x91400A73; // add x19, x19, #2, lsl #12
        const std::array caller_guest{
            kMovX19CodeBase, add_shared_page,
            MovX0Yield(mode), kSvcSleepThread,
            MovX0Yield(mode), kSvcSleepThread,
            kMovW4One, StrW4X19(kArmedOff),
            kLdrW5X19, kCbzW5Back1,
            MovX0Yield(mode), kSvcSleepThread, LdrW6X19(kPeerRanOff), StrW6X19(kResultOff),
            kMovW4One, StrW4X19(kDoneOff), 0x14000000U, // b .
        };
        constexpr std::array peer_guest{
            kMovX19CodeBase, add_shared_page, kMovW4One, StrW4X19(kPeerRanOff),
            // Block so the yielded caller can resume, including if -2 left it unassigned.
            0xD2A40000U, kSvcSleepThread, 0x14000000U, // movz x0, #0x2000, lsl #16; svc; b .
        };
        const auto make_code = [](const auto& guest) {
            auto code = BuildCodeSet();
            std::memcpy(code.memory.data(), guest.data(), sizeof(guest));
            return code;
        };
        auto* caller = CreateYieldProcess(system, make_code(caller_guest));
        SCOPE_EXIT {
            caller->Close(kernel);
        };
        auto* peer = CreateYieldProcess(system, make_code(peer_guest), false);
        SCOPE_EXIT {
            peer->Close(kernel);
        };
        REQUIRE(caller != peer);

        auto* shared = Kernel::KSharedMemory::Create(kernel);
        REQUIRE(shared != nullptr);
        SCOPE_EXIT {
            shared->Close(kernel);
        };
        constexpr auto rw = Kernel::Svc::MemoryPermission::ReadWrite;
        REQUIRE(shared->Initialize(kernel, system.DeviceMemory(), caller, rw, rw, 0x1000)
                    .IsSuccess());
        REQUIRE(shared->Map(*caller, shared_addr, 0x1000, rw).IsSuccess());
        REQUIRE(shared->Map(*peer, shared_addr, 0x1000, rw).IsSuccess());

        auto& mem = caller->GetMemory();
        StartCoreZeroMain(kernel, *caller);
        REQUIRE(WaitUntil([&] { return mem.Read32(shared_addr + kArmedOff) == 1; }));
        // At priority 20 there is no periodic equal-priority rotation. The newly runnable
        // peer is behind the caller on core 0 and runs only when the caller yields to it.
        StartCoreZeroMain(kernel, *peer);
        REQUIRE(mem.Read32(shared_addr + kPeerRanOff) == 0);
        mem.Write32(shared_addr + kGoOff, 1);
        REQUIRE(WaitUntil([&] { return mem.Read32(shared_addr + kDoneOff) == 1; }));
        CHECK(mem.Read32(shared_addr + kResultOff) == 1);
    });
}

} // namespace

TEST_CASE("SleepThread(0) must not turn a later SleepThread(-2) into a no-op",
          "[core][sched_yield]") {
    const auto peer_ran = RunYieldSequence(0);
    CAPTURE(peer_ran);
    // Yield without core migration never hands the core to a strictly lower priority thread.
    CHECK(peer_ran[0] == 0);
    // Yield to any thread does, and the peer completes before the pinned caller resumes.
    CHECK(peer_ran[1] == 1);
}

TEST_CASE("SleepThread(-1) must not turn a later SleepThread(-2) into a no-op",
          "[core][sched_yield]") {
    const auto peer_ran = RunYieldSequence(-1);
    CAPTURE(peer_ran);
    CHECK(peer_ran[0] == 0);
    CHECK(peer_ran[1] == 1);
}

TEST_CASE("A warmed empty yield runs a new equal-priority peer in another process",
          "[core][sched_yield]") {
    SECTION("without migration") { CheckNewCrossProcessPeer(0); }
    SECTION("with migration") { CheckNewCrossProcessPeer(-1); }
    SECTION("to any thread") { CheckNewCrossProcessPeer(-2); }
}

TEST_CASE("SleepThread(0) hot loop on one pinned guest core", "[.][sched_yield_bench]") {
    RunYieldBenchmark(1);
}

TEST_CASE("SleepThread(0) hot loop on four pinned guest cores", "[.][sched_yield_bench]") {
    RunYieldBenchmark(4);
}
