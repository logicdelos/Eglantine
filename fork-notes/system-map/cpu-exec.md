# CPU execution and scheduling

**Purpose.** This slice runs guest ARM code and decides which guest thread runs where. Each emulated core is a host thread that drives a Dynarmic JIT (or NCE on arm64 hosts) inside a fiber. The HLE kernel scheduler (`KScheduler`, a port of the Horizon scheduler) picks guest threads, and `CoreTiming` supplies timed events and the guest clock. `System` (core.cpp) starts, pauses and stops all of it. This fork emulates six cores (Switch 2 titles ask for cores 0..5; Switch 1 titles leave 4 and 5 idle).

## Files

| file | role | read depth |
|---|---|---|
| core/cpu_manager.cpp/.h | host thread per core, fiber setup, guest-thread run loops | full |
| core/core_timing.cpp/.h | event heap, host timer thread, guest tick clock | full |
| core/hle/kernel/physical_core.cpp | per-core interrupt flag, `RunThread`, `Idle`, SVC entry | full |
| core/hle/kernel/k_scheduler.cpp | scheduling decisions, switch fibers, core migration | full |
| core/hle/kernel/k_condition_variable.cpp, k_address_arbiter.cpp, k_light_lock.cpp (1-120) | guest sync primitives | full / partial |
| core/hle/kernel/k_hardware_timer.cpp, k_synchronization_object.cpp, k_thread_queue.cpp, k_scoped_scheduler_lock_and_sleep.h, k_scheduler_lock.h, k_spin_lock.*, k_interrupt_manager.cpp, global_scheduler_context.cpp | timers, waits, scheduler lock | full |
| core/hle/kernel/k_thread.cpp | thread state, dummy threads, exit/terminate | partial (112-460, 1150-1450; core-mask, waiter and pin code unread) |
| core/hle/kernel/kernel.cpp | preemption event, `SuspendEmulation`, shutdown | partial (68-333, 1040-1343) |
| core/core.cpp | `System::Impl` Run/Pause/Load/Shutdown | partial (236-723) |
| core/perf_stats.cpp/.h | frame stats, `SpeedLimiter`, CSV dump | full |
| common/thread.h, event_wait.h, windows/timer_resolution.cpp | host thread primitives | full |
| common/thread.cpp | Windows CPU sets, V-cache affinity, priority, `Event::WaitFor` | partial (458-723, 870-1207; Linux/Android parts skipped) |
| common/cpu_features.cpp/.h | `WallClock`, TSC conversion | partial |
| core/arm/dynarmic/arm_dynarmic_64.cpp/.h | JIT config and callbacks | partial (1-133, 230-550) |
| core/hle/kernel/svc.cpp (Call, 2509-2640), svc/svc_thread.cpp (SleepThread) | SVC dispatch | partial |
| arm_dynarmic_32.cpp, arm_interface.h, arm/nce/*, svc/* other files, k_server_session.cpp, k_worker_task_manager.cpp, k_priority_queue.h, common/fiber.cpp, debugger/*, nx2_sampler.cpp | | skimmed or not opened |

## Main types and ownership

- `System::Impl` owns `CpuManager`, `CoreTiming`, `KernelCore`. `KernelCore` owns six `KScheduler` and six `PhysicalCore`, the `KHardwareTimer`, and the `GlobalSchedulerContext` (holds the global scheduler lock `m_scheduler_lock`, the thread list, the woken-dummy-thread list).
- Each `KProcess` owns one `ArmInterface` per core (`ArmDynarmic64` or `32`; each holds its own `Dynarmic::A64::Jit` with a 512 MiB code cache and its own `DynarmicCallbacks64` with a one-page code cache `cached_code_page`).
- `CpuManager` owns `core_data[core]` (a `std::jthread`, the host fiber context, shutdown fiber) and the `gpu_barrier`.
- `CoreTiming` owns the event heap. Event types are `shared_ptr<EventType>` held by creators; the heap holds `weak_ptr`.
- `KThread` runs as a fiber (`GetHostContext`). A thread's `m_context_guard` mutex is taken by the scheduler of the core that switches to it and released by the same core's scheduler when it switches away.

## Threads and locking

- Host threads: `CPUCore_0..5` (multicore) or `CPUThread` (single core), each set to `Critical` priority on performance cores; one `HostTiming` thread (multicore only, also `Critical`); the UI/main thread, which calls Run/Pause/Shutdown; service and GPU threads outside this slice.
- Guest thread fibers can run on any `CPUCore_N` host thread over time; HLE services run on separate host threads and block through "dummy threads" (`m_dummy_thread_mutex`/`cv`).
- Lock order observed: `advance_lock` -> event callback -> scheduler lock (`KScopedSchedulerLock`) -> `basic_lock` is only taken briefly. `Advance` holds `advance_lock` (and `basic_lock` outside callbacks); `UnscheduleEvent` takes `basic_lock` and then `advance_lock` to wait for a running callback.
- `KSpinLock` (custom, `k_spin_lock.cpp`) spins 128 pauses then futex-waits. The scheduler lock uses it. `KHardwareTimer` has its own `KSpinLock`.
- `PhysicalCore`: `m_guard` mutex, `m_on_interrupt` condvar, atomic `m_is_interrupted`.
- `System::Impl::suspend_guard` serialises Run/Pause.

## Flow

1. `CpuManager::Initialize` spawns the core threads; each runs `RunThread`: set priority and CPU set, `Common::Fiber::ThreadToFiber`, wait on `gpu_barrier` (released by `OnGpuReady`), then switch to the scheduler's current thread fiber.
2. A guest fiber runs `MultiCoreRunGuestThread`: `while (!interrupted) PhysicalCore::RunThread(thread)`, then `KInterruptManager::HandleInterrupt` -> `RequestScheduleOnInterrupt`.
3. `PhysicalCore::RunThread` enters the context under `m_guard`, runs the JIT, then handles the halt reason: SVC -> `Svc::Call` (saves args, dispatch to `Call64`/`Call32`, optional Nx2 spike timing, `LoadSvcArguments`), breakpoints/watchpoints, step, data abort.
4. Anything that changes thread state takes `KScopedSchedulerLock`. On the final unlock, `KScheduler::UpdateHighestPriorityThreads` computes which cores need a reschedule, then `RescheduleCores` calls `PhysicalCore::Interrupt` (sets flag, notifies, `SignalInterrupt` -> `Jit::HaltExecution(BreakLoop)`). The target core runs `ScheduleImplFiber` on its switch fiber and yields to the chosen thread's fiber.
5. Idle cores run the idle thread: `PhysicalCore::Idle` spins `yield()` for `EDEN_NX2_IDLE_SPIN_US` (default 100 us for Switch 2 processes, 0 otherwise), then blocks on `m_on_interrupt`.
6. Timers: `KHardwareTimer` schedules one absolute `CoreTiming` event per earliest wakeup; the 10 ms looping preemption event (`kernel.cpp`, `InitializePreemption`) calls `GlobalSchedulerContext::PreemptThreads` (rotates queues at priority 59, 63 for core 3).
7. `HostTiming` thread: `Advance()` runs due events, then `Event::WaitFor(next - now)` (MWAITX/UMWAIT on CPUs that have them, else 100 ns `NtDelayExecution` ticks).
8. Pause/Resume: `System::Impl::Pause` -> `kernel.SuspendEmulation(true)` + `core_timing.SyncPause(true)`. Shutdown (`ShutdownMainProcess`): shutting-down flag, GPU notify, stop token, `SyncPause`, `SuspendEmulation`, close services, `ShutdownCores`, `ClearPendingEvents`, `cpu_manager.Shutdown`, `kernel.Shutdown`.

## Connections

- In: service threads and the UI call `KScopedSchedulerLock`, `KThread::*`, `kernel.SuspendEmulation`, `Timing::CreateEvent/ScheduleEvent/UnscheduleEvent` (hid, audio, vi, nvnflinger, nx2 gsv/audio device manager). GPU calls `CpuManager::OnGpuReady`. gdbstub calls `Core::InvalidateInstructionCacheRange` (all cores' `InvalidateCacheRange`).
- Out: `Svc::Call` into every SVC handler; `Memory` (guest reads and writes, via `DynarmicCallbacks64`); `Common::Fiber`; `Common::SetCurrentThreadToPerformanceCores`; `Nx2Spikes`/`Nx2Sampler` counters.

## Settings and switches

`Settings::values`: `use_multi_core`, `cpu_accuracy` and the `cpuopt_*` flags, `cpu_clock` (Boost 1734 MHz / Overclock 2040 MHz), `sync_core_speed`, `use_custom_cpu_ticks`/`cpu_ticks`, `use_speed_limit` and `SpeedLimit()`, `prefer_3d_vcache`, `record_frame_times`, `memory_layout_mode` (forced to 12 GiB for Switch 2).
Environment: `EDEN_NX2_IDLE_SPIN_US`, `EDEN_NX2_CNTFRQ` (guest counter frequency, default 31.25 MHz for Switch 2), `EDEN_NX2_INLINE_CNTPCT` (0 disables the inline TSC read), `EDEN_NX2_SVC_TRACE`, `EDEN_HYBRID_CORES=0`, `EDEN_NX2_VCACHE`.

## Invariants

- Scheduler state is changed only under the global scheduler lock (`ASSERT(IsSchedulerLockedByCurrentThread)` in `PreemptThreads`, enforced by `ASSERT`, which is evaluated in release too).
- Every `CoreTiming` event type is unscheduled before its owner dies. This is only assumed: `Advance` has no `else` branch for an expired `weak_ptr`.
- `NUM_CPU_CORES` (6) must match the Dynarmic exclusive monitor (comment in `hardware_properties.h`; not checked).
- `SpeedLimiter::DoSpeedLimiting` only paces single-core mode (`return` when `use_multi_core`). In multicore, pacing comes from present/vsync paths outside this slice.
- A core's `m_context_guard` unlock must happen on the host thread that locked it: true because lock and unlock are both in that core's scheduler fiber.

## Hazards and improvement candidates

See `findings/cpu-exec.json`. In short: `PerfStats` destructor range can be inverted; guest `IC IVAU` invalidates only the issuing core's JIT; `CoreTiming::Advance` cannot drop an expired event; several spin loops (`SuspendEmulation`, `SyncPause`, `FinishTermination`) have no yield; one dead field. Other notes: six JITs with 512 MiB code caches each (virtual reserve, 3 GiB); `Idle` spin trades CPU for latency by design (NX2_NOTES row 109: no gain measured); six `Critical`-priority threads plus the timer thread can starve other host threads on CPUs with fewer than 8 performance cores.
