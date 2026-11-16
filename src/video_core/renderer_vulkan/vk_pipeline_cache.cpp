// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <span>
#include <thread>
#include <unordered_set>
#include <vector>
#include "common/cityhash.h"
#include "common/env_gate.h"
#include "common/fs/fs.h"
#include "common/fs/path_util.h"
#include "common/scope_exit.h"
#include "common/thread_worker.h"
#include "core/core.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/environment.h"
#include "shader_recompiler/exception.h"
#include "shader_recompiler/frontend/maxwell/control_flow.h"
#include "shader_recompiler/frontend/maxwell/translate_program.h"
#include "shader_recompiler/ir_opt/passes.h"
#include "shader_recompiler/program_header.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/gpu_logging/gpu_logging.h"
#include "video_core/memory_manager.h"
#include "video_core/nx2_perf.h"
#include "video_core/nx2_spikes.h"
#include "video_core/renderer_vulkan/fixed_pipeline_state.h"
#include "video_core/renderer_vulkan/maxwell_to_vk.h"
#include "video_core/renderer_vulkan/pipeline_cache_dir.h"
#include "video_core/renderer_vulkan/pipeline_helper.h"
#include "video_core/renderer_vulkan/pipeline_statistics.h"
#include "video_core/renderer_vulkan/vertex_repack.h"
#include "video_core/renderer_vulkan/vk_compute_pipeline.h"
#include "video_core/renderer_vulkan/vk_cooperative_hmma_test.h"
#include "video_core/renderer_vulkan/vk_descriptor_pool.h"
#include "video_core/renderer_vulkan/vk_driver_build_report.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/renderer_vulkan/vk_update_descriptor.h"
#include "video_core/pipeline_prepare.h"
#include "video_core/shader_cache.h"
#include "video_core/shader_environment.h"
#include "video_core/shader_notify.h"
#include "video_core/shader_warm/shader_warm_builder.h"
#include "video_core/shader_warm/warm_records.h"
#include "video_core/surface.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

#ifdef __ANDROID__
#include "../../android/app/src/main/jni/android_settings.h"
#endif

namespace Vulkan {

namespace CE = VideoCommon::CacheEnvelope;
namespace PP = VideoCommon::PipelinePrepare;
namespace Spikes = VideoCommon::Nx2Spikes;

namespace {
using Shader::Backend::SPIRV::EmitSPIRV;
using Shader::Maxwell::ConvertLegacyToGeneric;
using Shader::Maxwell::GenerateGeometryPassthrough;
using Shader::Maxwell::MergeDualVertexPrograms;
using Shader::Maxwell::TranslateProgram;
using VideoCommon::ComputeEnvironment;
using VideoCommon::FileEnvironment;
using VideoCommon::GenericEnvironment;
using VideoCommon::GraphicsEnvironment;

// Bump CACHE_VERSION for translation changes: it is the shader format stamp of every cache file
// (see MakeCacheIdentity), so older files are rejected whole and recorded again.
constexpr u32 CACHE_VERSION = 35;
static_assert(CACHE_VERSION == VideoCommon::ShaderWarm::WarmCacheVersion,
              "Shader precache files use the pipeline cache version");
constexpr size_t VULKAN_CACHE_FLUSH_PIPELINES = 128;
constexpr size_t VULKAN_CACHE_FLUSH_MIN_SECONDS = 30;

template <typename Container>
auto MakeSpan(Container& container) {
    return std::span(container.data(), container.size());
}

Shader::OutputTopology MaxwellToOutputTopology(Maxwell::PrimitiveTopology topology) {
    switch (topology) {
    case Maxwell::PrimitiveTopology::Points:
        return Shader::OutputTopology::PointList;
    case Maxwell::PrimitiveTopology::LineStrip:
        return Shader::OutputTopology::LineStrip;
    default:
        return Shader::OutputTopology::TriangleStrip;
    }
}

Shader::CompareFunction MaxwellToCompareFunction(Maxwell::ComparisonOp comparison) {
    switch (comparison) {
    case Maxwell::ComparisonOp::Never_D3D:
    case Maxwell::ComparisonOp::Never_GL:
        return Shader::CompareFunction::Never;
    case Maxwell::ComparisonOp::Less_D3D:
    case Maxwell::ComparisonOp::Less_GL:
        return Shader::CompareFunction::Less;
    case Maxwell::ComparisonOp::Equal_D3D:
    case Maxwell::ComparisonOp::Equal_GL:
        return Shader::CompareFunction::Equal;
    case Maxwell::ComparisonOp::LessEqual_D3D:
    case Maxwell::ComparisonOp::LessEqual_GL:
        return Shader::CompareFunction::LessThanEqual;
    case Maxwell::ComparisonOp::Greater_D3D:
    case Maxwell::ComparisonOp::Greater_GL:
        return Shader::CompareFunction::Greater;
    case Maxwell::ComparisonOp::NotEqual_D3D:
    case Maxwell::ComparisonOp::NotEqual_GL:
        return Shader::CompareFunction::NotEqual;
    case Maxwell::ComparisonOp::GreaterEqual_D3D:
    case Maxwell::ComparisonOp::GreaterEqual_GL:
        return Shader::CompareFunction::GreaterThanEqual;
    case Maxwell::ComparisonOp::Always_D3D:
    case Maxwell::ComparisonOp::Always_GL:
        return Shader::CompareFunction::Always;
    }
    UNIMPLEMENTED_MSG("Unimplemented comparison op={}", comparison);
    return {};
}

Shader::AttributeType CastAttributeType(const FixedPipelineState::VertexAttribute& attr) {
    if (attr.enabled == 0) {
        return Shader::AttributeType::Disabled;
    }
    switch (attr.Type()) {
    case Maxwell::VertexAttribute::Type::UnusedEnumDoNotUseBecauseItWillGoAway:
        ASSERT_MSG(false, "Invalid vertex attribute type!");
        return Shader::AttributeType::Disabled;
    case Maxwell::VertexAttribute::Type::SNorm:
    case Maxwell::VertexAttribute::Type::UNorm:
    case Maxwell::VertexAttribute::Type::Float:
        return Shader::AttributeType::Float;
    case Maxwell::VertexAttribute::Type::SInt:
        return Shader::AttributeType::SignedInt;
    case Maxwell::VertexAttribute::Type::UInt:
        return Shader::AttributeType::UnsignedInt;
    case Maxwell::VertexAttribute::Type::UScaled:
        return Shader::AttributeType::UnsignedScaled;
    case Maxwell::VertexAttribute::Type::SScaled:
        return Shader::AttributeType::SignedScaled;
    }
    return Shader::AttributeType::Float;
}

Shader::AttributeType AttributeType(const FixedPipelineState& state, size_t index) {
    switch (state.DynamicAttributeType(index)) {
    case 0:
        return Shader::AttributeType::Disabled;
    case 1:
        return Shader::AttributeType::Float;
    case 2:
        return Shader::AttributeType::SignedInt;
    case 3:
        return Shader::AttributeType::UnsignedInt;
    }
    return Shader::AttributeType::Disabled;
}

// Switch 2 geometry shaders may write all 32 generic outputs next to Position and Layer: 133
// components where Vulkan devices allow 128 (maxGeometryOutputComponents). AMD's Windows driver
// closed the emulator compiling such a shader at the Mario Tennis Fever match start. Drop the
// outputs the fragment shader never reads; skipped where it can't tell (legacy, indexed, XFB) and
// for passthrough shaders the host runs natively (no IR writes the outputs there).
void PruneUnreadGeometryOutputs(Shader::IR::Program& geometry, const Shader::IR::Program& fragment,
                                const GraphicsPipelineCacheKey& key, bool native_passthrough) {
    const Shader::Info& fs{fragment.info};
    Shader::VaryingState& stores{geometry.info.stores};
    if (fragment.stage != Shader::Stage::Fragment || key.state.xfb_enabled ||
        (geometry.is_geometry_passthrough && native_passthrough) || fs.loads_indexed_attributes ||
        fs.loads.Legacy() || stores.Legacy()) {
        return;
    }
    for (size_t index = 0; index < Shader::IR::NUM_GENERICS; ++index) {
        if (!fs.loads.Generic(index)) {
            for (size_t element = 0; element < 4; ++element) {
                stores.Set(Shader::IR::Attribute::Generic0X + index * 4 + element, false);
            }
        }
    }
}

Shader::RuntimeInfo MakeRuntimeInfo(std::span<const Shader::IR::Program> programs,
                                    const GraphicsPipelineCacheKey& key,
                                    const Shader::IR::Program& program,
                                    const Shader::IR::Program* previous_program,
                                    const Vulkan::Device& device) {
    Shader::RuntimeInfo info;
    if (previous_program) {
        info.previous_stage_stores = previous_program->info.stores;
        info.previous_stage_legacy_stores_mapping = previous_program->info.legacy_stores_mapping;
        if (previous_program->is_geometry_passthrough) {
            info.previous_stage_stores.mask |= previous_program->info.passthrough.mask;
        }
    } else {
        info.previous_stage_stores.mask.set();
    }
    if (previous_program && previous_program->stage == Shader::Stage::Mesh) {
        for (const u32 line : previous_program->mesh.primitive_lines) {
            const auto attribute{static_cast<Shader::IR::Attribute>(line)};
            if (Shader::IR::IsGeneric(attribute)) {
                info.per_primitive_generics.set(Shader::IR::GenericAttributeIndex(attribute));
            }
        }
    }
    const Shader::Stage stage{program.stage};
    const bool has_geometry{key.unique_hashes[4] != 0 && !programs[4].is_geometry_passthrough};
    const bool gl_ndc{key.state.ndc_minus_one_to_one != 0};
    const float point_size{std::bit_cast<float>(key.state.point_size)};
    switch (stage) {
    case Shader::Stage::VertexB:
        if (!has_geometry) {
            if (key.state.topology == Maxwell::PrimitiveTopology::Points) {
                info.fixed_state_point_size = point_size;
            }
            if (key.state.xfb_enabled) {
                if (device.IsExtTransformFeedbackSupported()) {
                    auto [varyings, count] =
                        VideoCommon::MakeTransformFeedbackVaryings(key.state.xfb_state);
                    info.xfb_varyings = varyings;
                    info.xfb_count = count;
                } else {
                    LOG_WARNING(Render_Vulkan,
                                "XFB requested in pipeline key but device lacks "
                                "VK_EXT_transform_feedback; ignoring XFB decorations");
                }
            }
            info.convert_depth_mode = gl_ndc;
        }
        if (key.state.dynamic_vertex_input) {
            for (size_t index = 0; index < Maxwell::NumVertexAttributes; ++index) {
                info.generic_input_types[index] = AttributeType(key.state, index);
            }
        } else {
            std::ranges::transform(key.state.attributes, info.generic_input_types.begin(),
                                   &CastAttributeType);
        }
        break;
    case Shader::Stage::TessellationEval:
        info.tess_clockwise = key.state.tessellation_clockwise != 0;
        info.tess_primitive = [&key] {
            const u32 raw{key.state.tessellation_primitive.Value()};
            switch (static_cast<Maxwell::Tessellation::DomainType>(raw)) {
            case Maxwell::Tessellation::DomainType::Isolines:
                return Shader::TessPrimitive::Isolines;
            case Maxwell::Tessellation::DomainType::Triangles:
                return Shader::TessPrimitive::Triangles;
            case Maxwell::Tessellation::DomainType::Quads:
                return Shader::TessPrimitive::Quads;
            }
            ASSERT(false);
            return Shader::TessPrimitive::Triangles;
        }();
        info.tess_spacing = [&] {
            const u32 raw{key.state.tessellation_spacing};
            switch (static_cast<Maxwell::Tessellation::Spacing>(raw)) {
            case Maxwell::Tessellation::Spacing::Integer:
                return Shader::TessSpacing::Equal;
            case Maxwell::Tessellation::Spacing::FractionalOdd:
                return Shader::TessSpacing::FractionalOdd;
            case Maxwell::Tessellation::Spacing::FractionalEven:
                return Shader::TessSpacing::FractionalEven;
            }
            ASSERT(false);
            return Shader::TessSpacing::Equal;
        }();
        break;
    case Shader::Stage::Mesh:
        info.convert_depth_mode = gl_ndc;
        if (program.mesh.topology == 0) {
            info.fixed_state_point_size = point_size;
        }
        break;
    case Shader::Stage::Geometry:
        if (program.output_topology == Shader::OutputTopology::PointList) {
            info.fixed_state_point_size = point_size;
        }
        if (key.state.xfb_enabled != 0) {
            if (device.IsExtTransformFeedbackSupported()) {
                auto [varyings, count] =
                    VideoCommon::MakeTransformFeedbackVaryings(key.state.xfb_state);
                info.xfb_varyings = varyings;
                info.xfb_count = count;
            } else {
                LOG_WARNING(Render_Vulkan, "XFB requested in pipeline key but device lacks "
                                           "VK_EXT_transform_feedback; ignoring XFB decorations");
            }
        }
        info.convert_depth_mode = gl_ndc;
        break;
    case Shader::Stage::Fragment: {
        info.alpha_test_func = MaxwellToCompareFunction(
            key.state.UnpackComparisonOp(key.state.alpha_test_func.Value()));
        info.alpha_test_reference = std::bit_cast<float>(key.state.alpha_test_ref);

        info.dual_source_blend = key.state.attachment0_dual_source_blend != 0;

        if (device.IsMoltenVK()) {
            for (size_t i = 0; i < 8; ++i) {
                const auto format =
                    static_cast<Tegra::RenderTargetFormat>(key.state.color_formats[i]);
                if (format == Tegra::RenderTargetFormat::NONE) {
                    info.color_output_types[i] = Shader::AttributeType::Float;
                    continue;
                }
                const auto pixel_format =
                    VideoCore::Surface::PixelFormatFromRenderTargetFormat(format);
                if (VideoCore::Surface::IsPixelFormatInteger(pixel_format)) {
                    if (VideoCore::Surface::IsPixelFormatSignedInteger(pixel_format)) {
                        info.color_output_types[i] = Shader::AttributeType::SignedInt;
                    } else {
                        info.color_output_types[i] = Shader::AttributeType::UnsignedInt;
                    }
                } else {
                    info.color_output_types[i] = Shader::AttributeType::Float;
                }
            }
        }
        break;
    }
    default:
        break;
    }
    switch (key.state.topology) {
    case Maxwell::PrimitiveTopology::Points:
        info.input_topology = Shader::InputTopology::Points;
        break;
    case Maxwell::PrimitiveTopology::Lines:
    case Maxwell::PrimitiveTopology::LineLoop:
    case Maxwell::PrimitiveTopology::LineStrip:
        info.input_topology = Shader::InputTopology::Lines;
        break;
    case Maxwell::PrimitiveTopology::Triangles:
    case Maxwell::PrimitiveTopology::TriangleStrip:
    case Maxwell::PrimitiveTopology::TriangleFan:
    case Maxwell::PrimitiveTopology::Quads:
    case Maxwell::PrimitiveTopology::QuadStrip:
    case Maxwell::PrimitiveTopology::Polygon:
    case Maxwell::PrimitiveTopology::Patches:
        info.input_topology = Shader::InputTopology::Triangles;
        break;
    case Maxwell::PrimitiveTopology::LinesAdjacency:
    case Maxwell::PrimitiveTopology::LineStripAdjacency:
        info.input_topology = Shader::InputTopology::LinesAdjacency;
        break;
    case Maxwell::PrimitiveTopology::TrianglesAdjacency:
    case Maxwell::PrimitiveTopology::TriangleStripAdjacency:
        info.input_topology = Shader::InputTopology::TrianglesAdjacency;
        break;
    }
    info.force_early_z = key.state.early_z != 0;
    info.y_negate = key.state.y_negate != 0;
    return info;
}

size_t GetTotalPipelineWorkers() {
    const size_t max_core_threads =
        std::max<size_t>(static_cast<size_t>(std::thread::hardware_concurrency()), 2ULL) - 1ULL;
    static const size_t requested = [] {
        const char* const value = std::getenv("EDEN_NX2_PIPELINE_THREADS");
        if (!value || value[0] < '0' || value[0] > '9') {
            return size_t{0};
        }
        char* end{};
        const auto threads = std::strtoull(value, &end, 10);
        return *end == '\0' && threads <= 1024 ? static_cast<size_t>(threads) : size_t{0};
    }();
    if (requested != 0) {
        const size_t count = (std::min)(max_core_threads, requested);
        static const bool logged = [count] {
            LOG_INFO(Render_Vulkan, "Shader memory budget: pipeline worker cap {} threads", count);
            return true;
        }();
        (void)logged;
        return count;
    }
#ifdef __ANDROID__
    const int configured = AndroidSettings::values.pipeline_worker_count.GetValue();
    const int clamped = std::clamp(configured, 2, 8);
    const size_t desired = static_cast<size_t>(clamped);
    if (desired == 0) {
        return 1ULL;
    }
    return std::min(max_core_threads, desired);
#else
    return max_core_threads;
#endif
}

// Threads each of the two pipeline pools (driver compile, translation) keeps once the disk cache
// is loaded. The pools share the physical cores less six (guest cores, GPU thread, audio and
// present threads), at least two per pool.
size_t PostBootPoolThreads() {
    const size_t cores = Common::PhysicalCoreCount();
    const size_t budget = cores > 6 ? cores - 6 : 0;
    return std::max<size_t>((budget + 1) / 2, 2);
}

// With asynchronous shaders, new native graphics pipelines are translated on worker threads
// (PipelineCache::BuildNewGraphicsPipeline). EDEN_NX2_ASYNC_TRANSLATION=0 translates them on the
// GPU thread, as before.
bool AsyncTranslationAllowed() {
    const char* const value = std::getenv("EDEN_NX2_ASYNC_TRANSLATION");
    return value == nullptr || value[0] != '0';
}

// EDEN_NX2_PARALLEL_STAGES=0: a queued pipeline's stages are translated one after another.
bool ParallelStagesEnabled() {
    static const bool enabled = [] {
        const char* const value = std::getenv("EDEN_NX2_PARALLEL_STAGES");
        return value == nullptr || value[0] != '0';
    }();
    return enabled;
}

// How long the GPU thread waits for a pipeline it queued before skipping the draw:
// EDEN_NX2_ASYNC_TRANSLATION_WAIT_MS per pipeline (default 2, 0 never waits) and
// EDEN_NX2_ASYNC_TRANSLATION_FRAME_MS in total per frame (default 4).
std::chrono::microseconds MillisecondsFromEnv(const char* name, double default_ms) {
    const char* const value = std::getenv(name);
    const double ms = value != nullptr ? std::strtod(value, nullptr) : default_ms;
    return std::chrono::microseconds{ms > 0.0 ? static_cast<s64>(ms * 1000.0) : 0};
}

} // Anonymous namespace

size_t ComputePipelineCacheKey::Hash() const noexcept {
    const u64 hash = Common::CityHash64(reinterpret_cast<const char*>(this), sizeof *this);
    return static_cast<size_t>(hash);
}

bool ComputePipelineCacheKey::operator==(const ComputePipelineCacheKey& rhs) const noexcept {
    return std::memcmp(&rhs, this, sizeof *this) == 0;
}

size_t GraphicsPipelineCacheKey::Hash() const noexcept {
    const u64 hash = Common::CityHash64(reinterpret_cast<const char*>(this), Size());
    return static_cast<size_t>(hash);
}

bool GraphicsPipelineCacheKey::operator==(const GraphicsPipelineCacheKey& rhs) const noexcept {
    if (unique_hashes != rhs.unique_hashes) {
        return false;
    }
    return std::memcmp(reinterpret_cast<const char*>(this) + sizeof(unique_hashes),
                       reinterpret_cast<const char*>(&rhs) + sizeof(unique_hashes),
                       state.Size()) == 0;
}

/// What the loaded title's cache holds: the files (read for preparation, rewritten as a new
/// generation of the recorded pipelines), the preparation scheduler and the counters of the
/// report. Built by LoadDiskResources, ended by FinishCacheSession.
class PipelineCacheSession {
public:
    struct File {
        CE::PayloadKind kind{CE::PayloadKind::Transferable};
        std::filesystem::path path;
        u64 max_bytes{};
        CE::CacheLoadResult load;
        std::unique_ptr<CE::EnvelopeReader> reader;
        std::unique_ptr<CE::EnvelopeWriter> writer; ///< Serialization thread only
        bool writer_failed{};
    };
    /// Where candidate `i` of the scheduler lives: file and record index.
    struct Source {
        u32 file;
        u32 record;
    };

    PipelineCacheSession(const CE::CacheIdentity& identity_, const CE::PrepareOptions& options)
        : identity{identity_}, scheduler{options, &Spikes::NewWorkId} {}

    CE::CacheIdentity identity;
    std::vector<File> files;
    std::vector<CE::CacheFileReport> extra_reports; ///< Driver blob and warm file
    PP::Scheduler scheduler;
    std::vector<Source> sources;
    std::unordered_set<u64> used_hashes; ///< Graphics keys recent sessions used (may be empty)
    std::unique_ptr<PipelineStatistics> statistics;
    u64 generation{};
    std::atomic<u64> staged{};  ///< Records queued for the serialization thread
    std::atomic<u64> recorded{};
    std::atomic<u64> skipped{};
    std::atomic<u64> demand_wait_ns{};
    CE::WriteStatus write_status{CE::WriteStatus::NotAttempted};
};

namespace {
static_assert(std::endian::native == std::endian::little, "Cache keys are stored as in memory");

/// What a worker keeps between Loaded and the end of the build.
struct PreparedRecord {
    bool compute{};
    GraphicsPipelineCacheKey graphics_key{};
    ComputePipelineCacheKey compute_key{};
    std::vector<FileEnvironment> envs;
};

/// Results of a build, adopted by the GPU thread.
struct PreparedGraphics {
    PreparedGraphics(const GraphicsPipelineCacheKey& key_, std::unique_ptr<GraphicsPipeline> built)
        : key{key_}, pipeline{std::move(built)} {}
    GraphicsPipelineCacheKey key;
    std::unique_ptr<GraphicsPipeline> pipeline;
};

struct PreparedCompute {
    explicit PreparedCompute(ComputePipelineCacheEntry&& built) : entry{std::move(built)} {}
    ComputePipelineCacheEntry entry;
};

/// The key of a record: the hashes, then the size of the state in use and that many bytes of it
/// (the tail of FixedPipelineState past Size() is not part of the key).
std::vector<u8> EncodeGraphicsKey(const GraphicsPipelineCacheKey& key) {
    const u32 state_size = static_cast<u32>(key.state.Size());
    std::vector<u8> out(sizeof(key.unique_hashes) + sizeof(state_size) + state_size);
    u8* cursor = out.data();
    std::memcpy(cursor, key.unique_hashes.data(), sizeof(key.unique_hashes));
    cursor += sizeof(key.unique_hashes);
    std::memcpy(cursor, &state_size, sizeof(state_size));
    cursor += sizeof(state_size);
    std::memcpy(cursor, &key.state, state_size);
    return out;
}

std::optional<GraphicsPipelineCacheKey> DecodeGraphicsKey(std::span<const u8> bytes) {
    constexpr size_t hashes_size = sizeof(GraphicsPipelineCacheKey::unique_hashes);
    constexpr size_t head = hashes_size + sizeof(u32);
    if (bytes.size() < head) {
        return std::nullopt;
    }
    u32 state_size{};
    std::memcpy(&state_size, bytes.data() + hashes_size, sizeof(state_size));
    if (state_size > sizeof(FixedPipelineState) || bytes.size() != head + state_size) {
        return std::nullopt;
    }
    GraphicsPipelineCacheKey key{};
    std::memcpy(key.unique_hashes.data(), bytes.data(), hashes_size);
    std::memcpy(&key.state, bytes.data() + head, state_size);
    if (key.state.Size() != state_size) {
        return std::nullopt;
    }
    return key;
}

std::vector<u8> EncodeComputeKey(const ComputePipelineCacheKey& key) {
    std::vector<u8> out(sizeof(key));
    std::memcpy(out.data(), &key, sizeof(key));
    return out;
}

std::optional<ComputePipelineCacheKey> DecodeComputeKey(std::span<const u8> bytes) {
    if (bytes.size() != sizeof(ComputePipelineCacheKey)) {
        return std::nullopt;
    }
    ComputePipelineCacheKey key{};
    std::memcpy(&key, bytes.data(), sizeof(key));
    return key;
}

/// The cache report of the session as it stands.
void PublishSession(PipelineCacheSession& session) {
    CE::CacheReport report;
    report.identity = session.identity;
    for (const auto& file : session.files) {
        report.files.push_back(CE::CacheFileReport{.kind = file.kind, .load = file.load});
    }
    report.files.insert(report.files.end(), session.extra_reports.begin(),
                        session.extra_reports.end());
    session.scheduler.FillStats(report.stats);
    report.stats.recorded = session.recorded.load(std::memory_order_relaxed);
    report.stats.records_skipped = session.skipped.load(std::memory_order_relaxed);
    report.stats.demand_wait_ns = session.demand_wait_ns.load(std::memory_order_relaxed);
    report.stats.write_status = session.write_status;
    CE::PublishCacheReport(std::move(report));
}

/// Appends one record to the new generation of its file. Runs on the serialization thread, which
/// owns the writers; a record that cannot be written is counted and compiled normally next time.
void RecordPipeline(PipelineCacheSession& session, const CE::RecordMeta& meta,
                    std::span<const u8> key, std::span<const GenericEnvironment* const> envs,
                    bool mesh = false) {
    const auto skip = [&session] { session.skipped.fetch_add(1, std::memory_order_relaxed); };
    std::vector<u8> payload;
    const CE::PayloadKind kind =
        mesh ? CE::PayloadKind::TransferableMesh : CE::PayloadKind::Transferable;
    PipelineCacheSession::File* target = nullptr;
    for (auto& file : session.files) {
        if (file.kind == kind) {
            target = &file;
        }
    }
    if (target == nullptr || target->writer_failed ||
        !VideoCommon::EncodePipelineRecord(key, envs, mesh, payload)) {
        skip();
    } else {
        if (!target->writer) {
            CE::CacheIdentity identity = session.identity;
            identity.kind = kind;
            target->writer = CE::EnvelopeWriter::Create(target->path, identity, target->max_bytes);
            if (target->writer) {
                // The records of the previous generation stay: this run adds to them.
                if (target->reader) {
                    static_cast<void>(target->writer->Seed(*target->reader));
                }
            } else {
                target->writer_failed = true;
                LOG_WARNING(Render_Vulkan, "Pipeline cache {} cannot be written this session",
                            CE::PayloadKindName(kind));
            }
        }
        if (!target->writer) {
            skip();
        } else {
            switch (target->writer->Append(meta, payload)) {
            case CE::AppendResult::Ok:
                session.recorded.fetch_add(1, std::memory_order_relaxed);
                break;
            case CE::AppendResult::Duplicate:
                break;
            case CE::AppendResult::TooLarge:
            case CE::AppendResult::BudgetFull:
            case CE::AppendResult::IoError:
                skip();
                break;
            }
        }
    }
    session.staged.fetch_sub(1, std::memory_order_relaxed);
}

/// Whether this device can use a recorded graphics pipeline as it was keyed (the dynamic state
/// features are part of the key).
bool GraphicsRecordUsable(const GraphicsPipelineCacheKey& key,
                          const std::vector<FileEnvironment>& envs, bool mesh_file,
                          const DynamicFeatures& features) {
    // Task and mesh pipelines live in their own file, with their mesh registers.
    if (!mesh_file && std::ranges::any_of(envs, [](const FileEnvironment& env) {
            return env.ShaderStage() == Shader::Stage::Task ||
                   env.ShaderStage() == Shader::Stage::Mesh;
        })) {
        return false;
    }
    const FixedPipelineState& state = key.state;
    if ((state.extended_dynamic_state != 0) != features.has_extended_dynamic_state ||
        (state.extended_dynamic_state_2 != 0) != features.has_extended_dynamic_state_2 ||
        (state.extended_dynamic_state_2_logic_op != 0) !=
            features.has_extended_dynamic_state_2_logic_op ||
        (state.extended_dynamic_state_3_blend != 0) !=
            features.has_extended_dynamic_state_3_blend ||
        (state.extended_dynamic_state_3_enables != 0) !=
            features.has_extended_dynamic_state_3_enables ||
        (state.color_write_enable_dynamic != 0) != features.has_color_write_enable ||
        (state.dynamic_vertex_input != 0) != features.has_dynamic_vertex_input) {
        return false;
    }
    const bool provoking_last = state.provoking_vertex_last != 0;
    if (provoking_last && !features.has_provoking_vertex_last_mode) {
        return false;
    }
    return !(state.xfb_enabled != 0 && provoking_last &&
             !features.has_provoking_vertex_tf_preserve);
}

/// Reads and decodes record `task.id`, then tells the scheduler what it is.
void LoadPreparedRecord(PipelineCacheSession& session, const DynamicFeatures& features,
                        const PP::Scheduler::Task& task) {
    PP::Scheduler& prepare = session.scheduler;
    const PipelineCacheSession::Source source = session.sources[task.id];
    PipelineCacheSession::File& file = session.files[source.file];
    const bool mesh_file = file.kind == CE::PayloadKind::TransferableMesh;
    const CE::IndexEntry entry = file.reader->Index()[source.record];
    std::vector<u8> bytes;
    CE::RejectReason reason;
    {
        Spikes::SpanScope disk{Spikes::Cause::Disk, task.work, entry.length};
        reason = file.reader->ReadRecord(source.record, bytes);
        if (reason != CE::RejectReason::None) {
            disk.Fail();
        }
    }
    VideoCommon::PipelineRecord record;
    if (reason != CE::RejectReason::None ||
        VideoCommon::DecodePipelineRecord(bytes, mesh_file, record) != CE::RejectReason::None ||
        record.is_compute != (entry.meta.kind == CE::RecordKind::Compute)) {
        prepare.LoadFailed(task.id, task.generation);
        return;
    }
    auto payload = std::make_shared<PreparedRecord>();
    u64 hash{};
    bool variants{};
    if (record.is_compute) {
        const auto key = DecodeComputeKey(record.key);
        if (!key || record.envs.size() != 1) {
            prepare.LoadFailed(task.id, task.generation);
            return;
        }
        payload->compute = true;
        payload->compute_key = *key;
        hash = key->Hash();
        variants = true; // Translations of one program: each is built, the caller picks
    } else {
        const auto key = DecodeGraphicsKey(record.key);
        if (!key) {
            prepare.LoadFailed(task.id, task.generation);
            return;
        }
        hash = key->Hash();
        if (!GraphicsRecordUsable(*key, record.envs, mesh_file, features) ||
            (!session.used_hashes.empty() && !session.used_hashes.contains(hash))) {
            prepare.LoadSkipped(task.id, task.generation);
            return;
        }
        payload->graphics_key = *key;
    }
    payload->envs = std::move(record.envs);
    static_cast<void>(
        prepare.Loaded(task.id, task.generation, hash, record.key, std::move(payload), variants));
}

/// Asks the scheduler for a pipeline a draw needs now. Waits (as a Waiting span linked to the
/// build) when its record is being built; every other answer comes back at once.
PP::Scheduler::Demand RequestPrepared(PipelineCacheSession& session, u64 hash,
                                      std::span<const u8> key) {
    for (;;) {
        PP::Scheduler::Demand demand = session.scheduler.Request(hash, key);
        if (demand.kind != PP::Scheduler::DemandKind::Building) {
            return demand;
        }
        const u64 begin = Spikes::NowNs();
        {
            Nx2Bottleneck::WaitScope build_wait{Nx2Bottleneck::Wait::ShaderBuild};
            const Spikes::SpanScope wait{Spikes::Cause::Waiting};
            Spikes::LinkWait(wait.Token(), demand.work, Spikes::DependencyKind::Job);
            session.scheduler.Await(demand.id, demand.generation);
        }
        session.demand_wait_ns.fetch_add(Spikes::NowNs() - begin, std::memory_order_relaxed);
    }
}
} // Anonymous namespace

namespace {
// NVIDIA's and AMD's drivers keep a disk cache of built pipelines of their own. Eden's copy on top
// only kept every pipeline the title ever built in host memory (title 1: a 245 MB file, ~0.35 GB
// of RAM for the session) and copied it whole at each save, which stalled frames; pipelines built
// no slower without it.
bool DriverCachesPipelines(const Device& device) {
    switch (device.GetDriverID()) {
    case VK_DRIVER_ID_NVIDIA_PROPRIETARY:
    case VK_DRIVER_ID_AMD_PROPRIETARY:
        return true;
    default:
        return false;
    }
}
} // Anonymous namespace

PipelineCache::PipelineCache(Tegra::MaxwellDeviceMemoryManager& device_memory_,
                             const Device& device_, Scheduler& scheduler_,
                             DescriptorPool& descriptor_pool_,
                             GuestDescriptorQueue& guest_descriptor_queue_,
                             DescriptorBufferRing& descriptor_buffer_ring_,
                             RenderPassCache& render_pass_cache_, BufferCache& buffer_cache_,
                             TextureCache& texture_cache_, VideoCore::ShaderNotify& shader_notify_)
    : VideoCommon::ShaderCache{device_memory_}, device{device_}, scheduler{scheduler_},
      descriptor_pool{descriptor_pool_}, guest_descriptor_queue{guest_descriptor_queue_},
      descriptor_buffer_ring{descriptor_buffer_ring_}, render_pass_cache{render_pass_cache_},
      buffer_cache{buffer_cache_}, texture_cache{texture_cache_}, shader_notify{shader_notify_},
      use_asynchronous_shaders{Settings::values.use_asynchronous_shaders.GetValue()},
      use_vulkan_pipeline_cache{Settings::values.use_vulkan_driver_pipeline_cache.GetValue() &&
                                !DriverCachesPipelines(device_)},
      workers(device.HasBrokenParallelShaderCompiling() ? 1ULL : GetTotalPipelineWorkers(),
              "VkPipelineBuilder", {}, Common::ThreadPlacement::Background),
      disk_replay_workers(device.HasBrokenParallelShaderCompiling() ? 1ULL : 2ULL,
                          "VkDiskPipelineReplay", {}, Common::ThreadPlacement::Background),
      serialization_thread(1, "VkPipelineSerialization", {}, Common::ThreadPlacement::Background) {
    build_budget = std::chrono::milliseconds{[] {
        const char* const value = std::getenv("EDEN_NX2_SHADER_BUDGET_MS");
        return value != nullptr ? std::max(1, std::atoi(value)) : 24;
    }()};
    if (use_asynchronous_shaders && AsyncTranslationAllowed() &&
        !device.HasBrokenParallelShaderCompiling()) {
        // EDEN_NX2_TRANSLATION_THREADS overrides the count (1 to 32).
        if (const char* const threads = std::getenv("EDEN_NX2_TRANSLATION_THREADS")) {
            num_translation_threads =
                std::clamp<size_t>(static_cast<size_t>(std::strtoull(threads, nullptr, 10)), 1, 32);
        } else {
            // As many as the driver compile workers. Both run below normal priority, so a burst
            // of new pipelines takes idle cores without slowing the emulated CPU and GPU threads.
            num_translation_threads = GetTotalPipelineWorkers();
        }
        translation_wait = MillisecondsFromEnv("EDEN_NX2_ASYNC_TRANSLATION_WAIT_MS", 2.0);
        translation_frame_budget = MillisecondsFromEnv("EDEN_NX2_ASYNC_TRANSLATION_FRAME_MS", 4.0);
        translation_frame_left = translation_frame_budget;
        translation_threads_boot = num_translation_threads.load(std::memory_order_relaxed);
        translation_workers.reset(new Common::ThreadWorker(num_translation_threads,
                                                           "VkPipelineTranslator", {},
                                                           Common::ThreadPlacement::Background));
        LOG_INFO(Render_Vulkan,
                 "Native graphics pipelines translate on {} threads (wait {:.1f} ms per pipeline, "
                 "{:.1f} ms per frame)",
                 num_translation_threads.load(std::memory_order_relaxed),
                 translation_wait.count() / 1000.0,
                 translation_frame_budget.count() / 1000.0);
    }
    DriverBuildReport::BeginSession(
        (device.HasBrokenParallelShaderCompiling() ? 1ULL : GetTotalPipelineWorkers()) +
        num_translation_threads.load(std::memory_order_relaxed));
    const auto& float_control{device.FloatControlProperties()};
    const VkDriverId driver_id{device.GetDriverID()};
    const VkShaderStageFlags subgroup_stages{device.GetSubgroupSupportedStages()};
    const auto subgroup_stage_bit{[subgroup_stages](VkShaderStageFlags flag, Shader::Stage stage) {
        return (subgroup_stages & flag) != 0 ? (1u << static_cast<u32>(stage)) : 0u;
    }};
    u32 supported_subgroup_stages{
        subgroup_stage_bit(VK_SHADER_STAGE_VERTEX_BIT, Shader::Stage::VertexA) |
        subgroup_stage_bit(VK_SHADER_STAGE_VERTEX_BIT, Shader::Stage::VertexB) |
        subgroup_stage_bit(VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
                           Shader::Stage::TessellationControl) |
        subgroup_stage_bit(VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT,
                           Shader::Stage::TessellationEval) |
        subgroup_stage_bit(VK_SHADER_STAGE_GEOMETRY_BIT, Shader::Stage::Geometry) |
        subgroup_stage_bit(VK_SHADER_STAGE_FRAGMENT_BIT, Shader::Stage::Fragment) |
        subgroup_stage_bit(VK_SHADER_STAGE_COMPUTE_BIT, Shader::Stage::Compute) |
        subgroup_stage_bit(VK_SHADER_STAGE_TASK_BIT_EXT, Shader::Stage::Task) |
        subgroup_stage_bit(VK_SHADER_STAGE_MESH_BIT_EXT, Shader::Stage::Mesh)};
    if (driver_id == VK_DRIVER_ID_MESA_TURNIP) {
        supported_subgroup_stages &= ~(1u << static_cast<u32>(Shader::Stage::Geometry));
    }
    profile = Shader::Profile{
        .supported_spirv = device.SupportedSpirvVersion(),
        .unified_descriptor_binding = true,
        .support_descriptor_aliasing = device.IsDescriptorAliasingSupported(),
        .support_int8 = device.IsInt8Supported(),
        .support_uniform_and_storage_buffer_8bit =
            device.IsUniformAndStorageBuffer8BitAccessSupported(),
        .support_storage_buffer_8bit = device.IsStorageBuffer8BitAccessSupported(),
        .support_int16 = device.IsShaderInt16Supported(),
        .support_uniform_and_storage_buffer_16bit =
            device.IsUniformAndStorageBuffer16BitAccessSupported(),
        .support_storage_buffer_16bit = device.IsStorageBuffer16BitAccessSupported(),
        .support_int64 = device.IsShaderInt64Supported(),
        .support_vertex_instance_id = false,
        .support_float_controls = device.IsKhrShaderFloatControlsSupported(),
        .support_separate_denorm_behavior =
            float_control.denormBehaviorIndependence == VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_ALL,
        .support_separate_rounding_mode =
            float_control.roundingModeIndependence == VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_ALL,
        .support_fp16_denorm_preserve = float_control.shaderDenormPreserveFloat16 != VK_FALSE,
        .support_fp32_denorm_preserve = float_control.shaderDenormPreserveFloat32 != VK_FALSE,
        .support_fp16_denorm_flush = float_control.shaderDenormFlushToZeroFloat16 != VK_FALSE,
        .support_fp32_denorm_flush = float_control.shaderDenormFlushToZeroFloat32 != VK_FALSE,
        .support_fp16_signed_zero_nan_preserve =
            float_control.shaderSignedZeroInfNanPreserveFloat16 != VK_FALSE,
        .support_fp32_signed_zero_nan_preserve =
            float_control.shaderSignedZeroInfNanPreserveFloat32 != VK_FALSE,
        .support_fp64_signed_zero_nan_preserve =
            float_control.shaderSignedZeroInfNanPreserveFloat64 != VK_FALSE,
        .support_explicit_workgroup_layout = device.IsKhrWorkgroupMemoryExplicitLayoutSupported(),
        .support_workgroup_layout_8bit_access =
            device.IsWorkgroupMemoryExplicitLayout8BitAccessSupported(),
        .support_workgroup_layout_16bit_access =
            device.IsWorkgroupMemoryExplicitLayout16BitAccessSupported(),
        .support_shader_quad_control = device.IsKhrShaderQuadControlSupported(),
        .support_quad_shuffles = device.IsSubgroupFeatureSupported(VK_SUBGROUP_FEATURE_QUAD_BIT),
        .support_vote = device.IsSubgroupFeatureSupported(VK_SUBGROUP_FEATURE_VOTE_BIT),
        .support_shuffle_relative =
            device.IsSubgroupFeatureSupported(VK_SUBGROUP_FEATURE_SHUFFLE_RELATIVE_BIT),
        .supported_subgroup_stages = supported_subgroup_stages,
        .support_viewport_index_layer_non_geometry =
            device.IsExtShaderViewportIndexLayerSupported(),
        .support_viewport_mask = device.IsNvViewportArray2Supported(),
        .support_typeless_image_loads = device.IsFormatlessImageLoadSupported(),
        .support_demote_to_helper_invocation =
            device.IsExtShaderDemoteToHelperInvocationSupported(),
        .support_int64_atomics = device.IsExtShaderAtomicInt64Supported(),
        .support_shared_int64_atomics = device.IsSharedInt64AtomicsSupported(),
        .support_derivative_control = true,
        .support_geometry_shader_passthrough = device.IsNvGeometryShaderPassthroughSupported(),
        .support_native_ndc = device.IsExtDepthClipControlSupported(),
        .support_scaled_attributes = !device.MustEmulateScaledFormats(),
        .support_multi_viewport = device.SupportsMultiViewport(),
        .support_geometry_streams = device.AreTransformFeedbackGeometryStreamsSupported(),
        .support_sampled_image_array_nonuniform_indexing =
            device.IsSampledImageArrayNonUniformIndexingSupported(),
        .support_storage_image_array_nonuniform_indexing =
            device.IsStorageImageArrayNonUniformIndexingSupported(),
        .support_uniform_texel_buffer_array_nonuniform_indexing =
            device.IsUniformTexelBufferArrayNonUniformIndexingSupported(),
        .support_storage_texel_buffer_array_nonuniform_indexing =
            device.IsStorageTexelBufferArrayNonUniformIndexingSupported(),

        .warp_size_potentially_larger_than_guest = device.IsWarpSizePotentiallyBiggerThanGuest(),

        .lower_left_origin_mode = false,
        .need_declared_frag_colors = false,
        .need_gather_subpixel_offset =
            driver_id == VK_DRIVER_ID_AMD_PROPRIETARY ||
            driver_id == VK_DRIVER_ID_AMD_OPEN_SOURCE || driver_id == VK_DRIVER_ID_MESA_RADV ||
            driver_id == VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS ||
            driver_id == VK_DRIVER_ID_INTEL_OPEN_SOURCE_MESA ||
            driver_id == VK_DRIVER_ID_QUALCOMM_PROPRIETARY || driver_id == VK_DRIVER_ID_MESA_TURNIP,

        .has_broken_spirv_clamp = driver_id == VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS,
        .has_broken_spirv_position_input = driver_id == VK_DRIVER_ID_QUALCOMM_PROPRIETARY,
        .has_broken_unsigned_image_offsets = false,
        .has_broken_signed_operations = false,
        .has_broken_fp16_float_controls = driver_id == VK_DRIVER_ID_NVIDIA_PROPRIETARY,
        .has_broken_fp32_denorm_flush = driver_id == VK_DRIVER_ID_QUALCOMM_PROPRIETARY,
        .has_broken_mesh_global_memory_calls =
            device.IsNvidia() && device.GetNvidiaArch() <= NvidiaArchitecture::Arch_Pascal,
        .ignore_nan_fp_comparisons = false,
        .has_broken_spirv_subgroup_mask_vector_extract_dynamic =
            driver_id == VK_DRIVER_ID_QUALCOMM_PROPRIETARY &&
            device.GetDriverVersion() < VK_MAKE_VERSION(512, 672, 0),
        .has_broken_spirv_subgroup_shuffle = driver_id == VK_DRIVER_ID_QUALCOMM_PROPRIETARY,
        .has_broken_spirv_fragment_shuffle = driver_id == VK_DRIVER_ID_AMD_PROPRIETARY &&
                                             Common::EnvGate("EGLANTINE_AMD_FRAGMENT_SHUFFLE"),
        .max_subgroup_size = device.GetMaxSubgroupSize(),
        .has_broken_robust =
            device.IsNvidia() && device.GetNvidiaArch() <= NvidiaArchitecture::Arch_Pascal,
        .min_ssbo_alignment = device.GetStorageBufferAlignment(),
        .max_user_clip_distances = device.GetMaxUserClipDistances()};
    profile.support_cooperative_hmma =
        device.IsCooperativeHmmaSupported() && TestCooperativeHmma(device);

    host_info = Shader::HostTranslateInfo{
        .min_ssbo_alignment = device.GetStorageBufferAlignment(),
        .max_per_stage_descriptor_sampled_images = device.GetMaxPerStageDescriptorSampledImages(),
        .max_per_stage_resources = device.GetMaxPerStageResources(),
        .max_per_stage_descriptor_storage_buffers = device.GetMaxPerStageDescriptorStorageBuffers(),
        .max_descriptor_set_samplers = device.GetMaxDescriptorSetSamplers(),
        .max_descriptor_set_uniform_buffers = device.GetMaxDescriptorSetUniformBuffers(),
        .max_descriptor_set_uniform_buffers_dynamic =
            device.GetMaxDescriptorSetUniformBuffersDynamic(),
        .max_descriptor_set_storage_buffers = device.GetMaxDescriptorSetStorageBuffers(),
        .max_descriptor_set_storage_buffers_dynamic =
            device.GetMaxDescriptorSetStorageBuffersDynamic(),
        .max_descriptor_set_sampled_images = device.GetMaxDescriptorSetSampledImages(),
        .max_descriptor_set_storage_images = device.GetMaxDescriptorSetStorageImages(),
        .max_descriptor_set_input_attachements = device.GetMaxDescriptorSetInputAttachments(),
        .support_float64 = device.IsFloat64Supported(),
        .support_float16 = device.IsFloat16Supported(),
        .support_int64 = device.IsShaderInt64Supported(),
        .needs_demote_reorder = driver_id == VK_DRIVER_ID_AMD_PROPRIETARY ||
                                driver_id == VK_DRIVER_ID_AMD_OPEN_SOURCE ||
                                driver_id == VK_DRIVER_ID_SAMSUNG_PROPRIETARY,
        .support_snorm_render_buffer = true,
        .support_viewport_index_layer = device.IsExtShaderViewportIndexLayerSupported(),
        .support_viewport_mask = device.IsNvViewportArray2Supported(),
        .support_geometry_shader_passthrough = device.IsNvGeometryShaderPassthroughSupported(),
        .support_conditional_barrier = device.SupportsConditionalBarriers(),
        .single_lane_geometry_subgroups = !profile.SupportsSubgroupStage(Shader::Stage::Geometry),
    };
    host_info.ApplyDescriptorLimitPolicy();

    if (device.GetMaxVertexInputAttributes() < Maxwell::NumVertexAttributes) {
        LOG_WARNING(Render_Vulkan, "maxVertexInputAttributes is too low: {} < {}",
                    device.GetMaxVertexInputAttributes(), Maxwell::NumVertexAttributes);
    }
    if (device.GetMaxVertexInputBindings() < Maxwell::NumVertexArrays) {
        LOG_WARNING(Render_Vulkan, "maxVertexInputBindings is too low: {} < {}",
                    device.GetMaxVertexInputBindings(), Maxwell::NumVertexArrays);
    }

    LOG_INFO(Render_Vulkan, "DynamicState setting value: {}",
             u32(Settings::values.dyna_state.GetValue()));

    dynamic_features = {};
    dynamic_features.driver_id = device.GetDriverID();
    dynamic_features.driver_version = device.GetDriverVersion();

    // User granularity enforced in vulkan_device.cpp switch statement:
    //   Level 0: Core Dynamic States only
    //   Level 1: Core + EDS1
    //   Level 2: Core + EDS1 + EDS2 (accumulative)
    //   Level 3: Core + EDS1 + EDS2 + EDS3 (accumulative)
    // Here we only verify if extensions were successfully loaded by the device

    dynamic_features.has_extended_dynamic_state = device.IsExtExtendedDynamicStateSupported();

    dynamic_features.has_extended_dynamic_state_2 = device.IsExtExtendedDynamicState2Supported();
    dynamic_features.has_extended_dynamic_state_2_logic_op =
        device.IsExtExtendedDynamicState2ExtrasSupported();
    dynamic_features.has_extended_dynamic_state_2_patch_control_points = false;

    dynamic_features.has_extended_dynamic_state_3_blend =
        device.IsExtExtendedDynamicState3BlendingSupported();
    dynamic_features.has_extended_dynamic_state_3_enables =
        device.IsExtExtendedDynamicState3EnablesSupported();
    dynamic_features.has_color_write_enable = device.IsExtColorWriteEnableSupported();
    dynamic_features.has_dynamic_state3_depth_clamp_enable =
        dynamic_features.has_extended_dynamic_state_3_enables &&
        device.SupportsDynamicState3DepthClampEnable();
    dynamic_features.has_dynamic_state3_logic_op_enable =
        dynamic_features.has_extended_dynamic_state_3_enables &&
        device.SupportsDynamicState3LogicOpEnable();
    dynamic_features.has_dynamic_state3_line_stipple_enable =
        dynamic_features.has_extended_dynamic_state_3_enables &&
        device.SupportsDynamicState3LineStippleEnable();

    // VIDS: Independent toggle (not affected by dyna_state levels)
    dynamic_features.has_dynamic_vertex_input =
        device.IsExtVertexInputDynamicStateSupported() &&
        Settings::values.vertex_input_dynamic_state.GetValue();
    dynamic_features.has_static_vertex_repack =
        StaticVertexRepackEnabled(device.GetMinVertexInputBindingStrideAlignment());

    dynamic_features.has_provoking_vertex = device.IsExtProvokingVertexSupported();
    dynamic_features.has_provoking_vertex_first_mode = device.SupportsProvokingVertexFirstMode();
    dynamic_features.has_provoking_vertex_last_mode = device.SupportsProvokingVertexLastMode();
    dynamic_features.has_provoking_vertex_tf_preserve =
        device.SupportsTransformFeedbackProvokingVertexPreservation();
}

namespace {
// <title>/vulkan_used.bin: the graphics pipelines recent sessions used, as key hash + sessions
// since the last use.
constexpr u32 UsedPipelinesMagic = 0x31505545; // "EUP1"
// Pipelines the last two sessions used are built at boot. Each holds ~0.45 MB of driver memory:
// title 1 built 4,800 from eight sessions, 1.5 GB more than building them on first use (but that
// stalled frames 4x as often), while one session of its title screen uses ~1,500.
// MoltenVK keeps the previous eight sessions: a pipeline built while playing goes through
// SPIR-V -> MSL -> Metal, far more than the 2-3 ms the shorter window assumes, so each one
// built at boot is a stutter avoided.
// Ocarina still built pipelines for 1-2 s of every 10 s early in play with two sessions kept:
// keep sixteen (#161).
constexpr u8 UsedPipelinesMaxAge = 2;
constexpr u8 UsedPipelinesMaxAgePerf = 16;
constexpr u8 UsedPipelinesMaxAgeMoltenVK = 8;

u8 MaxUsedPipelineAge(const Device& device) {
    if (device.IsMoltenVK()) {
        return UsedPipelinesMaxAgeMoltenVK;
    }
    return VideoCommon::Nx2Perf::On(VideoCommon::Nx2Perf::Opt::PipelineRetention)
               ? UsedPipelinesMaxAgePerf
               : UsedPipelinesMaxAge;
}

std::unordered_map<u64, u8> LoadUsedPipelines(const std::filesystem::path& path, u8 max_age) {
    std::unordered_map<u64, u8> used;
    std::ifstream file{path, std::ios::binary};
    u32 magic{}, count{};
    if (!file.read(reinterpret_cast<char*>(&magic), sizeof(magic)) || magic != UsedPipelinesMagic ||
        !file.read(reinterpret_cast<char*>(&count), sizeof(count))) {
        return used;
    }
    used.reserve(count);
    for (u32 i = 0; i < count; ++i) {
        u64 hash{};
        u8 age{};
        if (!file.read(reinterpret_cast<char*>(&hash), sizeof(hash)) ||
            !file.read(reinterpret_cast<char*>(&age), sizeof(age))) {
            break;
        }
        if (age < max_age) { // lists saved with a longer window
            used.emplace(hash, age);
        }
    }
    return used;
}

// EDEN_NX2_BACKGROUND_PIPELINE_THREADS sets how many threads build the shader precache (default a
// quarter of the host's, 1 to 6).
size_t BackgroundPipelineThreads() {
    if (const char* value = std::getenv("EDEN_NX2_BACKGROUND_PIPELINE_THREADS")) {
        return std::clamp<size_t>(static_cast<size_t>(std::strtoull(value, nullptr, 10)), 1, 16);
    }
    return std::clamp<size_t>(std::thread::hardware_concurrency() / 4, 1, 6);
}
} // Anonymous namespace

PipelineCache::~PipelineCache() {
    DriverBuildReport::LogSkipSummary("session");
    StopShaderPrecache();
    if (!precache_keys.empty()) {
        LOG_INFO(Render_Vulkan,
                 "Shader precache: {} of the {} compute pipelines new this session were in the "
                 "precache file",
                 precached_compute_pipelines, new_compute_pipelines);
    }
    // Translations queue driver compiles: end them before anything below touches the workers.
    translation_workers.reset();
    {
        // Kept until graphics_cache goes, after the driver compile workers they may be queued on.
        std::scoped_lock lock{translated_mutex};
        for (auto& translated : translated_graphics) {
            if (!graphics_cache.try_emplace(translated.key, std::move(translated.pipeline)).second) {
                static_cast<void>(translated.pipeline.release());
            }
        }
        translated_graphics.clear();
    }
    SaveUsedPipelines();
    // Ends the preparation workers, commits the recorded files and writes the driver blob.
    FinishCacheSession();
}

void PipelineCache::SaveUsedPipelines() {
    used_pipelines_saved = std::chrono::steady_clock::now();
    // A session too short to say what the game uses (a boot to the menu) keeps the old list.
    constexpr size_t MinimumUsed = 64;
    if (used_pipelines_filename.empty() ||
        used_this_session.size() + used_and_evicted.size() < MinimumUsed) {
        return;
    }
    std::unordered_map<u64, u8> next;
    next.reserve(used_pipelines.size() + used_this_session.size());
    for (const auto& [key, pipeline] : graphics_cache) {
        if (used_this_session.contains(pipeline.get())) {
            next.emplace(key.Hash(), u8{0});
        }
    }
    for (const u64 hash : used_and_evicted) {
        next.emplace(hash, u8{0});
    }
    for (const auto& [hash, age] : used_pipelines) {
        if (age + 1 < MaxUsedPipelineAge(device)) {
            next.try_emplace(hash, static_cast<u8>(age + 1));
        }
    }
    std::ofstream file{used_pipelines_filename, std::ios::binary | std::ios::trunc};
    const u32 count = static_cast<u32>(next.size());
    file.write(reinterpret_cast<const char*>(&UsedPipelinesMagic), sizeof(UsedPipelinesMagic));
    file.write(reinterpret_cast<const char*>(&count), sizeof(count));
    for (const auto& [hash, age] : next) {
        file.write(reinterpret_cast<const char*>(&hash), sizeof(hash));
        file.write(reinterpret_cast<const char*>(&age), sizeof(age));
    }
}

bool PipelineCache::IsIndirectResetShader() const {
    constexpr size_t vertex_b = static_cast<size_t>(Maxwell::ShaderType::VertexB);
    constexpr u64 reset_key = 0x72fd62555e7196cdULL;
    const auto* info = shader_infos[vertex_b];
    return maxwell3d->regs.IsShaderConfigEnabled(vertex_b) && info && info->lowered_native &&
           info->lowered_native->source_size == 128 + 11 * 16 &&
           info->lowered_native->diagnostic_key == reset_key;
}

namespace {
size_t GraphicsFastCacheSlot(const GraphicsPipelineCacheKey& key, size_t size) {
    const auto* const bytes = reinterpret_cast<const u8*>(&key);
    const size_t length = key.Size();
    u64 hash = 0x9e3779b97f4a7c15ULL;
    size_t offset = 0;
    for (; offset + sizeof(u64) <= length; offset += sizeof(u64)) {
        u64 word;
        std::memcpy(&word, bytes + offset, sizeof(word));
        hash = (hash ^ word) * 0xff51afd7ed558ccdULL;
    }
    for (; offset < length; ++offset) {
        hash = (hash ^ bytes[offset]) * 0xc4ceb9fe1a85ec53ULL;
    }
    return static_cast<size_t>(hash ^ (hash >> 29)) & (size - 1);
}
} // Anonymous namespace

GraphicsPipeline* PipelineCache::CurrentGraphicsPipeline(bool is_indexed) {
    graphics_skip = DriverBuildReport::SkipReason::Unknown;
    if (!RefreshStages(graphics_key.unique_hashes)) {
        current_pipeline = nullptr;
        graphics_skip = DriverBuildReport::SkipReason::StageRejected;
        return nullptr;
    }
    graphics_key.state.Refresh(*maxwell3d, dynamic_features);

    if (current_pipeline) {
        GraphicsPipeline* const next{current_pipeline->Next(graphics_key)};
        if (next) {
            current_pipeline = next;
            current_pipeline->MarkUsed(frame_count);
            return BuiltPipeline(current_pipeline, is_indexed);
        }
    }
    static const bool fast_cache =
        VideoCommon::Nx2Perf::On(VideoCommon::Nx2Perf::Opt::PipelineFastCache);
    if (!fast_cache) {
        return CurrentGraphicsPipelineSlowPath(is_indexed);
    }
    // Every cached pipeline went through the slow path, so used_this_session already holds it.
    GraphicsPipeline*& fast_entry =
        graphics_fast_cache[GraphicsFastCacheSlot(graphics_key, GraphicsFastCacheSize)];
    if (fast_entry != nullptr && fast_entry->Key() == graphics_key) {
        bool use_fast = true;
        if (VideoCommon::Nx2Perf::Verify()) {
            const auto it = graphics_cache.find(graphics_key);
            GraphicsPipeline* const slow = it != graphics_cache.end() ? it->second.get() : nullptr;
            if (slow != fast_entry) {
                use_fast = false;
                if (graphics_fast_mismatches++ < 64) {
                    LOG_ERROR(HW_GPU,
                              "Nx2Perf pipeline_fast_cache: fast={} cache={} key={:016x} "
                              "mismatches={}",
                              fmt::ptr(fast_entry), fmt::ptr(slow), graphics_key.Hash(),
                              graphics_fast_mismatches);
                }
                fast_entry = nullptr;
            }
        }
        if (use_fast) {
            GraphicsPipeline* const pipeline = fast_entry;
            if (current_pipeline) {
                current_pipeline->AddTransition(pipeline);
            }
            current_pipeline = pipeline;
            current_pipeline->MarkUsed(frame_count);
            return BuiltPipeline(current_pipeline, is_indexed);
        }
    }
    GraphicsPipeline* const result = CurrentGraphicsPipelineSlowPath(is_indexed);
    if (current_pipeline != nullptr && current_pipeline->Key() == graphics_key) {
        fast_entry = current_pipeline;
    }
    return result;
}

constexpr u32 MaxComputeRebuilds = 4;

namespace {
/// Which pointer slots a native program's arguments fill depends on the area (Ocarina of Time's
/// foliage culling fills different list slots in Hyrule Field and Castle Town), so a translation
/// rebuilt in one area may lack pointers the first cached one binds. Try first the translations
/// that bind every pointer the first one binds, those binding the most first, then the others in
/// cache order. `keys` holds the pointer keys of each candidate; returns the order to use.
std::vector<size_t> OrderComputeTranslations(const std::vector<std::vector<u64>>& keys) {
    std::vector<size_t> order(keys.size());
    std::iota(order.begin(), order.end(), size_t{0});
    if (keys.size() < 2) {
        return order;
    }
    const auto keeps_first = [&](size_t index) {
        return std::ranges::includes(keys[index], keys.front());
    };
    std::ranges::stable_sort(order, [&](size_t lhs, size_t rhs) {
        const bool lhs_keeps{keeps_first(lhs)};
        if (lhs_keeps != keeps_first(rhs)) {
            return lhs_keeps;
        }
        return lhs_keeps && keys[lhs].size() > keys[rhs].size();
    });
    return order;
}
} // Anonymous namespace

bool PipelineCache::StaleNullPointerSlot(const ComputePipelineCacheEntry& entry) const {
    const auto& qmd{kepler_compute->launch_description};
    for (const auto& [index, offset] : entry.null_pointer_slots) {
        if (index >= qmd.const_buffer_config.size() ||
            ((qmd.const_buffer_enable_mask >> index) & 1) == 0) {
            continue;
        }
        const auto& cbuf{qmd.const_buffer_config[index]};
        if (offset + sizeof(u64) > cbuf.size) {
            continue; // The translator reads past the bound size as zero, too
        }
        const GPUVAddr slot{cbuf.Address() + offset};
        if (!gpu_memory->IsFullyMappedRange(slot, sizeof(u64))) {
            continue;
        }
        const GPUVAddr address{gpu_memory->Read<u64>(slot)};
        if (address != 0 && gpu_memory->IsWithinGPUAddressRange(address) &&
            gpu_memory->IsFullyMappedRange(address, 1)) {
            LOG_INFO(Render_Vulkan,
                     "Rebuilding compute program {:#x}: c{}[{:#x}] was null at translation and "
                     "now points to {:#x}",
                     kepler_compute->regs.code_loc.Address() + qmd.program_start, index, offset,
                     address);
            return true;
        }
    }
    return false;
}

ComputePipeline* PipelineCache::CurrentComputePipeline(u64& native_key) {
    compute_skip = DriverBuildReport::SkipReason::Unknown;
    const ShaderInfo* const shader{ComputeShader()};
    if (!shader) {
        compute_skip = DriverBuildReport::SkipReason::ComputeNoShader;
        return nullptr;
    }
    native_key = shader->lowered_native ? shader->lowered_native->native_program_key : 0;
    const auto& qmd{kepler_compute->launch_description};
    const ComputePipelineCacheKey key{
        .unique_hash = shader->unique_hash,
        .shared_memory_size = qmd.shared_alloc,
        .workgroup_size{qmd.block_dim_x, qmd.block_dim_y, qmd.block_dim_z},
    };
    const size_t fast_slot = static_cast<size_t>((key.unique_hash ^ (key.unique_hash >> 32) ^
                                                  key.shared_memory_size ^ key.workgroup_size[0]) &
                                                 (ComputeFastCacheSize - 1));
    auto& fast_entry = compute_fast_cache[fast_slot];
    if (fast_entry.pipeline != nullptr && fast_entry.key == key) {
        return fast_entry.pipeline;
    }
    const auto [pair, is_new]{compute_cache.try_emplace(key)};
    auto& entry{pair->second};
    const bool rebuild_async =
        VideoCommon::Nx2Perf::On(VideoCommon::Nx2Perf::Opt::ComputeRebuildAsync);
    if (entry.pending) {
        if (entry.pending->IsBuilt()) {
            if (entry.pipeline) {
                retired_compute_pipelines.emplace_back(scheduler.CurrentTick(),
                                                       std::move(entry.pipeline));
            }
            entry.pipeline = std::move(entry.pending);
        } else if (!rebuild_async) {
            if (entry.pipeline) {
                retired_compute_pipelines.emplace_back(scheduler.CurrentTick(),
                                                       std::move(entry.pipeline));
            }
            entry.pipeline = std::move(entry.pending);
        }
    }
    bool adopted = false;
    if (is_new) {
        ++new_compute_pipelines;
        if (precache_keys.contains(key.Hash())) {
            ++precached_compute_pipelines;
            ClaimPrecacheProgram(key);
        }
        adopted = prepare_active.load(std::memory_order_relaxed) && TakePreparedCompute(key, entry);
        if (!adopted) {
            entry = CreateComputePipeline(key, shader);
        }
    }
    if ((!is_new || adopted) && !entry.pending && !entry.null_pointer_slots.empty() &&
        entry.rebuilds < MaxComputeRebuilds && StaleNullPointerSlot(entry)) {
        if (prepare_active.load(std::memory_order_relaxed)) {
            TakePreparedCompute(key, entry); // Variants that finished since the first use
        }
        bool swapped_alternate = false;
        if (VideoCommon::Nx2Perf::On(VideoCommon::Nx2Perf::Opt::ComputePreloadVariants)) {
            for (auto& alt : entry.alternates) {
                ComputePipelineCacheEntry alt_check{
                    .null_pointer_slots = alt.null_pointer_slots,
                };
                if (alt.pipeline && alt.pipeline->IsBuilt() && !StaleNullPointerSlot(alt_check)) {
                    std::swap(entry.pipeline, alt.pipeline);
                    std::swap(entry.null_pointer_slots, alt.null_pointer_slots);
                    std::swap(entry.mapped_pointers, alt.mapped_pointers);
                    ++entry.rebuilds;
                    swapped_alternate = true;
                    break;
                }
            }
        }
        if (!swapped_alternate) {
            // The translation dropped accesses through a pointer that was null back then. Which
            // slots the game fills depends on the area, so the new one keeps binding every pointer
            // this one binds, null now or not: a null pointer binds nothing.
            auto rebuilt{CreateComputePipeline(key, shader, entry.mapped_pointers)};
            if (rebuilt.pipeline) {
                if (rebuild_async && entry.pipeline && !rebuilt.pipeline->IsBuilt()) {
                    entry.pending = std::move(rebuilt.pipeline);
                    entry.null_pointer_slots = std::move(rebuilt.null_pointer_slots);
                    entry.mapped_pointers = std::move(rebuilt.mapped_pointers);
                    ++entry.rebuilds;
                } else {
                    if (entry.pipeline) {
                        retired_compute_pipelines.emplace_back(scheduler.CurrentTick(),
                                                               std::move(entry.pipeline));
                    }
                    rebuilt.alternates = std::move(entry.alternates);
                    rebuilt.rebuilds = entry.rebuilds + 1;
                    entry = std::move(rebuilt);
                }
            } else {
                // Keep the working translation when the new one cannot be built (for example a
                // narrow access that now stays global across two buffers), and stop retrying.
                LOG_WARNING(Render_Vulkan, "Keeping compute shader {:016x}: rebuild failed: {}",
                            key.unique_hash, rebuilt.failure);
                entry.rebuilds = MaxComputeRebuilds;
            }
        }
    }
    if (entry.pipeline && !entry.pending &&
        (entry.null_pointer_slots.empty() || entry.rebuilds >= MaxComputeRebuilds)) {
        fast_entry.key = key;
        fast_entry.pipeline = entry.pipeline.get();
    }
    if (!entry.pipeline) {
        compute_skip = DriverBuildReport::SkipReason::ComputeNoPipeline;
    }
    return entry.pipeline.get();
}

void PipelineCache::LoadDiskResources(u64 title_id, std::stop_token stop_loading,
                                      const VideoCore::DiskResourceLoadCallback& callback) {
    // Loading validates and indexes the cache files and returns: the pipelines they hold are
    // prepared by the disk workers afterwards (RunPreparation), so the boot waits for none of them.
    SetWorkerBudget(true);
    FinishCacheSession();
    if (title_id == 0) {
        return;
    }
    const auto base_dir{
        CreatePipelineCacheDir(Common::FS::GetEdenPath(Common::FS::EdenPath::ShaderDir), title_id)};
    if (base_dir.empty()) {
        LOG_ERROR(Render_Vulkan,
                  "Pipeline cache disabled for this session: failed to create the cache "
                  "directory for title {:016x}",
                  title_id);
        // Do not keep writing to the previous title's cache files.
        pipeline_cache_filename.clear();
        mesh_pipeline_cache_filename.clear();
        used_pipelines_filename.clear();
        vulkan_pipeline_cache_filename.clear();
        return;
    }
    // Own names let this build's caches coexist with the ones of other builds in the same
    // directory; a file of the old layout under these names is backed up and replaced.
    const auto cache_path = [&](std::string_view stem) {
        return base_dir / fmt::format("{}.mythos.bin", stem);
    };
    pipeline_cache_filename = cache_path("vulkan");
    used_pipelines_filename = cache_path("vulkan_used");
    mesh_pipeline_cache_filename = cache_path("vulkan_mesh");
    used_pipelines = LoadUsedPipelines(used_pipelines_filename, MaxUsedPipelineAge(device));
    used_pipelines_saved = std::chrono::steady_clock::now();

    CE::PrepareOptions options;
    options.workers = device.HasBrokenParallelShaderCompiling() ? 1U : 2U;
    auto session = std::make_unique<PipelineCacheSession>(
        MakeCacheIdentity(CE::PayloadKind::Transferable), options);
    for (const auto& [hash, age] : used_pipelines) {
        session->used_hashes.insert(hash);
    }
    if (device.IsKhrPipelineExecutablePropertiesEnabled()) {
        session->statistics = std::make_unique<PipelineStatistics>(device);
    }
    const auto open_file = [&](CE::PayloadKind kind, const std::filesystem::path& path,
                               u64 max_bytes) {
        PipelineCacheSession::File file;
        file.kind = kind;
        file.path = path;
        file.max_bytes = max_bytes;
        CE::RemoveStaleTemp(path);
        file.reader = CE::EnvelopeReader::Open(path, MakeCacheIdentity(kind), file.load);
        file.load.kind = kind;
        if (!file.reader && file.load.reason != CE::RejectReason::Missing) {
            LOG_WARNING(Render_Vulkan, "{}", CE::CacheRejectLine(file.load));
        }
        session->files.push_back(std::move(file));
    };
    open_file(CE::PayloadKind::Transferable, pipeline_cache_filename,
              CE::MaxTransferableBytes / 4 * 3);
    if (device.IsExtMeshShaderSupported()) {
        open_file(CE::PayloadKind::TransferableMesh, mesh_pipeline_cache_filename,
                  CE::MaxTransferableBytes / 4);
    }

    vulkan_pipeline_cache_filename.clear();
    if (use_vulkan_pipeline_cache) {
        // Driver binaries with a different optimization policy live separately.
        vulkan_pipeline_cache_filename =
            cache_path(VideoCommon::Nx2Perf::On(VideoCommon::Nx2Perf::Opt::ComputeUnoptimizedSlow)
                           ? "vulkan_pipelines.unoptimized"
                           : "vulkan_pipelines");
        CE::CacheLoadResult blob_result;
        vulkan_pipeline_cache = LoadVulkanPipelineCache(vulkan_pipeline_cache_filename, blob_result);
        blob_result.kind = CE::PayloadKind::DriverBlob;
        if (!blob_result.accepted && blob_result.reason != CE::RejectReason::Missing) {
            LOG_WARNING(Render_Vulkan, "{}", CE::CacheRejectLine(blob_result));
        }
        session->extra_reports.push_back(
            CE::CacheFileReport{.kind = CE::PayloadKind::DriverBlob, .load = blob_result});
        // Before any build runs: Intel's Windows driver crashes reading the cache while a build
        // runs (CanReadPipelineCacheDuringBuilds).
        size_t size = 0;
        vulkan_pipeline_cache.Read(&size, nullptr);
        last_cache_size.store(size, std::memory_order_relaxed);
        last_flush = std::chrono::steady_clock::now();
    }

    std::vector<PP::Candidate> candidates;
    for (size_t file_index = 0; file_index < session->files.size(); ++file_index) {
        const auto& file = session->files[file_index];
        if (!file.reader) {
            continue;
        }
        const auto index = file.reader->Index();
        for (size_t record = 0; record < index.size(); ++record) {
            if (index[record].meta.kind == CE::RecordKind::Opaque) {
                continue;
            }
            candidates.push_back(PP::Candidate{index[record].meta, index[record].length});
            session->sources.push_back(PipelineCacheSession::Source{
                static_cast<u32>(file_index), static_cast<u32>(record)});
        }
    }
    cache_session = std::move(session);
    PipelineCacheSession& active = *cache_session;
    if (!candidates.empty() && !stop_loading.stop_requested()) {
        active.generation = active.scheduler.Begin(candidates);
        prepare_active.store(true, std::memory_order_release);
        for (u32 worker = 0; worker < options.workers; ++worker) {
            disk_replay_workers.QueueWork(
                [this, generation = active.generation] { RunPreparation(generation); });
        }
        LOG_INFO(Render_Vulkan, "Preparing {} recorded pipelines in the background ({} workers)",
                 candidates.size(), options.workers);
    }
    PublishSession(active);
    callback(VideoCore::LoadCallbackStage::Build, candidates.size(), candidates.size());
    if (!stop_loading.stop_requested()) {
        StartShaderPrecache(title_id);
    }
}

void PipelineCache::RunPreparation(u64 generation) {
    PipelineCacheSession& session = *cache_session;
    PP::Scheduler& prepare = session.scheduler;
    ShaderPools pools;
    for (;;) {
        const PP::Scheduler::Task task =
            prepare.Next(generation, PP::AvailablePhysicalMemoryBytes());
        switch (task.kind) {
        case PP::Scheduler::TaskKind::Done:
            return;
        case PP::Scheduler::TaskKind::Wait:
            prepare.WaitForChange(task.epoch, std::chrono::milliseconds{250});
            break;
        case PP::Scheduler::TaskKind::Load:
            LoadPreparedRecord(session, dynamic_features, task);
            break;
        case PP::Scheduler::TaskKind::Build: {
            // Demand wins: wait out a burst of runtime compilation, but not for good, so a
            // constant stream of new pipelines cannot starve preparation.
            for (int waited = 0; waited < 25 && RuntimeBuildsBusy() && prepare.Running();
                 ++waited) {
                std::this_thread::sleep_for(std::chrono::milliseconds{20});
            }
            const auto record = std::static_pointer_cast<PreparedRecord>(task.payload);
            std::shared_ptr<void> result;
            bool ok = false;
            {
                Spikes::SpanScope span{Spikes::Cause::Pipeline, task.work};
                const DriverBuildReport::DiskScope from_disk;
                try {
                    pools.ReleaseContents();
                    if (record->compute) {
                        auto built = CreateComputePipeline(pools, record->compute_key,
                                                           record->envs.front(),
                                                           session.statistics.get(), false);
                        ok = built.pipeline != nullptr;
                        if (ok) {
                            result = std::make_shared<PreparedCompute>(std::move(built));
                        }
                    } else {
                        boost::container::static_vector<Shader::Environment*, 5> env_ptrs;
                        for (auto& env : record->envs) {
                            env_ptrs.push_back(&env);
                        }
                        auto pipeline = CreateGraphicsPipeline(pools, record->graphics_key,
                                                               MakeSpan(env_ptrs),
                                                               session.statistics.get(), false);
                        ok = pipeline != nullptr;
                        if (ok) {
                            result = std::make_shared<PreparedGraphics>(record->graphics_key,
                                                                        std::move(pipeline));
                        }
                    }
                } catch (const std::exception& exception) {
                    LOG_ERROR(Render_Vulkan, "Preparing pipeline {:016x} failed: {}",
                              record->compute ? record->compute_key.Hash()
                                              : record->graphics_key.Hash(),
                              exception.what());
                    ok = false;
                }
                if (!ok) {
                    span.Fail();
                }
            }
            // False when the generation ended meanwhile: the result is dropped here, after the
            // scheduler lock was released.
            static_cast<void>(prepare.BuildFinished(task.id, task.generation, ok, result));
            break;
        }
        }
    }
}

void PipelineCache::FinishCacheSession() {
    if (!cache_session) {
        return;
    }
    PipelineCacheSession& session = *cache_session;
    // Cancel and join first: a worker mid-build finishes that build (a driver compile cannot be
    // interrupted) and drops its result, then every worker returns. Nothing refers to the session
    // afterwards.
    prepare_active.store(false, std::memory_order_release);
    auto dropped = session.scheduler.Cancel();
    disk_replay_workers.ClearRequests();
    disk_replay_workers.WaitForRequests();
    dropped.clear();
    // Records still staged, then the files: readers close before the commit replaces them.
    serialization_thread.WaitForRequests();
    if (session.statistics) {
        session.statistics->Report();
    }
    CE::WriteStatus status = CE::WriteStatus::NotAttempted;
    for (auto& file : session.files) {
        file.reader.reset();
        if (!file.writer) {
            continue;
        }
        if (file.writer->Commit() && status != CE::WriteStatus::Failed) {
            status = CE::WriteStatus::Ok;
        } else {
            status = CE::WriteStatus::Failed;
            LOG_WARNING(Render_Vulkan, "Pipeline cache {} was not written; the previous one is kept",
                        CE::PayloadKindName(file.kind));
        }
        file.writer.reset();
    }
    session.write_status = status;
    if (use_vulkan_pipeline_cache && !vulkan_pipeline_cache_filename.empty()) {
        if (!device.CanReadPipelineCacheDuringBuilds()) {
            // Drop queued builds, as destroying the workers would; running ones must end first.
            workers.ClearRequests();
            workers.WaitForRequests();
        }
        SerializeVulkanPipelineCache(vulkan_pipeline_cache_filename, vulkan_pipeline_cache);
    }
    PublishSession(session);
    cache_session.reset();
}

namespace {
constexpr u32 MaxStagedRecords = 128;
} // Anonymous namespace

bool PipelineCache::TakePreparedGraphics() {
    if (!cache_session) {
        return false;
    }
    PipelineCacheSession& session = *cache_session;
    const u64 hash = graphics_key.Hash();
    PP::Scheduler::Demand demand = RequestPrepared(session, hash, EncodeGraphicsKey(graphics_key));
    switch (demand.kind) {
    case PP::Scheduler::DemandKind::Ready:
        for (const auto& result : demand.results) {
            auto& ready = *static_cast<PreparedGraphics*>(result.get());
            if (ready.pipeline) {
                graphics_cache.try_emplace(ready.key, std::move(ready.pipeline));
            }
        }
        return graphics_cache.contains(graphics_key);
    case PP::Scheduler::DemandKind::Claimed:
    case PP::Scheduler::DemandKind::Unavailable:
        // The record is in the file already: the build the caller does next is not recorded again.
        demanded_hash = hash;
        return false;
    case PP::Scheduler::DemandKind::Miss:
    case PP::Scheduler::DemandKind::Building: // RequestPrepared waits these out
        break;
    }
    return false;
}

bool PipelineCache::TakePreparedCompute(const ComputePipelineCacheKey& key,
                                        ComputePipelineCacheEntry& entry) {
    if (!cache_session) {
        return false;
    }
    PP::Scheduler::Demand demand =
        RequestPrepared(*cache_session, key.Hash(), EncodeComputeKey(key));
    if (demand.kind != PP::Scheduler::DemandKind::Ready) {
        return entry.pipeline != nullptr;
    }
    std::vector<ComputePipelineCacheEntry*> ready;
    std::vector<std::vector<u64>> pointer_keys;
    for (const auto& result : demand.results) {
        ComputePipelineCacheEntry& built = static_cast<PreparedCompute*>(result.get())->entry;
        if (built.pipeline) {
            ready.push_back(&built);
            pointer_keys.push_back(built.mapped_pointers);
        }
    }
    const bool variants = VideoCommon::Nx2Perf::On(VideoCommon::Nx2Perf::Opt::ComputePreloadVariants);
    const auto known = [&](const std::vector<u64>& pointers) {
        return (entry.pipeline && entry.mapped_pointers == pointers) ||
               std::ranges::any_of(entry.alternates, [&](const ComputePipelineVariant& alternate) {
                   return alternate.mapped_pointers == pointers;
               });
    };
    for (const size_t index : OrderComputeTranslations(pointer_keys)) {
        ComputePipelineCacheEntry& built = *ready[index];
        if (!entry.pipeline) {
            entry.pipeline = std::move(built.pipeline);
            entry.null_pointer_slots = std::move(built.null_pointer_slots);
            entry.mapped_pointers = std::move(built.mapped_pointers);
        } else if (variants && !known(built.mapped_pointers)) {
            entry.alternates.push_back(ComputePipelineVariant{
                .pipeline = std::move(built.pipeline),
                .null_pointer_slots = std::move(built.null_pointer_slots),
                .mapped_pointers = std::move(built.mapped_pointers),
            });
        }
    }
    return entry.pipeline != nullptr;
}

bool PipelineCache::ReserveRecord() {
    if (!cache_session || cache_session->files.empty()) {
        return false;
    }
    PipelineCacheSession& session = *cache_session;
    if (session.staged.load(std::memory_order_relaxed) >= MaxStagedRecords) {
        session.skipped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    session.staged.fetch_add(1, std::memory_order_relaxed);
    return true;
}

CE::RecordMeta PipelineCache::NextRecordMeta(CE::RecordKind kind,
                                             std::chrono::steady_clock::duration build) {
    const bool boot = frame_count == 0;
    const s64 micros = std::chrono::duration_cast<std::chrono::microseconds>(build).count();
    return CE::RecordMeta{
        .kind = kind,
        .first_use_phase = boot ? CE::FirstUse::Boot : CE::FirstUse::Run,
        .first_use_ordinal = ++record_ordinal,
        .first_use_ns = boot ? 0 : Spikes::NowNs() - first_frame_ns,
        .translate_cost_us = 0,
        .build_cost_us = static_cast<u32>(
            std::clamp<s64>(micros, 0, std::numeric_limits<u32>::max())),
    };
}

void PipelineCache::SetWorkerBudget(bool after_boot) {
    const size_t builders_boot =
        device.HasBrokenParallelShaderCompiling() ? 1ULL : GetTotalPipelineWorkers();
    const size_t translators_boot = translation_threads_boot;
    size_t builders = builders_boot;
    size_t translators = translators_boot;
    const bool budget =
        after_boot && VideoCommon::Nx2Perf::On(VideoCommon::Nx2Perf::Opt::WorkerBudget);
    if (budget) {
        const size_t pool = PostBootPoolThreads();
        builders = std::min(builders, pool);
        // An explicit EDEN_NX2_TRANSLATION_THREADS stays as asked.
        if (std::getenv("EDEN_NX2_TRANSLATION_THREADS") == nullptr) {
            translators = std::min(translators, pool);
        }
    }
    workers.SetWorkerLimit(builders);
    if (translation_workers) {
        translation_workers->SetWorkerLimit(translators);
    }
    num_translation_threads.store(translators, std::memory_order_relaxed);
    DriverBuildReport::SetBuilderThreads(builders + translators);
    if (after_boot) {
        LOG_INFO(Render_Vulkan,
                 "Pipeline workers after the disk cache load: {} driver builders (boot {}), {} "
                 "translators (boot {}); {} physical cores, {} hardware threads, worker_budget {}",
                 builders, builders_boot, translators, translators_boot,
                 Common::PhysicalCoreCount(), std::thread::hardware_concurrency(),
                 budget ? "on" : "off");
    }
}

void PipelineCache::QueueVulkanPipelineCacheFlush() {
    if (!use_vulkan_pipeline_cache || vulkan_pipeline_cache_filename.empty() ||
        !device.CanReadPipelineCacheDuringBuilds()) {
        return;
    }
    if (++pipelines_since_flush < VULKAN_CACHE_FLUSH_PIPELINES) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    const auto megabytes = last_cache_size.load(std::memory_order_relaxed) / (1024 * 1024);
    const std::chrono::seconds interval{
        std::max<size_t>(VULKAN_CACHE_FLUSH_MIN_SECONDS, megabytes)};
    if (last_flush.time_since_epoch().count() != 0 && now - last_flush < interval) {
        return;
    }
    if (flush_in_flight.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    pipelines_since_flush = 0;
    last_flush = now;
    serialization_thread.QueueWork([this] {
        SerializeVulkanPipelineCache(vulkan_pipeline_cache_filename, vulkan_pipeline_cache);
        size_t size = 0;
        vulkan_pipeline_cache.Read(&size, nullptr);
        last_cache_size.store(size, std::memory_order_relaxed);
        flush_in_flight.store(false, std::memory_order_release);
    });
}

namespace {
// EDEN_NX2_PIPELINE_DIFF=1: for a new graphics pipeline whose shaders an earlier one uses too,
// log which parts of the fixed state differ from that one (finds state that splits the same
// draws into many pipelines, each translated on the GPU thread).
void ReportNewPipelineKey(const GraphicsPipelineCacheKey& key) {
    static const bool enabled = std::getenv("EDEN_NX2_PIPELINE_DIFF") != nullptr;
    if (!enabled) {
        return;
    }
    static std::unordered_map<u64, GraphicsPipelineCacheKey> last_by_shaders;
    static u32 logged{};
    const u64 shaders = Common::CityHash64(reinterpret_cast<const char*>(key.unique_hashes.data()),
                                           sizeof(key.unique_hashes));
    const auto [it, first] = last_by_shaders.try_emplace(shaders, key);
    if (first) {
        return;
    }
    const FixedPipelineState& now = key.state;
    const FixedPipelineState& before = it->second.state;
    std::string diffs;
    if (now.raw1 != before.raw1)
        diffs += fmt::format(" raw1^{:#x}", now.raw1 ^ before.raw1);
    if (now.raw2 != before.raw2)
        diffs += fmt::format(" raw2^{:#x}", now.raw2 ^ before.raw2);
    if (now.color_formats != before.color_formats) {
        diffs += fmt::format(" color_formats {:02x} -> {:02x}", fmt::join(before.color_formats, ""),
                             fmt::join(now.color_formats, ""));
    }
    if (now.alpha_test_ref != before.alpha_test_ref) {
        diffs += fmt::format(" alpha_test_ref {:#x} -> {:#x} (func {})", before.alpha_test_ref,
                             now.alpha_test_ref, now.alpha_test_func.Value());
    }
    if (now.point_size != before.point_size) {
        diffs += fmt::format(" point_size {:#x} -> {:#x}", before.point_size, now.point_size);
    }
    if (now.viewport_swizzles != before.viewport_swizzles)
        diffs += " viewport_swizzles";
    if (now.attribute_types != before.attribute_types) {
        diffs +=
            fmt::format(" attribute_types^{:#x}", now.attribute_types ^ before.attribute_types);
    }
    if (std::memcmp(&now.dynamic_state, &before.dynamic_state, sizeof(now.dynamic_state)) != 0) {
        diffs += " dynamic_state";
    }
    if (now.driver_id != before.driver_id || now.driver_version != before.driver_version) {
        diffs += " driver";
    }
    const size_t size = now.Size();
    if (diffs.empty() && size > offsetof(FixedPipelineState, attachments)) {
        diffs = fmt::format(" past attachments (key size {})", size);
    }
    it->second = key;
    if (logged++ < 400) {
        LOG_INFO(Render_Vulkan, "New pipeline for known shaders {:016x} differs in:{}", shaders,
                 diffs.empty() ? std::string{" nothing in the key prefix"} : diffs);
    }
}
u32 ActiveColorTargetCount(const Maxwell& regs) noexcept {
    const u32 count =
        std::min<u32>(regs.rt_control.count, static_cast<u32>(Maxwell::NumRenderTargets));
    u32 active = 0;
    for (u32 i = 0; i < count; ++i) {
        const u32 rt_index = regs.rt_control.Map(i);
        if (rt_index < Maxwell::NumRenderTargets &&
            regs.rt[rt_index].format != Tegra::RenderTargetFormat::NONE &&
            regs.color_mask[regs.color_mask_common ? 0 : rt_index].raw != 0) {
            ++active;
        }
    }
    return active;
}

bool IsDepthTestedColorDraw(const Maxwell& regs) noexcept {
    return regs.zeta_enable != 0 && ActiveColorTargetCount(regs) > 0;
}

bool IsDepthOnlyDraw(const Maxwell& regs) noexcept {
    return regs.zeta_enable != 0 && ActiveColorTargetCount(regs) == 0;
}

bool IsGBufferDraw(const Maxwell& regs) noexcept {
    return regs.zeta_enable != 0 && ActiveColorTargetCount(regs) > 1;
}

// EDEN_NX2_GEOMETRY_GUARD=legacy restores the guard as it was: a key without the base instance,
// and a draw skipped behind a disk replay suppresses depth-only draws of its geometry for the
// frame and the one after it, whether or not the replay finished in between.
bool LegacyGeometryGuard() {
    static const bool legacy = [] {
        const char* const value = std::getenv("EDEN_NX2_GEOMETRY_GUARD");
        const bool is_legacy = value != nullptr && std::strcmp(value, "legacy") == 0;
        LOG_INFO(Render_Vulkan, "Geometry consistency guard: {}",
                 is_legacy ? "legacy" : "base instance in the key, disk replay ends the skip");
        return is_legacy;
    }();
    return legacy;
}

u64 DrawGeometryKey(const Tegra::Engines::Maxwell3D& maxwell3d, bool is_indexed) noexcept {
    const auto& draw_state = maxwell3d.draw_manager.draw_state;
    if (draw_state.draw_element_count == 0) {
        return 0;
    }
    u64 hash = 0x9e3779b97f4a7c15ULL;
    const auto mix = [&](u64 word) noexcept {
        hash = (hash ^ word) * 0xff51afd7ed558ccdULL;
        hash ^= hash >> 33;
    };
    if (is_indexed) {
        mix(draw_state.index_buffer.StartAddress());
        mix((u64{draw_state.index_buffer.first} << 32) | draw_state.index_buffer.count);
        mix(draw_state.base_index);
    } else {
        mix(maxwell3d.regs.vertex_streams[0].Address());
        mix((u64{draw_state.vertex_buffer.first} << 32) | draw_state.vertex_buffer.count);
    }
    if (!LegacyGeometryGuard()) {
        mix(draw_state.base_instance);
    }
    return hash == 0 ? 1ULL : hash;
}

// Without VK_EXT_mesh_shader: logs, once per shader set, what a skipped task/mesh pipeline would
// need from an emulation (task stage, per-primitive outputs, topology, output sizes).
template <typename Programs>
void ReportUnsupportedMeshPipeline(const GraphicsPipelineCacheKey& key, const Programs& programs) {
    static std::mutex mutex;
    static std::unordered_set<u64> seen;
    static u32 with_task{}, per_primitive{}, non_triangle{}, total{};
    u64 shaders{};
    for (const u64 hash : key.unique_hashes) {
        shaders = shaders * 31 + hash;
    }
    std::scoped_lock lock{mutex};
    if (!seen.insert(shaders).second) {
        return;
    }
    const Shader::IR::Program* task{};
    const Shader::IR::Program* mesh{};
    bool has_pixel{};
    for (size_t index = 0; index < programs.size(); ++index) {
        if (key.unique_hashes[index] == 0) {
            continue;
        }
        const auto& program{programs[index]};
        if (program.stage == Shader::Stage::Task) {
            task = &program;
        } else if (program.stage == Shader::Stage::Mesh) {
            mesh = &program;
        } else if (program.stage == Shader::Stage::Fragment) {
            has_pixel = true;
        }
    }
    ++total;
    if (mesh == nullptr) {
        LOG_WARNING(Render_Vulkan, "Mesh emulation survey: pipeline {:016x} without a mesh stage",
                    shaders);
        return;
    }
    const auto& info{mesh->mesh};
    u32 position{}, generic{}, other{};
    std::string others;
    for (const u32 line : info.vertex_lines) {
        const auto attribute{static_cast<Shader::IR::Attribute>(line)};
        if (attribute >= Shader::IR::Attribute::PositionX &&
            attribute <= Shader::IR::Attribute::PositionW) {
            ++position;
        } else if (Shader::IR::IsGeneric(attribute)) {
            ++generic;
        } else {
            ++other;
            others += fmt::format(" {}", line);
        }
    }
    with_task += task != nullptr;
    per_primitive += !info.primitive_lines.empty();
    non_triangle += info.topology != 4;
    LOG_WARNING(Render_Vulkan,
                "Mesh emulation survey #{}: task={} (payload {} words, wg {}x{}x{}) mesh wg "
                "{}x{}x{} max_vertices={} max_primitives={} topology={} vertex_lines={} "
                "(position {} generic {} other {}:{}) primitive_lines={} pixel={} | totals: "
                "task {} per-primitive {} non-triangle {}",
                total, task != nullptr, info.payload_words, task ? task->workgroup_size[0] : 0,
                task ? task->workgroup_size[1] : 0, task ? task->workgroup_size[2] : 0,
                mesh->workgroup_size[0], mesh->workgroup_size[1], mesh->workgroup_size[2],
                info.max_vertices, info.max_primitives, info.topology, info.vertex_lines.size(),
                position, generic, other, others, info.primitive_lines.size(), has_pixel, with_task,
                per_primitive, non_triangle);
}
} // Anonymous namespace

bool PipelineCache::GeometryEntryPending(const SkippedGeometryEntry& entry) const noexcept {
    if (entry.pipeline != nullptr) {
        return !entry.pipeline->IsBuilt();
    }
    // No pipeline yet: the draw is held back until a later draw builds one.
    return true;
}

void PipelineCache::RecordSkippedGeometry(bool is_indexed,
                                          const GraphicsPipeline* pipeline) noexcept {
    if (!maxwell3d || !VideoCommon::Nx2Perf::On(VideoCommon::Nx2Perf::Opt::GeometryConsistency) ||
        !IsDepthTestedColorDraw(maxwell3d->regs)) {
        return;
    }
    const u64 geom_key = DrawGeometryKey(*maxwell3d, is_indexed);
    if (geom_key == 0) {
        return;
    }
    const size_t base =
        static_cast<size_t>(geom_key ^ (geom_key >> 29)) & (SkippedGeometryTableSize - 1);
    size_t target_slot = base;
    for (size_t probe = 0; probe < 4; ++probe) {
        const size_t slot = (base + probe) & (SkippedGeometryTableSize - 1);
        const auto& entry = skipped_geometry[slot];
        if (entry.key == geom_key || entry.key == 0 || frame_count > entry.frame + 1 ||
            !GeometryEntryPending(entry)) {
            target_slot = slot;
            break;
        }
    }
    skipped_geometry[target_slot] = {
        .key = geom_key,
        .frame = frame_count,
        .pipeline = pipeline,
    };
    last_skipped_geometry_frame = frame_count;
}

bool PipelineCache::ShouldSkipDepthOnlyGeometry(bool is_indexed) noexcept {
    if (!maxwell3d || last_skipped_geometry_frame == ~u64{} ||
        frame_count > last_skipped_geometry_frame + 1 ||
        !VideoCommon::Nx2Perf::On(VideoCommon::Nx2Perf::Opt::GeometryConsistency) ||
        !IsDepthOnlyDraw(maxwell3d->regs)) {
        return false;
    }
    const u64 geom_key = DrawGeometryKey(*maxwell3d, is_indexed);
    if (geom_key == 0) {
        return false;
    }
    const size_t base =
        static_cast<size_t>(geom_key ^ (geom_key >> 29)) & (SkippedGeometryTableSize - 1);
    for (size_t probe = 0; probe < 4; ++probe) {
        auto& entry = skipped_geometry[(base + probe) & (SkippedGeometryTableSize - 1)];
        if (entry.key != geom_key) {
            continue;
        }
        if (frame_count <= entry.frame + 1 && GeometryEntryPending(entry)) {
            return true;
        }
        entry = {};
        return false;
    }
    return false;
}

void PipelineCache::ClearSkippedGeometryIfDrawn(bool is_indexed) noexcept {
    if (!maxwell3d || last_skipped_geometry_frame == ~u64{} ||
        frame_count > last_skipped_geometry_frame + 1 ||
        !VideoCommon::Nx2Perf::On(VideoCommon::Nx2Perf::Opt::GeometryConsistency) ||
        !IsDepthTestedColorDraw(maxwell3d->regs)) {
        return;
    }
    const u64 geom_key = DrawGeometryKey(*maxwell3d, is_indexed);
    if (geom_key == 0) {
        return;
    }
    const size_t base =
        static_cast<size_t>(geom_key ^ (geom_key >> 29)) & (SkippedGeometryTableSize - 1);
    for (size_t probe = 0; probe < 4; ++probe) {
        auto& entry = skipped_geometry[(base + probe) & (SkippedGeometryTableSize - 1)];
        if (entry.key == geom_key) {
            entry = {};
            return;
        }
    }
}

bool PipelineCache::DeferRuntimeBuild() {
    if (!use_asynchronous_shaders ||
        !VideoCommon::Nx2Perf::On(VideoCommon::Nx2Perf::Opt::ShaderBuildBudget)) {
        return false;
    }
    const auto& regs = maxwell3d->regs;
    const bool use_priority =
        VideoCommon::Nx2Perf::On(VideoCommon::Nx2Perf::Opt::BuildOrderPriority);
    const u64 key_hash = use_priority ? graphics_key.Hash() : 0;
    const bool is_gbuffer = use_priority && IsGBufferDraw(regs);
    size_t priority_index = priority_build_key_count;
    if (use_priority) {
        for (size_t i = 0; i < priority_build_key_count; ++i) {
            if (priority_build_keys[i] == key_hash) {
                priority_index = i;
                break;
            }
        }
    }
    const bool has_priority =
        !use_priority || is_gbuffer || priority_index < priority_build_key_count;
    const auto effective_budget = has_priority ? build_budget : (build_budget / 2);
    if (build_budget_frame != frame_count || build_budget_spent < effective_budget) {
        if (priority_index < priority_build_key_count) {
            priority_build_keys[priority_index] = priority_build_keys[--priority_build_key_count];
        }
        return false;
    }
    // Small draws without depth wait for their pipeline (BuiltPipeline), so they are built now
    // unless stale_target_skip sees their target already holds an earlier frame's output.
    // Mesh draws are deferred too: their high-detail LODs then appear a few frames late.
    if (regs.IsMeshEnabled()) {
        ++over_budget_mesh;
    } else if (!regs.zeta_enable) {
        const auto& draw_state = maxwell3d->draw_manager.draw_state;
        const bool stale_skip =
            VideoCommon::Nx2Perf::On(VideoCommon::Nx2Perf::Opt::StaleTargetSkip);
        const bool is_small_draw =
            stale_skip ? (draw_state.draw_element_count > 0 && draw_state.draw_element_count <= 6)
                       : (draw_state.draw_element_count <= 6);
        if (is_small_draw && (!stale_skip || !texture_cache.ColorTargetHasPriorFrameOutput())) {
            ++over_budget_small;
            return false;
        }
    }
    if (use_priority) {
        bool recorded = false;
        for (size_t i = 0; i < deferred_candidate_count; ++i) {
            if (deferred_candidates[i].key_hash == key_hash) {
                ++deferred_candidates[i].draws;
                deferred_candidates[i].is_gbuffer |= is_gbuffer;
                recorded = true;
                break;
            }
        }
        if (!recorded) {
            if (deferred_candidate_count < DeferredCandidateSlots) {
                deferred_candidates[deferred_candidate_count++] = {
                    .key_hash = key_hash,
                    .draws = 1,
                    .is_gbuffer = is_gbuffer,
                };
            } else if (is_gbuffer) {
                for (size_t i = 0; i < DeferredCandidateSlots; ++i) {
                    if (!deferred_candidates[i].is_gbuffer) {
                        deferred_candidates[i] = {
                            .key_hash = key_hash,
                            .draws = 1,
                            .is_gbuffer = true,
                        };
                        break;
                    }
                }
            }
        }
    }
    ++deferred_builds;
    return true;
}

void PipelineCache::ChargeRuntimeBuild(std::chrono::steady_clock::time_point begin) {
    if (build_budget_frame != frame_count) {
        build_budget_frame = frame_count;
        build_budget_spent = {};
    }
    build_budget_spent += std::chrono::steady_clock::now() - begin;
}

GraphicsPipeline* PipelineCache::CurrentGraphicsPipelineSlowPath(bool is_indexed) {
    if (num_translated.load(std::memory_order_acquire) != 0) {
        AdoptTranslatedGraphicsPipelines();
    }
    auto it{graphics_cache.find(graphics_key)};
    if (it == graphics_cache.end()) {
        if (!BuildNewGraphicsPipeline(is_indexed)) {
            return nullptr;
        }
        it = graphics_cache.find(graphics_key);
    }
    auto& pipeline{it->second};
    if (!pipeline) {
        graphics_skip = DriverBuildReport::SkipReason::CreationFailed;
        return nullptr;
    }
    const auto save_interval =
        VideoCommon::Nx2Perf::On(VideoCommon::Nx2Perf::Opt::PipelineRetention)
            ? std::chrono::steady_clock::duration{std::chrono::seconds{15}}
            : std::chrono::steady_clock::duration{std::chrono::minutes{2}};
    if (used_this_session.insert(pipeline.get()).second &&
        std::chrono::steady_clock::now() - used_pipelines_saved > save_interval) {
        SaveUsedPipelines();
    }
    if (current_pipeline) {
        current_pipeline->AddTransition(pipeline.get());
    }
    current_pipeline = pipeline.get();
    current_pipeline->MarkUsed(frame_count);
    return BuiltPipeline(current_pipeline, is_indexed);
}

void PipelineCache::TickFrame() {
    DriverBuildReport::Tick(DriverBuildReport::DrainArmed() && !ShaderBuildsBusy());
    if (deferred_candidate_count != 0) {
        const auto span = std::span{deferred_candidates.data(), deferred_candidate_count};
        std::ranges::sort(span, [](const DeferredCandidate& lhs, const DeferredCandidate& rhs) {
            if (lhs.is_gbuffer != rhs.is_gbuffer) {
                return lhs.is_gbuffer;
            }
            return lhs.draws > rhs.draws;
        });
        priority_build_key_count = std::min(deferred_candidate_count, PriorityBuildKeySlots);
        for (size_t i = 0; i < priority_build_key_count; ++i) {
            priority_build_keys[i] = deferred_candidates[i].key_hash;
        }
        deferred_candidate_count = 0;
    } else {
        priority_build_key_count = 0;
    }
    if (frame_count == 0) {
        first_frame_ns = Spikes::NowNs();
    }
    ++frame_count;
    while (!evicted_pipelines.empty() && scheduler.IsFree(evicted_pipelines.front().first)) {
        evicted_pipelines.pop_front();
    }
    while (!retired_compute_pipelines.empty() &&
           scheduler.IsFree(retired_compute_pipelines.front().first) &&
           retired_compute_pipelines.front().second->IsBuildTaskDone()) {
        retired_compute_pipelines.pop_front();
    }
    constexpr u64 EvictCheckFrames = 600;
    if (frame_count % EvictCheckFrames == 0) {
        EvictUnusedPipelines();
        if (deferred_builds != 0 || over_budget_small != 0 || over_budget_mesh != 0) {
            LOG_INFO(Render_Vulkan,
                     "Shader build budget ({} ms/frame): {} draws skipped and their pipelines "
                     "built in a later frame in the last {} frames; built over budget: {} small "
                     "draws without depth; {} of the skipped draws were mesh draws",
                     std::chrono::duration_cast<std::chrono::milliseconds>(build_budget).count(),
                     deferred_builds, EvictCheckFrames, over_budget_small, over_budget_mesh);
            deferred_builds = 0;
            over_budget_small = 0;
            over_budget_mesh = 0;
        }
    }
}

void PipelineCache::EvictUnusedPipelines() {
    // Building a pipeline again on MoltenVK means a Metal compile mid-game, far dearer than the
    // rebuild cost below, so none are evicted there.
    if (device.IsMoltenVK()) {
        return;
    }
    // Each built pipeline holds ~0.45 MB of driver memory and none was ever destroyed, so it grew
    // with every place a session went (title 1: 1,500 on its title screen alone). One unused for
    // this long is destroyed; rebuilding it later requires a full GPU-thread shader translation
    // (~8.5 ms each) plus a driver pipeline cache hit.
    constexpr u64 EvictAfterFrames = 9000;
    if (frame_count <= EvictAfterFrames) {
        return;
    }
    const u64 cutoff = frame_count - EvictAfterFrames;
    const u64 tick = scheduler.CurrentTick();
    // Nx2Perf pipeline_keep_preloaded (on by default): the ones built at boot from the used list
    // that this session has not drawn with yet keep LastUse() == 0; they are kept, not evicted
    // after 150 s and rebuilt on the GPU thread when the player enters their area.
    const bool keep_preloaded =
        VideoCommon::Nx2Perf::On(VideoCommon::Nx2Perf::Opt::PipelineKeepPreloaded);
    size_t evicted = 0;
    size_t kept = 0;
    for (auto it = graphics_cache.begin(); it != graphics_cache.end();) {
        GraphicsPipeline* const pipeline = it->second.get();
        if (pipeline == nullptr || pipeline == current_pipeline || !pipeline->IsBuilt() ||
            pipeline->LastUse() >= cutoff) {
            ++it;
            continue;
        }
        if (keep_preloaded && !used_this_session.contains(pipeline)) {
            ++kept;
            ++it;
            continue;
        }
        if (used_this_session.erase(pipeline) != 0) {
            used_and_evicted.insert(it->first.Hash());
        }
        evicted_keys.insert(it->first.Hash());
        evicted_pipelines.emplace_back(tick, std::move(it->second));
        it = graphics_cache.erase(it);
        ++evicted;
    }
    if (keep_preloaded && VideoCommon::Nx2Perf::Verify()) {
        // Eviction only frees driver memory, so there is no wrong answer to fall back from.
        LOG_INFO(Render_Vulkan,
                 "Nx2Perf pipeline_keep_preloaded: old rule would evict {}, kept {} never-used",
                 evicted + kept, kept);
    }
    if (evicted == 0) {
        return;
    }
    for (auto& [key, pipeline] : graphics_cache) {
        if (pipeline) {
            pipeline->ClearTransitions();
        }
    }
    graphics_fast_cache.fill(nullptr);
    skipped_geometry = {};
    last_skipped_geometry_frame = ~u64{};
    LOG_INFO(Render_Vulkan, "Evicted {} graphics pipelines unused for {} frames, {} left", evicted,
             EvictAfterFrames, graphics_cache.size());
}

void PipelineCache::StartShaderPrecache(u64 title_id) {
    namespace ShaderWarm = VideoCommon::ShaderWarm;
    std::function<void()> write_marker = [] {};
    std::unordered_map<ComputePipelineCacheKey, std::vector<FileEnvironment>> programs;
    size_t entries = 0;
    const auto reject = [this](CE::RejectReason reason, u64 found) {
        const CE::CacheLoadResult result{.accepted = false,
                                         .reason = reason,
                                         .kind = CE::PayloadKind::WarmCompute,
                                         .expected = CACHE_VERSION,
                                         .found = found};
        LOG_WARNING(Render_Vulkan, "{}", CE::CacheRejectLine(result));
        if (cache_session) {
            cache_session->extra_reports.push_back({CE::PayloadKind::WarmCompute, result});
            PublishSession(*cache_session);
        }
    };
    const auto load_warm_file = [&] {
        const std::filesystem::path warm_file = ShaderWarm::ComputeWarmPath(title_id);
        std::error_code ec;
        if (!std::filesystem::exists(warm_file, ec)) {
            return;
        }
        const std::string path = Common::FS::PathToUTF8String(warm_file);
        const u32 driver = device.GetDriverVersion();
        const u32 device_id = device.GetDeviceID();
        const auto marker = ShaderWarm::ReadWarmMarker(warm_file);
        const auto current =
            ShaderWarm::CurrentWarmStamp(warm_file, 0, driver, device_id, CACHE_VERSION);
        const bool done = current && marker && marker->Matches(*current);
        size_t warm_entries = 0;
        try {
            std::ifstream file(warm_file, std::ios::binary | std::ios::ate);
            file.exceptions(std::ifstream::failbit);
            const auto end{file.tellg()};
            file.seekg(0, std::ios::beg);
            std::array<char, 8> magic;
            u32 version{};
            file.read(magic.data(), magic.size())
                .read(reinterpret_cast<char*>(&version), sizeof(version));
            constexpr std::array<char, 8> expected_magic{'y', 'u', 'z', 'u', 'c', 'a', 'c', 'h'};
            if (magic != expected_magic) {
                reject(CE::RejectReason::BadMagic, 0);
                return;
            }
            if (version != CACHE_VERSION) {
                // Not read: pre-compiling the game's shaders again writes a current file.
                reject(CE::RejectReason::FormatVersion, version);
                return;
            }
            while (file.tellg() != end) {
                u32 num_envs{};
                file.read(reinterpret_cast<char*>(&num_envs), sizeof(num_envs));
                FileEnvironment env;
                env.Deserialize(file);
                if (num_envs != 1 || env.ShaderStage() != Shader::Stage::Compute) {
                    throw std::runtime_error("unexpected entry");
                }
                ComputePipelineCacheKey key;
                file.read(reinterpret_cast<char*>(&key), sizeof(key));
                ++warm_entries;
                precache_keys.insert(key.Hash());
                if (!done && !programs.contains(key)) {
                    static const VideoCommon::GuessMissing guess{};
                    env.SetGuessMissing(&guess);
                    programs[key].push_back(std::move(env));
                }
            }
        } catch (const std::exception& e) {
            LOG_WARNING(Render_Vulkan, "Shader precache: reading {} failed: {}", path, e.what());
            precache_keys.clear();
            programs.clear();
            reject(CE::RejectReason::BadRecord, 0);
            return;
        }
        LOG_INFO(Render_Vulkan, "Shader precache: {} compute programs", warm_entries);
        entries += warm_entries;
        if (done) {
            LOG_INFO(Render_Vulkan, "Shader precache: already built for this driver");
            return;
        }
        write_marker = [warm_file, driver, device_id, warm_entries] {
            if (const auto stamp = VideoCommon::ShaderWarm::CurrentWarmStamp(
                    warm_file, warm_entries, driver, device_id, CACHE_VERSION)) {
                VideoCommon::ShaderWarm::WriteWarmMarker(warm_file, *stamp);
            }
        };
    };
    load_warm_file();
    if (programs.empty()) {
        write_marker();
        return;
    }
    const size_t threads = BackgroundPipelineThreads();
    LOG_INFO(Render_Vulkan,
             "Shader precache: building {} of {} compute programs in the background ({} threads)",
             programs.size(), entries, threads);
    precache_left.store(programs.size(), std::memory_order_relaxed);
    precache_start = std::chrono::steady_clock::now();
    for (const auto& [key, env] : programs) {
        precache_states.try_emplace(key, PrecacheState::Queued);
        precache_keys.insert(key.Hash());
    }
    precache_workers.reset(new Common::ThreadWorker(threads, "VkShaderPrecache", {},
                                                    Common::ThreadPlacement::Background));
    for (auto& [key, environments] : programs) {
        precache_workers->QueueWork(
            [this, key, envs = std::move(environments), write_marker]() mutable {
            WaitForIdleRuntimeBuilds();
            if (precache_stop.load(std::memory_order_relaxed)) {
                return;
            }
            auto& state{precache_states.at(key)};
            PrecacheState expected{PrecacheState::Queued};
            if (state.compare_exchange_strong(expected, PrecacheState::Building)) {
                try {
                    ShaderPools pools;
                    const DriverBuildReport::PrecacheScope precache_scope;
                    const Spikes::SpanScope build_span{Spikes::Cause::Pipeline};
                    for (auto& env : envs) {
                        pools.ReleaseContents();
                        if (CreateComputePipeline(pools, key, env, nullptr, false).pipeline) {
                            precache_built.fetch_add(1, std::memory_order_relaxed);
                            if (!VideoCommon::Nx2Perf::On(
                                    VideoCommon::Nx2Perf::Opt::ComputePreloadVariants)) {
                                break;
                            }
                        }
                    }
                } catch (const std::exception& e) {
                    LOG_DEBUG(Render_Vulkan, "Shader precache: {}", e.what());
                }
                {
                    std::scoped_lock lock{precache_mutex};
                    state.store(PrecacheState::Done);
                }
                precache_condvar.notify_all();
            }
            if (precache_left.fetch_sub(1, std::memory_order_acq_rel) == 1 &&
                !precache_stop.load(std::memory_order_relaxed)) {
                write_marker();
                LOG_INFO(Render_Vulkan,
                         "Shader precache: {} compute programs built in {:.1f} s ({:.1f} s of it "
                         "waiting for the game's own builds); {} left to dispatches that needed "
                         "them first, {} in progress when a dispatch needed them",
                         precache_built.load(std::memory_order_relaxed),
                         std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                                       precache_start)
                             .count(),
                         precache_paused_us.load(std::memory_order_relaxed) / 1e6,
                         precache_claimed.load(std::memory_order_relaxed),
                         precache_waited.load(std::memory_order_relaxed));
            }
        });
    }
}

bool PipelineCache::RuntimeBuildsBusy() const {
    // Long enough to cover the gaps between a loading burst's pipelines.
    constexpr std::chrono::milliseconds Quiet{250};
    const auto last = std::chrono::steady_clock::time_point{
        std::chrono::steady_clock::duration{last_runtime_build.load(std::memory_order_relaxed)}};
    return (translation_workers && translation_workers->PendingWork() != 0) ||
           workers.PendingWork() != 0 || std::chrono::steady_clock::now() - last < Quiet;
}

bool PipelineCache::ShaderBuildsBusy() const {
    return RuntimeBuildsBusy() || disk_replay_workers.PendingWork() != 0;
}

void PipelineCache::WaitForIdleRuntimeBuilds() {
    const auto begin = std::chrono::steady_clock::now();
    bool paused = false;
    while (!precache_stop.load(std::memory_order_relaxed) && RuntimeBuildsBusy()) {
        paused = true;
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    if (paused) {
        precache_paused_us.fetch_add(std::chrono::duration_cast<std::chrono::microseconds>(
                                         std::chrono::steady_clock::now() - begin)
                                         .count(),
                                     std::memory_order_relaxed);
    }
}

void PipelineCache::ClaimPrecacheProgram(const ComputePipelineCacheKey& key) {
    const auto it{precache_states.find(key)};
    if (it == precache_states.end()) {
        return;
    }
    auto& state{it->second};
    PrecacheState expected{PrecacheState::Queued};
    if (state.compare_exchange_strong(expected, PrecacheState::Claimed)) {
        precache_claimed.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (expected != PrecacheState::Building) {
        return;
    }
    // Ordinary warm-file entries keep a bounded wait before normal first-use creation.
    precache_waited.fetch_add(1, std::memory_order_relaxed);
    std::unique_lock lock{precache_mutex};
    Nx2Bottleneck::WaitScope build_wait{Nx2Bottleneck::Wait::ShaderBuild};
    const Spikes::SpanScope wait_span{Spikes::Cause::Waiting};
    precache_condvar.wait_for(lock, std::chrono::milliseconds{250},
                              [&] { return state.load() != PrecacheState::Building; });
}

void PipelineCache::StopShaderPrecache() {
    precache_stop.store(true, std::memory_order_relaxed);
    // Joins the threads: a build in progress finishes, queued ones are dropped.
    precache_workers.reset();
}

GraphicsPipeline* PipelineCache::BuiltPipeline(GraphicsPipeline* pipeline,
                                               bool is_indexed) noexcept {
    if (pipeline->IsBuilt()) {
        if (ShouldSkipDepthOnlyGeometry(is_indexed)) {
            graphics_skip = DriverBuildReport::SkipReason::GeometryConsistency;
            return nullptr;
        }
        ClearSkippedGeometryIfDrawn(is_indexed);
        return pipeline;
    }
    if (!use_asynchronous_shaders || pipeline->IsMeshPipeline()) {
        ClearSkippedGeometryIfDrawn(is_indexed);
        return pipeline;
    }
    // Native draws can produce terrain data used only once. Submit them after compilation
    // instead of leaving permanent holes when their first use misses the shader cache.
    // Depth-tested draws into a target first written in an earlier frame are skipped as for other
    // titles: the title redraws that target every frame, so the draw is back once the pipeline is
    // built. Waiting for those held Mario Kart World's frames 50-330 ms per new pipeline.
    // EDEN_NX2_NATIVE_ASYNC=0 waits for every native draw again.
    if (maxwell3d->GetNativeMacroEngine().native_turing) {
        static const bool native_async = [] {
            const char* value = std::getenv("EDEN_NX2_NATIVE_ASYNC");
            return value == nullptr || value[0] != '0';
        }();
        const auto& regs = maxwell3d->regs;
        // Depth test on (a bound depth buffer alone doesn't test) and a color target written:
        // draws with every color write masked are depth-only and keep waiting.
        if (!native_async || !regs.zeta_enable || !regs.depth_test_enable ||
            ActiveColorTargetCount(regs) == 0 ||
            !texture_cache.ColorTargetWrittenBeforeThisFrame()) {
            return pipeline;
        }
    }
    // If something is using depth, we can assume that games are not rendering anything which
    // will be used one time.
    if (maxwell3d->regs.zeta_enable) {
        RecordSkippedGeometry(is_indexed, pipeline);
        graphics_skip = DriverBuildReport::SkipReason::AsyncBuild;
        return nullptr;
    }
    // If games are using a small index count, we can assume these are full screen quads.
    // Usually these shaders are only used once for building textures so we can assume they
    // can't be built async unless the target already holds an earlier frame's output.
    const auto& draw_state = maxwell3d->draw_manager.draw_state;
    const bool stale_skip = VideoCommon::Nx2Perf::On(VideoCommon::Nx2Perf::Opt::StaleTargetSkip);
    const bool is_small_draw =
        stale_skip ? (draw_state.draw_element_count > 0 && draw_state.draw_element_count <= 6)
                   : (draw_state.draw_element_count <= 6);
    if (is_small_draw) {
        if (stale_skip && texture_cache.ColorTargetHasPriorFrameOutput()) {
            graphics_skip = DriverBuildReport::SkipReason::AsyncBuildStale;
            return nullptr;
        }
        return pipeline;
    }
    // Switch 2 titles also draw things they keep with more vertices and no depth buffer, such as
    // the glyphs of a font atlas, each drawn once: skipping one lost it for the session (Star
    // Fox's menus lost their most common letters). Their cached pipelines are all built at boot,
    // so only new ones reach this: wait for those unless the draw is depth-tested.
    if (Settings::CurrentProgramIsSwitch2()) {
        return pipeline;
    }
    graphics_skip = DriverBuildReport::SkipReason::AsyncBuild;
    return nullptr;
}

bool PipelineCache::CanSkipDraw() noexcept {
    // The rules BuiltPipeline and DeferRuntimeBuild apply, for a pipeline still translating.
    const auto& regs = maxwell3d->regs;
    if (regs.IsMeshEnabled() || regs.zeta_enable) {
        return true;
    }
    // Small draws are full screen quads, often used once to build a texture: wait for them
    // unless their target already holds an earlier frame's output.
    const auto& draw_state = maxwell3d->draw_manager.draw_state;
    const bool stale_skip = VideoCommon::Nx2Perf::On(VideoCommon::Nx2Perf::Opt::StaleTargetSkip);
    const bool is_small_draw =
        stale_skip ? (draw_state.draw_element_count > 0 && draw_state.draw_element_count <= 6)
                   : (draw_state.draw_element_count <= 6);
    if (is_small_draw) {
        return stale_skip && texture_cache.ColorTargetHasPriorFrameOutput();
    }
    // Switch 2 titles draw things they keep without depth, such as font glyphs (BuiltPipeline).
    return !Settings::CurrentProgramIsSwitch2();
}

bool PipelineCache::BuildNewGraphicsPipeline(bool is_indexed) {
    if (const auto pending{translating_graphics.find(graphics_key)};
        pending != translating_graphics.end()) {
        // An earlier draw queued it. A draw that can't be skipped needs it now.
        const QueuedGraphicsTranslation queued{pending->second};
        if (CanSkipDraw()) {
            if (!WaitForGraphicsTranslation(queued.done)) {
                RecordSkippedGeometry(is_indexed, nullptr);
                graphics_skip = DriverBuildReport::SkipReason::AsyncTranslation;
                return false;
            }
        } else {
            FinishGraphicsTranslation(queued);
        }
        AdoptTranslatedGraphicsPipelines();
        if (graphics_cache.contains(graphics_key)) {
            return true;
        }
        graphics_skip = DriverBuildReport::SkipReason::TranslationMissing;
        return false;
    }
    // A pipeline the cache file recorded and the preparation has built (or is building) is
    // adopted here, never rebuilt. One relaxed load while preparation is not running.
    if (prepare_active.load(std::memory_order_relaxed) && TakePreparedGraphics()) {
        return true;
    }
    if (DeferRuntimeBuild()) {
        RecordSkippedGeometry(is_indexed, nullptr);
        graphics_skip = DriverBuildReport::SkipReason::BuildDeferred;
        return false;
    }
    ReportNewPipelineKey(graphics_key);
    if (CanTranslateAsynchronously()) {
        if (const auto done{QueueGraphicsTranslation()}) {
            if (!WaitForGraphicsTranslation(*done)) {
                RecordSkippedGeometry(is_indexed, nullptr);
                graphics_skip = DriverBuildReport::SkipReason::AsyncTranslation;
                return false;
            }
            AdoptTranslatedGraphicsPipelines();
            if (graphics_cache.contains(graphics_key)) {
                return true;
            }
            graphics_skip = DriverBuildReport::SkipReason::TranslationMissing;
            return false;
        }
    }
    graphics_cache.emplace(graphics_key, CreateGraphicsPipeline());
    return true;
}

namespace {
// Where the time of a build goes: the GPU thread's share at run time (ns, summed per 10 s) and,
// for every build, the phases of the shader build report (DriverBuildReport).
enum class BuildPhase : size_t { Environments, Translate, Emit, Module, Object, Count };
std::array<std::atomic<u64>, static_cast<size_t>(BuildPhase::Count)> runtime_phase_ns{};

struct PhaseTimer {
    BuildPhase phase;
    bool active; ///< Also counts toward the GPU thread's run time figures
    std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();

    PhaseTimer(BuildPhase phase_, bool active_) : phase{phase_}, active{active_} {}

    ~PhaseTimer() {
        const auto elapsed = std::chrono::steady_clock::now() - begin;
        if (active) {
            runtime_phase_ns[static_cast<size_t>(phase)].fetch_add(
                static_cast<u64>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()),
                std::memory_order_relaxed);
        }
        // A pipeline object built inline includes its driver build, which is reported on its own.
        if (phase == BuildPhase::Object && !active) {
            return;
        }
        DriverBuildReport::AddPhase(ReportPhase(), elapsed);
    }

private:
    static Spikes::Cause CauseOf(BuildPhase phase) {
        return phase == BuildPhase::Object ? Spikes::Cause::Pipeline : Spikes::Cause::Shader;
    }

    Spikes::SpanScope span{CauseOf(phase)}; ///< Attribution of the same stretch

    DriverBuildReport::Phase ReportPhase() const {
        switch (phase) {
        case BuildPhase::Translate:
            return DriverBuildReport::Phase::Translate;
        case BuildPhase::Emit:
        case BuildPhase::Module:
            return DriverBuildReport::Phase::Spirv;
        default:
            return DriverBuildReport::Phase::Other;
        }
    }
};
} // Anonymous namespace

std::unique_ptr<GraphicsPipeline> PipelineCache::CreateGraphicsPipeline(
    ShaderPools& pools, const GraphicsPipelineCacheKey& key,
    std::span<Shader::Environment* const> envs, PipelineStatistics* statistics,
    bool build_in_parallel, bool native_turing, bool parallel_stages) try {
    const DriverBuildReport::BuildScope busy;
    const auto build_begin = std::chrono::steady_clock::now();
    auto hash = key.Hash();
    LOG_DEBUG(Render_Vulkan, "{:#016x}", hash);
    size_t env_index{0};
    std::array<Shader::IR::Program, Maxwell::MaxShaderProgram> programs;
    const bool uses_vertex_a{key.unique_hashes[0] != 0};
    const bool uses_vertex_b{key.unique_hashes[1] != 0};

    // Native stages translated before for other pipelines (VideoCommon::StageCache). A merged
    // VertexA/VertexB pair is always translated.
    using StageCache = VideoCommon::StageCache;
    const bool cache_stages{stage_cache.Enabled() && !uses_vertex_a};
    std::array<Shader::Environment*, Maxwell::MaxShaderProgram> stage_envs{};
    std::array<std::shared_ptr<const StageCache::Translation>, Maxwell::MaxShaderProgram> reused{};
    std::array<std::shared_ptr<StageCache::Translation>, Maxwell::MaxShaderProgram> recorded{};
    std::vector<std::pair<std::shared_ptr<StageCache::Translation>,
                          std::shared_ptr<const StageCache::Emitted>>>
        new_translations;

    // Detached environments (asynchronous translation): the stages the stage cache lacks are
    // translated at once, each with its own pools, the extra ones on translation workers.
    std::array<std::vector<u8>, Maxwell::MaxShaderProgram> signatures;
    std::array<bool, Maxwell::MaxShaderProgram> looked_up{};
    std::array<bool, Maxwell::MaxShaderProgram> translated_ahead{};
    std::array<std::unique_ptr<ShaderPools>, Maxwell::MaxShaderProgram> stage_pools;
    if (parallel_stages && cache_stages && translation_workers && ParallelStagesEnabled()) {
        boost::container::static_vector<size_t, Maxwell::MaxShaderProgram> missing;
        std::array<Shader::Environment*, Maxwell::MaxShaderProgram> ahead_envs{};
        size_t ahead_env_index{};
        for (size_t index = 0; index < Maxwell::MaxShaderProgram; ++index) {
            if (key.unique_hashes[index] == 0) {
                continue;
            }
            Shader::Environment& env{*envs[ahead_env_index++]};
            if (!env.IsNativeProgram()) {
                continue;
            }
            signatures[index] = StageCache::Signature(env, key.unique_hashes[index], index);
            reused[index] = stage_cache.Find(signatures[index], env);
            if (reused[index] && reused[index]->program.info.requires_layer_emulation) {
                reused[index] = nullptr;
            }
            looked_up[index] = true;
            if (!reused[index]) {
                missing.push_back(index);
                ahead_envs[index] = &env;
            }
        }
        if (missing.size() >= 2) {
            RunInParallel(missing.size(), [&](size_t task) {
                const DriverBuildReport::BuildScope task_busy;
                const PhaseTimer ahead_timer{BuildPhase::Translate, false};
                const size_t index{missing[task]};
                Shader::Environment& env{*ahead_envs[index]};
                stage_pools[index] = std::make_unique<ShaderPools>();
                ShaderPools& own{*stage_pools[index]};
                StageCache::Recorder recorder{env};
                const u32 cfg_offset{
                    static_cast<u32>(env.StartAddress() + sizeof(Shader::ProgramHeader))};
                Shader::Maxwell::Flow::CFG cfg(recorder, own.flow_block, cfg_offset, index == 0);
                programs[index] = TranslateProgram(own.inst, own.block, recorder, cfg, host_info);
                auto translation = std::make_shared<StageCache::Translation>();
                translation->signature = signatures[index];
                translation->reads = recorder.TakeReads();
                translation->program = StageCache::Summary(programs[index]);
                recorded[index] = std::move(translation);
            });
            for (const size_t index : missing) {
                translated_ahead[index] = true;
            }
            parallel_stage_pipelines.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // Layer passthrough generation for devices without VK_EXT_shader_viewport_index_layer
    Shader::IR::Program* layer_source_program{};
    bool mesh_unsupported{};

    std::optional<PhaseTimer> translate_timer{std::in_place, BuildPhase::Translate,
                                              build_in_parallel};
    for (size_t index = 0; index < Maxwell::MaxShaderProgram; ++index) {
        const bool is_emulated_stage = layer_source_program != nullptr &&
                                       index == static_cast<u32>(Maxwell::ShaderType::Geometry);
        if (key.unique_hashes[index] == 0 && is_emulated_stage) {
            auto topology = MaxwellToOutputTopology(key.state.topology);
            programs[index] = GenerateGeometryPassthrough(pools.inst, pools.block, host_info,
                                                          *layer_source_program, topology);
            continue;
        }
        if (key.unique_hashes[index] == 0) {
            continue;
        }
        Shader::Environment& env{*envs[env_index]};
        ++env_index;

        if (index == static_cast<size_t>(Maxwell::ShaderType::Geometry) &&
            !device.IsGeometryShaderSupported() && (native_turing || env.IsNativeProgram())) {
            // A native (Switch 2) geometry program on a device without geometry shaders: the
            // pipeline cannot run it, so its draws are skipped.
            static std::atomic<bool> logged{};
            if (!logged.exchange(true)) {
                LOG_WARNING(Render_Vulkan, "Skipping native geometry shader pipelines: the device "
                                           "has no geometry shaders");
            }
            return nullptr;
        }

        // Without VK_EXT_mesh_shader the driver cannot build a task/mesh pipeline; the draw is
        // skipped (see the pipeline check in the rasterizer). The stages are still translated
        // so the skipped pipeline's needs can be reported below.
        if ((env.ShaderStage() == Shader::Stage::Task ||
             env.ShaderStage() == Shader::Stage::Mesh) &&
            !device.IsExtMeshShaderSupported()) {
            static std::atomic<bool> logged{};
            if (!logged.exchange(true, std::memory_order_relaxed)) {
                LOG_WARNING(Render_Vulkan,
                            "{} has no VK_EXT_mesh_shader: task/mesh pipelines run through the "
                            "compute-based mesh emulation",
                            device.GetModelName());
            }
            mesh_unsupported = true;
        }

        const u32 cfg_offset{static_cast<u32>(env.StartAddress() + sizeof(Shader::ProgramHeader))};
        stage_envs[index] = &env;
        const bool cacheable{cache_stages && env.IsNativeProgram()};
        std::vector<u8> signature;
        if (cacheable && looked_up[index]) {
            signature = std::move(signatures[index]);
        } else if (cacheable) {
            signature = StageCache::Signature(env, key.unique_hashes[index], index);
            reused[index] = stage_cache.Find(signature, env);
            // A layer emulation source is needed with its IR (GenerateGeometryPassthrough).
            if (reused[index] && reused[index]->program.info.requires_layer_emulation) {
                reused[index] = nullptr;
            }
        }
        if (reused[index]) {
            programs[index] = reused[index]->program;
        } else if (translated_ahead[index]) {
            // programs[index] and recorded[index] came from the parallel translation above.
        } else if (cacheable) {
            StageCache::Recorder recorder{env};
            Shader::Maxwell::Flow::CFG cfg(recorder, pools.flow_block, cfg_offset, index == 0);
            programs[index] = TranslateProgram(pools.inst, pools.block, recorder, cfg, host_info);
            auto translation = std::make_shared<StageCache::Translation>();
            translation->signature = std::move(signature);
            translation->reads = recorder.TakeReads();
            translation->program = StageCache::Summary(programs[index]);
            recorded[index] = std::move(translation);
        } else if (!uses_vertex_a || index != 1) {
            // Normal path
            Shader::Maxwell::Flow::CFG cfg(env, pools.flow_block, cfg_offset, index == 0);
            programs[index] = TranslateProgram(pools.inst, pools.block, env, cfg, host_info);
        } else {
            Shader::Maxwell::Flow::CFG cfg(env, pools.flow_block, cfg_offset, index == 0);
            // VertexB path when VertexA is present.
            auto& program_va{programs[0]};
            auto program_vb{TranslateProgram(pools.inst, pools.block, env, cfg, host_info)};
            programs[index] = MergeDualVertexPrograms(program_va, program_vb, env, host_info);
        }

        if (device.GetDriverID() != VK_DRIVER_ID_NVIDIA_PROPRIETARY &&
            Shader::Optimization::HasBrokenPattern(programs[index])) {
            LOG_WARNING(Render_Vulkan, "0x{:016x}: Skipping shader with known issues",
                        key.unique_hashes[index]);
            return nullptr;
        }

        if (Settings::values.dump_guest_shaders) {
            env.Dump(hash, key.unique_hashes[index]);
        }

        if (programs[index].info.requires_layer_emulation) {
            layer_source_program = &programs[index];
        }
    }
    translate_timer.reset();
    if (host_info.max_descriptor_set_storage_buffers != 0) {
        size_t total_storage_buffers{};
        for (size_t index = uses_vertex_a && uses_vertex_b ? 1 : 0;
             index < Maxwell::MaxShaderProgram; ++index) {
            total_storage_buffers +=
                Shader::NumDescriptors(programs[index].info.storage_buffers_descriptors);
        }
        if (total_storage_buffers > host_info.max_descriptor_set_storage_buffers) {
            throw Shader::NotImplementedException(
                "Pipeline binds {} storage buffers; descriptor set limit is {}",
                total_storage_buffers, host_info.max_descriptor_set_storage_buffers);
        }
    }
    // Mesh emulation without VK_EXT_mesh_shader (prototype): a mesh program, after an optional
    // task program, drawing triangles without per-primitive outputs, before a pixel program only.
    std::optional<MeshEmulationSetup> mesh_emulation;
    size_t mesh_index{};
    if (mesh_unsupported) {
        ReportUnsupportedMeshPipeline(key, programs);
        const bool has_task{key.unique_hashes[1] != 0 && programs[1].stage == Shader::Stage::Task};
        for (size_t index = 1; index < Maxwell::MaxShaderProgram; ++index) {
            if (key.unique_hashes[index] != 0 && programs[index].stage == Shader::Stage::Mesh) {
                mesh_index = index;
                break;
            }
        }
        bool eligible{mesh_index != 0 && (mesh_index == 1 || has_task) &&
                      layer_source_program == nullptr};
        const Shader::IR::Program& mesh{programs[mesh_index]};
        // Points (0), lines (1) and triangles (4).
        const u32 topology{mesh.mesh.topology};
        eligible = eligible && (topology == 0 || topology == 1 || topology == 4) &&
                   std::ranges::all_of(mesh.mesh.primitive_lines, [](u32 line) {
                       return Shader::IR::IsGeneric(static_cast<Shader::IR::Attribute>(line));
                   }) &&
                   !mesh.mesh.vertex_lines.empty() && mesh.mesh.max_vertices != 0 &&
                   mesh.mesh.max_primitives != 0;
        for (const u32 line : mesh.mesh.primitive_lines) {
            eligible = eligible && std::ranges::find(mesh.mesh.vertex_lines, line) ==
                                       mesh.mesh.vertex_lines.end();
        }
        for (size_t index = 1; index < Maxwell::MaxShaderProgram && eligible; ++index) {
            const bool emulated{index == mesh_index || (has_task && index == 1)};
            eligible = emulated || key.unique_hashes[index] == 0 ||
                       programs[index].stage == Shader::Stage::Fragment;
        }
        if (!eligible) {
            return nullptr;
        }
        // The output buffer's binding follows every program's (unified bindings, one per
        // descriptor element, as the SPIR-V backend numbers them). The task and mesh programs
        // share the compute stage, so their descriptors and the emulation's own buffers count
        // against one set of per-stage limits.
        u32 output_binding{};
        u32 compute_storage_buffers{has_task ? 4U : 1U};
        u32 compute_sampled_images{};
        u32 compute_resources{compute_storage_buffers};
        for (size_t index = 1; index < Maxwell::MaxShaderProgram; ++index) {
            if (key.unique_hashes[index] == 0) {
                continue;
            }
            const Shader::Info& info{programs[index].info};
            const bool emulated{index == mesh_index || (has_task && index == 1)};
            const auto add = [&](const auto& descriptors, u32* category) {
                const u32 count{Shader::NumDescriptors(descriptors)};
                output_binding += count;
                if (emulated) {
                    compute_resources += count;
                    if (category) {
                        *category += count;
                    }
                }
            };
            add(info.constant_buffer_descriptors, nullptr);
            add(info.storage_buffers_descriptors, &compute_storage_buffers);
            add(info.texture_buffer_descriptors, &compute_sampled_images);
            add(info.image_buffer_descriptors, nullptr);
            add(info.texture_descriptors, &compute_sampled_images);
            add(info.image_descriptors, nullptr);
        }
        if (compute_storage_buffers > device.GetMaxPerStageDescriptorStorageBuffers() ||
            compute_sampled_images > device.GetMaxPerStageDescriptorSampledImages() ||
            compute_resources > device.GetMaxPerStageResources()) {
            LOG_WARNING(Render_Vulkan,
                        "Mesh emulation skipped: {} storage buffers, {} sampled images and {} "
                        "resources exceed the compute stage limits",
                        compute_storage_buffers, compute_sampled_images, compute_resources);
            return nullptr;
        }
        mesh_emulation.emplace();
        mesh_emulation->output_binding = output_binding;
        if (has_task) {
            // After the output: the task payload, offset and argument buffers.
            mesh_emulation->has_task = true;
            mesh_emulation->mesh_stage = static_cast<u32>(mesh_index - 1);
            mesh_emulation->payload_stride = (std::max)(programs[1].mesh.payload_words, 2U);
            mesh_emulation->payload_binding = output_binding + 1;
            mesh_emulation->offsets_binding = output_binding + 2;
            mesh_emulation->args_binding = output_binding + 3;
            mesh_emulation->distribution_spirv =
                Shader::Backend::SPIRV::EmitMeshEmulationDistribution(profile);
        }
        mesh_emulation->block_words = Shader::Backend::SPIRV::MeshEmulationBlockWords(mesh.mesh);
        mesh_emulation->max_primitives = mesh.mesh.max_primitives;
        mesh_emulation->primitive_vertices = mesh.mesh.PrimitiveVertices();
        mesh_emulation->max_vertices = mesh.mesh.max_vertices;
        mesh_emulation->vertex_lines = static_cast<u32>(mesh.mesh.vertex_lines.size());
        for (u32 line = 0; line < mesh.mesh.vertex_lines.size(); ++line) {
            const u32 attribute{mesh.mesh.vertex_lines[line]};
            const u32 position_x{static_cast<u32>(Shader::IR::Attribute::PositionX)};
            if (attribute >= position_x && attribute < position_x + 4) {
                mesh_emulation->position_lines[attribute - position_x] = line;
            }
        }
        static std::atomic<u32> emulated{};
        if (const u32 count{++emulated}; count <= 64) {
            // The input region addresses the mesh program reads (0 is its workgroup index).
            std::string reads;
            for (const Shader::IR::Block* block : mesh.blocks) {
                for (const Shader::IR::Inst& inst : block->Instructions()) {
                    if (inst.GetOpcode() != Shader::IR::Opcode::IsbeRead) {
                        continue;
                    }
                    const Shader::IR::Value address{inst.Arg(0)};
                    reads += address.IsImmediate() ? fmt::format(" {:#x}", address.U32())
                                                   : std::string{" dynamic"};
                }
            }
            LOG_INFO(Render_Vulkan,
                     "Mesh emulation #{}: pipeline {:016x}, {} vertices x {} lines, {} triangles "
                     "per workgroup, output binding {}, task {} (payload {} words), input "
                     "reads:{}",
                     count, hash, mesh.mesh.max_vertices, mesh.mesh.vertex_lines.size(),
                     mesh.mesh.max_primitives, output_binding, has_task,
                     mesh_emulation->payload_stride, reads.empty() ? " none" : reads);
        }
    }
    // A tessellation pipeline the driver would refuse is skipped like a missing mesh pipeline:
    // the draw is dropped (see the pipeline check in the rasterizer) instead of failing the build.
    {
        const Shader::IR::Program* control{};
        const Shader::IR::Program* evaluation{};
        for (size_t index = 0; index < Maxwell::MaxShaderProgram; ++index) {
            if (key.unique_hashes[index] == 0) {
                continue;
            }
            if (programs[index].stage == Shader::Stage::TessellationControl) {
                control = &programs[index];
            } else if (programs[index].stage == Shader::Stage::TessellationEval) {
                evaluation = &programs[index];
            }
        }
        if (control != nullptr || evaluation != nullptr) {
            const u32 max_patch_size = device.GetMaxTessellationPatchSize();
            const char* reason = nullptr;
            if (!device.IsTessellationSupported()) {
                reason = "the device has no tessellation shaders";
            } else if (control == nullptr || evaluation == nullptr) {
                reason = "it has only one of the control and evaluation stages";
            } else if (control->invocations == 0 || control->invocations > max_patch_size) {
                reason = "the control shader's output vertex count is out of range";
            }
            if (reason != nullptr) {
                LOG_WARNING(Render_Vulkan,
                            "Skipping tessellation pipeline: {} (tcs={} tes={}, tcs output "
                            "vertices {}, patch control points {}, device max patch size {})",
                            reason, control != nullptr, evaluation != nullptr,
                            control != nullptr ? control->invocations : 0,
                            key.state.patch_control_points_minus_one.Value() + 1, max_patch_size);
                return nullptr;
            }
        }
    }
    std::array<const Shader::Info*, Maxwell::MaxShaderStage> infos{};
    std::array<vk::ShaderModule, Maxwell::MaxShaderStage> modules;

    const Shader::IR::Program* previous_stage{};
    Shader::Backend::Bindings binding;
    for (size_t index = uses_vertex_a && uses_vertex_b ? 1 : 0; index < Maxwell::MaxShaderProgram;
         ++index) {
        const bool is_emulated_stage = layer_source_program != nullptr &&
                                       index == static_cast<u32>(Maxwell::ShaderType::Geometry);
        if (key.unique_hashes[index] == 0 && !is_emulated_stage) {
            continue;
        }
        UNIMPLEMENTED_IF(index == 0);

        Shader::IR::Program& program{programs[index]};
        const size_t stage_index{index - 1};
        infos[stage_index] = &program.info;

        std::optional<PhaseTimer> emit_timer{std::in_place, BuildPhase::Emit, build_in_parallel};
        auto runtime_info{MakeRuntimeInfo(programs, key, program, previous_stage, device)};
        // Mesh emulation and geometry output pruning shape the stage per pipeline beyond what the
        // runtime key covers: those are emitted again every time.
        const bool emulated_mesh_stage =
            mesh_emulation &&
            (program.stage == Shader::Stage::Mesh || program.stage == Shader::Stage::Task);
        const bool pruned_geometry = index == static_cast<size_t>(Maxwell::ShaderType::Geometry);
        const StageCache::Translation* const translation{
            emulated_mesh_stage || pruned_geometry
                ? nullptr
                : (reused[index] ? reused[index].get() : recorded[index].get())};
        std::vector<u8> runtime_key;
        std::shared_ptr<const StageCache::Emitted> emitted;
        if (translation) {
            runtime_key = StageCache::RuntimeKey(runtime_info, binding);
            if (reused[index]) {
                emitted = stage_cache.FindEmitted(*reused[index], runtime_key);
            }
        }
        std::vector<u32> code;
        if (emitted && !stage_cache.Verify()) {
            code = emitted->spirv;
            program = emitted->program;
            binding = emitted->bindings_out;
            stage_cache.CountReuse(true);
        } else {
            if (reused[index]) {
                // Emitted for other runtime info only, or verifying: the IR is needed again.
                emit_timer.reset();
                const PhaseTimer retranslate_timer{BuildPhase::Translate, false};
                Shader::Environment& env{*stage_envs[index]};
                const u32 cfg_offset{
                    static_cast<u32>(env.StartAddress() + sizeof(Shader::ProgramHeader))};
                Shader::Maxwell::Flow::CFG cfg(env, pools.flow_block, cfg_offset, index == 0);
                program = TranslateProgram(pools.inst, pools.block, env, cfg, host_info);
                emit_timer.emplace(BuildPhase::Emit, build_in_parallel);
            }
            if (mesh_emulation &&
                (program.stage == Shader::Stage::Mesh || program.stage == Shader::Stage::Task)) {
                runtime_info.mesh_as_compute = true;
                runtime_info.mesh_output_binding = mesh_emulation->output_binding;
                if (mesh_emulation->has_task) {
                    runtime_info.mesh_task_payload_binding = mesh_emulation->payload_binding;
                    runtime_info.mesh_task_offsets_binding = mesh_emulation->offsets_binding;
                    runtime_info.mesh_task_args_binding = mesh_emulation->args_binding;
                    runtime_info.mesh_task_payload_stride = mesh_emulation->payload_stride;
                }
                if (program.stage == Shader::Stage::Mesh) {
                    mesh_emulation->vertex_spirv = Shader::Backend::SPIRV::EmitMeshEmulationVertex(
                        profile, program.mesh, mesh_emulation->output_binding,
                        mesh_emulation->has_task ? mesh_emulation->args_binding : ~0U,
                        runtime_info.convert_depth_mode && !profile.support_native_ndc);
                }
            }
            ConvertLegacyToGeneric(program, runtime_info);
            // EDEN_NX2_MESH_EMULATION_DUMP=<dir>: write the emulation's vertex shader for spirv-val.
            if (mesh_emulation && program.stage == Shader::Stage::Mesh) {
                if (const char* const dump{std::getenv("EDEN_NX2_MESH_EMULATION_DUMP")}) {
                    const auto& vertex_code{mesh_emulation->vertex_spirv};
                    std::ofstream file{fmt::format("{}/{:016x}.vert.spv", dump, hash),
                                       std::ios::binary};
                    file.write(reinterpret_cast<const char*>(vertex_code.data()),
                               static_cast<std::streamsize>(vertex_code.size() * sizeof(u32)));
                }
            }
            if (index == static_cast<size_t>(Maxwell::ShaderType::Geometry)) {
                PruneUnreadGeometryOutputs(program,
                                           programs[static_cast<size_t>(Maxwell::ShaderType::Pixel)],
                                           key, device.IsNvGeometryShaderPassthroughSupported());
            }
            code = EmitSPIRV(profile, runtime_info, program, binding);
            if (emitted) {
                // The metadata too: it says where descriptors are read from at bind time.
                if (code != emitted->spirv ||
                    std::memcmp(&binding, &emitted->bindings_out, sizeof(binding)) != 0 ||
                    !StageCache::SameSummary(program, emitted->program)) {
                    stage_cache.CountVerifyMismatch(key.unique_hashes[index]);
                }
                stage_cache.CountReuse(true);
            } else if (translation) {
                auto entry = std::make_shared<StageCache::Emitted>();
                entry->runtime_key = std::move(runtime_key);
                entry->spirv = code;
                entry->program = StageCache::Summary(program);
                entry->bindings_out = binding;
                if (reused[index]) {
                    stage_cache.AddEmitted(*reused[index], std::move(entry));
                    stage_cache.CountReuse(false);
                } else {
                    new_translations.emplace_back(recorded[index], std::move(entry));
                }
            }
        }
        device.SaveShader(code);
        emit_timer.reset();
        {
            PhaseTimer module_timer{BuildPhase::Module, build_in_parallel};
            modules[stage_index] = BuildShader(device, code);
        }

        // Text log + .spv dump. Text log is gated by gpu_log_level != Off; .spv dump
        // is independent and gated only by gpu_log_shader_dumps.
        const bool should_log = GPU::Logging::IsActive();
        const bool should_dump = Settings::values.gpu_log_shader_dumps.GetValue();
        if (should_log || should_dump) {
            static constexpr std::array stage_names{"vertex", "tess_control", "tess_eval",
                                                    "geometry", "fragment"};
            const std::string shader_name = fmt::format(
                "shader_{:016x}_{}", key.unique_hashes[index], stage_names[stage_index]);
            if (should_log) {
                const std::string shader_info =
                    fmt::format("SPIR-V size: {} bytes, hash: {:016x}", code.size() * sizeof(u32),
                                key.unique_hashes[index]);
                GPU::Logging::GPULogger::GetInstance().LogShaderCompilation(shader_name,
                                                                            shader_info);
            }
            if (should_dump) {
                GPU::Logging::DumpSpirvShader(key.unique_hashes[index],
                                              std::span<const u32>(code.data(), code.size()));
            }
        }

        if (device.HasDebuggingToolAttached()) {
            const std::string name{fmt::format("Shader {:016x}", key.unique_hashes[index])};
            modules[stage_index].SetObjectNameEXT(name.c_str());
        }
        previous_stage = &program;
    }
    for (auto& [new_translation, entry] : new_translations) {
        stage_cache.Insert(std::move(new_translation), std::move(entry));
    }
    DriverBuildReport::NoteStart(hash);
    DriverBuildReport::SetFrontEnd(build_begin);
    Common::ThreadWorker* const thread_worker{build_in_parallel ? &workers : nullptr};
    PhaseTimer object_timer{BuildPhase::Object, build_in_parallel};
    return std::make_unique<GraphicsPipeline>(
        scheduler, buffer_cache, texture_cache, vulkan_pipeline_cache, &shader_notify, device,
        descriptor_pool, guest_descriptor_queue, descriptor_buffer_ring, thread_worker, statistics,
        render_pass_cache, key, std::move(modules), infos,
        std::function<
            std::array<vk::ShaderModule, Tegra::Engines::Maxwell3D::Regs::MaxShaderStage>()>{},
        std::move(mesh_emulation));

} catch (const Shader::Exception& exception) {
    auto hash = key.Hash();
    size_t env_index{0};
    for (size_t index = 0; index < Maxwell::MaxShaderProgram; ++index) {
        if (key.unique_hashes[index] == 0) {
            continue;
        }
        Shader::Environment& env{*envs[env_index]};
        ++env_index;

        const u32 cfg_offset{static_cast<u32>(env.StartAddress() + sizeof(Shader::ProgramHeader))};
        try {
            // Walks the program for the dump; the one that failed may fail here again, and an
            // exception leaving this handler would end the process.
            Shader::Maxwell::Flow::CFG cfg(env, pools.flow_block, cfg_offset, index == 0);
        } catch (const Shader::Exception&) {
        }
        env.Dump(hash, key.unique_hashes[index]);
    }
    LOG_ERROR(Render_Vulkan, "{}", exception.what());
    return nullptr;
}

namespace {
// Pipelines built on the GPU thread at run time (shader translation; the driver compile is on
// workers): every one over 2 ms is logged, and the total every 10 s, to tell how much of the
// stutter on new content is translation.
void ReportRuntimePipeline(const char* kind, std::chrono::steady_clock::time_point begin,
                           bool after_eviction, bool from_disk) {
    static std::chrono::steady_clock::duration total{};
    static u32 count{}, logged{}, rebuilt{}, on_disk{}, true_misses{};
    static auto window = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    const auto elapsed = now - begin;
    DriverBuildReport::NoteGpuBuild(elapsed);
    VideoCommon::Nx2Spikes::Note(VideoCommon::Nx2Spikes::Event::RuntimePipeline, elapsed);
    total += elapsed;
    ++count;
    rebuilt += after_eviction ? 1 : 0;
    on_disk += from_disk ? 1 : 0;
    true_misses += (!after_eviction && !from_disk) ? 1 : 0;
    const auto ms = [](auto d) { return std::chrono::duration<double, std::milli>(d).count(); };
    if (elapsed > std::chrono::milliseconds{2} && logged < 400) {
        ++logged;
        LOG_INFO(Render_Vulkan, "{} pipeline built on the GPU thread in {:.1f} ms{}", kind,
                 ms(elapsed),
                 after_eviction ? " (after eviction)"
                 : from_disk    ? " (in disk cache, skipped at boot)"
                                : " (new miss)");
    }
    if (now - window > std::chrono::seconds{10}) {
        const auto phase_ms = [](BuildPhase phase) {
            return static_cast<double>(runtime_phase_ns[static_cast<size_t>(phase)].exchange(0)) /
                   1e6;
        };
        const double env_ms = phase_ms(BuildPhase::Environments);
        const double translate_ms = phase_ms(BuildPhase::Translate);
        const double emit_ms = phase_ms(BuildPhase::Emit);
        const double module_ms = phase_ms(BuildPhase::Module);
        const double object_ms = phase_ms(BuildPhase::Object);
        LOG_INFO(Render_Vulkan,
                 "Runtime pipelines in the last 10 s: {} built ({} new misses, {} in disk cache, "
                 "{} rebuilt after eviction), {:.1f} ms on the GPU thread (graphics: environments "
                 "{:.1f}, translate {:.1f}, SPIR-V {:.1f}, modules {:.1f}, pipeline object {:.1f} "
                 "ms)",
                 count, true_misses, on_disk, rebuilt, ms(total), env_ms, translate_ms, emit_ms,
                 module_ms, object_ms);
        total = {};
        count = 0;
        rebuilt = 0;
        on_disk = 0;
        true_misses = 0;
        window = now;
    }
}
} // Anonymous namespace

std::unique_ptr<GraphicsPipeline> PipelineCache::CreateGraphicsPipeline() {
    const auto begin = std::chrono::steady_clock::now();
    const u64 key_hash = graphics_key.Hash();
    const bool after_eviction = evicted_keys.contains(key_hash);
    // The last demand found this key's record in the cache file: the file has it already.
    const bool in_file = std::exchange(demanded_hash, 0) == key_hash;
    const bool from_disk = !after_eviction && in_file;
    SCOPE_EXIT {
        ReportRuntimePipeline("Graphics", begin, after_eviction, from_disk);
        ChargeRuntimeBuild(begin);
        last_runtime_build.store(std::chrono::steady_clock::now().time_since_epoch().count(),
                                 std::memory_order_relaxed);
    };
    const bool native_turing =
        maxwell3d != nullptr && maxwell3d->GetNativeMacroEngine().native_turing;
    GraphicsEnvironments environments;
    {
        PhaseTimer env_timer{BuildPhase::Environments, true};
        GetGraphicsEnvironments(environments, graphics_key.unique_hashes);
    }

    main_pools.ReleaseContents();
    auto pipeline{CreateGraphicsPipeline(main_pools, graphics_key, environments.Span(), nullptr,
                                         true, native_turing)};
    if (pipeline) {
        if (in_file) {
            QueueVulkanPipelineCacheFlush();
        } else {
            StoreGraphicsPipeline(graphics_key, std::move(environments.envs),
                                  pipeline->IsMeshPipeline(),
                                  std::chrono::steady_clock::now() - begin);
        }
    }
    return pipeline;
}

void PipelineCache::StoreGraphicsPipeline(const GraphicsPipelineCacheKey& key,
                                          GraphicsEnvironmentArray envs, bool is_mesh,
                                          std::chrono::steady_clock::duration build) {
    if (!cache_session) {
        return;
    }
    // Task and mesh programs are translated with the mesh registers, which the main file doesn't
    // carry: they go to their own file (builds without it never read those records).
    if (ReserveRecord()) {
        const CE::RecordMeta meta{NextRecordMeta(CE::RecordKind::Graphics, build)};
        serialization_thread.QueueWork(
            [session = cache_session.get(), key, envs = std::move(envs), is_mesh, meta] {
                boost::container::static_vector<const GenericEnvironment*, Maxwell::MaxShaderProgram>
                    env_ptrs;
                for (size_t index = 0; index < Maxwell::MaxShaderProgram; ++index) {
                    if (key.unique_hashes[index] != 0) {
                        env_ptrs.push_back(&envs[index]);
                    }
                }
                RecordPipeline(*session, meta, EncodeGraphicsKey(key), env_ptrs, is_mesh);
            });
    }
    QueueVulkanPipelineCacheFlush();
}

// Native graphics pipelines translated on worker threads.
//
// A new pipeline was translated (lowered code -> IR -> SPIR-V) on the GPU thread before its draw.
// With asynchronous shaders, the GPU thread now builds the environments (the pipeline key needs
// the lowered code anyway), detaches them from the 3D engine (GraphicsEnvironment::Detach) and
// queues the translation on translation_workers, which also queue the driver compile on workers.
// It waits within translation_wait (and what is left of translation_frame_budget) for the result;
// a pipeline not ready by then skips the draw like one whose driver compile isn't done, and a
// later draw adopts it. Draws that can't be skipped (CanSkipDraw) translate on the GPU thread, or
// wait for a translation already queued. Compute pipelines and Switch 1 programs, whose code is
// read from guest memory while translating, stay on the GPU thread.
bool PipelineCache::CanTranslateAsynchronously() {
    if (!translation_workers || !maxwell3d->GetNativeMacroEngine().native_turing ||
        !CanSkipDraw()) {
        return false;
    }
    for (size_t index = 0; index < Maxwell::MaxShaderProgram; ++index) {
        if (graphics_key.unique_hashes[index] != 0 &&
            (!shader_infos[index] || !shader_infos[index]->lowered_native)) {
            return false;
        }
    }
    return true;
}

void PipelineCache::RunInParallel(size_t count, const std::function<void(size_t)>& task) {
    // Workers that start after every task was claimed return at once, so the caller never waits
    // for a worker that hasn't started: when all are busy it runs the tasks itself.
    struct State {
        std::atomic<size_t> next{};
        size_t count{};
        const std::function<void(size_t)>* task{};
        std::mutex mutex;
        std::condition_variable done_cv;
        size_t done{};
        std::vector<std::exception_ptr> errors;
    };
    const auto state = std::make_shared<State>();
    state->count = count;
    state->task = &task;
    state->errors.resize(count);
    const auto run = [](State& shared) {
        for (;;) {
            const size_t index{shared.next.fetch_add(1, std::memory_order_relaxed)};
            if (index >= shared.count) {
                return;
            }
            try {
                (*shared.task)(index);
            } catch (...) {
                shared.errors[index] = std::current_exception();
            }
            std::scoped_lock lock{shared.mutex};
            ++shared.done;
            shared.done_cv.notify_all();
        }
    };
    for (size_t helper = 1; helper < count; ++helper) {
        translation_workers->QueueWork([state, run] { run(*state); });
    }
    run(*state);
    {
        std::unique_lock lock{state->mutex};
        state->done_cv.wait(lock, [&] { return state->done == state->count; });
    }
    for (const std::exception_ptr& error : state->errors) {
        if (error) {
            std::rethrow_exception(error);
        }
    }
}

namespace {
// Logged every 10 s from the GPU thread.
struct TranslationReport {
    using Duration = std::chrono::steady_clock::duration;
    u32 queued{};
    u32 not_detached{};
    u32 ready_in_time{};
    u32 skipped{};
    u32 adopted{};
    u32 claimed{}; ///< Translated on the GPU thread for a draw that can't be skipped
    u64 parallel{};
    size_t peak_queued{};
    size_t copied_bytes{};
    Duration preparation{};
    Duration waited{};
    Duration unskippable{}; ///< GPU thread time on draws that can't be skipped
    Duration worker{};
    Duration latency{}; ///< Queued to finished, summed over adopted pipelines
    Duration max_latency{};
    // Texture pool measurements (late pool read on workers)
    u32 pool_max{};        ///< Largest pool detached, in entries
    u64 pool_sum{};
    u32 pool_small{};      ///< Detached pools of at most 4096 entries (128 KiB)
    u32 pool_reads{};      ///< Texture reads checked again at adoption
    u32 pool_changed{};    ///< ... that give another answer now
    u32 pool_pipelines_changed{};
    u32 pool_remapped{};
    u32 pool_dirty{};
    std::chrono::steady_clock::time_point window{std::chrono::steady_clock::now()};

    void Log() {
        const auto now = std::chrono::steady_clock::now();
        if (now - window < std::chrono::seconds{10}) {
            return;
        }
        const auto ms = [](Duration d) { return std::chrono::duration<double, std::milli>(d).count(); };
        if (queued != 0 || not_detached != 0 || adopted != 0 || claimed != 0) {
            LOG_INFO(Render_Vulkan,
                     "Async translation in the last 10 s: {} queued ({} KiB copied, {:.1f} ms "
                     "preparing), {} ready in time, {} draws skipped, {} not detached; {:.1f} ms "
                     "waited; {} adopted, {:.1f} ms on workers, latency {:.1f} ms mean {:.1f} ms "
                     "max, peak {} queued, {} with stages in parallel; {} taken from the queue "
                     "for draws that can't be skipped, {:.1f} ms on those draws",
                     queued, copied_bytes / 1024, ms(preparation), ready_in_time, skipped,
                     not_detached, ms(waited), adopted, ms(worker),
                     adopted != 0 ? ms(latency) / adopted : 0.0, ms(max_latency), peak_queued,
                     parallel, claimed, ms(unskippable));
            LOG_INFO(Render_Vulkan,
                     "Async translation texture pools: {} entries max, {:.0f} mean, {} of {} "
                     "detached pools with at most 4096 entries; at adoption {} of {} texture "
                     "reads changed ({} pipelines); pool remapped for {} stages, written by the GPU "
                     "for {}",
                     pool_max, queued != 0 ? static_cast<double>(pool_sum) / queued : 0.0,
                     pool_small, queued, pool_changed, pool_reads, pool_pipelines_changed,
                     pool_remapped, pool_dirty);
        }
        *this = TranslationReport{};
    }
};

TranslationReport& GetTranslationReport() {
    static TranslationReport report;
    return report;
}
} // Anonymous namespace

std::optional<std::shared_future<void>> PipelineCache::QueueGraphicsTranslation() {
    const auto begin = std::chrono::steady_clock::now();
    auto& report{GetTranslationReport()};
    GraphicsEnvironments environments;
    GetGraphicsEnvironments(environments, graphics_key.unique_hashes);
    size_t copied{};
    for (size_t index = 0; index < Maxwell::MaxShaderProgram; ++index) {
        if (graphics_key.unique_hashes[index] == 0) {
            continue;
        }
        if (!environments.envs[index].Detach(device_memory)) {
            ++report.not_detached;
            report.Log();
            return std::nullopt;
        }
        copied += environments.envs[index].SnapshotSizeBytes();
    }
    {
        // Every stage of a pipeline has the same pool: count it once.
        u32 entries{};
        for (const auto& env : environments.envs) {
            entries = std::max(entries, env.TexturePoolEntries());
        }
        report.pool_max = std::max(report.pool_max, entries);
        report.pool_sum += entries;
        report.pool_small += entries <= 4096 ? 1 : 0;
    }
    auto task{std::make_shared<PendingGraphicsTranslation>()};
    task->envs = std::move(environments.envs);
    std::shared_future<void> done{task->done.get_future().share()};
    translating_graphics.emplace(graphics_key, QueuedGraphicsTranslation{done, task});
    report.peak_queued = std::max(report.peak_queued, translating_graphics.size());
    last_runtime_build.store(begin.time_since_epoch().count(), std::memory_order_relaxed);
    task->queued_at = std::chrono::steady_clock::now();
    task->work = Spikes::NewWorkId();
    translation_workers->QueueWork([this, key = graphics_key, task] {
        if (!task->claimed.exchange(true)) {
            TranslateGraphicsPipeline(key, std::move(task->envs), std::move(task->done),
                                      task->queued_at, task->work);
        }
    });
    ++report.queued;
    report.copied_bytes += copied;
    report.preparation += std::chrono::steady_clock::now() - begin;
    report.Log();
    return done;
}

bool PipelineCache::WaitForGraphicsTranslation(const std::shared_future<void>& done) {
    auto& report{GetTranslationReport()};
    if (translation_frame != frame_count) {
        // TickFrame may run on another thread: the budget is reset here, at the frame's first wait.
        translation_frame = frame_count;
        translation_frame_left = translation_frame_budget;
    }
    // Behind a backlog, waiting would spend the budget without getting the pipeline.
    const auto wait{std::min(translation_wait, translation_frame_left)};
    bool ready{done.wait_for(std::chrono::seconds{0}) == std::future_status::ready};
    if (!ready && wait.count() > 0 &&
        translating_graphics.size() <= num_translation_threads.load(std::memory_order_relaxed)) {
        const auto begin = std::chrono::steady_clock::now();
        {
            Nx2Bottleneck::WaitScope build_wait{Nx2Bottleneck::Wait::ShaderBuild};
            const Spikes::SpanScope wait_span{Spikes::Cause::Waiting};
            if (const auto queued = translating_graphics.find(graphics_key);
                queued != translating_graphics.end()) {
                Spikes::LinkWait(wait_span.Token(), queued->second.task->work,
                                 Spikes::DependencyKind::Job);
            }
            ready = done.wait_for(wait) == std::future_status::ready;
        }
        const auto spent{std::chrono::steady_clock::now() - begin};
        DriverBuildReport::NoteGpuWait(spent);
        translation_frame_left -=
            std::min(translation_frame_left,
                     std::chrono::duration_cast<std::chrono::microseconds>(spent));
        report.waited += spent;
        report.ready_in_time += ready ? 1 : 0;
    }
    report.skipped += ready ? 0 : 1;
    if (!ready) {
        VideoCommon::Nx2Spikes::Note(VideoCommon::Nx2Spikes::Event::DrawSkipped);
    }
    report.Log();
    return ready;
}

void PipelineCache::FinishGraphicsTranslation(const QueuedGraphicsTranslation& queued) {
    auto& report{GetTranslationReport()};
    const auto begin = std::chrono::steady_clock::now();
    const bool claimed_here = !queued.task->claimed.exchange(true);
    if (claimed_here) {
        // Still in the queue, possibly behind hundreds of others: translate it here.
        ++report.claimed;
        TranslateGraphicsPipeline(graphics_key, std::move(queued.task->envs),
                                  std::move(queued.task->done), queued.task->queued_at,
                                  queued.task->work);
        DriverBuildReport::NoteGpuBuild(std::chrono::steady_clock::now() - begin);
    }
    // A worker that started it first finishes one translation.
    const auto wait_begin = std::chrono::steady_clock::now();
    {
        Nx2Bottleneck::WaitScope build_wait{Nx2Bottleneck::Wait::ShaderBuild};
        const Spikes::SpanScope wait_span{Spikes::Cause::Waiting};
        Spikes::LinkWait(wait_span.Token(), queued.task->work, Spikes::DependencyKind::Job);
        queued.done.wait();
    }
    const auto end = std::chrono::steady_clock::now();
    report.unskippable += end - begin;
    if (!claimed_here) {
        DriverBuildReport::NoteGpuWait(end - wait_begin);
    }
    report.Log();
}

void PipelineCache::TranslateGraphicsPipeline(const GraphicsPipelineCacheKey& key,
                                              GraphicsEnvironmentArray envs,
                                              std::promise<void> done,
                                              std::chrono::steady_clock::time_point queued_at,
                                              u64 work) {
    const auto begin = std::chrono::steady_clock::now();
    DriverBuildReport::AddQueued(begin - queued_at);
    std::unique_ptr<GraphicsPipeline> pipeline;
    {
        const Spikes::SpanScope build_span{Spikes::Cause::Pipeline, work};
        ShaderPools pools;
        boost::container::static_vector<Shader::Environment*, Maxwell::MaxShaderProgram> env_ptrs;
        for (size_t index = 0; index < Maxwell::MaxShaderProgram; ++index) {
            if (key.unique_hashes[index] != 0) {
                env_ptrs.push_back(&envs[index]);
            }
        }
        try {
            pipeline = CreateGraphicsPipeline(pools, key, MakeSpan(env_ptrs), nullptr, true,
                                              true, true);
        } catch (const std::exception& exception) {
            // Shader errors are handled inside; nothing else may end the worker thread.
            LOG_ERROR(Render_Vulkan, "Graphics pipeline translation failed: {}", exception.what());
        }
    }
    for (size_t index = 0; index < Maxwell::MaxShaderProgram; ++index) {
        envs[index].ReleaseSnapshotBuffers();
    }
    {
        std::scoped_lock lock{translated_mutex};
        translated_graphics.push_back({
            .key = key,
            .pipeline = std::move(pipeline),
            .envs = std::move(envs),
            .time = std::chrono::steady_clock::now() - begin,
            .latency = std::chrono::steady_clock::now() - queued_at,
        });
        num_translated.fetch_add(1, std::memory_order_release);
    }
    done.set_value();
}

void PipelineCache::AdoptTranslatedGraphicsPipelines() {
    std::vector<TranslatedGraphicsPipeline> finished;
    {
        std::scoped_lock lock{translated_mutex};
        finished.swap(translated_graphics);
        num_translated.store(0, std::memory_order_relaxed);
    }
    auto& report{GetTranslationReport()};
    for (TranslatedGraphicsPipeline& translated : finished) {
        translating_graphics.erase(translated.key);
        ++report.adopted;
        report.parallel = parallel_stage_pipelines.load(std::memory_order_relaxed);
        report.worker += translated.time;
        report.latency += translated.latency;
        report.max_latency = std::max(report.max_latency, translated.latency);
        u32 changed{};
        for (const auto& env : translated.envs) {
            const auto check{env.CheckTextureReads(*gpu_memory)};
            report.pool_reads += check.reads;
            changed += check.changed;
            report.pool_remapped += check.remapped ? 1 : 0;
            report.pool_dirty += check.dirty ? 1 : 0;
        }
        report.pool_changed += changed;
        report.pool_pipelines_changed += changed != 0 ? 1 : 0;
        const bool is_mesh{translated.pipeline && translated.pipeline->IsMeshPipeline()};
        const bool built{static_cast<bool>(translated.pipeline)};
        const auto [it, inserted]{graphics_cache.try_emplace(translated.key)};
        if (!inserted) {
            // Unreachable: a key being translated is never built elsewhere. Its driver compile
            // may still be queued, so it can't be destroyed.
            ASSERT(false);
            static_cast<void>(translated.pipeline.release());
            continue;
        }
        it->second = std::move(translated.pipeline);
        if (built) {
            StoreGraphicsPipeline(translated.key, std::move(translated.envs), is_mesh,
                                  translated.time);
        }
    }
    report.Log();
}

ComputePipelineCacheEntry PipelineCache::CreateComputePipeline(const ComputePipelineCacheKey& key,
                                                               const ShaderInfo* shader,
                                                               std::span<const u64> keep_pointers) {
    const auto begin = std::chrono::steady_clock::now();
    SCOPE_EXIT {
        ReportRuntimePipeline("Compute", begin, false, false);
        ChargeRuntimeBuild(begin);
        last_runtime_build.store(std::chrono::steady_clock::now().time_since_epoch().count(),
                                 std::memory_order_relaxed);
    };
    const GPUVAddr program_base{kepler_compute->regs.code_loc.Address()};
    const auto& qmd{kepler_compute->launch_description};
    ComputeEnvironment env{*kepler_compute, *gpu_memory, program_base, qmd.program_start,
                           shader->lowered_native};
    env.SetCachedSize(shader->size_bytes);
    env.KeepStoragePointers(keep_pointers);

    main_pools.ReleaseContents();
    auto entry{CreateComputePipeline(main_pools, key, env, nullptr, true)};
    if (!entry.pipeline || !cache_session) {
        return entry;
    }
    if (ReserveRecord()) {
        const CE::RecordMeta meta{
            NextRecordMeta(CE::RecordKind::Compute, std::chrono::steady_clock::now() - begin)};
        serialization_thread.QueueWork(
            [session = cache_session.get(), meta, key_bytes = EncodeComputeKey(key),
             env_ = std::move(env)] {
                const std::array<const GenericEnvironment*, 1> envs{&env_};
                RecordPipeline(*session, meta, key_bytes, envs);
            });
    }
    QueueVulkanPipelineCacheFlush();
    return entry;
}

ComputePipelineCacheEntry PipelineCache::CreateComputePipeline(ShaderPools& pools,
                                                               const ComputePipelineCacheKey& key,
                                                               Shader::Environment& env,
                                                               PipelineStatistics* statistics,
                                                               bool build_in_parallel,
                                                               bool cache_only) try {
    const DriverBuildReport::BuildScope busy;
    const auto build_begin = std::chrono::steady_clock::now();
    auto hash = key.Hash();
    if (device.HasBrokenCompute()) {
        LOG_ERROR(Render_Vulkan, "Skipping {:#016x}", hash);
        return {.failure = "device has broken compute"};
    }

    LOG_DEBUG(Render_Vulkan, "{:#016x}", hash);

    std::optional<PhaseTimer> translate_timer{std::in_place, BuildPhase::Translate, false};
    Shader::Maxwell::Flow::CFG cfg{env, pools.flow_block, env.StartAddress()};

    // Dump it before error.
    if (Settings::values.dump_guest_shaders) {
        env.Dump(hash, key.unique_hash);
    }

    auto program{TranslateProgram(pools.inst, pools.block, env, cfg, host_info)};
    translate_timer.reset();
    const VkDriverIdKHR driver_id = device.GetDriverID();
    const bool needs_shared_mem_clamp = driver_id == VK_DRIVER_ID_QUALCOMM_PROPRIETARY ||
                                        driver_id == VK_DRIVER_ID_ARM_PROPRIETARY ||
                                        driver_id == VK_DRIVER_ID_MOLTENVK;
    const u32 max_shared_memory = device.GetMaxComputeSharedMemorySize();
    if (needs_shared_mem_clamp && program.shared_memory_size > max_shared_memory) {
        LOG_WARNING(
            Render_Vulkan,
            "Compute shader {:#016x} requests {}KB shared memory but device max is {}KB - clamping",
            key.unique_hash, program.shared_memory_size / 1024, max_shared_memory / 1024);
        program.shared_memory_size = max_shared_memory;
    }
    std::optional<PhaseTimer> emit_timer{std::in_place, BuildPhase::Emit, false};
    const std::vector<u32> code{EmitSPIRV(profile, program)};
    device.SaveShader(code);
    vk::ShaderModule spv_module{BuildShader(device, code)};
    emit_timer.reset();

    // Text log + .spv dump. Same split as the graphics path.
    const bool should_log = GPU::Logging::IsActive();
    const bool should_dump = Settings::values.gpu_log_shader_dumps.GetValue();
    if (should_log || should_dump) {
        const std::string shader_name = fmt::format("shader_{:016x}_compute", key.unique_hash);
        if (should_log) {
            const std::string shader_info = fmt::format("SPIR-V size: {} bytes, hash: {:016x}",
                                                        code.size() * sizeof(u32), key.unique_hash);
            GPU::Logging::GPULogger::GetInstance().LogShaderCompilation(shader_name, shader_info);
        }
        if (should_dump) {
            GPU::Logging::DumpSpirvShader(key.unique_hash,
                                          std::span<const u32>(code.data(), code.size()));
        }
    }

    if (device.HasDebuggingToolAttached()) {
        const auto name{fmt::format("Shader {:016x}", key.unique_hash)};
        spv_module.SetObjectNameEXT(name.c_str());
    }
    std::vector<u64> mapped_pointers{env.IsNativeProgram() ? env.MappedStoragePointerKeys()
                                                           : std::vector<u64>{}};
    // Variants of one program with other mapped pointers are builds of their own.
    u64 identity{hash};
    for (const u64 mapped : mapped_pointers) {
        identity = (identity ^ mapped) * 0x9E3779B97F4A7C15ULL;
    }
    DriverBuildReport::NoteStart(identity);
    DriverBuildReport::SetFrontEnd(build_begin);
    Common::ThreadWorker* const thread_worker{build_in_parallel ? &workers : nullptr};
    return {.pipeline = std::make_unique<ComputePipeline>(
                device, scheduler, vulkan_pipeline_cache, descriptor_pool, guest_descriptor_queue,
                descriptor_buffer_ring, thread_worker, statistics, &shader_notify, program.info,
                std::move(spv_module), key.unique_hash, code, cache_only),
            .null_pointer_slots = env.IsNativeProgram() ? env.NullStoragePointerSlots()
                                                        : std::vector<std::pair<u32, u32>>{},
            .mapped_pointers = std::move(mapped_pointers)};

} catch (const Shader::Exception& exception) {
    LOG_ERROR(Render_Vulkan, "{}", exception.what());
    return {.failure = exception.what()};
}

VideoCommon::CacheEnvelope::CacheIdentity PipelineCache::MakeCacheIdentity(
    CE::PayloadKind kind) const {
    const VkPhysicalDeviceProperties properties = device.GetPhysical().GetProperties();
    CE::CacheIdentity identity{
        .kind = kind,
        .envelope_version = CE::EnvelopeVersion,
        .shader_format_version = CACHE_VERSION,
        .host = CE::GetHostIdentity(),
        .device = {.vendor_id = properties.vendorID,
                   .device_id = properties.deviceID,
                   .driver_id = static_cast<u32>(device.GetDriverID()),
                   .driver_version = properties.driverVersion},
    };
    std::memcpy(identity.device.pipeline_cache_uuid.data(), properties.pipelineCacheUUID,
                identity.device.pipeline_cache_uuid.size());
    return identity;
}

void PipelineCache::SerializeVulkanPipelineCache(const std::filesystem::path& filename,
                                                 const vk::PipelineCache& pipeline_cache) {
    if (!pipeline_cache) {
        return;
    }
    size_t cache_size = 0;
    pipeline_cache.Read(&cache_size, nullptr);
    if (cache_size == 0) {
        return;
    }
    std::vector<u8> blob(cache_size);
    pipeline_cache.Read(&cache_size, blob.data());
    blob.resize(cache_size);

    // Written beside the old file and renamed over it by Commit: a failure at any step leaves the
    // previous blob in place.
    auto writer = CE::EnvelopeWriter::Create(filename, MakeCacheIdentity(CE::PayloadKind::DriverBlob),
                                             CE::MaxDriverBlobBytes);
    bool ok = writer != nullptr;
    if (ok) {
        const CE::RecordMeta meta{.kind = CE::RecordKind::Opaque};
        ok = PP::ForEachDriverBlobChunk(blob, [&](std::span<const u8> chunk) {
            return writer->Append(meta, chunk) == CE::AppendResult::Ok;
        });
    }
    ok = ok && writer->Commit();
    if (!ok) {
        LOG_ERROR(Render_Vulkan, "Vulkan driver pipeline cache not saved, keeping the previous file");
        return;
    }
    LOG_INFO(Render_Vulkan, "Vulkan driver pipelines cached at: {}",
             Common::FS::PathToUTF8String(filename));
}

vk::PipelineCache PipelineCache::LoadVulkanPipelineCache(const std::filesystem::path& filename,
                                                         CE::CacheLoadResult& result) {
    const auto create_pipeline_cache = [this](size_t data_size, const void* data) {
        VkPipelineCacheCreateInfo pipeline_cache_ci = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .initialDataSize = data_size,
            .pInitialData = data};
        return device.GetLogical().CreatePipelineCache(pipeline_cache_ci);
    };
    const CE::CacheIdentity identity = MakeCacheIdentity(CE::PayloadKind::DriverBlob);
    const auto reader = CE::EnvelopeReader::Open(filename, identity, result);
    if (!reader) {
        return create_pipeline_cache(0, nullptr);
    }
    // The blob reaches the driver only after its Vulkan header names this device and driver.
    std::vector<u8> blob;
    const CE::RejectReason reason = PP::JoinDriverBlob(
        reader->Index().size(),
        [&](size_t index, std::vector<u8>& chunk) { return reader->ReadRecord(index, chunk); },
        identity.device, blob);
    if (reason != CE::RejectReason::None) {
        result.accepted = false;
        result.reason = reason;
        return create_pipeline_cache(0, nullptr);
    }
    LOG_INFO(Render_Vulkan, "Loaded Vulkan driver pipeline cache: {}",
             Common::FS::PathToUTF8String(filename));
    return create_pipeline_cache(blob.size(), blob.data());
}

} // namespace Vulkan
