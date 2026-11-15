# Dynarmic JIT (ARM64 to x64 recompiler)

**Purpose.** Dynarmic translates guest AArch64 code (A32 for 32-bit titles) into x64 machine code, one basic block at a time, and runs it on the host core thread. On Windows x64 it is the CPU backend: `ArmDynarmic64` in `src/core/arm/dynarmic/` creates one `Dynarmic::A64::Jit` per emulated core and feeds it memory and timing callbacks. The fork adds ARMv8.1 to 8.5 instructions needed by Switch 2 titles, lazy code-cache commit, JIT statistics, an inline CNTPCT and a vector-based fastmem patch table.

All dynarmic paths below are under `src/dynarmic/src/dynarmic/`. The arm64, riscv64 and loongarch64 backends and the Linux/macOS exception handlers exist but are not built for the target and were not read.

## Files

| file | role | read depth |
|---|---|---|
| `backend/x64/a64_interface.cpp` | `Jit` / `Jit::Impl`: Run, Step, GetBlock, invalidation | full |
| `backend/x64/block_of_code.{h,cpp}` | code buffer, prelude (`run_code`, `step_code`), lazy commit | full |
| `backend/x64/exception_handler_windows.cpp` | SEH handler and unwind table for the code buffer | full |
| `backend/x64/emit_x64.{h,cpp}` | block emission, patch/link bookkeeping, `InvalidateBasicBlocks` | full |
| `backend/x64/a64_emit_x64.cpp`, `a64_emit_x64_memory.cpp`, `emit_x64_memory.cpp.inc`, `emit_x64_memory.h` | A64 opcodes, terminals, fastmem and exclusive emitters, fallbacks | full (a64_emit_x64.cpp lines 1-823) |
| `backend/block_range_information.{h,cpp}` | guest range to block map (fork: flat vector) | full |
| `backend/x64/abi.cpp`, `stack_layout.h`, `constant_pool.*`, `exclusive_monitor.cpp` | ABI save/restore, stack frame, constants, global monitor | full or partial |
| `interface/A64/config.h`, `interface/optimization_flags.h`, `interface/exclusive_monitor.h`, `interface/jit_stats.h` | `UserConfig`, flags, monitor, counters | full |
| `frontend/A64/translate/impl/{load_store_atomic,pointer_authentication,memory_tagging}.cpp`, `decoder/a64.inc` (top) | fork-added ARMv8.1-8.5 instructions | full |
| `frontend/A64/translate/a64_translate.cpp`, `impl/system.cpp`, `impl/impl.cpp` | translation loop, MRS/MSR, exclusive helpers | partial |
| `ir/opt_passes.cpp` | pass pipeline | skimmed (names only) |
| `backend/x64/reg_alloc.*`, `a64_jitstate.*`, `emit_x64_vector.cpp` (6330 lines), `emit_x64_floating_point.cpp`, `emit_x64_vector_floating_point.cpp`, `emit_x64_data_processing.cpp` | register allocation, state, SIMD/FP emitters | not opened |
| `frontend/A64/translate/impl/simd_*.cpp` (fork edits), `frontend/A32/*`, `backend/x64/a32_*`, `a32_interface.cpp` | rest of decoder, A32 | not opened |
| `src/core/arm/dynarmic/arm_dynarmic_64.{h,cpp}`, `dynarmic_exclusive_monitor.*` | glue: config, callbacks | full / lines 1-550 |
| `arm_dynarmic_32.cpp`, `dynarmic_cp15.cpp` | A32 glue | skimmed / not opened |

## Main types and ownership

- `ArmDynarmic64` (one per core, `m_core_index`) owns `DynarmicCallbacks64 m_cb` and `std::unique_ptr<Jit> m_jit`. All cores share one `DynarmicExclusiveMonitor` (wraps `Dynarmic::ExclusiveMonitor`, `MAX_NUM_CPU_CORES = 6`).
- `Jit::Impl` owns `BlockOfCode block_of_code`, `A64EmitX64 emitter`, `A64JitState jit_state`, a reusable `IR::Block`, `invalidation_mutex`, `invalid_cache_ranges` (`boost::icl::interval_set`) and the `invalidate_entire_cache` flag.
- `BlockOfCode` reserves `conf.code_cache_size` (512 MiB per core on x64, 8 MiB for the no-page-table JIT) with a fork `ReservingAllocator` (`VirtualAlloc` `MEM_RESERVE`, never freed), then commits lazily. Six cores reserve 3 GiB of address space but commit only what is emitted.
- `A64EmitX64` owns `block_descriptors`, `patch_information`, `block_ranges`, a 16 MiB `fast_dispatch_table` (0x100000 entries of location + code pointer), `fastmem_patch_info` (sorted `std::vector`, fork) and `do_not_fastmem`.
- `g_jit_stats` (fork, `interface/jit_stats.h`) holds relaxed atomics `blocks_compiled`, `compile_ns`, `fastmem_recompiles`, polled by `VideoCommon::Nx2Spikes`.

## Threads and locking

- Each `Jit` runs on exactly one host thread, the one executing its core. `Run` asserts `!is_executing`. Emission, fastmem fault handling and invalidation work all happen on that thread, so `block_descriptors`, `fastmem_patch_info`, `do_not_fastmem` and the code buffer have no locks.
- Cross-thread entry points: `HaltExecution` (atomic `Or` on `jit_state.halt_reason`), `ClearCache`, `InvalidateCacheRange` (take `invalidation_mutex`, set flags, halt). The target thread applies them in `PerformRequestedCacheInvalidation` at the top of `Run`.
- The kernel calls `InvalidateCacheRange` on every core's interface from the calling thread (`k_page_table_base.cpp: InvalidateInstructionCache`, `core/arm/debug.cpp`). `ArmDynarmic64::InvalidateCacheRange` also writes `m_cb->last_code_addr` from that thread without a lock (finding 2).
- Global exclusive monitor: a spin lock inside `ExclusiveMonitor`; JIT code takes it via `EmitExclusiveLock` unless `Unsafe_IgnoreGlobalMonitor` is set. Lock order: monitor lock is taken inside memory callbacks only.

## Flow

1. `ArmDynarmic64::RunThread` calls `ClearExclusiveState()` then `Jit::Run`.
2. `Jit::Impl::Run`: `PerformRequestedCacheInvalidation` (`jit_state.ResetRSB()`; full `block_of_code.ClearCache()` + `emitter.ClearCache()`, or `emitter.InvalidateCacheRanges`), then `BlockOfCode::RunCode` through `run_code`.
3. `run_code` checks `halt_reason`, optionally the cycle budget, calls `LookupBlock` to `Jit::Impl::GetBlock`, switches MXCSR and jumps into the block.
4. `GetBlock`: on cache miss, if `SpaceRemaining() < 1 MiB` clear everything; `EnsureMemoryCommitted(1 MiB)`; `A64::Translate` (decoder table in `a64.inc`, translator methods emit IR via `A64::IREmitter`, stops at a terminal instruction); `Optimization::Optimize` (PolyfillPass, NamingPass, A64CallbackConfigPass, A64GetSetElimination, ConstantPropagation, DeadCodeElimination, IdentityRemovalPass, VerificationPass; each gated by `conf.HasOptimization`); `A64EmitX64::Emit`; timing added to `g_jit_stats`.
5. `Emit`: per-instruction `Emit*` functions through `RegAlloc`; terminal handling (`ReturnToDispatch`, `LinkBlock`, `LinkBlockFast`, `PopRSBHint`, `FastDispatchHint`, `If`, `CheckBit`, `CheckHalt`); deferred (far) code afterwards; `block_ranges.AddRange` records the guest range. Links to not-yet-compiled blocks are recorded in `patch_information` (kinds Jg, Jz, Jmp, MovRcx) and patched when the target appears.
6. Guest memory access: `ShouldFastmem` then inline `[r13 + vaddr]` access with its address in `fastmem_patch_info`, else page-table path (`r14`), else callback. Exclusive accesses use inline `lock cmpxchg` / `cmpxchg16b` against the value saved in the monitor.
7. Fastmem fault: Windows SEH handler -> `A64EmitX64::FastmemCallback(rip)` -> `lower_bound` in `fastmem_patch_info`; hit returns a fake call to the generated fallback; if `recompile` is set the (location, op) goes into `do_not_fastmem` and the block is invalidated.
8. Exits: `HaltReason` bits (`CacheInvalidation`, `SupervisorCall`, `BreakLoop`, step, memory abort) are returned to `ArmDynarmic64::RunThread`.

Exception handler details: one `RUNTIME_FUNCTION` covers the whole buffer (`RtlAddFunctionTable`, `UNW_FLAG_EHANDLER`). A stub checks `RIP` is inside the buffer, calls the callback, pushes `ret_rip`, sets `RIP = call_rip` and returns `ExceptionContinueExecution`. It does not look at the exception code.

## Connections

- In: `ArmDynarmic64` (`MakeJit`, `RunThread`, `StepThread`, `SignalInterrupt`, `ClearInstructionCache`, `InvalidateCacheRange`); kernel page table (`InvalidateInstructionCache`), gdbstub and `core/arm/debug.cpp` for cache invalidation.
- Out: `Core::Memory::Memory` (`Read8..64`, `WriteExclusive8..128`, `ReadBlock` for code pages), `Core::System::CoreTiming().downcount`, `Common::g_wall_clock` (CNTPCT, CNTFRQ), `Kernel` SVC dispatch via `CallSVC`, `VideoCommon::Nx2Spikes`, `Common::PageTable` (`entries`, `fastmem_arena`).

## Settings and switches

`MakeJit` (`arm_dynarmic_64.cpp`) maps `Settings::values.cpu_accuracy` and `cpuopt_*` keys:

| mode | effect |
|---|---|
| Accurate | defaults, `unsafe_optimizations = false` |
| Paranoid | `no_optimizations` |
| Auto | `unsafe_optimizations = true`, `Unsafe_UnfuseFMA` if no host FMA, `fastmem_address_space_bits = 64`, `Unsafe_IgnoreGlobalMonitor` |
| Unsafe | per `cpuopt_unsafe_*`: unfuse FMA, reduced FP error + ignore FPCR, inaccurate NaN, 64-bit fastmem check, ignore global monitor |
| Debugging | honours `cpuopt_page_tables`, `block_linking`, `return_stack_buffer`, `fast_dispatcher`, `context_elimination`, `const_prop`, `misc_ir`, `reduce_misalign_checks`, `fastmem`, `fastmem_exclusives`, `recompile_exclusives`, `ignore_memory_aborts` |

`Settings::IsFastmemEnabled()` can switch fastmem off. Fixed values: `define_unpredictable_behaviour`, `absolute_offset_page_table`, `detect_misaligned_access_via_page_table = 16|32|64|128`, `silently_mirror_fastmem = false`, `fastmem_exclusive_access` equals "fastmem on", `ctr_el0 = 0x8444c004`, `dczid_el0 = 4`. Environment: `EDEN_NX2_INLINE_CNTPCT=0` turns off the inline rdtsc CNTPCT; `EDEN_NX2_CNTPCT_PAUSE` controls the `_mm_pause` spin heuristic in `GetCNTPCT`; `PERF_BUILDID_DIR` (perf map only). `DYNARMIC_IGNORE_ASSERTS` defaults ON (CMake) but dynarmic uses `common/assert.h`, so `ASSERT` logs and continues while `UNREACHABLE` aborts.

## Invariants

- Only the owning thread touches a `Jit` except through `HaltExecution`, `ClearCache`, `InvalidateCacheRange`: assumed (assert on `is_executing` only).
- 1 MiB of committed space exists before each block is emitted: enforced in `GetBlock`, but a single block is not bounded by it (finding 5).
- Every fastmem instruction has a patch entry: enforced by `AddFastmemPatch`; an unmatched fault is fatal (`FastmemCallback` ends in `UNREACHABLE`).
- Patch offsets fit in u32 from code base: `ASSERT` (soft-fail only).
- Fork decoder entries stay first in `a64.inc` ("DO NOT REORDER"): comment only.
- LSE atomics rely on a retry loop: exclusive load, exclusive store, `CheckBit` terminal back to the same PC on failure.

## Fork frontend additions

LDAPR, LSE atomics (LDADD, LDCLR, LDEOR, LDSET, LDSMAX/MIN, LDUMAX/MIN, SWP, CAS, CASP) are built from exclusive load plus exclusive store. Pointer authentication is the identity (PACGA is `(Xn ^ Xm) & 0xFFFFFFFF00000000`, RETA/BRAA/BLRAA use RSB/dispatch hints, LDRA is a plain load). Memory tagging ignores tags: STZG/STZ2G zero, STG/ST2G only write back, LDG leaves Xt, IRG copies Xn, ADDG/SUBG add the offset. I checked the bit patterns against the ARM encodings and found no mismatch.

## Hazards and improvement candidates

- Hard-coded unwind info (frame 0xC8, prolog 89) does not match the real prelude frame (8 GPR pushes plus 0x4E8 bytes, xmm saves at `rsp+1088`); stack walks through JIT frames are wrong (finding 1).
- Cross-thread write to `last_code_addr` can leave a stale code page cached (finding 2).
- `ConstantPool` is constructed before `committed_size` is initialised (finding 3).
- Auto mode uses 64-bit fastmem with no range check; a wild guest pointer just below 2^64 wraps onto host memory below the arena (finding 4).
- Invalidation is a linear scan of all block ranges (finding 6).
- `do_not_fastmem` is never cleared; it grows only per faulting (location, op), so it is small.
- Dead code space of invalidated blocks is reclaimed only by a full cache clear at 1 MiB remaining.
