# Shader Recompiler

**Purpose.** `src/shader_recompiler` turns one guest GPU program (Maxwell SASS from a Switch 1 title, or Ampere "native" SASS from a Switch 2 `.nup` title) into an IR program and then into SPIR-V that the Vulkan renderer compiles into a `VkShaderModule`. It has no GPU state of its own: everything it learns about the draw comes through the `Shader::Environment` interface, a `Profile` (host capabilities), a `HostTranslateInfo`, and a per-pipeline `RuntimeInfo`. Eglantine adds an Ampere-to-Maxwell lowering step in front of the frontend, mesh/task shader support, and several fork-specific tweaks in the optimisation passes and the SPIR-V backend.

## Files

| file | role | read depth |
|---|---|---|
| `frontend/maxwell/translate_program.cpp` | pass driver: `TranslateProgram`, `MergeDualVertexPrograms`, `ConvertLegacyToGeneric`, `GenerateGeometryPassthrough` | full |
| `frontend/ampere/lower_to_maxwell.h` (3849 lines, fork) | `LowerToMaxwell`: native opcodes to Maxwell words, register-pressure shadowing, relocations | partial (1-423, 3560-3849; rest only via grep of rejection reasons) |
| `frontend/ampere/program_address_map.h`, `program_capture.h`, `program_override.h` (fork) | native address map appended to the lowered code, instruction capture bounds, override file format | full |
| `frontend/maxwell/translate/impl/internal_stage_buffer_entry_read.cpp`, `move_register.cpp` (fork edits) | `ISBE_nx2` task/mesh buffer access; native register-copy MOV | partial |
| `frontend/maxwell/control_flow.cpp`, `structured_control_flow.cpp`, `indirect_branch_table_track.cpp`, `decode.cpp`, `translate/impl/*` | CFG build, structurizer, per-opcode translation (mostly upstream) | skimmed (control_flow.cpp head only) |
| `frontend/ir/*` | IR types, emitter, `breadth_first_search.h` | breadth_first_search.h full; others not opened |
| `ir_opt/global_memory_to_storage_buffer_pass.cpp` | turns global loads/stores into SSBO accesses; native pointer tracking | partial (34-110, 1135-1732) |
| `ir_opt/texture_pass.cpp` | texture/image descriptor discovery, handle programs | partial (300-500, 1130-1408) |
| `ir_opt/position_pass.cpp` | `PositionPass`, fork `BlitConstantsPass` | full for BlitConstantsPass |
| `ir_opt/constant_propagation_pass.cpp` | folding; fork `FoldIMul32` etc. | skimmed |
| `ir_opt/*` others (ssa_rewrite, dce, identity_removal, lower_*, collect_shader_info, rescaling, layer, vendor_workaround, geometry_compaction, dual_vertex, conditional_barrier, verification) | upstream-style passes | not opened (names and order only) |
| `backend/spirv/emit_spirv.cpp`, `spirv_emit_context.cpp/.h` | `EmitSPIRV`, `EmitContext` | partial (emit_spirv.cpp 29-333; context 439-643, 988-1123) |
| `backend/spirv/emit_spirv_undefined.cpp`, `ssbo_align_gate.h` | undefined values, A/B gate | full |
| `backend/spirv/emit_spirv_mesh.cpp`, `emit_spirv_tensor.cpp`, other `emit_spirv_*.cpp` | mesh/tensor and per-opcode emission | not opened |
| `backend/glasm/*`, `backend/glsl/*` | OpenGL backends | not opened (OpenGL only) |
| `environment.h`, `runtime_info.h`, `shader_info.h`, `profile.h`, `stage.h`, `host_translate_info.h` | interfaces and data | environment.h, runtime_info.h full; others grep only |
| Outside the slice, read for the interface: `video_core/stage_cache.cpp/.h`, `video_core/shader_environment.cpp` (LowerNativeCode), `renderer_vulkan/vk_pipeline_cache.cpp` (translate/emit call sites) | consumers | partial |

## Main types and ownership

- `Environment` (abstract; implementations `GenericEnvironment`, `FileEnvironment` in `video_core/shader_environment.cpp`, plus `StageCache::Recorder`, a forwarding wrapper). Owned by the pipeline-building caller. It holds three public pass caches (`cbuf_word_cache`, `handle_cache`, `track_cache`) that only `TexturePass` uses and clears.
- `ObjectPool<IR::Inst/Block>` and `Flow::Block` pools are passed in by the caller (`ShaderPools` in vk_pipeline_cache.cpp, one per stage when stages are translated in parallel). IR lives until the SPIR-V is emitted; `StageCache::Summary` keeps only the metadata.
- `IR::Program` owns `blocks`, `post_order_blocks`, `syntax_list` (pointers into the pools) and `Info` (descriptors, handle programs, storage buffers, flags). `EmitSPIRV` returns `std::vector<u32>`.
- `Profile`: built once per `PipelineCache` from `Device` (capabilities, driver bug flags). `HostTranslateInfo`: same, for the frontend.
- `Ampere::LoweredProgram` is a value returned by `LowerToMaxwell`, laid out into guest-visible form by `LayOutLoweredNativeCode` (in the environment, not in the slice), which inserts a scheduling word every 32 bytes and appends the native address map.

## Threads and locking

The recompiler has no locks. All state is per call, except thread-local scratch in `texture_pass.cpp` (`g_handle_*`) and function-local `static const` env-var gates (initialised once, thread-safe). It is called from the GPU thread, from translation workers (`translation_workers`, only when an environment can be detached) and from boot-time disk-replay workers. `StageCache` has its own mutex (`mutex` for the table, `emitted_mutex` per translation); the lock is never held while translating. Environments for live draws read guest GPU memory and registers while translating, so a live environment must stay on the GPU thread (NX2_NOTES row 121 records translation-on-workers as not done for that reason); `parallel_stages` uses detached environments.

## Flow

1. Caller builds an `Environment` per stage (`ReadInstruction`, `ReadCbufValue`, texture type/format reads, `IsCbufStorageBufferPointer`). For a native program `GenericEnvironment::LowerNativeCode` captures the program (`ProgramCaptureState::Observe`), calls `Ampere::LowerToMaxwell`, lays the result out; failure throws `Shader::RuntimeError` ("Native shader at ..., instruction ...: reason").
2. Optionally `StageCache::Signature/Find` replays recorded reads; a hit skips `TranslateProgram` (and, when runtime key and bindings are equal, `EmitSPIRV`).
3. `TranslateProgram` (translate_program.cpp) in order: `ApplyDescriptorLimitPolicy`; `BuildASL` (CFG + structured control flow); `GenerateBlocks`, `PostOrder`; per-stage setup (tess, geometry pass-through, compute sizes, task/mesh from `MeshRegisters()`); `RemoveUnreachableBlocks`; `LowerFp64ToFp32`, `LowerFp16ToFp32`, `LowerInt64ToInt32`, `ConditionalBarrierPass` (each only if the host lacks the feature); `SsaRewritePass`; `ConstantPropagationPass`; `PositionPass`; `BlitConstantsPass`; `GlobalMemoryToStorageBufferPass`; `GeometryCompactionPass`; `TexturePass`; `RescalingPass` (if `resolution_info.active`); `DeadCodeEliminationPass`; `TightenOutputVertices`; `VerificationPass` (if `renderer_debug`); `CollectShaderInfoPass`; `LayerPass`; `VendorWorkaroundPass`; `CollectInterpolationInfo`; `AddNVNStorageBuffers`.
4. Caller combines stages: `MergeDualVertexPrograms`, `PruneUnreadGeometryOutputs`, `ConvertLegacyToGeneric(program, runtime_info)` (needs the previous stage), then `EmitSPIRV(profile, runtime_info, program, binding)`: `EmitContext` constructor defines types, interfaces, mesh, local/shared memory, constant buffers, storage buffers, textures, images, global-memory helper functions; `DefineMain` walks `syntax_list` (`Traverse`); `DefineEntryPoint` sets execution mode.
5. Caller wraps the words in a shader module (`BuildShader`). A `Shader::Exception` anywhere above is caught in `CreateGraphicsPipeline`, logged, environments dumped, and the pipeline is not built: the draw is skipped, and a negative cache remembers the first exception per shader address.

## Connections

- In: `renderer_vulkan/vk_pipeline_cache.cpp` (`CreateGraphicsPipeline`, compute path ~3464-3520, `MakeRuntimeInfo`, Profile and host_info construction), `video_core/shader_environment.cpp`, `video_core/stage_cache.cpp`, boot-time replay (`LoadPipelines`), `shader_warm/warm_records.cpp`.
- Out: `Settings::values` (below), `Common::EnvGate`, `Common::CityHash64`; guest memory only through `Environment`. SPIR-V is built with the vendored `sirit`.

## Settings and switches

Settings: `resolution_info.{active,up_scale,down_shift,up_factor,down_factor}`, `renderer_debug`, `disable_shader_loop_safety_checks` (default false), `fix_bloom_effects`, `dump_guest_shaders`, `gpu_log_shader_dumps`. All of the first four are in `StageCache::Signature`. Environment variables (read once per process unless noted): `EDEN_NX2_UNDEF_ZERO` (default on: undefined U1/U32 become zero), `EDEN_NX2_LEAN_SSBO` (default on: global-memory helpers are not inlined; avoids multi-second NVIDIA driver compiles), `EDEN_NX2_WIDE_COMPUTE_SSBOS`, `EDEN_NX2_LOWER_EXTENDED` (per call), `EDEN_NX2_TENSOR`, `EGLANTINE_SSBO_ALIGN_FIX`, `EGLANTINE_CAS_RETRY_FIX`, `EGLANTINE_DEPTH_FLOAT_FIX`, `EGLANTINE_AMD_FRAGMENT_SHUFFLE` (Profile), `EDEN_NX2_STAGE_CACHE` (`0`, `verify`), `EDEN_NX2_SHADER_DUMP`, `EDEN_NX2_REJECT`, `EDEN_NX2_REJECT_HASH`, `EDEN_NX2_RESETFIX`, `EDEN_NATIVE_SHADER_OVERRIDE_DIR`, `EDEN_NX2_MESH_EMULATION_DUMP`. The env gates change SPIR-V but are not in any cache key; the comments say to clear the shader cache when switching. `CACHE_VERSION` (35) in vk_pipeline_cache.cpp must be bumped by hand when translation changes.

## Invariants

- Translation is deterministic given (program, environment answers, settings, profile): the stage cache relies on this and checks it only in `verify` mode.
- Every environment read that affects IR must go through an `Environment` virtual so `Recorder` logs it (assumed; not enforced). `Signature` lists the non-virtual inputs.
- `Ampere::LowerToMaxwell` output addresses are `0x50 + index*8 + ((index+1)/3)*8`; branch targets must be 16-byte aligned and not beyond the current instruction when resolved (enforced in `finish()`).
- Native address map: count at most 65536, entries 16-byte aligned (enforced in `NativeProgramAddress`).
- Storage buffer count per stage must not exceed `max_ssbos` (enforced by throwing in `GlobalMemoryToStorageBufferPass`).
- `RuntimeKey` must contain every `RuntimeInfo` field that changes SPIR-V (comment says so; assumed, not checked, except by `verify`).
- Mesh output topology is 0, 1 or 4 (enforced by throw).

## Hazards and improvement candidates

See `findings/shader-recompiler.json`. In short: the loop safety counter budget is shared across re-entries; the native-pointer global-memory path has no upper bound and truncates offsets to 32 bits; `BlitConstantsPass` is not gated on native programs; the native-override debug path can block a translating thread for 30 minutes.

## Extra: nx2 (fork) code and failure behaviour

- Rejection reasons: `lower_to_maxwell.h` has over 150 distinct "native ..." `fail(...)` reasons and ends in `unsupported native opcode`. Other hard rejects: `NotImplementedException` for per-component interpolation, mesh topologies other than 0/1/4, `AddMeshLines` on clip distances/point size/fog, ISBE on non task/mesh stages, U8 attribute writes, `UndefU8/U16/U64` emission, too many SSBOs, unexpected separate sampler for images. All become "draw skipped" through the catch in `CreateGraphicsPipeline`.
- Silent fallbacks that do not throw: handle-program textures assume a 2D float texture (`ReadTextureTypeCached`); undefined values become zero; non-pointer cbuf slots in native programs are classified by reading guest memory at translate time (`IsCbufStorageBufferPointer`), cached in `storage_pointer_types`, with `NullStoragePointerSlots` marking stale translations; `MOV` with mask 0 and bits 44-47 = 0xA is a native register copy; ISBERD/ISBEWR lower to the pseudo-instruction `ISBE_nx2` (0xefc0).
- Passes with non-linear cost: `BreadthFirstSearch` (fork switches to a hash set past 64 visited nodes: one 28k-instruction compute shader went from 3.4 s to 0.85 s), `GlobalMemoryToStorageBufferPass` (up to three `collect()` rounds, memoised pruned BFS), `Track` in `texture_pass.cpp` (one BFS per texture instruction, cache cleared per block), the O(instructions x call targets) loop in `LowerToMaxwell` after the main exit (lower_to_maxwell.h:320), and the re-lowering loop in `LowerToMaxwell` while `high_uniform_registers` grows.
- Compile-stutter sources: translation on the GPU thread for live environments; large native compute shaders in the global-memory pass; SPIR-V with many SSBO candidates (NVIDIA compile time, hence lean mode); `BuildShader` module creation; stage-cache misses (a hit skips translation and, with equal runtime key, emission). The cache drops everything when it passes 256 MB (`StageCache::Account`).
