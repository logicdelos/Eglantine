# GPU front end (GPU thread, pushbuffer front end, channels, syncpoints, Host1x, GPU split)

**Purpose.** This slice takes guest GPU work (nvdrv ioctls submit command lists) and gets it to the engines and the Vulkan rasterizer. A dedicated GPU thread pops submit commands, walks pushbuffers (`DmaPusher`), dispatches methods to the puller and the engines, and signals fences and syncpoints. On Switch 2 (NX2/Ampere) titles the fork adds a two-thread "GPU split": the GPU thread becomes a *front* that parses and runs macros, and a second thread (`GPUBack`) executes the engine work from an 8 MiB ring. The slice also holds the Host1x/CDMA side that drives NVDEC and VIC video decode.

## Files
Paths are under `src/video_core/`.

| file | role | read depth |
|---|---|---|
| gpu_thread.cpp/.h | `ThreadManager`: GPU thread loop, command queue, fences, nx2 latency hold | full |
| gpu.cpp/.h | `GPU::Impl`: sync requests, syncpoints, composite requests, `MapInOrder`, ticks | full / partial (.h) |
| gpu_split.cpp/.h, split_write_tracker.h | fork: front/back ring, write tracking | full |
| dma_pusher.cpp/.h | pushbuffer parsing, GPU-written command detection, split dispatch | full |
| engines/puller.cpp | channel methods: semaphores, fences, bind, Ampere semaphore acquire | full |
| engines/maxwell_3d.cpp | `Front()` state, `FrontWrite`, `FrontRunMacro`, `MacroDmaRead` | partial (about 1,500 of 2,500 lines) |
| macro_turing.h, macro_memo.h | fork: native Turing macro interpreter and memo cache | full |
| control/scheduler, channel_state | channel binding, pending (suspended) channels, Nx2 syncpoint helpers | full |
| host1x/syncpoint_manager, fence_manager.h | guest syncpoints and actions; fence bookkeeping | full / partial (fence_manager 59-233) |
| cdma_pusher, host1x/host1x, nvdec.cpp | Host1x command stream and device table | full |
| host1x/ffmpeg.cpp, codecs/decoder.cpp, codecs/codec.cpp, host1x/vic.cpp | decode wrappers, VIC | partial (vic.cpp lines 60-330 only) |
| macro.cpp, macro_turing_jit.h, macro_dma_forward.h | macro engines, JIT, DMA forwarding plans | not opened |
| kepler_compute.cpp, maxwell_dma.cpp, fermi_2d, draw_manager.cpp, engine_upload | engines | not opened |
| nx2_* headers (diag, spikes, perf, profile, timeline, hazard, gpu_latency, job_stats, debug_control, sampler), gpu_logging, stage_cache, host1x/nx2_video_decoder, channel_state_cache | fork diagnostics and caches | not opened |

## Main types and ownership
- `ThreadManager` is owned by `GPU::Impl`. It owns the `std::jthread` "GPU" and a `SynchState` (SPSC queue of `CommandDataContainer`, `write_lock`, `cv`, `last_fence`, `signaled_fence`).
- `GPU::Impl` owns the `Scheduler`, the `Host1x` reference, the syncpoint manager, sync-request state (`sync_requests`, `last_sync_fence`, `sync_request_cv`) and `pending_maps`.
- `Scheduler` owns `ChannelState` objects (by channel id). Each `ChannelState` holds the engines (`maxwell_3d`, `fermi_2d`, `kepler_compute`, `maxwell_dma`, `kepler_memory`), a `DmaPusher`, and the Nx2 syncpoint words. `pending_channels` (channels suspended on an Ampere semaphore acquire) is touched only by the GPU thread.
- `GpuSplit` is a process singleton (`GpuSplit::Get()`). The front owns `write_position`/`published_local`; the back owns `consumed`. `SplitWriteTracker` records which memory ranges queued back work will write. `FrontState` inside `Maxwell3D` is created lazily and returned by `ReleaseFront()`.
- `Host1x::devices` is a fixed `std::array<variant<monostate,Nvdec,Vic>,1024>` indexed by nvdrv fd. Each `CDmaPusher` owns one `jthread` and a `command_lists` deque. `FrameQueue` (mutex `m_mutex`) holds decoded frames per fd.

## Threads and locking
- **GPU thread** ("GPU", critical priority): runs `ThreadManager::ThreadFunc`, the whole front end, the puller and (without the split) all engines. With the split on, it is the front and runs macros and parsing only.
- **GPUBack thread**: executes ring entries (engine methods, copies, compute dispatches), runs the idle hook (`Scheduler::FlushDeferred` in the Vulkan scheduler) and sleeps on an atomic `wake`.
- **CPU (guest) threads**: call `PushCommand`, `OnCPURead`, `InvalidateRegion`, `RequestComposite`, `WaitForIdle`. `PushCommand` holds `write_lock` across the queue emplace so fences stay monotonic; with `block` it then waits on `cv` for `signaled_fence`.
- **CDMA jthreads** (one per NVDEC/VIC device): wait on `command_cv`.
- **Locks**: `write_lock` + `cv` (fence handshake); `sync_request_mutex` (sync requests); `pending_maps_mutex`; `request_swap_mutex`; `Scheduler::scheduling_guard` (channel table; BindChannel deferred to the back takes it there); `SyncpointManager::guard` (action lists, and held while actions run in `Increment`); `DmaPusher::sync_mutex`; `FrameQueue::m_mutex`. The ring is lock-free SPSC (u64 positions with acquire/release).
- **Observed order**: `write_lock` is never held while running GPU work. `SyncpointManager::guard` is held while running registered actions, so an action that takes `sync_request_mutex` or renders (the Composite action in `RequestComposite`) runs under it.

## Flow
1. A guest ioctl reaches `GPU::PushGPUEntries(channel, command_list)`, which calls `ThreadManager::SubmitList` and `PushCommand` (fence++, enqueue, optionally wait).
2. `ThreadFunc` loop: `Profile::Report`, `GpuSplit::MaybeStart` (can start or stop the split), then, if channels are pending, `ResumePending` + `TryPop` (spin 100 us then sleep 1 ms or wait on `Nx2PendingWake`), otherwise `PopWait`.
3. `SubmitListCommand`: optional nx2 `HoldForGpuLatency` (Ampere, non-blocking submits), then `Scheduler::Push`, which binds the channel, calls `DmaPusher::Push` + `DispatchCalls`, and adds the channel to `pending_channels` if it has a pending acquire.
4. `DmaPusher::Step` per pushbuffer entry: `IsGpuWrittenCommands` (page walk), `WaitForWriters` when the split is active and the entry looks unwritten, then `ProcessCommands`. Each header runs `CallMethod`/`CallMultiMethod`.
5. Without the split, methods run inline: `Puller::CallPullerMethod` for subchannel 0-class methods, engines otherwise. With the split, `CallMethod` writes either `FrontCallMethod` (native Turing Maxwell3D, runs macros on the front) or `split.Method`/`split.Multi` into the ring; the back replays them through `CallMethod`/`CallMethodFast`.
6. `Puller` fence/semaphore methods call `rasterizer->SignalFence`/`SignalSyncPoint`, or `channel->Nx2RaiseSyncpoint`. An Ampere semaphore acquire suspends the channel (`PendingAcquire`) and `PollAcquire` retries from `ResumePending`.
7. `SignalCommand` stores `signaled_fence` and notifies `cv`. With the split, the signal is deferred to the back so it follows prior work.
8. CPU reads and invalidations use `RequestSyncOperation`/`WaitForSyncOperation`: a GPUTick command makes the GPU thread run queued operations in `TickWork`.
9. Video: nvdrv `StartDevice(fd)` emplaces `Nvdec`/`Vic`; `PushEntries` queues a list on the device thread; `ExecuteCommand` → `ProcessMethod` → `Execute`; `Decoder::Decode` pushes frames into `FrameQueue`; VIC finds them by luma offset.

## Connections
- In: nvdrv (`PushGPUEntries`, `Host1x::PushEntries/StartDevice`), memory manager/CPU memory callbacks (`OnCPURead`, `InvalidateRegion`), service `vi`/`nvnflinger` (`RequestComposite`, `RequestSwapBuffers` path), applet capture (`GetAppletCaptureBuffer`).
- Out: `RasterizerInterface` (`SignalFence`, `SignalSyncPoint`, `FlushRegion`, `InvalidateRegion`, `ReleaseFences`, `OnCacheInvalidation`), `RendererBase::Composite`, the Vulkan scheduler idle hook (via `GpuSplit` back loop), `MemoryManager` (`Read`, `GpuToCpuAddress`), FFmpeg (decode), and `Core::System` (`IsPoweredOn`, `RegisterHostThread`).

## Settings and switches
- `Settings::values`: `sync_memory_operations` (default false), `gpu_clock`, `renderer_backend`, `nvdec_emulation`, GPU accuracy (`IsGPULevelHigh`), DMA accuracy (`IsDMALevelDefault/Safe`), fence behaviour (`IsGPUFenceBehaviorDefault/Balanced/Accurate`, which also controls delayed fences). `UpdateGPUAccuracy` is called in `Start` and `OnCommandListEnd`.
- Environment: `EDEN_NX2_SPLIT` (0/1/2, default 2 = macros on the front), `EDEN_NX2_SPLIT_CHECK`, `EDEN_NX2_SPLIT_SKIP`, `EDEN_NX2_MME_JIT`, `EDEN_NX2_METHOD_FAST`, `EDEN_NX2_FETCH_DRAIN`, `EDEN_NX2_PARK_SPLIT_METHODS`, `EDEN_NX2_PB_CHECK`, `EDEN_NX2_PB_DUMP`, `EDEN_NX2_SEM_WATCH`, `EDEN_NX2_RESNAPSHOT`, `EDEN_NX2_PERF_VERIFY`, `EDEN_NX2_GPU_PROFILE`, `EDEN_NX2_MACRO_TRACE`, `EDEN_NX2_PULLER_TRACE`, `EDEN_NX2_METHOD_TRACE`, `EDEN_NX2_BUFFER_DOWNLOADS`.
- `Nx2Perf` options: `FrontSleep`, `EagerPublish`, `MacroReadEarly`, `MacroReadCbOverlay`, `MacroReadWriterSet`, `CbBatch`, `DrawBeginCoalesce`, `FrontRun`, `FrontSameFirst`, `FrontClassTable`, `DmaForward`, `DrawPreamble`.
- The split starts only when `g_nx2_ampere` is set, the renderer is Vulkan, and GPU accuracy is not accurate/safe-reads.

## Invariants
- Fence ids are monotonic: `last_fence` is incremented and the command queued under `write_lock` (enforced).
- Only the GPU thread touches `pending_channels` and the front-side ring cursors (assumed, `MarkGpuThread`).
- Ring entries must be trivially copyable (`Defer<Func>` static check; `DeferFunction` is the exception and heap-allocates).
- The back must have run every queued writer before the front reads memory those writers touch: enforced by `WaitForWriters`, `DrainBack`, `SplitWriteTracker::ReadTarget` and the early-read path in `MacroDmaRead`; the correctness of `SplitWriteTracker` collapsing to "Unknown" after 512 writes is assumed.
- `Host1x::devices[fd]` index is valid (assumed; no bounds check). Method indices into `thi_regs`, `Nvdec` and `Vic` register arrays are in range (assumed, not enforced).
- A guest semaphore acquire eventually becomes true; on non-Ampere this is a busy loop with no escape.

## Hazards and improvement candidates
Details are in `findings/gpu-frontend.json`.
- Unchecked guest-controlled register indices in CDMA/NVDEC/VIC (out-of-bounds writes).
- Nvdec/Vic destruction does not join the base jthread first.
- `WaitForSyncOperation` and the non-Ampere acquire loop and `sync_cv` wait have no shutdown path.
- The static 256/8 gates in Ampere `ProcessSemaphoreExecute` stop fence release after warm-up.
- `GpuSplit` busy-spins in `Drain` waiting for `back_quiet`; `MaybeStart` and `Profile::Report` run every loop iteration.
- `async_writes` growth, `DeferFunction` leak on Stop, and arbitrary eviction in `FrameQueue::PushDecodeOrder`.
- `SyncpointManager::Increment` runs actions (including a composite) under `guard`.
- Not examined: engines other than Maxwell3D front, macro JIT, nx2 diagnostics; defects there are unknown.
