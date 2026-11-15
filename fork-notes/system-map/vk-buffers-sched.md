# Vulkan scheduler, buffer cache, fences and queries

**Purpose.** This slice records Vulkan commands on one thread and replays them on a worker thread. It tracks GPU progress with one timeline counter (the "tick"). It owns the pools that hand out command buffers, descriptor sets and staging memory, and it holds the buffer cache, the fence manager and the query cache. Nearly all guest-visible GPU memory traffic (uploads, downloads, query results, fences) passes through here. The Switch 2 (Nx2) additions are: deferred submission, batched fences, optional async-download gating, command-list page copies and a multi-range storage buffer cache.

## Files
| file | role | read depth |
|---|---|---|
| renderer_vulkan/vk_scheduler.h/.cpp | command chunks, worker thread, submit, wait, deferred flush | full |
| vk_master_semaphore.h/.cpp | tick counter, timeline or fence-based GPU progress | full |
| vk_resource_pool, vk_command_pool | tick-guarded reuse of command buffers | full |
| vk_descriptor_pool, vk_update_descriptor | descriptor banks and sets; per-frame payload ring | full |
| vk_staging_buffer_pool.h/.cpp | 128 MiB stream buffer and size-class staging caches | full |
| vk_fence_manager.h/.cpp, video_core/fence_manager.h | guest fences, fencing thread | full |
| vk_buffer_cache.cpp | `BufferCacheRuntime`, `Buffer`, quad index buffers | full |
| buffer_cache/buffer_cache_base.h | `BufferCache` members, Nx2 list-page state | full |
| buffer_cache/buffer_cache.h | cache logic (3687 lines) | partial: 60-460, 800-970, 1165-1285, 2517-2780, 2973-3687 |
| vk_query_cache.cpp | sample, transform feedback and primitive streamers, conditional rendering, `SyncValues` | partial: 36-1103 and 1372-1719; primitives streamer skimmed |
| query_cache/bank_base.h, query_stream.h | bank pool, simple streamer | full |
| vk_multi_range_buffer.cpp/.h | Nx2 sparse or gathered multi-range storage buffers | partial: header and 174-352 |
| gpu_split.h | split front/back thread ownership | partial: 59-135 |
| vk_compute_pass.cpp/.h | compute passes used by the buffer and query caches | skimmed (function index only) |
| vk_buffer_cache.h; buffer_cache.h 460-800 and 1285-2517; word_manager.h, memory_tracker_base.h, usage_tracker.h, uniform_stream_reuse.h, virtual_range_cache.h, read_trap_tracker.h, storage_buffer_size.h; query_cache.h and query_cache/query_cache.h, query_base.h | binding and tracking details | not opened |

## Main types and ownership
- `Scheduler` owns the worker thread, `MasterSemaphore`, `CommandPool`, the chunk queue and the render-pass state. `RasterizerVulkan` owns the scheduler and the pools below, and passes references to the caches.
- `CommandChunk` is a 0x8000-byte arena of placement-new `TypedCommand`s. Spent chunks go to `chunk_reserve`.
- `MasterSemaphore`: `current_tick` starts at 1 and `NextTick()` returns the old value. A submission signals that old value. `IsFree(t)` is `t <= gpu_tick`. `IsSubmitted(t)` is `t < CurrentTick`. On the RTX path the GPU tick is the timeline counter. Without timeline semaphores a wait thread and a deque of fences are used.
- `ResourcePool` stamps each entry with `CurrentTick()` at commit and reuses it once that tick is free. `CommandPool` grows by 4 buffers at a time. `DescriptorAllocator` grows by 16 sets. Neither ever shrinks. `DescriptorBank`s are chosen by `DescriptorBankInfo` and are never freed.
- `UpdateDescriptorQueue` is a ring of 8 frames of payload entries (guest 0x80000, compute 0x20000).
- `StagingBufferPool` owns the stream buffer (16 regions of 8 MiB, `sync_ticks[16]`) and per-log2-size caches for DeviceLocal, Upload and Download. Deferred entries carry tick = max until `FreeDeferred`. `TickFrame` releases up to 16 idle entries of one level.
- `BufferCache<P>` owns `slot_buffers`, the page table (`CACHING_PAGEBITS` 16), the LRU, `gpu_modified_ranges`, async download queues and the Nx2 list-page map. `BufferCacheRuntime` (Vulkan side) owns the quad index buffers, compute passes, `MultiRangeBufferCache` and a null buffer. Dead buffers go to `delayed_destruction_ring` (8 frames).
- `FenceManager` owns `GPUFencingThread` and its fence queue. The query cache owns the streamers; each bank is a `Vulkan` query pool or a counter buffer.

## Threads and locking
- Recording thread: the GPU thread, or the split's back thread (`GpuSplit::OwnsScheduler()`). It runs the rasterizer, buffer and texture caches, staging pool, descriptor queue and query streamers. None of those has its own lock for this; single-thread use is the rule.
- Worker (`VulkanWorker`): pops chunks and executes them. `queue_mutex` guards the queue; `execution_mutex` is held while a chunk runs (taken before `queue_mutex` is released, so `WaitWorker` can drain then lock); `reserve_mutex` guards `chunk_reserve`; `submit_mutex` wraps `SubmitQueue`.
- Fencing thread: waits on the fence, then takes `texture_cache.mutex` and `buffer_cache.mutex` (separately) to pop async flushes, runs query `PopAsyncFlushes` and the fence callbacks. `SignalFence` takes both mutexes via `scoped_lock{texture_cache.mutex, buffer_cache.mutex}` and holds `FenceManager::guard`.
- `BufferCache::mutex` is a spin recursive mutex taken by the rasterizer around cache calls. Streamers use `flush_guard` (samples, transform feedback) and `guard` (`SimpleStreamer`).
- Nx2: `nx2_list_mutex` guards `nx2_list_pages` between the split's front thread (reads copies) and the back thread. `nx2_list_copy_requests` and `nx2_retired_list_copies` are touched only under `buffer_cache.mutex`.
- Lock order seen: `buffer_cache.mutex` then `nx2_list_mutex`; `guard` (fence manager) then cache mutexes in `SignalFence`.

## Flow
1. Draw or compute code calls `Scheduler::Record(lambda)`. The lambda is placement-new'd into the current chunk. When the chunk is full, `DispatchWork` queues it and wakes the worker if `worker_waiting`.
2. Uploads: `BufferCache::SynchronizeBuffer` → `UploadMemory`/`MappedUploadMemory` → `StagingBufferPool::Request(Upload)`. Sizes up to 8 MiB use `GetStreamBuffer`; if the next regions are still in flight it falls back to `GetStagingBuffer`.
3. `runtime.CanReorderUpload` (region not used this tick, `Settings::values.disable_buffer_reorder` off) puts the copy into the upload command buffer through `RecordWithUploadBuffer`. Otherwise `RecordUpload` batches stream copies, or `Record` adds barriers.
4. Binding: `BindIndexBuffer`, `BindVertexBuffers`, uniform and storage binds go through `UpdateDescriptorQueue`. Quad and uint8 index paths use compute passes.
5. `Scheduler::SubmitExecution`: `FlushUploads`, `EndPendingOperations` (closes the ZPass counter, ends the render pass), `InvalidateState`, `NextTick`, then a recorded lambda ends both command buffers, runs `on_submit`, locks `submit_mutex` and calls `MasterSemaphore::SubmitQueue`. `vk::Check` failures throw on the worker, which ends the process.
6. Nx2 deferral: `DeferFlush` holds a flush for `EDEN_NX2_FENCE_FLUSH_US` (default 500 us); the back's idle hook calls `FlushDeferred`. Other threads use `WaitSubmitted`, which has no limit but logs every 2 s.
7. Waiting: `Scheduler::Wait(tick, ...)` flushes if the tick is still open, then `MasterSemaphore::Wait` (refresh, then a blocking timeline wait). The present thread passes `may_flush=false` and polls every 20 ms.
8. Fences: `FenceManager::SignalFence` → `CommitCacheFlushes` → `QueueFence`. `InnerFence::Queue` records `wait_tick` and flushes unless deferred. The fencing thread waits on up to 64 queued fences (`EDEN_NX2_FENCE_BATCH`), calls `PopAsyncBuffers`, writes guest memory, then runs fence operations.
9. Downloads: `CommitAsyncFlushesHigh` queues copies into deferred staging buffers. `PopAsyncBuffers` (fence thread) copies into guest memory and moves the buffer to `async_buffers_death_ring`; the recording thread frees it in `BufferCache::TickFrame`. For Nx2 titles `AsyncBufferDownloads()` is off by default and read traps flush on CPU reads instead.
10. Per frame: `RasterizerVulkan::TickFrame` → `StagingBufferPool::TickFrame`, `UpdateDescriptorQueue::TickFrame`, `BufferCache::TickFrame`, which advances `frame_tick`, runs the GC when memory is high, and frees dead buffers.
11. Queries: `SamplesStreamer` begins and ends host queries, closes the counter at every submission, resolves into buffers (`PresyncWrites`, prefix-scan pass). `PopUnsyncedQueries` on the fence thread reads results with `VK_QUERY_RESULT_WAIT_BIT`. `SyncValues` writes final values into guest-visible buffers.

## Connections
- Called by: `RasterizerVulkan` (draws, dispatches, `TickFrame`, `FlushSplitIdle`, `SubmitNx2ListCopies`, `ReadGpuWrittenCommandList`), `TextureCache` (staging pool, scheduler), the memory manager (`WriteMemory`, `InvalidateRegion` callbacks), and the GPU split (`GpuSplit::Get()`).
- Calls out to: `vulkan_common` (`Device`, `MemoryAllocator`, `vk::` wrappers), `Maxwell3D` registers (query and transform-feedback state), the device memory manager for guest reads and writes, and `Nx2Perf`/`Nx2Profile` for fork switches.

## Settings and switches
- `Settings::values`: `use_speed_limit` (frame pacing in `Wait`), `renderer_debug` (semaphore debug thread), `disable_buffer_reorder`, `gpu_log_vulkan_calls`; accuracy helpers `IsGPUFenceBehavior*` and `IsGPULevelHigh`.
- Environment: `EDEN_NX2_FENCE_FLUSH_US`, `EDEN_NX2_FENCE_BATCH`, `EDEN_NX2_BUFFER_DOWNLOADS`, `EDEN_NX2_DOWNLOAD_SMALL`, `EDEN_NX2_PREFLUSH`, `EDEN_NX2_INLINE_PAGE_WRITES`, `EDEN_NX2_SPLIT`, `EDEN_NX2_CMDPAGE_DRAIN`.
- `Nx2Perf::Opt`: `FenceFlush`, `DownloadBatch`, `PreflushAging`, `BindMemo`, `CleanWords`, `IndexRebind`.

## Invariants
- Staging pool, descriptor queue, buffer cache and streamers run on one thread. Only assumed; no assertion beyond `GpuSplit::check_owner` for the scheduler.
- A resource is reused only when `IsFree(tick)`. Enforced in `ResourcePool`, `StagingBufferPool` (`IsFree`, `AreRegionsActive`) and `MultiRangeBufferCache::DrainRetired`. `KnownGpuTick` can lag the real counter between `Refresh()` calls, so the checks are conservative.
- Buffer destruction is protected by 8 frames in `delayed_destruction_ring`, not by a tick. Assumed.
- `UpdateDescriptorQueue` frame reuse has no tick guard; it relies on at most 8 frames being in flight and on `AcquireOverflow` calling `WaitWorker`. Assumed.
- A deferred staging entry stays pinned until `FreeDeferred`. The `ASSERT`s in `FreeDeferred` check the entry exists and is deferred.
- Banks and queries are reused only after their references drop to zero and their tick is free. Enforced in `SamplesQueryBank::IsDead`.

## Hazards and improvement candidates
- `std::deque` growth on the recording thread races with unlocked indexing on the fencing thread in `BankPool::GetBank` and `SimpleStreamer::GetQuery` (findings 1 and 2). Fixed in `6d94d18` with one mutex for each deque.
- `StagingBufferPool::GetStreamBuffer` can build an inverted region range after a wrap fallback with an exactly 8 MiB request (finding 3).
- `MasterSemaphore::Wait` has no timeout for a tick that was never submitted (finding 4).
- `RunGarbageCollector` compares ticks as signed values, so the early-frame unsigned wrap is harmless (checked, not a defect).
- `BindHostGraphicsUniformBuffer` syncs a uniform binding of the maximum size 0x10000 although the guest maps only part of the range. This was the source of every `Unmapped Device ReadBlock` line. Since `f74d90d` that one sync reads with `ReadBlockSparse` (`SynchronizeBuffer(..., sparse_source)`); the README has the call stack and the addresses.
- Not examined: the buffer cache binding paths, `vk_compute_pass`, the primitives streamer and `word_manager` bookkeeping.
