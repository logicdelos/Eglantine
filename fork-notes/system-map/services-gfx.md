# Graphics services (nvdrv, nvnflinger, vi, caps, Switch 2 nx2 gsv)

**Purpose.** These services are what a guest game uses to get memory, GPU channels, sync points and a displayable frame. `nvdrv` is the /dev/nv* ioctl layer (nvmap, nvhost-gpu, as-gpu, ctrl, ctrl-gpu, disp0, decoders). `nvnflinger` is the Android-style BufferQueue plus `SurfaceFlinger`/`HardwareComposer` that turns queued buffers into `GPU().RequestComposite` calls. `vi` is the IPC front end (display, layers, shared buffer). The fork adds Switch 2 ("NX2") paths: `nx2_*.h` TIPC sessions that replace the Switch 2 graphics stack (`nx2:gsv`, `nx2:vi2`, `nx2:gpu`, `nx2:syncpt`, `nx2:vdecsvc`), plus nx2 hooks in nvdrv.

All paths below are under `src/core/hle/service/` unless stated.

## Files
| file | role | read depth |
|---|---|---|
| nvdrv/nvdrv.cpp, nvdrv/core/container.cpp | `Module`, fd table, event interface, sessions; resets nx2 globals in the constructor | full |
| nvdrv/core/nvmap.{h,cpp} | handle table, SMMU pin/unpin, unmap queue | full |
| nvdrv/core/syncpoint_manager.{h,cpp} | guest syncpoint reservation and counters | full |
| nvdrv/devices/nvhost_ctrl.cpp | syncpoint wait, NvEvents | full |
| nvdrv/devices/nvhost_as_gpu.cpp | GPU address space; nx2 map/sparse/release hooks | partial (nx2 part, lines 30-345, full) |
| nvdrv/devices/nvdisp_disp0.cpp | `Composite` into `GPU().RequestComposite` | full |
| nvdrv/devices/nx2_stub_device.h | `Nx2GpuMemoryRegistry`, `nx2_stub_device` for unknown nodes | full |
| nvdrv/devices/nvhost_gpu.cpp, nvhost_ctrl_gpu.cpp | channel submit, characteristics; nx2 hooks (`g_nx2_driver`, `Nx2GetEngineInfo`) | skimmed (grep of the nx2 hooks) |
| nvdrv/devices/nvhost_nvdec*, vic, nvjpg, heap_mapper, nvmap (device), nvdrv_interface | upstream decoder and glue | skimmed / not opened |
| nvnflinger/buffer_queue_{producer,core,consumer}.cpp, consumer_base.cpp | BufferQueue | full |
| nvnflinger/hardware_composer.cpp, surface_flinger.cpp, hos_binder_driver*.cpp, display.h | composition, binder | full |
| nvnflinger/buffer_item_consumer.cpp, parcel.h, graphic_buffer.h | helpers | skimmed |
| vi/conductor.cpp, vi/container.cpp, vi/shared_buffer_manager.{h,cpp} | vsync driver, VI state, shared frame buffer | full |
| vi/*_display_service.cpp, display_list.h, layer_list.h | IPC wrappers | not opened |
| caps/* | screenshots | not opened |
| nx2_gsv.h (4366 lines) | all Switch 2 TIPC sessions | lines 1-3560 read; 3552-4366 (`INx2GraphicsSystemSession`, a logging stub) only the start |
| nx2_frame_interval.h, nx2_movie_pacing.h, nx2_oot_time.h, nx2_streamplay.h, nx2_stub_interface.h | pacing and stub helpers | not opened |

## Main types and ownership
- `nvdrv::Module` owns `Container` (`NvMap`, `SyncpointManager`, session deque), `open_files` (fd to device) and the `EventInterface`. An unknown `/dev/nvhost-*` open becomes an `nx2_stub_device` that echoes input and succeeds. The constructor resets the nx2 globals (`Nx2GpuMemoryRegistry::Reset`, `g_nx2_driver`, `g_nx2_present_count`, `Nx2GpuLatency::Reset`).
- `NvMap` owns `Handle`s by id (`handles_lock`) and an SMMU allocator range; handles are refcounted and parked in `unmap_queue` when unpinned but still mapped.
- `HosBinderDriverServer` owns binders. `SurfaceFlinger` owns layers/displays and each layer's `BufferQueueCore`, shared by `BufferQueueProducer` and a `BufferItemConsumer`. `~Layer` calls `Abandon()`.
- `vi::Container` owns displays, layers, the `Conductor` and the `SharedBufferManager` (a 7-slot 1280x768 RGBA secure-pool page group mapped into each owner process).
- Nx2: `Nx2NativeObjects` (memories, images) is shared by gpu, vi2 and video sessions through a `shared_ptr`. `INx2GpuSession` owns the sparse and GPU-only backing (`KPageGroup`s). `Nx2GpuMemoryRegistry` is a process-wide singleton that records gpu-address to cpu-address mappings so vi2 and the decoder can map an application address to a device address.

## Threads and locking
- Guest service threads run the ioctls and IPC. The core-timing/vsync thread (`Conductor`, separate thread in multicore, inline otherwise) runs `ProcessVsync` then `Container::ComposeOnDisplay` then `HardwareComposer::ComposeLocked`. The GPU thread runs `RunInOrder`/`MapInOrder` lambdas queued by nx2 map/unmap code, and the `nx2:vdec` decode thread and an `nx2:ColdHint` jthread (every 30 s, working-set trim only) exist per session/process.
- BufferQueue: `BufferQueueCore::mutex` guards slots and queue; `dequeue_condition` waits on it. Listener callbacks run without the core lock. `buffer_history_mutex` is a leaf.
- `vi::Container::m_lock` is held for every layer operation and for the whole composition; `SurfaceFlinger` has no lock of its own. `SharedBufferManager::m_guard` is taken first in `CreateSession`/`DestroySession`, which then call into the container and nvdrv.
- NvMap: `handle->mutex` then `unmap_queue_lock`; `handles_lock` is a leaf. `PinHandle` takes `handle->mutex` then another handle's mutex while draining the queue (see findings).
- `SyncpointManager` counters are atomics; `reservation_lock` guards reservation. `nvhost_ctrl::events_mutex` guards NvEvent slots; the host1x action lambda only touches atomics.
- Nx2: `Nx2GpuMemoryRegistry::mutex` then `nvhost_as_gpu::mutex` (via listener callbacks) then `device_mutex` (leaf). `INx2GpuSession::m_region_mutex` (recursive) is taken before those. `AllocateGpuOnlyBacking` runs outside it because `MapPageGroup` can wait on the guest page-table lock. GPU-thread work is deferred through `RunInOrder` to avoid inverting the texture-cache lock.
- `INx2Vi2Session::PresentClientBuffer` blocks the service thread in `WaitForFrame` (up to 15 s, 4 s with `unless_gpu_waits`); `WaitForBackThread` waits up to 4 s. The vsync state mutex is shared with a HostTiming callback.
- `INx2VideoSession`: `m_mutex` plus `m_cv` between service thread and `DecodeLoop`; `ReadyState::mutex` is shared with a HostTiming 4 ms timer.

## Flow
1. Guest opens `/dev/nvmap`, creates and allocates handles, `IocPin`/`PinHandle` maps them to device addresses (or into the preallocated area through the session's mapper).
2. Producer path: `IHOSBinderDriver::TransactParcel` then `BufferQueueProducer::Transact`: `DequeueBuffer` (may block in `WaitForDequeueCondition`), `RequestBuffer`, `QueueBuffer` (pushes a `BufferItem`, may replace a droppable front item, calls `OnFrameAvailable`).
3. Composition: `Conductor::ProcessVsync` then `Container::ComposeOnDisplay` then `SurfaceFlinger::ComposeDisplay` then `HardwareComposer::ComposeLocked`: `nvdisp.WaitForComposite`, acquire via `CacheFramebufferLocked`, sort by z, `nvdisp_disp0::Composite` (`GPU().RequestComposite`, `SpeedLimiter().DoSpeedLimiting`), `ReleaseFramebuffersLocked` (release frame = acquire frame + swap interval), `SignalVsync`. With no visible layer it returns early without acquiring or releasing.
4. Guest waits: `nvhost_ctrl::IocCtrlEventWait` checks the fence, otherwise registers an NvEvent on the host syncpoint manager and returns Timeout/AlreadyAssigned; after more than 2 failed waits it stalls the application and blocks in `WaitHost`.
5. Switch 2: `nx2:gpu` register-memory/map jobs (TIPC 211, 216) go to `Nx2GpuMemoryRegistry` and `nvhost_as_gpu::Nx2Map/Nx2Sparse/Nx2Unmap`. `INx2Vi2Session::PresentClientBuffer` (TIPC 229) goes `WaitForFrame`, `Composite` (image lookup, `DeviceAddressOf`, `RequestComposite` with native tiling), `QueueRelease` (a guest syncpoint incremented by a CoreTiming event), then `ReleaseBackstop`, or `PaceToVblank` when single-core or `EDEN_NX2_PRESENT_QUEUE=0`.
6. Video: `Decode` queues input, `DecodeLoop` decodes, drops overtaken frames, waits up to 100 ms on the release fence, writes into the guest image (`WriteImage`), invalidates the GPU region, increments the syncpoint.

## Connections
- In: HLE service manager (nvdrv, vi:u/m/s, nx2 TIPC names), the Vulkan renderer through `GPU().RequestComposite`, `Tegra::Host1x` syncpoints (`GetGuestSyncpointValue`, `IncrementGuest`, `RegisterAction`).
- Out: `system.GPU()` (`RequestComposite`, `MapInOrder`, `RunInOrder`, `InvalidateRegion`), `Host1x().MemoryManager()` (SMMU), `Core::Timing`, kernel page tables (`MapPageGroup`, `UnmapPageGroup`), `Tegra::Host1x::Nx2VideoDecoder`, `VideoCommon::Nx2GpuLatency`, `Settings::SpeedLimit`.

## Settings and switches
- Settings: `enable_buffer_history`, `use_multi_core`, `use_speed_limit`/`SpeedLimit()`, `use_video_framerate`, `hyrule_30fps`, `hyrule_dynamic_fps`, `mkw_dynamic_fps`, `IsDockedMode()`, `cold_hint_mode`, `GetDebugKnobAt(1)`.
- Environment: `EDEN_NX2_PRIVATE_DEVICE_RANGES`, `EDEN_NX2_SPARSE_REMAP`, `EDEN_NX2_SPARSE_LAZY`, `EDEN_NX2_GPU_ONLY_LAZY`, `EDEN_NX2_FRAMES_AHEAD`, `EDEN_NX2_BACK_WAIT`, `EDEN_NX2_PRESENT_QUEUE`, `EDEN_NX2_SWAP_INTERVAL`, `EDEN_NX2_VSYNC_HZ`, `EDEN_NX2_CROP_WIDTH`/`HEIGHT`, `EDEN_NX2_FRAME_CAPTURE`, `EDEN_NX2_CAPTURE_SIZE`, `EDEN_NX2_FLASH_WATCH`, `EDEN_NX2_DUMP`, `EDEN_NX2_PB_CHECK`, `EDEN_NX2_PB_SNAPSHOT`, `EDEN_NX2_VDEC_PACE`, `EDEN_NX2_VDEC_TRACE`, `EDEN_NX2_VDEC_DUMP`, `EDEN_NX2_VDEC_EOS_RELEASE_NOW`, `EDEN_VI2_MAX_RESOLUTION_MODE`. Most are read once into function-local statics.

## Invariants
- `MaxNvEvents = 64`, `MaxSyncPoints = 192`: slot checks exist in `IocCtrlEventWait` after the slot is chosen, but not for the scope-exit write (assumed, see findings).
- Handles on the unmap queue are unpinned (enforced by `refs`/`pins` counters under `handle->mutex`).
- BufferQueue slots only change under `core->mutex`; `is_abandoned` is checked after every wait.
- `INx2Vi2Session` releases through a per-session guest syncpoint; its destructor forces the counter to `m_release_value` (assumes the guest value never exceeds it).
- A GPU-thread lambda may run after the session that queued it is gone. Nx2 code captures raw `gmmu`/`mapper` pointers and assumes the GPU queue drains before teardown (assumed only).
- `SharedBufferMaxSessions = 2` assumes distinct slot bases per session (not enforced; see findings).

## Hazards and improvement candidates
- `NvMap::PinHandle` can spin forever or read an empty list when SMMU space runs out.
- `IocCtrlEventWait` writes `events[event_id]` out of bounds on an early return.
- `SharedBufferSession::presentation_slot_base` is never set; a half-built session survives a failed `CreateSession`.
- Long blocking waits on service threads (`DequeueBuffer`, `WaitHost`, `PresentClientBuffer`) and composition under `vi::Container::m_lock`.
- `Nx2GpuMemoryRegistry::TrimMappings` is O(n) per map/unmap.
- Skimmed or unread: `nvhost_gpu.cpp`, `nvhost_ctrl_gpu.cpp`, the decoders, vi display services, caps, and `INx2GraphicsSystemSession`.
