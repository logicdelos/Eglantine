# HLE kernel memory (physical allocator, page tables, memory layout)

**Purpose.** This slice is the kernel's view of memory: the board memory layout (including the Eglantine NX2 12 GB layout plus a 2 GiB graphics-server area), the physical page allocator (`KMemoryManager` over `KPageHeap`), per-process page tables (`KPageTableBase`, memory blocks, page groups), the slab and dynamic slab heaps, resource limits, and the shared, transfer and code memory objects. Guest "physical" memory is one host buffer (`Core::DeviceMemory::buffer`, a `Common::HostMemory`); the kernel decides which pages belong to whom and asks `Core::Memory::Memory` to map them into the guest address space.

## Files

| file | role | read depth |
|---|---|---|
| `k_page_table_base.cpp/.h` (5837+771 lines) | process address-space operations: heap, MapPhysicalMemory, IPC, code, insecure memory, `Operate` | partial (heap, physical memory, insecure memory, page-group helpers, `Finalize`, `Operate`, IPC partial pages read; `CheckMemoryState`, `LockMemoryAndOpen`, permission/attribute, device/IO maps, most IPC not read) |
| `k_memory_manager.cpp/.h` | pool allocator, per-page reference counts, zeroing policy | full |
| `k_page_heap.cpp/.h` | buddy-style free-block allocator | partial (Allocate/Free paths) |
| `k_page_group.cpp/.h`, `k_dynamic_resource_manager.h` | lists of physical runs; `KBlockInfo` slab | full |
| `k_process.cpp/.h` | memory accounting, `Initialize`, `Run`, `Finalize`, NX2 limits | partial |
| `k_resource_limit.cpp/.h` | Reserve/Release with waiters | partial |
| `board/nintendo/nx/k_system_control.*`, `k_memory_layout.cpp` (board) | DRAM size, pool sizes, partitioning, NX2 sizes | full |
| `init/init_slab_setup.cpp` | slab counts and heap set-up | full |
| `kernel.cpp` (memory parts) | `DeriveInitialMemoryLayout`, `InitializeSystemResourceLimit`, resource managers | partial |
| `k_shared_memory.cpp`, `k_code_memory.cpp`, `k_transfer_memory.cpp`, `k_system_resource.*` | memory objects | full (cpp), header of system resource full |
| `k_memory_block_manager.*`, `k_dynamic_page_manager.h`, `k_dynamic_slab_heap.h`, `k_slab_heap.h`, `k_page_table_slab_heap.h` | block tree and slab machinery | partial (block manager lines 1-123; others full) |
| `k_memory_layout.cpp` (generic), `svc/svc_physical_memory.cpp` | region trees; SetHeapSize / Map(Unmap)PhysicalMemory SVCs | partial / full |
| `k_memory_block.h`, `k_memory_region*.h`, `k_capabilities.*`, `k_page_bitmap.h`, `k_device_address_space.*`, `k_address_space_info.*`, `k_page_table_manager.h`, `k_process_page_table.h`, `slab_helpers.h` | not opened (or only grepped) | not opened |

`common/host_memory.cpp` (dirty-granule code), `device_memory.h` and `service/vi/shared_buffer_manager.cpp` were read only as callers/callees.

## Main types and ownership

- `DeviceMemory` owns the `HostMemory` buffer, sized `GetIntendedMemorySize()`: 4/6/8/10 GiB, or 12 GiB + `Nx2GraphicsServerMemorySize` (2 GiB) = 14 GiB for the 12 GB layout. Physical addresses start at `0x80000000`. Slab heaps live in a reserved area above `KernelReserveBase`.
- `KMemoryManager` (owned by `KernelCore`) has up to 10 `Impl` managers, each with a `KPageHeap` and a `u16` reference count per page, plus per-pool lists. Pages are owned by reference count: `AllocateAndOpen` gives count 1, `Open`/`Close` change it, and `Close` returns runs that reach 0 to the heap.
- `KPageTableBase` (one per process) owns a `KMemoryBlockManager` (intrusive red-black tree of `KMemoryBlock`) and the `Core::Memory::Memory` mapping of the guest address space (`m_impl`). Blocks come from `KMemoryBlockSlabManager`, page-group nodes from `KBlockInfoManager`; both are dynamic slab heaps backed by `KDynamicPageManager` over a **host** `std::vector` (not guest RAM). The system-wide ones come from the page-table heap region; processes with a system resource size get a `KSecureSystemResource`.
- `KPageGroup` is a singly linked list of `KBlockInfo` (u32 page index, u32 page count). It does not own page references by itself; `Open`/`Close`/`CloseAndReset` and `KScopedPageGroup` do.
- `KResourceLimit` is shared by processes; `KProcess` holds the reservation of its memory through `KScopedResourceReservation`.
- `KSharedMemory`, `KTransferMemory`, `KCodeMemory` own a page group each and hold page references until `Finalize`.

## Threads and locking

Kernel objects are used from every guest core thread (SVC handlers run on the emulated CPU threads) and from service threads.
- `KMemoryManager::m_pool_locks[4]` (`KLightLock`, one per pool) guards the heaps and reference counts of that pool. `HostMemory` granule flags are atomics.
- `KPageTableBase` has `m_general_lock` (blocks and tables), `m_map_physical_memory_lock` (taken first by `SetHeapSize`, `MapPhysicalMemory`, `UnmapPhysicalMemory`) and `m_device_map_lock`. `SetHeapSize` growth allocates and zeroes pages outside `m_general_lock`, relying on `m_map_physical_memory_lock`, then takes `m_general_lock` and asserts the heap end did not move.
- `KResourceLimit` uses its own `KLightLock` and a condition variable; `Reserve` can wait (default 10 s).
- `KSlabHeapImpl` takes a `std::mutex` on every allocation and free; `KDynamicPageManager` takes a `KSpinLock`.
- Order observed: map-physical lock, then general lock, then pool lock (inside `AllocateAndOpen`/`Close`), then resource limit.

## Flow

1. Boot: `KernelCore::Initialize` calls `InitializeSlabResourceCounts`, `DeriveInitialMemoryLayout` (region trees, linear map, `SetupPoolPartitionMemoryRegions`), `InitializeSlabHeaps`, then `InitializeSystemResourceLimit` and `InitializeMemoryLayout`, which calls `KMemoryManager::Initialize`.
2. `KMemoryManager::Initialize` calls `buffer.ForgetContents()` (marks every used granule dirty), builds an `Impl` per DRAM user-pool region, and frees the regions around the initial process image.
3. Process creation: `KProcess::Initialize` -> `KPageTableBase::InitializeForProcess` (address-space and alias/heap sizes by `KAddressSpaceInfo`, with the `is_switch2` variant), reserves code memory from the resource limit, allocates code pages with `AllocateForProcess`, maps them. For `.nup` metadata (`IsSwitch2()`), `m_max_process_memory = heap region size + alias region size`.
4. Heap: `SVC SetHeapSize` -> `KPageTableBase::SetHeapSize`: reserve from the resource limit, `AllocateAndOpen` pages, `ClearBackingRegion(m_heap_fill_value)` for each block, map with `Operate(MapGroup)`, update the memory block tree. Shrinking unmaps and closes pages.
5. Physical memory: `MapPhysicalMemory` computes the unmapped parts of the alias range, calls `AllocateForProcess` (which clears pages unless they were never used, via `ProcessOptimizedAllocation`) and maps them; `UnmapPhysicalMemory` is the reverse.
6. `Operate(Unmap)` builds a `pages_to_close` group with `MakePageGroup`, unmaps the host mapping, and closes the group on scope exit; `Close` marks the granules dirty first and then decrements the counts.
7. Zeroing policy (fork): `ClearBackingRegion` skips granules (64 KiB) that are not dirty; whole dirty granules are cleared by `PunchHole` on Linux or `memset` elsewhere, partial ones by `memset` and stay dirty.
8. Process exit: `KProcess::Finalize` computes used memory, `KPageTableBase::Finalize` walks every memory block (`BlockCallback`: unmap host range, `MakePageGroup`, `CloseAndReset`), then releases the memory reservations.

## Connections

- In: SVC layer (`svc_physical_memory.cpp`, `svc_memory.cpp`, `svc_info.cpp`, code/transfer/shared memory SVCs), `KProcess`, loader (`Loader::...` -> `LoadFromMetadata`), `KThread` stacks, IPC (`KPageTableBase::SetupForIpc`), `service/vi/shared_buffer_manager.cpp`, `nx2_gsv.h` (allocates and clears memory), `core.cpp` (forces 12Gb layout for NX2).
- Out: `Core::Memory::Memory` (`MapMemoryRegion`, `UnmapRegion`), `HostMemory` (`ClearBackingRegion`, `MarkUsed`, `MarkDirty`, `ForgetContents`, `HintCold`), `KResourceLimit`, `KSystemControl`, the scheduler lock for `KProcess`.

## Settings and switches

- `Settings::values.memory_layout_mode` selects 4/6/8/10/12 Gb layouts; `core.cpp` forces 12Gb for NX2 titles.
- `Settings::values.cold_hint_mode` selects `HintCold` behaviour (on Windows only `VirtualUnlock`).
- Environment: `EDEN_NX2_MEM_TRACE` (logs heap and physical-memory requests), `EDEN_PUNCH_HOLE` (Linux only, `host_memory.cpp`).
- Switch 2 `svc_info` entries: `Nx2RegionAddress`, `Nx2RegionSize`, `Nx2RemoteMemorySize`, `Nx2RemoteMemoryUsed`, `Nx2VammSize` (not read in this pass).

## Invariants

- A granule that is not marked Dirty reads as zero. Enforced only by marking dirty in `KMemoryManager::Close` and in `ForgetContents`; any code that writes to free pages or frees them by another path would break it (the failure path of `AllocatePageGroupImpl` frees without `MarkDirty`, safe because nothing was written).
- Page reference counts are `u16`: `Open` asserts `> 1`, `OpenFirst` asserts `== 1`.
- `SetHeapSize` requires `m_map_physical_memory_lock`; `ASSERT(cur_address == m_current_heap_end)` checks it.
- A page group must be closed exactly once per open; scopes (`SCOPE_EXIT`, `KScopedPageGroup`) enforce this in the paths read.
- Memory-block updates allocate their blocks up front through `KMemoryBlockManagerUpdateAllocator` so they cannot fail half-way (assumed, not checked).
- The 2 GiB NX2 graphics-server area is not a separate pool; it lands in the non-secure system pool because `unsafe_system_pool_start` is derived from the applet pool start. The application pool is 9809 MiB for the 12 GB layout.

## Hazards and improvement candidates

Details in `findings/mem-kernel.json`.
- `RemapPageGroup` passes `map_pages` where `cur_pages` is meant (rollback path only).
- On Windows freed guest RAM is never decommitted; the 14 GiB section is committed from boot and dirty granules are `memset` on reuse.
- `Operate(Unmap)` and `Finalize` drop the `MakePageGroup` result, so pages can leak when the block-info slab is exhausted.
- Each `KMemoryManager::Impl` allocates a refcount array for the whole 14 GiB.
- `KDynamicPageManager::Free` is wrong and unused; `KSecureSystemResource` allocates guest secure memory it does not use and ignores an init result.
- Stubs worth knowing: `Operate(Separate)` and `FinalizeUpdate` are no-ops, `MapPhysicalMemoryUnsafe`/`UnmapPhysicalMemoryUnsafe` return `ResultNotImplemented`, `KPageTableBase::Finalize` hits `UNIMPLEMENTED()` if unsafe physical memory is still mapped, `AllocateSecureMemory` is `UNIMPLEMENTED_IF` for the applet pool, `Impl::DumpFreeList` is `UNIMPLEMENTED`. `AllocateForProcess` overwrites `any_new` for each manager chunk instead of OR-ing it (upstream behaviour, rarely matters).
