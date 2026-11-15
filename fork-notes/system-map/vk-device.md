# Vulkan device, allocator and wrappers

**Purpose.** `src/video_core/vulkan_common/` creates the Vulkan instance, picks and opens the physical device, decides which extensions and features are enabled, and owns the VMA allocator and the memory-budget numbers the texture and buffer caches use. It also holds the thin RAII wrappers (`vk::Instance`, `vk::Device`, `vk::Image`, `vk::Buffer`, ...) every other Vulkan file uses. Fork additions here are the NVIDIA Streamline (DLSS) loader, the VRAM limit override, the unbacked/sparse image fallback, and the separate VMA pools.

## Files

| file | role | read depth |
|---|---|---|
| `vulkan_device.cpp` (2058) | `Device` ctor, `GetSuitability`, driver workarounds, VMA creation, budget functions, static pipeline cache | full |
| `vulkan_device.h` (1401) | feature/extension macro lists, accessors, members | partial (lines 29-260, 575-600, 1036-1100, 1195-1243, member greps) |
| `vulkan_memory_allocator.cpp` (616) | `MemoryAllocator`, `MemoryCommit`, image/buffer/sparse allocation, OOM fallbacks | full |
| `vulkan_memory_allocator.h` (183) | allocator class, `pressure_events`, buffer type cache | partial (120-183) |
| `unbacked_image_policy.h`, `vma.h` | size thresholds for unbacked images; VMA include wrapper | full |
| `vulkan_instance.cpp` | `CreateInstance`, layer filtering | full |
| `vulkan_wrapper.cpp` (1193) | device sorting, `Instance::Create`, image/buffer release and flush, `Device::Create*` | partial (29-113, 439-503, 545-648, 820-900, 1082-1100) |
| `vulkan_wrapper.h` (1797) | wrapper class declarations, `vk::Check` | skimmed (greps only) |
| `streamline.cpp` (469) | loads `sl.interposer.dll`, checks NVIDIA signature, hooks swapchain entry points | full |
| `nsight_aftermath_tracker.cpp` | Aftermath crash-dump callbacks (only with `HAS_NSIGHT_AFTERMATH`) | full |
| `vulkan_library.cpp`, `vulkan_surface.cpp` | open `vulkan-1.dll`; create Win32 surface | full (Windows branch) |
| `vulkan_debug_callback.cpp` | debug-utils messenger | partial |
| `streamline.h`, `streamline_api.h`, `nsight_aftermath_tracker.h`, `vulkan_library.h`, `vulkan_surface.h`, `vulkan_instance.h`, `vulkan_debug_callback.h`, `vulkan.h` | headers | not opened |

## Main types and ownership

- `vk::Instance` and `vk::DebugUtilsMessenger` are built in `RendererVulkan` and held by it. The `Device` is built after the surface and is destroyed after the memory allocator, scheduler and swapchain that use it.
- `Device` owns `vk::Device logical`, the VMA `allocator`, a `separate_pools` table of `VmaPool`s, queue handles, the feature/extension/property structs, and the static pipeline cache file. `~Device` saves the pipeline cache, destroys the pools, then `vmaDestroyAllocator`.
- `MemoryAllocator` holds a reference to `Device` and the raw `VmaAllocator`. It returns `vk::Image`, `vk::Buffer` (each carries its `VmaAllocation` and releases it in `Release()`), `MemoryCommit` (move-only VMA allocation) and sparse-memory handles.
- Streamline state is one function-local static `State` in `streamline.cpp`.

## Threads and locking

- No thread is created in this slice. Everything runs on whichever thread calls it. Construction runs on the thread that creates the renderer; `Instance::Create` runs inside `std::async(...).get()`, which blocks.
- VMA is created with `VMA_ALLOCATOR_CREATE_EXTERNALLY_SYNCHRONIZED_BIT`: VMA takes no locks. So all `vmaCreate*/vmaDestroy*/vmaGetHeapBudgets` calls must be serialized by the callers. I found no second thread that allocates (the present thread only uses frames; the GPU thread split puts rasterizer and caches on one thread). This is assumed, not enforced.
- `separate_pools_mutex` guards only the `separate_pools` table. `MemoryAllocator::buffer_type_mutex` guards only the `(usage, flags) -> memory type` cache. `pressure_events` is an atomic counter. `custom_border_color_samplers_used` is an atomic.
- Queue submission is serialized outside this slice by `Scheduler::submit_mutex`.
- Function-local `static bool logged / host_logged` in `CreateImage` are plain bools (benign while single-threaded).

## Flow

1. `RendererVulkan::CreateInstance` / `CreateDevice` (outside) call `Vulkan::CreateInstance(library, dld, VK_API_VERSION_1_1, window_system, enable_validation, enable_debug)`. It enumerates extensions once, adds the Win32 surface extension, `VK_KHR_surface`, `VK_EXT_debug_utils` when validation is on, and Streamline's instance extensions when started. `RemoveUnavailableLayers` filters layers. `Instance::Create` requests API 1.3 ("DO NOT TOUCH, breaks RDNA3").
2. `vk::EnumeratePhysicalDevices` sorts devices (`SortPhysicalDevices`: name, discrete first, NVIDIA > AMD > Intel, Dozen last). `Settings::values.vulkan_device` is an index into that list.
3. `Device::Device`: format properties -> `GetSuitability` (extension discovery by macro, features2 and properties2 chains, `RemoveUnsuitableExtensions`, limits) -> an unsuitable device only logs "continuing anyways" -> driver flags -> `GetNvidiaArchitecture` -> queue families -> blit and ASTC probes -> `CollectPhysicalMemoryInfo`, `CollectToolingInfo` -> per-driver workarounds (float16 off on Ampere+/AMD/Intel Windows, VIDS version check, `dyna_state` switch, descriptor indexing, descriptor buffer / buffer device address interplay) -> `vk::Device::Create` -> Streamline `AttachDevice` -> VMA allocator creation (budget extension flag, BDA flag, 16 MiB large-heap block size, optional heap limit from `EDEN_NX2_VRAM_LIMIT_MIB`) -> `LoadStaticPipelineCache` -> GPU logging.
4. `MemoryAllocator::CreateImage` uses `WITHIN_BUDGET | STRATEGY_MIN_MEMORY`. On `VK_ERROR_OUT_OF_DEVICE_MEMORY` it retries without the budget (bumps `pressure_events`), then in host memory, then returns an unbacked sparse image (`CreateUnbackedImage`). Images over 2^28 texels are probed first via `ShouldLeaveImageUnbacked` (more than a quarter of device-local memory).
5. `CreateBuffer`: device-local buffers up to 8 MiB try `CreateBufferInSeparatePool` (a 16 MiB-block `VmaPool` per memory type), else `vmaCreateBuffer`. `Commit` relaxes budget then device-local.
6. Budget path: `CollectPhysicalMemoryInfo` fixes `device_access_memory` (discrete: summed device-local heap budgets minus min(sum/8, 1 GiB), capped to 6 GiB + 1 GiB x resolution scale unless `vram_usage_mode` is aggressive) and `main_heap`. Per frame, `GetDeviceMemoryUsage` (driver usage minus VMA unused block bytes) is read by the buffer cache and texture cache; `GetDeviceMemoryBudget` (main heap budget plus other heaps' usage) is re-read by the texture cache about every 30 frames; `MemoryPressureEvents()` makes the texture cache raise `pressure_margin` by 512 MiB, up to 3 GiB.

## Connections

- In: `RendererVulkan` (construction order instance, messenger, surface, device, allocator, scheduler, swapchain, present manager); `BufferCache`/`TextureCache` runtimes (`GetDeviceMemoryUsage/Budget/Slack`, `MemoryPressureEvents`); `vk_texture_cache.cpp` (`CreateUnbackedImage`, `AllocateSparseMemory`, `FreeSparseMemory`); staging pool, buffer cache, descriptor buffer ring, multi-range buffer (`CreateBuffer`); pipeline code (`IsExtDescriptorBufferSupported`, dynamic-state accessors); present code (`Streamline` hooks installed into `dld`).
- Out: Vulkan loader (`vulkan-1.dll`), VMA, Streamline (`sl.interposer.dll`, `WinVerifyTrust`), Aftermath DLL, `Settings::values`, `Common::FS` (shader dir for the static pipeline cache), GPU logging (`GPU::Logging`).

## Settings and switches

`Settings::values`: `vulkan_device`, `renderer_debug`, `dyna_state`, `vertex_input_dynamic_state`, `enable_nsight_aftermath`, `enable_compute_pipelines`, `use_mesh_shaders`, `renderer_shader_feedback`, `emulate_bgr565`, `vram_usage_mode`, `resolution_info`, `nvidia_dlss`, `gpu_log_*`, `gpu_log_memory_tracking`.
Env vars: `EDEN_NX2_NO_DESCRIPTOR_BUFFER`, `EDEN_NX2_FORCE_MESH_FALLBACK`, `EDEN_NX2_VRAM_LIMIT_MIB`, `EDEN_NX2_DLSS_LOG`, `EDEN_STREAMLINE_DIR`.

## Invariants

- VMA calls are single-threaded: assumed only (see above).
- `Device` outlives every `vk::Buffer`/`vk::Image` it allocated: held by construction order in `RendererVulkan`, no check.
- Extension flags in `extensions.*` mean "loaded"; accessors like `IsBufferDeviceAddressSupported()` read the flag, not the feature bit. Enforced by `RemoveExtensionFeature*` helpers after the feature query; the DLSS branch deliberately clears the flag while keeping the feature on.
- Vulkan 1.1 is the minimum (`ASSERT`); core-promoted features beyond that are enabled by feature struct, not extension name.
- Only the device that owns a surface loads and saves the static pipeline cache (`owns_static_pipeline_cache`).
- Mandatory extensions missing: logged, then device creation proceeds anyway ("Unsuitable driver - continuing anyways").

## Hazards and improvement candidates

- The VIDS driver-version check compares mismatched encodings, so VIDS is always disabled on NVIDIA proprietary and Intel Windows.
- `CreateBuffer` has no budget-relaxing fallback, unlike `CreateImage` and `Commit`; images may overshoot the budget and then buffers throw.
- `GetDeviceMemoryUsage` costs one driver query plus VMA stats per call, twice per frame, even when its result is unused.
- `GetDeviceMemoryUsage` passes a budget struct without checking `extensions.memory_budget`.
- `features_1_2` / `features_1_3` in `GetSuitability` are filled in but never part of the queried chain.
- Not opened: `streamline.h`, `streamline_api.h`, Aftermath header, `vulkan_wrapper.h` bodies. Details are in `findings/vk-device.json`.
