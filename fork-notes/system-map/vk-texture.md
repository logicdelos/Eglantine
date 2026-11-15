# Vulkan Texture Cache

**Purpose.** The texture cache maps guest texture and render-target memory (found through GPU virtual addresses and TIC/TSC descriptors) to host Vulkan images, image views, samplers and framebuffers. It uploads guest data (unswizzle, ASTC/BC decode or transcode), writes GPU-modified images back for guest reads, resolves overlapping or aliased images, rescales for resolution scaling, and keeps total device memory inside a budget with a garbage collector. The generic logic is the `TextureCache<P>` template; the Vulkan side (`TextureCacheRuntime`, `Image`, `ImageView`, `Sampler`, `Framebuffer`) is `vk_texture_cache.cpp`. The fork adds Switch 2 sparse images, feedback-loop shadows, adaptive memory thresholds and DLSS/FSR hooks.

## Files

| file | role | read depth |
|---|---|---|
| `texture_cache/texture_cache_base.h` | class layout, slot vectors, page tables, queues | full |
| `texture_cache/texture_cache.h` (4523 lines) | all template logic: lookup, join, upload, GC, downloads, render targets | partial (about 85%; 1474-1740 DLSS/upscale and 2334-2552 sparse helpers only skimmed) |
| `texture_cache/pending_download.h` | download retention for deleted images | full |
| `common/thread_worker.h` | worker used for async decode | full |
| `renderer_vulkan/vk_texture_cache.cpp` (4901) | runtime, Image, ImageView, Sampler, Framebuffer | partial (150-483, 1262-1566, 2596-3420, 3888-4901) |
| `renderer_vulkan/vk_texture_cache.h` | declarations | partial (1-123, 280-320) |
| `texture_cache/util.cpp` | unswizzle plans, layout maths | partial (1123-1215) |
| `texture_cache/image_base.cpp`, `surface.cpp` | image bookkeeping, `TranscodedAstcSize` | skimmed |
| `image_base.h`, `descriptor_table.h`, `blit_image.cpp`, `maxwell_to_vk.cpp`, `textures/workers.cpp` | not opened except by grep | skimmed or not opened |
| `astc.cpp`, `decoders.cpp`, `smaa_area_tex.h` and other decoder or table files | CPU decoders, data tables | not opened |

## Main types and ownership

- `TextureCache<P>` derives from `ChannelSetupCaches<TextureCacheChannelInfo>` and owns everything below. `P` is `Vulkan::TextureCacheParams`.
- `Common::SlotVector` stores for `slot_images`, `slot_image_views`, `slot_map_views`, `slot_image_allocs`, `slot_samplers`, `slot_framebuffers`, `slot_buffer_downloads`. IDs (`ImageId` etc.) are indexes; slots are recycled after erase.
- `page_table`: 1 MiB CPU pages (`YUZU_PAGEBITS=20`) to `ImageMapId`. Per channel: `gpu_page_table` and `sparse_page_table` (in `gpu_page_table_storage`), `image_views` and `samplers` maps keyed by raw `TICEntry`/`TSCEntry`. `image_allocs_table` is keyed by `gpu_addr`.
- `lru_cache` (`ImageId` by tick), `framebuffers` keyed by `RenderTargets`, `image_view_descriptors` (fork reverse map view to channel + TIC).
- Deleted images are not destroyed at once. With a runtime that has `CurrentTick`/`IsFree` (Vulkan) they go to `gpu_sentenced_images` with their byte size and are destroyed when the GPU has passed that tick; otherwise `sentenced_images` (`DelayedDestructionRing`, 8 ticks). Views and framebuffers use `sentenced_image_view` and `sentenced_framebuffers`. An image with queued downloads is moved into a `shared_ptr` by `RetainPendingImageDownloads` and lives until `PopAsyncFlushes`.
- Feedback shadows (`feedback_shadows`) are fork-only copies of a colour target sampled while bound as target; they are not in page tables or the LRU and are dropped after 120 idle frames.
- `Vulkan::Image` holds `original_image`, optional `scaled_image`, `current_image` (member pointer), sparse state (`SparseLayerMemory`), storage views. `TextureCacheRuntime` owns scratch resources: temporary buffers, MSAA scratch images (freed after 60 idle frames), `free_transcodes`/`pending_transcodes` (free list capped at 256 MiB), resolve shadows, the compute passes (ASTC decoder, BC encoder, BL3D unswizzle) and DLSS/FSR glue.

## Threads and locking

- All cache methods run on the GPU thread. The public `Common::SpinRecursiveMutex mutex` is taken by the rasterizer around cache calls; `ColorTargetHasPriorFrameOutput` locks it itself. Nothing inside the template locks it otherwise.
- One worker thread, `texture_decode_worker` ("TextureDecoder", efficiency cores), runs `ConvertImage` CPU decodes. The GPU thread creates an `AsyncDecodeContext` (in `async_decodes`), the worker fills `decoded_data`/`copies` and sets `complete` under `AsyncDecodeContext::mutex`; `TickAsyncDecode` (GPU thread) checks `complete` under the lock, releases it, then uploads or drops. `cancelled` is set by `DeleteImage` and honoured in `TickAsyncDecode`.
- `EDEN_NX2_TRANSCODE_THREADS` (`textures/workers.cpp`) sizes a separate transcode pool.
- Lock order: cache `mutex` first, then the context mutex; the worker never takes the cache mutex.

## Flow

1. Draw/compute setup calls `SynchronizeGraphicsDescriptors`/`FillGraphicsImageViews`; each TIC is resolved by `VisitImageView` → `FindImageView` → `CreateImageView` → `FindOrInsertImage`.
2. `FindOrInsertImage` → `FindImage` (CPU page table, then GPU page table, then `image_allocs_table` for stand-in addresses `~(1<<40)+gpu_addr`) → `InsertImage` → `JoinImages`.
3. `JoinImages` resolves overlaps (`ResolveOverlap`: copy, alias, bad overlap), skips the upload when an alias copy or clear covers the image, then `RegisterImage` (adds `ImageMemoryBytes` to `total_used_memory`, tracks pages, LRU).
4. `RefreshContents` (before use of a CpuModified image): `UploadImageContents` unswizzles into a staging buffer. Variants: `UploadInChunks` for big 2D images, `UploadsByLayer` (sparse layers), accelerated upload via `runtime.AccelerateImageUpload` (GPU ASTC decode, `TranscodeAstcToBcn`), `QueueAsyncDecode` for CPU ASTC, `QueueAsyncUnswizzle` for large BC 3D (`TickAsyncUnswizzle`).
5. Render targets: `UpdateRenderTargets` → `FindColorBuffer`/`FindDepthBuffer` → `RescaleRenderTargets` → `GetFramebuffer` (cached by `RenderTargets`, validated by `render_targets_serial`). `BindRenderTarget` queues preemptive downloads. `CheckFeedbackLoop` / `ReplaceColorFeedbackViews` handle sample-while-render.
6. Blits and copies: `BlitImage` → `GetBlitImages` → `runtime.BlitImage`; `CopyImage` picks `runtime.CopyImage`, `ReinterpretImage` or `ConvertImage`. `TemporalUpscale`/`CameraMotionUpscale` are the DLSS paths.
7. Scaling: `ScaleUp`/`ScaleDown` create `scaled_image` lazily and blit; `InvalidateScale` drops views.
8. `TickFrame` (once per frame): advance `frame_tick`, run `TickAsyncDecode`/`TickAsyncUnswizzle`, destroy sentenced items, re-read the driver budget, adjust thresholds, run `RunGarbageCollector` when `total_used_memory > minimum_memory`.
9. Download: `DownloadMemory`/`CommitAsyncFlushes` (dedup by `DeduplicatePendingImageDownloads`) → `PopAsyncFlushes` swizzles back in reverse order.

**Garbage collector.** `Configure` gives ticks_to_destroy 50/25/10 and iterations 10/20/40 for normal/high/aggressive priority. `Collect` orders candidates by the emptiest device-memory block. `Cleanup` skips `IsDecoding` images, and CostlyLoad images unless aggressive or older than `EDEN_NX2_TC_OLD_AGE` frames (1800). A GpuModified image is downloaded (with `runtime.Finish()`) and swizzled back before deletion.

**Budget.** The constructor takes expected/critical/minimum from `GetDeviceLocalMemory` (defaults 1 GiB+125 MiB / 1 GiB+625 MiB, 4 GiB target). `ApplyThresholdOverrides` caps critical at `max(5 GiB, budget/8*5)`. `TickFrame` every 30 frames sets `critical = budget - min(margin + pressure_margin + slack_margin, budget/2)`, `expected = critical - min(margin, critical/2)`, `margin = min(budget/8, 1 GiB)`. `pressure_margin` grows 512 MiB (max 3 GiB) per `MemoryPressureEvents()` and decays 128 MiB per 600 quiet frames; `slack_margin` comes from `GetDeviceMemorySlack` (max 1536 MiB). On Switch 2 native tiling the thresholds are raised to leave 256/1280 MiB free.

## Connections

- In: `RasterizerVulkan` (`vk_rasterizer.cpp`) for descriptors, render targets, clears, blits, `TickFrame`, `OnCacheInvalidation`/`UnmapMemory`; Fermi2D and DMA engines through `BlitImage`/`DmaImage*`; the present path reads images through `TextureCache::TryFindFramebufferImageView`.
- Out: `Tegra::MemoryManager` (`ReadBlock`, `GetSubmappedRange`), `MemoryTracker`/`device_memory` (`UpdatePagesCachedCount`), `Scheduler` (`Record`, `CurrentTick`, `Finish`), `StagingBufferPool`, `MemoryAllocator` (`CreateImage`, `MemoryPressureEvents`), `BlitImageHelper`, compute passes in `vk_compute_pass.cpp`, `RenderPassCache`, `Device` (`GetDeviceMemoryBudget`, sparse and format caps), `Nx2DrawStats`, `Nx2Profile`, `Tegra::Nx2Sparse*` (sparse written ranges), FSR3/DLSS upscalers.

## Settings and switches

`Settings::values`: `gpu_unswizzle_enabled` (default false), `gpu_unswizzle_texture_size`, `gpu_unswizzle_stream_size`, `gpu_unswizzle_chunk_size`, `barrier_feedback_loops`, `resolution_info` (`up_scale`, `down_shift`, `active`), `accelerate_astc` (Gpu/CpuAsynchronous/Cpu), `GetAstcRecompression()` (None/Bc1/Bc3).
Environment: `EDEN_NX2_TC_LOG`, `EDEN_NX2_TC_MIN_AGE`, `EDEN_NX2_TC_OLD_AGE`, `EDEN_NX2_TC_EXPECTED_MIB`, `EDEN_NX2_TC_CRITICAL_MIB`, `EDEN_NATIVE_DRAW_DIAGNOSTICS`, `EDEN_NX2_SPARSE_ALIASED`, `EDEN_NX2_ASTC_ARRAY_MIB`, `EDEN_NX2_ASTC_BC1`, `EDEN_NX2_KEEP_ASTC`, `EDEN_NX2_TRANSCODE_THREADS`, `EDEN_NX2_FORCE_LOD0`.

## Invariants

- A slot ID is valid only while its image is registered or sentenced; enforced by `ASSERT`s in `DeleteImage`, assumed in `TickAsyncUnswizzle` (not enforced, see findings).
- A deleted image must not be destroyed before the GPU is past its last use: enforced by `gpu_sentenced_images` with `IsFree(tick)`.
- A decode context is never freed while the worker may write it: enforced for deletion by `cancelled` + `complete` handshake; not enforced at shutdown (member order).
- `ImageView::image_handle` is the handle that was current at creation; `InvalidateScale` must run after any scale change so views are rebuilt: by convention.
- `total_used_memory` is an estimate reset from the driver each frame; per-image subtractions are clamped except the scaled one.
- Feedback shadows are only sampled, never rendered to: by construction.
- `unswizzle_queue` head is the only active task; `IsDecoding` guards double-queueing.

## Hazards and improvement candidates

See `findings/vk-texture.json`. Main points: stale `unswizzle_queue` entries after image deletion (only with GPU unswizzle on); decode worker declared before `async_decodes` (shutdown order); one `runtime.Finish()` per evicted GpuModified image in the GC; memory estimate for trimmed ASTC arrays and temporary buffers outside the budget; the always-false immediate-delete test; unbounded sampler cache.

Fixed in `700ab4e` and `2118911`: the stale unswizzle entries (the task of a deleted image is marked cancelled and dropped on the GPU thread) and the wait per evicted image (one `runtime.Finish()` per collection pass). An evicted image with a pending download stays tracked and registered until its bytes are in guest memory; a change to `RunGarbageCollector` must keep that order.

## Switch 2 (fork) code in this slice

`UsesNativeTiling` thresholds in `TickFrame`; sparse layers and tiles (`InitSparseLayers`, `InitSparseTiles`, `BindZeroBlock`, `BindLayers`, `SparseLayersCap` 384/768 MiB) for huge ASTC terrain arrays; `TrimmedHostInfo` drops top mips above `TrimmedAstcArrayLimit`; `IsOpaqueBc1Astc` and GPU ASTC to BC transcode with reuse pool; feedback shadows; MSAA resolve shadows for tilers; DLSS/FSR stand-ins. Not read in full: DLSS probe code at `texture_cache.h` 1474-1740 and sparse helpers 2334-2552.
