// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <string_view>

#include "common/common_types.h"
#include "common/logging.h"

// Switches for the NX2 GPU optimizations, all on by default (names in opt_names).
// EDEN_NX2_PERF_OPT=0 turns every one off; EDEN_NX2_PERF_SAFE=1 turns off the experimental ones
// (DefaultOffMask); EDEN_NX2_PERF_ON=<name>,... turns named ones back on and
// EDEN_NX2_PERF_OFF=<name>,... turns named ones off. EDEN_NX2_PERF_VERIFY=1 computes each
// optimized answer the old way too, logs any difference and uses the old answer.
namespace VideoCommon::Nx2Perf {

enum class Opt : unsigned {
    Bounds,            ///< MaxContinuousRange / GetMemoryLayoutSize from map-time runs
    Method,            ///< Maxwell3D fast method dispatch on the back
    CbBatch,           ///< Consecutive constant buffer words uploaded in one write
    CleanWords,        ///< SynchronizeBuffer skips clean tracker words
    IndexRebind,       ///< Repeated vkCmdBindIndexBuffer skipped
    UsageTracker,      ///< UsageTracker::Track skips pages known to be used
    FrontSleep,        ///< The split front sleeps instead of yielding while it waits
    SubmitBudget,      ///< 4096-draw native submission budget outside MoltenVK
    FragmentBarrier,   ///< FragmentBarrier deferred to the next draw that reads the pass
    FenceFlush,        ///< 5 ms DeferFlush, command list copies submitted at once
    PipelineRetention, ///< Sixteen sessions of used pipelines, saved every 15 s
    SortedPageBatches,
    QmdScratch,
    QueryDiagnostics,
    QueryUploadScratch,
    MacroReadEarly, ///< MME DMA reads outside GPU-written 1 MiB regions skip the full drain
    MacroReadCbOverlay, ///< Early MME reads overlay queued constant buffer words instead of waiting
    MacroReadWriterSet, ///< Early MME reads wait only for entries that can write memory
    PreflushAging,      ///< Preflush marks the CPU stopped reading age out
    EagerPublish,       ///< The front publishes ring entries while the back is idle, and at writers
    FrontSameFirst,     ///< Split front: same-value state writes skipped before any other work
    FrontClassTable,    ///< Split front: executable method classes from one precomputed table
    FrontRun,           ///< Split front: a command's direct 3D register words written in one call
    MacroBatch,  ///< Front macro writes buffered in the JIT, same-value ones dropped inline
    MacroBorrow, ///< Front macros read their parameters from the command words in place
    MacroState,  ///< The front macro JIT keeps its State across calls
    DownloadBatch,      ///< Fence-time downloads: one copy per buffer, ranges <= 4 KiB apart merged
    PipelineFastCache,  ///< Direct-mapped graphics pipeline cache in front of the slow path
    DrawPreamble,       ///< GPU::TickWork skips the sync mutex when no request is queued
    BindMemo,           ///< Unchanged non-written binds skip touch, sync and usage across draws
    /// Split back, Ampere: submits recorded draws while the front waits for the back or the
    /// host GPU has run dry, so a command list copy waits only for the tail behind it
    SubmitWhileWaited,
    /// Pipeline eviction keeps the ones built at boot that this session has not drawn with yet
    PipelineKeepPreloaded,
    /// Native 64 KiB uniform banks: guest bytes compared with the shadow on the frame's first
    /// bind only, not on every draw (a later CPU rewrite in the frame shows one frame late)
    BankOncePerFrame,
    /// GPUDirtyMemoryManager::Gather returns without its locks when nothing was collected
    DirtyGatherFast,
    /// Texture handles of one draw stage read through one translated constant buffer page,
    /// not a GPU page walk per handle
    HandlePageCache,
    /// Split front: a native macro call whose MME DMA read only feeds its last writes (render
    /// enable) runs without waiting for the back; the back writes those from the real words
    DmaForward,
    /// Split front: an instance loop's draw.begin right after the same one, still unpublished,
    /// is counted into that ring entry (bits 24-31); the back adds the instances
    DrawBeginCoalesce,
    /// A suspended channel's 1 ms front sleep ends at once on a new submit or on a GPU release
    /// that latches a waiter (Tegra::Nx2PendingWake in nx2.h)
    PendingWake,
    /// Diagnostics (EDEN_NX2_DIAG*): the back counts draws in a thread-local table with cached
    /// ablate/inspect answers and folds them into the shared state under one lock
    DiagDrawLite,
    /// Async shaders: once a frame spent its budget building pipelines at run time, a draw that
    /// may be skipped while its pipeline builds is skipped without building it (built next frame)
    ShaderBuildBudget,
    /// Stale-slot compute rebuilds keep dispatching the previous pipeline while the replacement
    /// compiles on a worker thread, swapping it in once IsBuilt() is true
    ComputeRebuildAsync,
    /// Boot disk-cache loading compiles every persisted compute translation with a distinct
    /// MappedStoragePointerKeys and swaps to a built alternate on a stale-slot trigger
    ComputePreloadVariants,
    /// Async shaders: skip a small no-depth draw instead of waiting when its color target already
    /// holds an earlier frame's output and was not fully cleared this frame; treat indirect draws
    /// (draw_element_count == 0) as not small
    StaleTargetSkip,
    /// Async shaders: when a depth-tested color draw is skipped, record its geometry identity and
    /// skip depth-only draws for the same geometry for up to 2 frames or until the material
    /// pipeline is built
    GeometryConsistency,
    /// Async shaders: reserve half the per-frame shader build budget for G-buffer draws and the
    /// highest-priority deferred keys from the previous frame (G-buffer first, then by draw count)
    BuildOrderPriority,
    /// A Switch 2 semaphore acquire that an already issued release (its guest write still
    /// delayed by the fence behavior) will meet continues at once instead of waiting for the
    /// host GPU (Tegra::Nx2IssuedReleases in nx2.h)
    AcquireIssued,
    /// Texture cache: final host image extents clamped to >= 1, image construction failure
    /// non-fatal (emergency eviction and one retry, then native resolution), budget refreshed on
    /// failure
    ImageAdmission,
    /// Switch 2 titles: a GPFIFO entry's sync bit keeps its hardware meaning (fetch ordered after
    /// Host processing) and no longer waits for the host GPU when "sync memory operations" is on
    GpSyncHost,
    /// Pipeline driver-build and translation pools run at the host's physical cores less six,
    /// shared by both pools (at least 2 each), once the disk cache is loaded
    WorkerBudget,
    /// Discrete GPU with VK_EXT_memory_budget: device memory limit = reported heap budget minus
    /// the reserve, not 6 GiB + 1 GiB per resolution step
    VramBudgetReported,
    /// VK_EXT_memory_priority and VK_EXT_pageable_device_local_memory: render-target images 0.75,
    /// everything else 0.5
    VramPriority,
    /// Query cache: a report whose delayed operation already ran is not rewritten by a later
    /// partial flush
    QueryLanded,
    /// A macro's DMA read of guest memory first settles issued releases (AcquireIssued) whose
    /// guest write is still pending for the range
    DmaReadSettle,
    /// Native uniform bank shadow: an entry is not zero-filled on insert and one entry is evicted
    /// at the cap, not the whole map
    UniformShadowLean,
    /// Compute pipelines known to take tens of seconds in the driver's optimiser are built with
    /// optimisation disabled
    ComputeUnoptimizedSlow,
    /// Disk cache load: a compute pipeline the driver cache does not hold is built in the
    /// background, not waited for before the guest starts
    ComputeBootDeferred,
    /// Image view and sampler handles of a draw's descriptors are memoised per binding
    DescriptorHandleMemo,
    /// An unmet semaphore acquire suspends its channel behind the ordered fences, not the thread
    SemaphoreFenceLean,
    /// A draw whose image views and samplers equal the previous draw's reuses its descriptors
    DescriptorRunReuse,
    Count,
};

inline constexpr std::array<std::string_view, static_cast<size_t>(Opt::Count)> opt_names{
    "bounds", "method", "cb_batch", "clean_words", "index_rebind", "usage_tracker", "front_sleep",
    "submit_budget", "fragment_barrier", "fence_flush", "pipeline_retention", "sorted_page_batches",
    "qmd_scratch", "query_diagnostics", "query_upload_scratch", "macro_read_early",
    "macro_read_cb_overlay", "macro_read_writer_set", "preflush_aging", "eager_publish",
    "front_same_first", "front_class_table", "front_run", "macro_batch", "macro_borrow",
    "macro_state", "download_batch", "pipeline_fast_cache", "draw_preamble", "bind_memo",
    "submit_while_waited", "pipeline_keep_preloaded", "bank_once_per_frame", "dirty_gather_fast",
    "handle_page_cache", "dma_forward", "draw_begin_coalesce", "pending_wake", "diag_draw_lite",
    "shader_build_budget", "compute_rebuild_async", "compute_preload_variants", "stale_target_skip",
    "geometry_consistency", "build_order_priority", "acquire_issued",
    "image_admission", "gp_sync_host", "worker_budget", "vram_budget_reported", "vram_priority",
    "query_landed", "dma_read_settle", "uniform_shadow_lean", "compute_unoptimized_slow",
    "compute_boot_deferred", "descriptor_handle_memo", "semaphore_fence_lean",
    "descriptor_run_reuse",
};

static_assert(static_cast<size_t>(Opt::Count) <= 64, "the off-mask is 64 bits");
static_assert(
    [] {
        for (const std::string_view name : opt_names) {
            if (name.empty()) {
                return false;
            }
        }
        return true;
    }(),
    "every switch needs a name in opt_names");

constexpr u64 Bit(Opt opt) {
    return u64{1} << static_cast<unsigned>(opt);
}

/// Bit i of an off-mask is opt_names[i]: set means off.
constexpr bool IsOff(u64 mask, size_t index) {
    return ((mask >> index) & 1) != 0;
}

/// Off under EDEN_NX2_PERF_SAFE unless EDEN_NX2_PERF_ON names them: the experimental switches.
inline constexpr u64 DefaultOffMask() {
    u64 mask = 0;
    for (const Opt opt : {Opt::MacroReadCbOverlay, Opt::MacroReadWriterSet, Opt::PreflushAging,
                          Opt::EagerPublish, Opt::FrontSameFirst, Opt::FrontClassTable,
                          Opt::FrontRun, Opt::MacroBatch, Opt::MacroBorrow, Opt::MacroState,
                          Opt::DownloadBatch, Opt::PipelineFastCache, Opt::DrawPreamble,
                          Opt::BindMemo, Opt::SubmitWhileWaited, Opt::DmaForward,
                          Opt::BankOncePerFrame, Opt::DrawBeginCoalesce, Opt::PendingWake,
                          Opt::DiagDrawLite, Opt::WorkerBudget, Opt::VramBudgetReported,
                          Opt::VramPriority, Opt::SemaphoreFenceLean, Opt::DescriptorRunReuse}) {
        mask |= Bit(opt);
    }
    return mask;
}

/// Off unless EDEN_NX2_PERF_ON names them: built, not yet proven by a lab run.
inline constexpr u64 OptInMask() {
    return Bit(Opt::SemaphoreFenceLean) | Bit(Opt::DescriptorRunReuse);
}

/// The off-mask from EDEN_NX2_PERF_SAFE (safe), EDEN_NX2_PERF_ON (on) and EDEN_NX2_PERF_OFF (off).
inline u64 ParseOffMask(bool safe, std::string_view on, std::string_view off) {
    u64 mask = (safe ? DefaultOffMask() : u64{0}) | OptInMask();
    while (!on.empty()) {
        const size_t comma = on.find(',');
        const std::string_view name = on.substr(0, comma);
        for (size_t i = 0; i < opt_names.size(); ++i) {
            if (name == opt_names[i]) {
                mask &= ~Bit(static_cast<Opt>(i));
            }
        }
        on = comma == std::string_view::npos ? std::string_view{} : on.substr(comma + 1);
    }
    while (!off.empty()) {
        const size_t comma = off.find(',');
        const std::string_view name = off.substr(0, comma);
        bool known = false;
        for (size_t i = 0; i < opt_names.size(); ++i) {
            if (name == opt_names[i]) {
                mask |= Bit(static_cast<Opt>(i));
                known = true;
            }
        }
        if (!known && !name.empty()) {
            LOG_WARNING(HW_GPU, "Nx2Perf: unknown name {} in EDEN_NX2_PERF_OFF", name);
        }
        off = comma == std::string_view::npos ? std::string_view{} : off.substr(comma + 1);
    }
    return mask;
}

inline bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_PERF_OPT");
        return value == nullptr || value[0] != '0';
    }();
    return enabled;
}

namespace detail {
/// Off-mask in effect (bit set = off; every switch when EDEN_NX2_PERF_OPT=0); ~0 before the
/// first On().
inline std::atomic<u64> live_off_mask{~u64{0}};
u64 InitOffMask();
} // namespace detail

inline bool On(Opt opt) {
    u64 off = detail::live_off_mask.load(std::memory_order_relaxed);
    if (off == ~u64{0}) [[unlikely]] {
        off = detail::InitOffMask();
    }
    return !IsOff(off, static_cast<size_t>(opt));
}

inline bool Verify() {
    static const bool verify = [] {
        const char* value = std::getenv("EDEN_NX2_PERF_VERIFY");
        return value != nullptr && value[0] == '1';
    }();
    return verify;
}

} // namespace VideoCommon::Nx2Perf
