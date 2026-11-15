# Nx2 graphics server (nx2:gsv) and helpers

**Purpose.** Switch 2 (`.nup`) titles do not talk to nvdrv alone. They send TIPC requests over raw session handles to a set of "nx2:*" services: a graphics supervisor, a GPU memory server, a syncpoint server, a display (vi2) server and a video-decoder service. `nx2_gsv.h` implements all of these as HLE services, in one 4366-line header. It maps guest memory into the GPU address space, hands out native image handles, mirrors syncpoint values into a guest page, paces presentation and drives a software/Vulkan video decoder.

## Files

| file | role | read depth |
|---|---|---|
| `src/core/hle/service/nx2_gsv.h` | All service classes (top `nx2:gsv`, `nx2:gpu`, `nx2:syncpt`, `nx2:vi2`, `nx2:vdec*`, `nx2:chat:gsv`, `nx2:camera`) | full |
| `nx2_frame_interval.h` | Hyrule Warriors 60/30 fps interval controller | full |
| `nx2_movie_pacing.h` | Mario Kart World movie-activity detector | full |
| `nx2_oot_time.h` | Ocarina of Time smoothed frame delta and patch offsets | full |
| `nx2_streamplay.h` | `stpl:u` stream-play stub with one event | full |
| `nx2_stub_interface.h` | Generic logging stub interface plus interned-name pool | full |
| `nvdrv/devices/nx2_stub_device.h` | `Nx2GpuMemoryRegistry` (mappings, sparse, device ranges), stub nvhost node | partial (lines 1-300 of 339) |
| `video_core/nx2.h` | Globals the GPU side shares with nx2:gpu (sparse state, semaphore regions, written-page bitmaps) | full |
| `video_core/host1x/syncpoint_manager.{h,cpp}` | Guest actions, guest mirror, `WithGuestValuesStable` | full |
| `service/hle_ipc.cpp`, `am/service/self_controller.cpp` | Copy/move handle semantics; the only constructor of the top session (`GetGraphicsSupervisor`, line 109) | partial |
| `core_timing.cpp`, `k_auto_object.h`, `k_session.cpp` | UnscheduleEvent semantics, refcounts | partial |
| `nx2_diag.h`, `nx2_spikes.h`, `nx2_job_stats.h`, `nx2_gpu_latency.h`, `nx2_hazard.h`, `nx2_timeline.h`, `nx2_knobs.h`, `host1x/nx2_video_decoder.h` | Included diagnostics and decoder | not opened |
| `nvhost_gpu.cpp`, `nvhost_ctrl_gpu.cpp`, `memory_manager.cpp`, `nvhost_as_gpu.cpp` | Callers and listeners | grep only |

## Main types and ownership

- `INx2GraphicsSystemSession` (top `nx2:gsv`) is created in `ISelfController::GetGraphicsSupervisor` and returned as a raw session. It owns `std::shared_ptr<Nx2NativeObjects> m_objects` (native memories and images, guarded by `Nx2NativeObjects::mutex`), shared with `INx2Vi2Session` and `INx2VideoSession`. It also owns the syncpoint page mirror and the 500 µs `m_syncpt_poll` CoreTiming event.
- `INx2GpuSession` (`nx2:gpu`) owns sparse and GPU-only region tables, the memory-handle map, 32 job-slot events, and a `nx2:ColdHint` jthread (last member). It registers a global sparse-backing handler in `Tegra::Nx2SetSparseBacking`. `OpenSubSession` (22) returns the same object again.
- `INx2SyncptSession` keeps `SharedState{mutex, alive}`; guest actions check `alive` before signalling.
- `INx2Vi2Session` owns a `VsyncState` (shared with a looping vsync timer), a release timer, capture buffers. `INx2VideoSession` owns a `nx2:vdec` decode `std::thread` (started in `Configure`), a `ReadyState` shared with a 4 ms looping ready timer, and a guest syncpoint from `NvdrvSyncpoints().AllocateSyncpoint`.
- Sessions are created by `CreateRawSession` (new `KSession`, `SessionRequestManager`, registered on the creator's server manager). Process-wide state lives in `Nx2GpuMemoryRegistry::Get()` and in inline globals in `video_core/nx2.h`.

## Threads and locking

- Guest service thread: runs all handlers. It blocks the thread in `WaitForFrame` (up to 15 s, 4 s with `unless_gpu_waits`), `WaitForBackThread` (4 s) and `ReleaseBackstop` (host sleep, up to 250 ms). `PaceToVblank` uses the guest-kernel `Sleep` instead.
- GPU thread: runs `RunInOrder` lambdas (`SignalAfterGpu`, `QueueBackingRelease`), `BackSparseWrite` through `Tegra::Nx2BackSparseWrite` (from `memory_manager.cpp:409,598`), and syncpoint guest actions under the manager's `guard`.
- CoreTiming thread: vsync, release, ready and syncpoint-poll callbacks. `UnscheduleEvent` (default type) in destructors waits for in-flight callbacks.
- `nx2:vdec` thread (decode), `nx2:ColdHint` thread (every 30 s, low priority).
- Locks, outer to inner as the code uses them: `INx2GpuSession::m_region_mutex` (recursive; `m_region_cv` for pending blocks) -> `Nx2GpuMemoryRegistry::mutex` (listeners run under it, into nvhost-as-gpu) -> `device_mutex`; `m_region_mutex` -> `g_nx2_sparse_state_mutex` (taken last, per its comment). GPU thread: `g_nx2_sparse_backing_mutex` -> `m_region_mutex` (released around `AllocateGpuOnlyBacking`). `SyncpointManager::guard` -> `shared->mutex` (syncpt actions). `INx2VideoSession::m_mutex` guards queues; `Nx2NativeObjects::mutex` guards handle maps. `RunInOrder` is non-blocking by design, which is what keeps the service-thread -> GPU-thread direction from closing a cycle; no ABBA was found, but it rests on that.

## Flow

1. Guest asks AM for the graphics supervisor: `GetGraphicsSupervisor` -> `ReturnRawSession(INx2GraphicsSystemSession)`. Every later request is TIPC; each class registers a `FunctionInfo` table, with `TipcHandshake` (0) first.
2. `nx2:gsv` 204 `AttachSyncptMemory` writes the header into the transfer memory, installs `SetGuestMirror` and starts `PollSyncpts`. `OpenGpuSession` (497), `OpenViSession` (305), `OpenVideoSession` and `OpenNvsrvSession` (builds an `Nvidia::NVDRV` "nx2:nvsrv") hand out the other sessions.
3. Memory: `CreateNativeMemory`/`BindMemory`, then `CreateNativeImage`/`QueryNativeImage` fill `Nx2NativeObjects` using `Nx2CalculateImageLayout`.
4. GPU mappings: `SubRegisterMemory` (101) -> `Nx2GpuMemoryRegistry::Register`; a 0x2000-byte region also goes to `Nx2AddSemaphoreRegion`. `SubSubmitJob` (211) maps a handle at a GPU address, `SubUnmapJob` (213) unmaps. `SubCreateGpuOnlyRegion` (214) creates a sparse/GPU-only region; with `EDEN_NX2_SPARSE_LAZY`/`GPU_ONLY_LAZY` blocks are backed on the first GPU write by `BackSparseWrite`. `SubSparseMap` (216) applies 40-byte requests via `ApplySparseMapRequest` and `MapSparseTile`.
5. Presentation: `PresentClientBuffer` (vi2 229) waits for the buffer's fence (`WaitForFrame`), calls `Composite` -> `GPU().RequestComposite`, then `QueueRelease` (timer increments a guest syncpoint) or `PaceToVblank`. It updates `g_nx2_video_last_frame_ns`, `UpdateFrameInterval` (Hyrule) and the Ocarina delta, and replies with 0x7fff available slots plus the release fence.
6. Video: `Configure` (101) starts `DecodeLoop`; `SubmitInput` (109/400) queues up to 8 inputs; the thread decodes at most 2 frames ahead, `WriteImage` renders into the guest image memory, `WaitForRelease` polls up to 100 ms, the ready timer re-signals `GetEvent`.

## Connections

- In: `ISelfController::GetGraphicsSupervisor` (only constructor); nvdrv (`nvhost_as_gpu.cpp`, `nvdrv.cpp`, `container.cpp`) uses `Nx2GpuMemoryRegistry`; `memory_manager.cpp` calls `Nx2BackSparseWrite`; texture cache reads `Nx2SparseBlockIsWritten`; `nvhost_gpu.cpp:446` comment depends on the present ordering here; `loader/nso.cpp` and `patch_manager.cpp` use `Nx2OotTime`; `svc.cpp:2535` reads `g_nx2_present_count`.
- Out: `GPU().RunInOrder`, `RequestComposite`, `SyncpointManager::{RegisterGuestAction, DeregisterGuestAction, IncrementGuest, SetGuestMirror, WithGuestValuesStable}`, `Nx2GpuMemoryRegistry`, `Tegra::Nx2*`, `Common::ColdHintModeInEffect`/`DeviceMemory().buffer.HintCold`, `Kernel::GetCurrentThread().Sleep`, CoreTiming, `ServiceContext::CreateEvent`.

## Settings and switches

- Settings: `use_multi_core`, `use_speed_limit`, `SpeedLimit()`, `hyrule_30fps`, `hyrule_dynamic_fps`, `mkw_dynamic_fps`, `IsDockedMode()`, `GetDebugKnobAt(1)`, `ColdHintMode`, `VideoCommon::Nx2Knob::BackWait`. Title ids are hard-coded: Donkey Kong Bananza `0x040074A01BF12000`, Ocarina `0x0400486024DEC000`, Mario Kart World `0x0400C3F00006E000`.
- Environment: `EDEN_NX2_SPARSE_LAZY`, `GPU_ONLY_LAZY`, `FRAMES_AHEAD`, `BACK_WAIT`, `PRESENT_QUEUE`, `SWAP_INTERVAL`, `CROP_WIDTH`, `CROP_HEIGHT`, `FRAME_CAPTURE`, `CAPTURE_SIZE`, `FLASH_WATCH`, `DUMP`, `VSYNC_HZ` (default 60), `VDEC_TRACE`, `VDEC_EOS_RELEASE_NOW`, `VDEC_PACE`, `VDEC_DUMP`, `EDEN_VI2_MAX_RESOLUTION_MODE`.

## Invariants

- Sparse state lock `g_nx2_sparse_state_mutex` is taken last (comment in `nx2.h`; assumed elsewhere).
- Guest values are published to the page before the lock-free readers see them: enforced in `SyncpointManager::Increment`.
- `RunInOrder` lambdas never block (enforced in `gpu.cpp`). The destructor of `INx2GpuSession` does not drain them; the `[this, event]` captures assume the session outlives queued work (assumed).
- Event slot ids wrap with `id %= 32` (enforced); guest ranges in 101/211/214 are mostly unchecked (assumed well-formed).
- Syncpoint action handles may be stale; `DeregisterAction` scans the list before erasing (enforced).
- One `nx2:gpu` session per process: the sparse-backing handler is a single global (assumed).

## Hazards and improvement candidates

- `ReturnRawSession` pushed the client end as a copy object and never closed the creator reference, so sessions and their threads were never destroyed (finding 1). Since `3aa4b25` it reserves the guest handle itself, writes it into the reply as an already translated copy handle, and closes the creator reference: a session is destroyed when the guest closes it. Three classes still live until server teardown, `INx2GpuSession`, `INx2GraphicsSystemSession` and `Nvidia::NVDRV` (`Nx2RetainRawSession`), because GPU mappings and native objects outlive the guest handle. A move handle in the reply makes Fortune's Weave abort at boot with 2162-0001.
- Guest-controlled sizes/counts without caps: 101 range wrap, 911 count, `WriteImage` allocation (findings).
- `Nx2GpuMemoryRegistry::TrimMappings` rebuilds the whole mapping vector on every map/unmap under the registry mutex.
- Flash-watch debug history never stores samples.
- Unused code: `INx2NvsrvSession`, `SubSlotJob`.
- Service thread blocks on host waits (`WaitForFrame`, `WaitForBackThread`, `ReleaseBackstop`); this is deliberate pacing but stalls other service requests of the same session.
- `PollSyncpts` wakes at 2000 Hz for the life of the session.

## Request table

| session | command | handler | what it does |
|---|---|---|---|
| gsv | 4,5,101,601,901,1001,1201 | `Stub` | logged success |
| gsv | 202 / 204 | `OpenSyncptSession` / `AttachSyncptMemory` | syncpt session; header + mirror + poll |
| gsv | 305 / 497 / 602 / 1202 / 1402 | `OpenViSession` / `OpenGpuSession` / `OpenCameraSession` / `OpenChatSession` / `OpenNvsrvSession` | return raw sessions |
| gsv | 902,905,906,1002 | `OpenVideoSession` | vdec/vdec2/vdecsvc session |
| gsv | 111 / 112 / 114,423 | `CreateNativeMemory` / `DestroyNativeMemory` / `BindMemory` | native memory objects |
| gsv | 429,1211,311,321,615,811,1611 / 430,912,... | `CreateNativeImage` / `DestroyNativeImage` | image handles |
| gsv | 426,428 / 427 / 435 | `QueryNativeImage` / `UnmapNativeImage` / `CalculateNativeImageSize` | image info, GPU range map |
| gsv | 314,315 / 434 / 1510 | `ConfigureWindowImagePool` / `AttachGpuAddressSpace` / `CalculateRequirements` | pool config |
| gsv | 911 / 991 / 961,361,861 | `CreateDecoderImage` / `CalculateDecoderImagePoolBuffer` / `GetCpuImageDescriptor` | decoder images |
| gsv | 1291,391,691,891,1691 | `CalculateChatImagePoolSize` | pool size |
| gsv | 11 / 115 / 126 / 1111 | `RegisterClientHandle` / `QueryGrantedFeatures` / `GetSessionFlag` / `SetFeatureVersion` | flags |
| gsv | 201,301,401-403,494,801,1101,1102,1501-1503,1601 | `CheckPermission` | empty success |
| gpu | 0, 11, 12, 21, 22, 33 | `Handshake`, `GetIdRange` ([0,31]), `GetIdEvent`, `Start`, `OpenSubSession`, `GetCount33` | ids and events |
| gpu | 41 / 42 | `CreateGraphicsDevice` / `DestroyGraphicsDevice` | device ids |
| gpu | 201,202,203 | `GetUserPartition`, `GetGsvPartition`, `GetVaRange` | address ranges |
| gpu | 101 / 102 | `SubRegisterMemory` / `SubDestroyMemory` | registry map/unmap |
| gpu | 211 / 212 / 213 | `SubSubmitJob` / `SubModifyKind` / `SubUnmapJob` | GPU mapping |
| gpu | 214 / 215 / 216 | `SubCreateGpuOnlyRegion` / `SubDestroySparseRegion` / `SubSparseMap` | sparse regions |
| syncpt | 0,1,22,103,201,202 | `Handshake`, `GetCount` (32), `GetSlotEvent`, `QueryCounter`, `RequestSlotEvent`, `CancelSlotEvent` | slot waits via guest actions |
| vi2 | 229 | `PresentClientBuffer` | fence wait, composite, release, pacing |
| vi2 | 261-272 | `BindVsync`, `BindChange`, `UnbindNativeEvent` | event binding |
| vi2 | 11,31,32 | `ReturnEvent`, `AcquireNativeEvent`, `DuplicateNativeEvent` | events |
| vi2 | 103-106, 212, 5, 228 | display queries, `GetBufferMaxResolution`, `GetLayerBufferBits`, `GetTimestampRecorderCount` (7), `CancelClientBuffer` | queries |
| vdec | 1,3,6,8,101,109,115,200-202,207,210,212,213,311,314,400 | `CreateDecoderService`, `GetEvent`, `Succeed`, `Flush`, `Configure`, `SubmitInput`, `Finalize`, `UnregisterBuffer`, `GetOutputInfo`, `RegisterBuffer`, `ReleaseBuffer` | decode pipeline |
| chat:gsv | 1, 11 | `GetEventId`, `GetEvent` | events |
| camera | 3, 21, 29 | `Stub` | logged success |
| stpl:u | 0 / 300 | `INx2StreamPlayService` open / event getter | stub |
