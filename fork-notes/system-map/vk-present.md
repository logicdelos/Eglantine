# Vulkan presentation, upscaling and frame generation

**Purpose.** This subsystem takes the guest framebuffers that the GPU front end hands to the renderer, draws them (with optional anti-aliasing, scaling filter and ReShade-style effect chain) into a window-sized frame image, optionally synthesises extra frames (LSFG or FSR3-style interpolation), and delivers every frame to the Vulkan swapchain, either from the render thread or from a dedicated present thread. It also owns swapchain creation and recreation, present-mode choice, frame pacing and a "turbo" helper that keeps the GPU clocked up. All paths below are under `src/video_core/` unless stated.

## Files

| file | role | read depth |
|---|---|---|
| `renderer_vulkan/renderer_vulkan.{h,cpp}` | `RendererVulkan`: `Composite`, applet capture, screenshot, member order | full |
| `renderer_vulkan/vk_present_manager.{h,cpp}` | `Frame` pool, present queue, present thread, `CopyToSwapchain` | full |
| `renderer_vulkan/vk_swapchain.{h,cpp}` | `Swapchain`: create, acquire, present, present mode, semaphores | full |
| `renderer_vulkan/vk_blit_screen.{h,cpp}` | `BlitScreen`: owns window-adapt pass and `Layer` list, rebuild policy | full |
| `renderer_vulkan/vk_turbo_mode.cpp` | second `VkDevice` that spins compute work while the scheduler submits | full |
| `renderer_vulkan/vk_fsr3_upscaler.cpp` | FSR 3.1 upscaler used by the texture cache (not present path) | full |
| `renderer_vulkan/present/layer.{h,cpp}` | per-guest-layer draw setup: raw image upload, AA, post chain, scaler | full |
| `renderer_vulkan/present/window_adapt_pass.cpp` | final composite of layers into the frame | partial (`Draw`) |
| `renderer_vulkan/present/post_process.{h,cpp}` | `PostProcessChain`: runs `FxChain` effects | full |
| `renderer_vulkan/present/frame_gen.cpp`, `frame_gen_pacer.{h,cpp}` | `FrameGen` orchestration and output pacing | full |
| `renderer_vulkan/present/lsfg_chain.{h,cpp}`, `lsfg_generate.cpp`, `lsfg_common.cpp` | LSFG passes and descriptor helpers | partial |
| `renderer_vulkan/present/fsr3_frame_gen.cpp` | optical-flow plus interpolation compute path | full |
| `renderer_vulkan/present/util.cpp`, `fsr.cpp` | image/shader helpers; FSR1 EASU/RCAS present filter | partial |
| `renderer_vulkan/present/fsr3.cpp`, `fxaa.cpp` | FSR1-style filter named "FSR3"; FXAA | skimmed |
| `frame_gen/lossless_dll.cpp` | loads shader resources from a user-installed Lossless Scaling DLL, cache file | full |
| `post_processing/fx_chain.{h,cpp}` | global `FxChain` singleton (entries, per-uniform overrides, generation counter) | partial |
| `present/smaa.cpp`, `sgsr.cpp`, `buf.cpp`, `filters.cpp`, `lsfg_alpha/beta/gamma/delta/mipmaps/shaders.cpp`, `post_processing/fx_effect.cpp`, `fx_preset.cpp`, `fx_compile.cpp`, `frame_gen/lsfg_translate.cpp`, `renderer_null/*`, `renderer_base.*`, `video_core.cpp`, `framebuffer_config.*` | | not opened |

## Main types and ownership

- `RendererVulkan` declares in this order: `device`, `memory_allocator`, `state_tracker`, `scheduler`, `swapchain`, `present_manager`, `blit_swapchain`, `blit_capture`, `blit_applet`, `rasterizer`, `frame_gen` (`HAS_LSFG`), `turbo_mode`, `applet_frame`. Destruction is the reverse. The destructor body registers an empty on-submit callback and calls `vkDeviceWaitIdle`.
- `Frame` (`vk_present_manager.h`): image, view, optional storage view, framebuffer, command buffer, `render_ready` semaphore, `present_done` fence (created signalled), and frame-generation bookkeeping (`is_generated`, `generation_index/count`).
- `PresentManager` owns `frames` (a vector sized once from the initial `image_count`, never resized), `free_queue`, `present_queue`, a command pool, and a `std::jthread` that exists only if `async_presentation` is on. `MAX_FRAMES_IN_FLIGHT = 7`. `SetImageCount()` recomputes `image_count` after each swapchain recreation, but the pool size stays as built.
- `Swapchain` keeps per-image `render_semaphores` and `resource_ticks`, and per-slot `present_semaphores`.
- `BlitScreen` (three instances: present, capture, applet) owns one `WindowAdaptPass` and a `std::list<Layer>`. Each `Layer` owns its raw images, staging buffer, descriptor sets, an AA variant (`FXAA`/`SMAA`), a scaler variant (`SGSR`/`FSR`/`FSR3`/`BUF`) and an optional `PostProcessChain`.
- `FrameGen` owns either an `LsfgChain` (input image pair, mipmaps, alpha/beta/gamma/delta/generate passes, sampler and uniform caches) or an `Fsr3FrameGen`, plus a `FrameGenPacer` and the current plan.
- `TurboMode` creates its own `VkDevice` and a jthread.

## Threads and locking

- **Render (GPU) thread** runs `Composite`, `Layer::ConfigureDraw`, `FrameGen::Process` and all descriptor writes.
- **Scheduler worker** executes recorded lambdas, including the lambda that pushes a frame into `present_queue` and the LSFG dispatch lambdas.
- **Present thread** ("VulkanPresent", high priority) pops `present_queue`, runs `CopyToSwapchain`, then returns the frame to `free_queue`. In sync mode (`!async_presentation`) `Present()` does `scheduler.WaitWorker()` and `CopyToSwapchain` inline on the worker's caller.
- **TurboMode thread** submits a small compute dispatch while a scheduler submit happened in the last 100 ms.
- Locks: `queue_mutex` (`present_queue`, `presenting_frame`, `wake_present_hold`; cv `frame_cv`), `free_mutex` (`free_queue`; cv `free_cv`), `swapchain_mutex` (held for the whole of `CopyToSwapchain`), `Scheduler::submit_mutex` (every queue submit and present). Order in `PresentThread`: `queue_mutex`, then `swapchain_mutex` (handed over with `std::exchange`), then `free_mutex` after the frame is done. `WaitPresent` waits for an empty queue and no frame in flight, then takes `swapchain_mutex`.
- `GetRenderFrame` blocks on `free_cv` then on the frame's `present_done` fence with no timeout. `TryGetRenderFrame` never blocks.
- `Swapchain::AcquireNextImage(may_flush)` also sleeps for frame pacing through `scheduler.Wait(tick, target_fps, may_flush)`. On the present thread `may_flush` is false.

## Flow

1. `RendererVulkan::Composite(framebuffers)`: `RenderAppletCaptureLayer`; return if `!render_window.IsShown()` (Qt: not minimised); `RenderScreenshot`; `present_manager.GetRenderFrame()`.
2. `blit_swapchain.DrawToFrame`: decides whether to rebuild (scaling filter, swapchain image count, view format or frame size changed) with `WaitIdle` = `WaitPresent` + `scheduler.Finish` + `vkDeviceWaitIdle`, then `window_adapt->Draw`.
3. `WindowAdaptPass::Draw` calls `Layer::ConfigureDraw` for each layer: `AccelerateDisplay`, `RefreshResources`, `SetAntiAliasPass`, `SetPostProcessPass`, wait on that image slot's tick, `UpdateRawImage` (CPU path), FXAA/SMAA, the post chain, one scaler, push constants, descriptor update. It then records the composite render pass.
4. With `HAS_LSFG`: `frame_gen.WantedGenerations(...)`, `frame_gen.Process(frame, ...)` copies the frame into the chain input and dispatches the shared passes. `Process` rebuilds the chain (with `scheduler.Finish`) when extent, format or flow scale changes. For each planned generation: `TryGetRenderFrame`, `PrepareFrame`, `GenerateInto`, `scheduler.Flush(render_ready)`, `Present`. Generated frames are submitted before the real one.
5. `scheduler.Flush(*frame->render_ready)`; `Present(frame)` records a worker lambda that pushes onto `present_queue` and notifies; `scheduler.DispatchWork()`; `gpu.RendererFrameEndNotify()`; `rasterizer.TickFrame()`.
6. Present thread: `CopyToSwapchainImpl` recreates the swapchain on `NeedsRecreation()` or size change, loops on `AcquireNextImage` while out of date, records barriers and a blit (copy if blit is unsupported) from the frame image to the swapchain image, submits under `submit_mutex` waiting on the acquire semaphore and `render_ready`, signals `render_semaphore` and `present_done`, then `Swapchain::Present`. VRR pacing (`smoothed_base_period_us`, spin with yield) and a once-per-second `PresentCadence` log live here.
7. `CopyToSwapchain` retries up to 3 times only for `VK_ERROR_SURFACE_LOST_KHR`, then `DiscardFrame`.
- Present mode (`ChooseSwapPresentMode`): Fifo or FifoRelaxed when frame generation is on; otherwise the user's `vsync_mode` if `use_speed_limit` and not turbo; otherwise Fifo promoted to Mailbox or Immediate. `NeedsPresentModeUpdate` re-evaluates every frame.
- LSFG shader source: `ParseShaderSpans` reads PE RCDATA resources (IDs 255, 256, 280-302) from the DLL in `GetEdenPath(LosslessDir)`, converts to SPIR-V, and caches them (magic `0x4746534C`, version 3, CityHash64 of the DLL).
- `FrameGenPacer::Plan` uses a smoothed frame interval, burst detection, an output credit accumulator and a probe/backoff search for the sustainable rate (backoff 5/15/30/60 s).

## Connections

- In: Qt `GRenderWindow` (`IsShown`, `OnFrameDisplayed`, `GetFramebufferLayout`); `Tegra::GPU` calls `Composite` and receives `RendererFrameEndNotify`.
- Out: `RasterizerVulkan::AccelerateDisplay`/`TickFrame`; `Scheduler` (`Record`, `Flush`, `Finish`, `Wait`, `WaitWorker`, `DispatchWork`, `RegisterOnSubmit`, `submit_mutex`); `MemoryAllocator`; `VideoCore::FxChain` (`Snapshot`, `Size`, `EntryValues`); `Common::FS` for the Lossless directory.
- `Fsr3Upscaler` (`vk_fsr3_upscaler.cpp`) is consumed by the texture cache (`vk_texture_cache.cpp`), not by this present path.

## Settings and switches

`Settings::values`: `async_presentation`, `vsync_mode`, `use_speed_limit`, `current_speed_mode` (turbo), `frame_pacing_mode` (Auto/30/60/90/120), `frame_gen`, `frame_gen_type` (LSFG or Fsr3), `frame_gen_queue_target`, `frame_gen_target_rate`, `frame_gen_flow_scale`, `frame_gen_flow_scale_auto`, `frame_gen_dump_flow`, `resolution_info` (scale), `bg_red/green/blue`, `fsr_sharpening_slider`, `post_shader_enabled`, `post_shader_chain`, `post_shader_preset`, `renderer_force_max_clock`, `renderer_debug`, `vulkan_device`; scaling filter and anti-aliasing come through `PresentFilters`. Helpers: `Settings::FrameGenMaxGenerations()`, `FrameGenGenerations()`. Compile-time: `HAS_LSFG`, `HAS_RESHADE`, `EGLANTINE_STREAMLINE`. No environment variables in this slice (`EDEN_NX2_FENCE_FLUSH_US` is in `vk_scheduler.cpp`).

## Invariants

- A frame taken with `GetRenderFrame` has a signalled-then-reset `present_done`; only then may its image be recreated. Enforced by the fence wait and `Reset`.
- Any object a `Layer` frees (raw images, post chain, AA pass) must not be in flight. Enforced for raw images and the post chain by waits on `resource_ticks`; not enforced for the AA pass or when the post chain is reset for applet layers (see findings).
- `frames.size()` equals the initial `image_count`; `image_count` later changes only the usable limit. Assumed.
- `queue_mutex` is never taken while holding `free_mutex`. Enforced by structure only.
- `vkQueueSubmit`/`vkQueuePresentKHR` are externally synchronised through `submit_mutex`. Enforced for the present manager and swapchain creation; `BlitScreen::WaitIdle` and the destructor call `vkDeviceWaitIdle` without it.
- Swapchain extent is non-zero when acquired. Assumed; `Swapchain::Create` returns early on zero extent and leaves the old swapchain.

## Hazards and improvement candidates

See `findings/vk-present.json`. In short: a possible endless recreate loop on a zero-size surface; AA pass and applet-layer post chain destroyed without waiting; every pooled frame triggers a full rebuild after a resize; per-frame heap copies in the FxChain/post-process path; `vkDeviceWaitIdle` and teardown without queue synchronisation; present-thread exceptions terminate the process; uninitialised `applet_frame` size.

Not findings but worth knowing: `Pass::backbuffer_slot` in `post_process.cpp` is set and never read; the pacing assumes all planned generations were queued although `TryGetRenderFrame` may return fewer; `fsr3_frame_gen.cpp` comments contain garbled characters; the "FSR3" present filter (`fsr3.cpp`) is an FSR1-style EASU/RCAS pass, unrelated to `vk_fsr3_upscaler.cpp`.
