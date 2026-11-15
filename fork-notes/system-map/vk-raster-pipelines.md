# Vulkan rasterizer and pipeline cache

**Purpose.** `RasterizerVulkan` turns Maxwell 3D draws and Kepler compute dispatches into Vulkan commands. For each draw it asks `PipelineCache` for a `GraphicsPipeline` that matches the current shaders and fixed-function state, binds resources through the pipeline's `Configure`, then records whichever dynamic states are dirty. `PipelineCache` also owns everything that makes pipelines cheap at run time: disk cache, background and asynchronous translation, a per-frame build budget and eviction. The Switch 2 fork added most of that machinery (Nx2Perf options, async translation, disk replay, mesh-shader emulation, shader precache).

## Files

| file | role | read depth |
|---|---|---|
| `renderer_vulkan/vk_pipeline_cache.{h,cpp}` (464 / 3640) | pipeline lookup, build, disk load, eviction, translation workers | h full; cpp about 90% (anonymous helpers and `MakeRuntimeInfo`, lines 1-375, skimmed) |
| `renderer_vulkan/vk_rasterizer.{h,cpp}` (420 / 4165) | draw/dispatch/clear, `PrepareDraw`, dynamic state, `TickFrame` | h full; cpp partial (draw path, `UpdateDynamicStates` and updaters, `TickFrame`; Clear, DispatchCompute, queries, texture/buffer glue skimmed) |
| `renderer_vulkan/fixed_pipeline_state.{h,cpp}` | pipeline key struct and `Refresh` from Maxwell registers | full |
| `renderer_vulkan/vk_graphics_pipeline.{h,cpp}` (362 / 1726) | pipeline object, async build, `Configure`, transitions | h mostly; cpp partial (ctor, `RunBuild`, transitions) |
| `renderer_vulkan/vk_compute_pipeline.{h,cpp}` | compute pipeline | cpp partial (ctor, `Configure` start) |
| `renderer_vulkan/vk_render_pass_cache.{h,cpp}` | `RenderPassKey` to `VkRenderPass` | skimmed |
| `renderer_vulkan/vk_state_tracker.{h,cpp}` | dirty-flag tables and `Touch*` helpers | cpp full; h grepped |
| `renderer_vulkan/vertex_repack.h` | 69 to 72 byte vertex repack rules | full |
| `video_core/shader_cache.{h,cpp}` | guest shader lookup, `RefreshStages`, invalidation | cpp full; h grepped |
| `video_core/shader_environment.{h,cpp}` | guest/file environments for the recompiler | not opened (only callers seen) |
| `video_core/shader_warm/*` | warm-up records and archives | not opened |
| `vk_streamline_dlss.cpp`, `vk_camera_motion_pass.cpp`, `vk_dlss_network_probe.cpp`, `vk_frame_dump.*`, `pipeline_cache_dir.*`, `pipeline_statistics.*`, `vk_driver_build_report.*`, `vk_descriptor_buffer.*`, `upload_batch.h`, `dirty_flags.*` | DLSS, frame dump, cache directory, statistics, helpers | not opened (call sites seen, `dirty_flags` grepped) |

## Main types and ownership

- `PipelineCache` (derives `VideoCommon::ShaderCache`) is owned by `RasterizerVulkan`. It owns every `GraphicsPipeline` in `graphics_cache` (`unique_ptr`, key `GraphicsPipelineCacheKey` = 6 shader hashes + `FixedPipelineState`). `compute_cache` holds `ComputePipelineCacheEntry` (pipeline, alternates, null-pointer slots, at most 4 rebuilds).
- Raw pointers into `graphics_cache` live in: `current_pipeline`, each pipeline's `transitions` (up to 16), `graphics_fast_cache` (256 slots, Nx2Perf `PipelineFastCache`), `skipped_geometry`, `used_this_session`. `EvictUnusedPipelines` is the only place that erases entries: it clears every transition list, the fast cache and the skipped table, and moves the pipeline to `evicted_pipelines`, destroyed once `scheduler.IsFree(tick)`.
- `FixedPipelineState` is the variable-length key part. `Size()` shrinks with dynamic-state support (`extended_dynamic_state`, `dynamic_vertex_input`, `extended_dynamic_state_3_blend`); hash and `operator==` cover only `Size()` bytes.
- `StateTracker` holds the Vulkan-side "last recorded" state and exchanges dirty flags in `maxwell3d->dirty.flags` (shared with `FixedPipelineState::Refresh`).
- `GraphicsPipeline` owns its layouts, descriptor allocator and `VkPipeline`; the driver compile runs from a `build_task` that either a worker or the GPU thread executes (`build_claimed` atomic, `is_built` + condvar).

## Threads and locking

- GPU thread: all lookups (`CurrentGraphicsPipeline`), adoption of finished pipelines, `TickFrame` (called from `RendererVulkan::Composite`), eviction. `frame_count` and `translation_frame` are plain `u64`; I found no caller on another thread (the comment at `WaitForGraphicsTranslation` claims one).
- `translation_workers` ("VkPipelineTranslator"): `TranslateGraphicsPipeline` runs the shader recompiler for a queued key; results go to `translated_graphics` under `translated_mutex` with `num_translated` as an atomic hint. The GPU thread may claim a queued task (`PendingGraphicsTranslation::claimed`) instead of waiting.
- `workers` ("VkPipelineBuilder"): driver compile via `GraphicsPipeline::RunBuild` / `ComputePipeline::RunBuild`. Disk load at boot queues onto the same workers and guards its own `state.mutex`.
- `disk_replay_workers` (2): rebuild skipped disk records on demand; results in `ready_disk_pipelines` under `ready_disk_pipelines_mutex`; `DrainReadyDiskPipelines` takes it with `try_to_lock`.
- `background_workers` (`VkPipelinePrebuild`): `background_ready`, `background_claimed`, `background_taken` under `background_mutex`.
- `serialization_thread` (1): `StoreGraphicsPipeline`, Vulkan pipeline cache flush. `precache_workers`: compute programs built only to warm the driver cache; the GPU thread waits up to 250 ms in `ClaimPrecacheProgram`.
- `ThreadWorker` does not catch exceptions from tasks (`common/thread_worker.h`), so an exception escaping a worker lambda ends the process.
- Lock order seen: `buffer_cache.mutex` then `texture_cache.mutex` in `PrepareDraw`; no lock is held across pipeline lookup except `texture_cache.mutex` in `DrawTexture`. `ShaderCache`: `invalidation_mutex` before `lookup_mutex`; `rejected_mutex` taken alone.

## Flow

1. `RasterizerVulkan::Draw` / `DrawIndirect` call `PrepareDraw`: `FlushWork`, then `PipelineCache::CurrentGraphicsPipeline`.
2. `CurrentGraphicsPipeline`: `RefreshStages` (re-reads shader hashes only if `Dirty::Shaders`), `graphics_key.state.Refresh` (copies registers; clears the shared flags `Dirty::ViewportSwizzles`, `Dirty::VertexInput` in non-dynamic mode and `Dirty::Blending` when blend is not dynamic), then `current_pipeline->Next(key)`, then the 256-slot fast cache, then `CurrentGraphicsPipelineSlowPath`.
3. Slow path: adopt translated pipelines, drain disk results, look in `graphics_cache`; on a miss either start a disk replay (draw skipped) or `BuildNewGraphicsPipeline`.
4. `BuildNewGraphicsPipeline`: wait for an earlier queued translation; else `TakeBackgroundPipeline`; else `DeferRuntimeBuild` (per-frame budget, default 24 ms, env `EDEN_NX2_SHADER_BUDGET_MS`); else `QueueGraphicsTranslation` + `WaitForGraphicsTranslation` (2 ms per pipeline, 4 ms per frame); else synchronous `CreateGraphicsPipeline` and `StoreGraphicsPipeline`.
5. `BuiltPipeline` decides whether a still-compiling pipeline skips its draw (`zeta_enable` draws and mesh draws skip; small draws skip only if the target already holds earlier output; Switch 2 native draws are not skipped).
6. Back in `PrepareDraw`: take buffer and texture cache mutexes, `pipeline->SetEngine`, `SetVertexRepackLayout`, `pipeline->Configure`, frame dump, `UpdateDynamicStates`, query cache, then the Vulkan draw call.
7. `UpdateDynamicStates` bit-casts the 32-byte dirty set to four `u64` words and ANDs it with constant masks, so a clean draw costs a few instructions. Updaters call `state_tracker.Touch*` and `scheduler.Record`.
8. Every frame `TickFrame` drains disk results, sorts deferred build candidates, increments `frame_count`, retires pipelines, and every 600 frames runs `EvictUnusedPipelines` (unused for 9000 frames; never-used preloaded pipelines are kept).
9. Boot: `LoadDiskResources` reads `vulkan.mythos.bin` (graphics/compute records with their environments), queues builds, optionally defers the rest to background workers, then `StartShaderPrecache`.

## Connections

- In: `RendererVulkan::Composite` (`TickFrame`), the GPU channel code (`CreateChannel`/`BindToChannel`), `Maxwell3D` draw manager (`Draw`, `DrawIndirect`, `DrawTexture`), `KeplerCompute` (`DispatchCompute`), memory invalidation callbacks (`InvalidateRegion`, `OnCacheInvalidation`).
- Out: shader recompiler (`Shader::Maxwell::TranslateProgram`, `EmitSPIRV`), `TextureCache` (`UpdateRenderTargets`, `SynchronizeDescriptors`, `ColorTargetHasPriorFrameOutput`), `BufferCache` and `BufferCacheRuntime` (`SetVertexRepackLayout`, bind calls), `Scheduler` (`Record`, `IsFree`, `CurrentTick`), `RenderPassCache::Get`, `DescriptorPool`, `QueryCache`, `FenceManager`.
- Shared state: `maxwell3d->dirty.flags` is read by `FixedPipelineState::Refresh`, `StateTracker`, `BufferCache` and `TextureCache`.

## Settings and switches

- `Settings::values`: `use_asynchronous_shaders`, `use_vulkan_driver_pipeline_cache`, `dyna_state`, `vertex_input_dynamic_state`, `use_squashed_iterated_blend`, `dump_guest_shaders`, `gpu_log_shader_dumps`, `renderer_debug`, `CurrentProgramIsSwitch2()`.
- `VideoCommon::Nx2Perf::Opt`: `PipelineFastCache`, `ComputeRebuildAsync`, `ComputePreloadVariants`, `ShaderBuildBudget`, `BuildOrderPriority`, `StaleTargetSkip`, `GeometryConsistency`, `PipelineRetention`, `PipelineKeepPreloaded`, `ReplayDiskRecordings`, `MacroReadWriterSet`.
- Environment: `EDEN_NX2_ASYNC_TRANSLATION`, `_PARALLEL_STAGES`, `_ASYNC_TRANSLATION_WAIT_MS`, `_ASYNC_TRANSLATION_FRAME_MS`, `_TRANSLATION_THREADS`, `EDEN_NX2_SHADER_BUDGET_MS`, `EDEN_NX2_BACKGROUND_PIPELINES`, `_BACKGROUND_PIPELINE_THREADS`, `EDEN_NX2_PRELOAD_ALL_PIPELINES`, `EDEN_NX2_PIPELINE_DIFF`, mesh options (`EDEN_NX2_MESH_EMULATION_DUMP`, `_PROBE`, `EDEN_NX2_NO_MESH`), tracing (`EDEN_NX2_TRACE_VS`, `EDEN_NX2_RT_TRACE`, `EDEN_NX2_FRAME_DUMP`, `EDEN_NATIVE_DRAW_CAPTURE`), `EGLANTINE_EXPERIMENTAL_STATIC_VERTEX_REPACK`, `EGLANTINE_TEST_VERTEX_REPACK`, `EGLANTINE_SYNC_FIX`, `EGLANTINE_AMD_FRAGMENT_SHUFFLE`.
- On the owner's NVIDIA driver `use_vulkan_pipeline_cache` is off (the driver caches pipelines itself), so only the Eglantine record file is used.

## Invariants

- A cached pipeline's key bytes up to `Size()` equal the key it was looked up with. Enforced by `operator==`; the `Size()` rule is only assumed to match which registers `Refresh` writes.
- Pointers to a pipeline are dropped before it is erased (transition lists, fast cache, `skipped_geometry` cleared in `EvictUnusedPipelines`; `current_pipeline` is never evicted). Enforced in that function only.
- Dirty flags consumed by `Refresh` are not needed by the dynamic-state code: the flags it clears are only those whose state is static in that mode. Assumed.
- `shader_infos[]` stay valid between `RefreshStages` calls. Assumed: nothing sets `Dirty::Shaders` when `RemovePendingShaders` frees a `ShaderInfo` (see findings).
- An evicted pipeline outlives its GPU use: `evicted_pipelines` waits for `scheduler.IsFree(tick)`.
- Disk keys loaded from file only keep entries whose dynamic-feature bits match the device (`load_graphics` check).

## Hazards and improvement candidates

- Background prebuild and a draw that needs the same pipeline can cancel each other: the draw marks the key claimed even when it then defers.
- `ShaderInfo` freed by invalidation while `shader_infos[]` and `unique_hashes` are reused without a refresh.
- Alpha-to-coverage and alpha-to-one updaters re-run the whole pipeline lookup (with `is_indexed=false`), also from `DrawTexture`.
- Blend-enable for integer targets depends on render-target formats but only updates on blend register writes.
- Disk replay and boot workers do not catch non-`Shader::Exception` errors.
- Not covered: `vk_rasterizer.cpp` Clear/Dispatch/query code, `shader_environment`, DLSS/camera motion, shader warm-up files, and `GraphicsPipeline::ConfigureImpl`.
