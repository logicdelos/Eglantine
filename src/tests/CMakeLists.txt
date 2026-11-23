# SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# SPDX-FileCopyrightText: 2018 yuzu Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

add_executable(tests
    core/nx2_frame_interval.cpp
    video_core/upload_batch.cpp
    video_core/texture_swizzle.cpp
    video_core/texture_unswizzle_bands.cpp
    core/nx2_oot_time.cpp
    core/nvn_epoch_slot.cpp
    core/nvn_census_verdict.cpp
    video_core/unbacked_image_policy.cpp
    core/builtin_patches.cpp
    core/launch_params.cpp
    core/jit_prewarm.cpp
    core/jit_prewarm_rebase.cpp
    core/gpu_dirty_memory_manager.cpp
    video_core/macro_dma_forward.cpp
    video_core/gpu_timestamp.cpp
    video_core/maxwell_fast_method.cpp
    video_core/split_write_tracker.cpp
    video_core/list_writer_trace.cpp
    video_core/list_copy_demand.cpp
    video_core/buffer_dependency_trace.cpp
    video_core/nx2_written_pages.cpp
    video_core/frame_dump_schedule.cpp
    core/nx2_debug_server.cpp
    core/system_test_context.h
    video_core/ampere_logged_numeric.cpp
    video_core/pipeline_optimization.cpp
    video_core/spirv_atomic_retry.cpp
    video_core/spirv_storage_alignment.cpp
    video_core/spirv_fragment_shuffle.cpp
    video_core/mesh_fallback.cpp
    video_core/spirv_software_matrix.cpp
    common/baseline_settings.cpp
    common/baseline_config.cpp
    common/fresh_profile.cpp
    core/launch_params_early.cpp
    common/speed_limit.cpp
    common/log_retention.cpp
    common/read_attribution.cpp
    common/nx2_bottleneck_present.cpp
    common/system_log.cpp
    common/nx2_diagnostics.cpp
    common/test_frame_delay.cpp
    common/ui_e_profile_migration.cpp
    common/ui_e_restore_eden_path.h
    common/frame_timing_log.cpp
    core/conductor_pace.cpp
    yuzu_cmd/run_report_documents.cpp
    ../yuzu_cmd/run_report.cpp
    common/bit_field.cpp
    common/cityhash.cpp
    common/container_hash.cpp
    common/cpu_layout.cpp
    common/event.cpp
    common/fibers.cpp
    common/game_presets.cpp
    common/host_memory.cpp
    common/key_script.cpp
    common/page_table.cpp
    common/progress_counter.cpp
    common/param_package.cpp
    common/range_map.cpp
    common/range_set_subtract.cpp
    common/ring_buffer.cpp
    common/scratch_buffer.cpp
    common/slot_vector.cpp
    common/unique_function.cpp
    common/zstd_compression.cpp
    video_core/replay_session.cpp
    core/core_timing.cpp
    core/ldn.cpp
    core/internal_network/socket_proxy.cpp
    core/nup_writer.cpp
    core/nup_integrity.cpp
    core/nup_zstd_read.cpp
    core/nx2_debug_server.cpp
    core/nx2_movie_pacing.cpp
    core/nx2_gpu_memory_registry.cpp
    core/guest_sample.cpp
    core/priority_queue.cpp
    core/replay_kernel.cpp
    core/scheduler_idle_migration.cpp
    core/scheduler_mixed_yield.cpp
    core/internal_network/network.cpp
    video_core/frame_dump_schedule.cpp
    video_core/framebuffer_limits.cpp
    video_core/memory_tracker.cpp
    video_core/nx2_perf.cpp
    video_core/nx2_perf_draw_binds.cpp
    video_core/nx2_perf_texture_writes.cpp
    video_core/nx2_perf_native_qmd.cpp
    video_core/nx2_perf_query_callback.cpp
    video_core/nx2_perf_switches.cpp
    video_core/memory_manager_runs.cpp
    video_core/nx2_sparse_residency.cpp
    video_core/spirv_storage_alignment.cpp
    video_core/pipeline_cache_dir.cpp
    video_core/pipeline_optimization.cpp
    video_core/spirv_atomic_retry.cpp
    video_core/spirv_fragment_shuffle.cpp
    video_core/ampere_depth_compare.cpp
    video_core/shader_bench.cpp
    video_core/shader_precache.cpp
    video_core/shader_precache_identity.cpp
    video_core/cache_envelope.cpp
    video_core/nx2_attribution.cpp
    video_core/pipeline_prepare.cpp
    video_core/replay_capture.cpp
    video_core/report_schema2.cpp
    video_core/replay_checkpoint.cpp
    video_core/replay_codec.cpp
    video_core/replay_format.cpp
    video_core/replay_memory.cpp
    video_core/replay_options.cpp
    video_core/replay_report.cpp
    ../yuzu_cmd/report_schema2.cpp
    video_core/h264_packet.cpp
    video_core/vertex_repack.cpp
    video_core/virtual_range_cache.cpp
    video_core/query_copy.cpp
    video_core/ampere_lowering.cpp
    video_core/ampere_capture_end.cpp
    video_core/spirv_software_matrix.cpp
    video_core/ampere_geometry.cpp
    video_core/ampere_i2f.cpp
    video_core/ampere_gradient.cpp
    video_core/ampere_returns.cpp
    video_core/ampere_guarded_ulea.cpp
    video_core/ampere_compute_lowering.cpp
    video_core/native_macro_fifo.cpp
    video_core/native_image_matching.cpp
    video_core/texture_cache_budget.cpp
    video_core/usage_tracker.cpp
    video_core/native_swizzle.cpp
    video_core/image_allocation_pool.cpp
    video_core/plain_unswizzle.cpp
    video_core/uniform_stream_reuse.cpp
    video_core/shader_invalidation.cpp
    video_core/descriptor_table.cpp
    video_core/global_address_tracking.cpp
    video_core/global_atomic_routing.cpp
    video_core/nvn_storage_descriptor.cpp
    video_core/ampere_f2i.cpp
    video_core/ampere_ldc.cpp
    video_core/ampere_vote.cpp
    yuzu_cmd/cli_settings.cpp
    ../yuzu_cmd/cli_settings.cpp
    yuzu_cmd/control_channel.cpp
    ../yuzu_cmd/control_channel.cpp
    yuzu_cmd/frame_capture.cpp
    ../yuzu_cmd/frame_capture.cpp
    video_core/shader_attributes.cpp
    video_core/indirect_reset.cpp
    video_core/ocarina_upscale.cpp
    video_core/compute_clean_cut.cpp
    video_core/nx2_profile_lite.cpp
    video_core/nx2_host_gpu.cpp
    input_common/calibration_configuration_job.cpp
    frontend_common/ui_e_install_keys.cpp
    frontend_common/ui_e_profile_import.cpp
)

create_target_directory_groups(tests)

if (ENABLE_UPDATE_CHECKER)
    target_sources(tests PRIVATE
        common/update_checker.cpp
        common/update_installer.cpp
        ../frontend_common/update_checker.cpp
        ../frontend_common/update_installer.cpp)
endif()

target_link_libraries(tests PRIVATE common core input_common video_core frontend_common)
if (MSVC)
    # tests/core/launch_params.cpp includes <getopt.h>: the bundled getopt that core links.
    target_link_libraries(tests PRIVATE getopt)
endif()

if (ENABLE_MCP_SERVER)
    target_sources(tests PRIVATE mcp/protocol.cpp)
    target_link_libraries(tests PRIVATE mcp)
endif()
target_link_libraries(tests PRIVATE ${PLATFORM_LIBRARIES} Catch2::Catch2WithMain Threads::Threads)

add_test(NAME tests COMMAND tests)

find_package(Python3 3.8 QUIET COMPONENTS Interpreter)
if (Python3_Interpreter_FOUND)
    set(replay_tool "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/replay_qualify")
    add_test(NAME replay_qualify_controls COMMAND Python3::Interpreter -I -S -B
             "${replay_tool}/replay_qualify.py" --selftest)
    add_test(NAME replay_qualify_fixture COMMAND Python3::Interpreter -I -S -B
             "${replay_tool}/replay_qualify.py" --verify "${replay_tool}/fixtures/synthetic_scene")
endif()

# needed for vma
if(NOT MSVC)
    target_compile_options(tests PRIVATE
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-conversion>
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-unused-variable>
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-unused-parameter>
        $<$<COMPILE_LANGUAGE:C,CXX>:-Wno-missing-field-initializers>)
endif()
