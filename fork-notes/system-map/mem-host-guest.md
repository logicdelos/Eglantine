# Host/Guest Memory

**Purpose** Guest RAM is one host file mapping (a Windows section). This slice creates that section, exposes it as a fastmem arena for the JIT, tracks which guest pages the GPU has cached, and translates guest CPU, device (GPU-visible) and GPU virtual addresses to host pointers. It also holds the Nx2 additions: cold-page hints, granule zeroing, read traps and host-pointer import into Vulkan.

## Files

| file | role | read depth |
|---|---|---|
| `src/common/host_memory.cpp/.h` | `HostMemory`: section, placeholders, `Map/Unmap/Protect`, `HintCold`, `ClearBackingRegion`, granule flags | full (Windows impl); POSIX impl skipped |
| `src/core/device_memory.h/.cpp` | `DeviceMemory`: owns `HostMemory`; `GetPointer` on the backing view | full |
| `src/core/device_memory_manager.h/.inc` | device address space (`DAddr`), physical-page table, cached-page counters, cold-hint worker | full |
| `src/core/memory.cpp` | `Memory::Impl`: guest page tables, fastmem map, rasterizer cache marking, read traps, dirty collection | full |
| `src/common/page_table.h`, `src/core/guest_memory.h`, `src/video_core/guest_memory.h` | page-entry encoding, scoped read/write helpers | full |
| `src/video_core/memory_manager.h/.cpp` | GPU virtual `MemoryManager`: small/big page tables, kinds, continuity runs | full |
| `src/video_core/renderer_vulkan/vk_host_import.h` | imports host pointers as `VkDeviceMemory` | full |
| `src/core/gpu_dirty_memory_manager.h` | per-core CPU-write log feeding GPU invalidation | full |
| `src/common/address_space.inc`, `range_sets.inc`, `multi_level_page_table.inc` | allocator, sorted range set, GPU page table storage | partial |
| `buffer_cache.h` (1570-1683), `vk_buffer_cache.h`, `k_memory_manager.cpp`, `kernel.cpp`, `k_process.cpp`, `arm_dynarmic_64.cpp` | callers | fragments |
| `memory.h`, `range_map.h`, `slot_vector.h`, `lru_cache.h`, `free_region_manager.h`, `pte_kind.h`, `invalidation_accumulator.h`, `host1x/*device_memory_manager*`, `cache_types.h`, `page_table.cpp`, `dmnt_cheat_*`, `cheat_engine.*`, `heap_tracker.*` (Android only), `sparse_large_vector.*` | support | skimmed or not opened |

## Main types and ownership

- `Core::DeviceMemory` (owned by `System`) owns one `HostMemory` of `GetIntendedMemorySize()` bytes, reserving 1<<39 of address space (1<<38 with NCE).
- `HostMemory` owns the Windows `Impl`: `backing_handle` (section, `SEC_COMMIT`), `backing_base` (one permanent `PAGE_READWRITE` view of the whole section) and `virtual_base` (a 512 GiB placeholder arena plus 2 MiB alignment slack). `Map` maps section views into the arena at `host_offset`; `Impl::Map` ignores permissions and always uses `PAGE_READWRITE`. It also keeps `granules[]` (64 KiB, flags Used/Dirty).
- `Memory::Impl` owns the per-process `Common::PageTable`; entries are 64-bit `PageEntryData` (marked/type/block/page). The marked bit means "rasterizer-cached, pointer hidden". Entry pointers derive from `backing_base`, not from the arena. The arena is used only by dynarmic (`config.fastmem_pointer`).
- `MaxwellDeviceMemoryManager` (`DeviceMemoryManager<...>`) owns the device allocator, a `compressed_physical_ptr` table ((phys>>12)+1, `MULTI_FLAG` for aliases), `continuity_tracker`, the `u8` cached-page counters, per-process interfaces (`registered_processes`) and the cold-hint `ThreadWorker`.
- GPU `Tegra::MemoryManager` owns 2-bit entry bitmaps (Free/Reserved/Mapped), `page_table` (`MultiLevelPageTable<u32>`, 4 KiB pages), `big_page_table_dev` (`SparseLargeVector<u32>`, 64 KiB), `big_page_continuous`, `kind_map`, `continuous_runs` plus a copy-on-write `MappingSnapshot`.

## Threads and locking

- `HostMemory::Impl::placeholder_mutex`: `placeholders`, `placeholder_host_pointers`, and Map/Unmap/Protect.
- `DeviceMemoryManager::mapping_guard` (mutex): Map/Unmap/TrackContinuity/gather. `counter_guard` (`RangeMutex`): cached-page counters. Per-thread `t_cache` (4 slots) is invalidated by `translation_generation`. The worker thread `DeviceMemory:ColdHint` runs `HostMemory::HintCold`.
- `Memory::Impl`: `read_trap_mutex` guards `armed_read_traps`; `sys_core_guard` serialises writes from non-core threads (mapped to the sys core); per-core `rasterizer_*_areas`, `scratch_buffers`, `gpu_dirty_managers` are touched only by their core thread (plus gatherers, see below).
- `GPUDirtyMemoryManager`: `guard` protects `back_buffer`; `gather_guard` serialises gatherers; `current` is a lock-free atomic pair.
- GPU `MemoryManager`: `guard` for `kind_map`; `runs_guard` for runs and snapshot; `map_serial` (atomic) invalidates the `thread_local` 2048-slot `GpuToCpuAddress` cache. The entry bitmaps and page tables are read without a lock, and rely on the nvdrv mapping thread not racing readers.
- Order seen: `mapping_guard` before `counter_guard`; `runs_guard` then `guard` (in `CurrentSnapshot`). No inversion found.

## Flow

1. `DeviceMemory` ctor builds `HostMemory` (`CreateFileMapping2`, `VirtualAlloc2` placeholders, `MapViewOfFile3`). If `Init()` fails, the ctor falls back to `VirtualAlloc(MEM_COMMIT)` and no fastmem.
2. Kernel page-table code calls `Memory::MapMemoryRegion`, which sets page-table entries from `DeviceMemory::GetPointer` and, if `fastmem_arena` exists, `host_buffer->Map(vaddr, phys_offset, size, perms)`.
3. Host kernel structures (slabs, shared memory) are read through `GetPointer` on `backing_base` without any `Map`.
4. Guest CPU access via `Memory::Impl::ReadBlockImpl/WriteBlockImpl` uses `GetPointerImpl`. Rasterizer-cached pages take the slow path: `HandleRasterizerDownload` (flush) or `HandleRasterizerWrite` (`OnCPUWrite`, `Nx2Hazard::NoteGuestWrite`, `GPUDirtyMemoryManager::Collect`).
5. `RasterizerMarkRegionCached` -> `DeviceMemoryManager::UpdatePagesCachedCount` flips the marked bit and calls `HostMemory::Protect` on the arena.
6. At fence time `core.cpp:807` `Gather`s each core manager; the GPU invalidates those ranges.
7. GPU mapping: nvdrv calls `MemoryManager::Map/MapSparse` -> `BigPageTableOp`/`PageTableOp` -> `UpdateContinuousRuns` (bumps `map_serial`). Reads go `GpuToCpuAddress` -> `DeviceMemoryManager` -> `backing_base` pointer.
8. Buffer cache optionally imports a contiguous `GetSpan` pointer through `HostImporter` (`vkGetMemoryHostPointerPropertiesEXT`).
9. Free of guest memory: `KMemoryManager` calls `MarkDirty`; allocation calls `ClearBackingRegion`, which skips granules not marked Dirty; `HintCold` (policy from `cold_hint_mode`) calls `VirtualUnlock` on idle ranges.
10. Shutdown: `kernel.cpp:157-158` `arena.Unmap(0, VirtualSize(), false)`.

## Connections

In: kernel (`KPageTableBase`, `KMemoryManager`, `KProcess`), HLE services (`GetPointer`, `ReadBlock`), dynarmic (`fastmem_pointer`, page table), nvdrv (GPU `Map`), buffer/texture/query caches (`GetSpan`, `GpuGuestMemoryScoped`, `ReadBlock`), audio decode (`GuestMemory` spans), debugger/GDB stub (`GetPointerSilent`).
Out: Win32 `VirtualAlloc2`, `MapViewOfFile3`, `VirtualProtect`, `VirtualUnlock`; rasterizer interface (`OnCPUWrite`, `InvalidateRegion`, `FlushRegion`, `InnerInvalidation`, `MarkRegionCaching`); Vulkan host import.

## Settings and switches

`Settings::values.cold_hint_mode` (Off/WhenRamIsLow/Always), `use_reactive_flushing`, `cpu_backend`, `memory_layout_mode` (4 GiB -> `physical_min_bits` 32, else 35), `IsFastmemEnabled()`, `IsNceEnabled()`, `CurrentProgramIsSwitch2()`.
Env: `EDEN_NX2_COLD_HINT`, `EDEN_NX2_NO_REACTIVE`, `EDEN_NX2_READ_TRAPS` (0 disables), `EDEN_NX2_PROTECT_STATS`, `EDEN_NX2_HOST_IMPORT`, `EDEN_NX2_BANK_REUPLOAD`, `EDEN_NX2_NO_TRANSLATION_CACHE`, Nx2Perf options (`Bounds`, `DirtyGatherFast`), `EDEN_PUNCH_HOLE` (Linux).

## Invariants

- Page-table pointers come from `backing_base`: enforced in `MapPages`; arena pointers never appear there.
- `Impl::Protect` only touches mapped placeholder ranges: enforced by iterating `placeholders` under `placeholder_mutex`.
- Fresh guest memory reads as zero unless the granule is Dirty: assumed from `MarkDirty` calls in `KMemoryManager`; `ClearBackingRegion` relies on it.
- `continuity_tracker` correctness: only if Map(track=true) is used for contiguous ranges; assumed.
- `big_page_continuous[i]` stays valid while the device mapping behind it is unchanged: assumed.
- Guest permissions (R/X/None) are not enforced in the arena on Windows: `Impl::Map` ignores `perms`.

## Hazards and improvement candidates

- Fallback path in the `HostMemory` ctor does not check `Allocate`'s null return.
- `Impl::Init` failure paths call `Release()` and then `~Impl` calls it again.
- Partial unmap of a placeholder leaves a window in which neighbours are unmapped (acknowledged in comments).
- `GPUDirtyMemoryManager`: the "invalid" sentinel test is always true, and the mask builder is wrong for writes that cross a 2 KiB chunk.
- `MemoryManager::IsGranularRange` is unused and indexes by page index instead of offset.
- `Impl::EnableDirectMappedAddress` is `UNREACHABLE()` on Windows.
- `UpdatePagesCachedCountNoLock` counters are `u8` (wrap above 255 cachers; not confirmed reachable).

### Lazy commit feasibility

Idea: create the section with `SEC_RESERVE` and commit pages on demand, instead of `SEC_COMMIT` (12-14 GiB charged at boot). Pages are zero on first commit, which matches the Granule Used/Dirty logic. The problem is every place a host pointer to guest RAM can reach a kernel or driver. Sites:

- **Vulkan host import** (driver pins pages): `vk_host_import.h:100`, `:148`, `:152`. Source pointer `buffer_cache.h:1601` (`device_memory.GetSpan`, a `backing_base` pointer), bound at `:1602`; `vk_buffer_cache.h:206-216`. Off by default on Windows (`HostImportByDefault()` is Linux-only); opt-in via `EDEN_NX2_HOST_IMPORT=1`. Failure falls back to a copy (`vk_host_import.h:170-174`). These pages come from guest mappings, so they would be committed.
- **`HostMemory::HintCold`**: `VirtualUnlock` at `host_memory.cpp:960` on `backing_base+physical_offset` (`:971`) and arena (`:973`). It fails harmlessly on uncommitted pages.
- **`Impl::Protect`**: `VirtualProtect` at `host_memory.cpp:239`, arena views only. Callers: `memory.cpp:127,139,436,514,646,676,690`, and NCE in `k_process.cpp:1297+` (`HAS_NCE` only). These are mapped ranges, so they would be committed at Map time.
- **Fallback allocator**: `host_memory.cpp:194` commits everything already.
- **Other `MEM_COMMIT` users**: `multi_level_page_table.inc:65`, `sparse_large_vector.cpp:85-135`; not guest memory. `sparse_large_vector.cpp:131` installs its own `AddVectoredExceptionHandler`, so a lazy-commit handler must coexist with it and with dynarmic's handler.
- **Network and file I/O**: `network.cpp:1080,1101,1129,1175,1199` pass `message.data()`, likely HLE-copied vectors; I did not trace the bsd service buffers to confirm. `mcp/tools/debugger_tools.cpp:121,155` use host strings. No `ReadFile`/`WriteFile`/`DeviceIoControl`/`ReadProcessMemory` found on guest pointers; VFS reads into guest spans not fully confirmed.
- **Kernel structures on `backing_base` with no `Map`** (would rely on a fault handler in user mode): `init_slab_setup.cpp:129,297`, `k_page_buffer.cpp:15`, `k_page_table_base.h:728,743`, `k_server_session.cpp:384,571`, `k_system_resource.cpp:46`, `k_memory_manager.cpp:484,492,511,529`, `k_shared_memory.cpp:67`, `vi/shared_buffer_manager.cpp:52,53,244`, `nx2_gsv.h:859,1485`, `platform_service_manager.cpp:278,357`, `hidbus.cpp:81`, `irs.cpp:56`, `shared_memory_holder.cpp:26`, `ro.cpp:480`. `ClearBackingRegion` (`host_memory.cpp:984,1011`) memsets in user mode and would fault-commit.
- **Fastmem off** (`memory.cpp:69,93`): `host_buffer->Map` never runs, so commit-at-Map would never happen; commits must then come from the handler.
- **Teardown**: `kernel.cpp:157-158` `Unmap` does not decommit. [INFERENCE] decommit of mapped section views is unsupported, so a release path cannot give pages back.
- **Savings**: the game heap is mapped in full through `Map`, so commit-at-Map saves only physical memory never mapped into the arena.

Verdict: feasible only with a vectored handler that commits on first user-mode touch plus commit-at-Map for arena ranges; the only kernel-visible sites found are the opt-in Vulkan import and `HintCold`, both tolerant of failure. Not verified by running.
