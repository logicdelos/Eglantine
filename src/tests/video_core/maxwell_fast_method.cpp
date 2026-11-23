// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/page_table.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/device_memory.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/kernel.h"
#include "core/memory.h"
#include "tests/core/system_test_context.h"
#include "video_core/dirty_flags.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/gpu.h"
#include "video_core/gpu_split.h"
#include "video_core/memory_manager.h"
#include "video_core/renderer_null/null_rasterizer.h"
#include "video_core/replay/run_state.h"
#include "video_core/control/channel_state.h"
#include "video_core/dma_pusher.h"
#include "video_core/nx2.h"
#include "video_core/rasterizer_interface.h"

namespace VideoCommon {

struct GpuSplitTestAccess {
    static void Initialize(GpuSplit& split, Core::System& system, u64 position = 0) {
        split.system = &system;
        split.ring = std::make_unique<u64[]>(GpuSplit::RingWords);
        split.write_position = position;
        split.consumed_cache = position;
        split.published_local = position;
        split.publish_every = GpuSplit::RingWords;
    }

    static u64 End(const GpuSplit& split) {
        return split.write_position;
    }

    static u64 Word(const GpuSplit& split, u64 position) {
        return split.ring[position & GpuSplit::RingMask];
    }

    static u64 NearWrap() {
        return GpuSplit::RingWords - 1;
    }

    static u64 Execute(GpuSplit& split, u64 position, u64 available) {
        const size_t index = position & GpuSplit::RingMask;
        const size_t size = split.Execute(
            &split.ring[index], std::min<u64>(available - position, GpuSplit::RingWords - index));
        return position + (size != 0 ? size : GpuSplit::RingWords - index);
    }

    static void SetActive(GpuSplit& split, bool active) {
        split.active = active;
    }

    // Back to the state of a split that never started.
    static void Reset(GpuSplit& split) {
        split.active = false;
        split.system = nullptr;
        split.ring.reset();
        split.write_position = 0;
        split.consumed_cache = 0;
        split.published_local = 0;
    }
};

} // namespace VideoCommon

namespace Tegra {

// Runs the decoder over a span of words as DmaPusher::Step does, and returns the words consumed.
struct DmaPusherTestAccess {
    static size_t Process(DmaPusher& pusher, std::span<const CommandHeader> commands,
                          size_t word_offset) {
        return pusher.ProcessCommands(commands, word_offset);
    }
};

} // namespace Tegra

namespace {

using Tegra::Engines::Maxwell3D;

constexpr u32 CbSize = MAXWELL3D_REG_INDEX(const_buffer.size);
constexpr u32 CbAddressHigh = MAXWELL3D_REG_INDEX(const_buffer.address_high);
constexpr u32 CbAddressLow = MAXWELL3D_REG_INDEX(const_buffer.address_low);
constexpr u32 CbOffset = MAXWELL3D_REG_INDEX(const_buffer.offset);
constexpr u32 CbData = MAXWELL3D_REG_INDEX(const_buffer.buffer);
constexpr u32 DrawBegin = MAXWELL3D_REG_INDEX(draw.begin);
constexpr u32 DrawEnd = MAXWELL3D_REG_INDEX(draw.end);
constexpr u32 ShadowControl = MAXWELL3D_REG_INDEX(shadow_ram_control);

// Executable methods without memory or macro effects a null rasterizer cannot take.
constexpr std::array<u32, 16> executable_methods{
    DrawBegin,
    DrawEnd,
    MAXWELL3D_REG_INDEX(vertex_buffer.first),
    MAXWELL3D_REG_INDEX(vertex_buffer.count),
    MAXWELL3D_REG_INDEX(index_buffer.first),
    MAXWELL3D_REG_INDEX(index_buffer.count),
    MAXWELL3D_REG_INDEX(topology_override),
    MAXWELL3D_REG_INDEX(bind_groups[0].raw_config),
    MAXWELL3D_REG_INDEX(bind_groups[1].raw_config),
    MAXWELL3D_REG_INDEX(bind_groups[2].raw_config),
    MAXWELL3D_REG_INDEX(bind_groups[3].raw_config),
    MAXWELL3D_REG_INDEX(bind_groups[4].raw_config),
    MAXWELL3D_REG_INDEX(vertex_array_instance_first),
    MAXWELL3D_REG_INDEX(vertex_array_instance_subsequent),
    Maxwell3D::RootTables::SelectMethod,
    Maxwell3D::RootTables::LoadMethod,
};

// Binds the rasterizer, gives every register a dirty flag so that a missed or extra write shows,
// and points the constant buffer at an unmapped address.
void PrepareEngine(Core::System& system, Maxwell3D& engine,
                   VideoCore::RasterizerInterface* rasterizer, bool fast_maxwell3d) {
    engine.fast_maxwell3d = fast_maxwell3d;
    engine.BindRasterizer(rasterizer);
    VideoCommon::Dirty::SetupDirtyFlags(engine.dirty.tables);
    for (u32 method = 0; method < Maxwell3D::Regs::NUM_REGS; ++method) {
        if (engine.dirty.tables[1][method] == 0) {
            engine.dirty.tables[1][method] =
                static_cast<u8>(VideoCommon::Dirty::LastCommonEntry + method % 61);
        }
    }
    engine.dirty.flags.reset();
    // An unmapped buffer: the upload writes nothing, the offset still advances.
    engine.CallMethod(system, CbAddressHigh, 0x1, true);
    engine.CallMethod(system, CbAddressLow, 0x2000'0000, true);
    engine.CallMethod(system, CbSize, 0x10000, true);
}

void CompareEngines(const Maxwell3D& fast, const Maxwell3D& slow) {
    REQUIRE(fast.regs.reg_array == slow.regs.reg_array);
    REQUIRE(fast.shadow_state.reg_array == slow.shadow_state.reg_array);
    REQUIRE(fast.shadow_state.shadow_ram_control == slow.shadow_state.shadow_ram_control);
    REQUIRE(fast.dirty.flags == slow.dirty.flags);
    const auto& a = fast.draw_manager.draw_state;
    const auto& b = slow.draw_manager.draw_state;
    REQUIRE(a.topology == b.topology);
    REQUIRE(a.draw_mode == b.draw_mode);
    REQUIRE(a.draw_indexed == b.draw_indexed);
    REQUIRE(a.draw_element_count == b.draw_element_count);
    REQUIRE(a.base_index == b.base_index);
    REQUIRE(a.base_instance == b.base_instance);
    REQUIRE(a.instance_count == b.instance_count);
    REQUIRE(a.drawn_instances == b.drawn_instances);
    REQUIRE(std::memcmp(&a.vertex_buffer, &b.vertex_buffer, sizeof(a.vertex_buffer)) == 0);
    REQUIRE(std::memcmp(&a.index_buffer, &b.index_buffer, sizeof(a.index_buffer)) == 0);
    for (size_t stage = 0; stage < fast.state.shader_stages.size(); ++stage) {
        for (size_t slot = 0; slot < Maxwell3D::Regs::MaxConstBuffers; ++slot) {
            const auto& x = fast.state.shader_stages[stage].const_buffers[slot];
            const auto& y = slow.state.shader_stages[stage].const_buffers[slot];
            REQUIRE(x.address == y.address);
            REQUIRE(x.size == y.size);
            REQUIRE(x.enabled == y.enabled);
        }
    }
    REQUIRE(fast.root_tables.tables == slow.root_tables.tables);
    REQUIRE(fast.root_tables.selected == slow.root_tables.selected);
    REQUIRE(fast.root_tables.offset == slow.root_tables.offset);
}

struct Fixture {
    ScopedSystemTestContext context;
    Core::System system;
    Core::DeviceMemory physical;
    Tegra::MaxwellDeviceMemoryManager device_memory{physical};
    Tegra::MemoryManager memory{system, device_memory, 32, 1ULL << 31};
    Tegra::GPU gpu{system, false, false};
    Null::RasterizerNull rasterizer{gpu};
    Maxwell3D slow{memory};
    Maxwell3D fast{memory};
    std::vector<u32> state_methods;

    Fixture() {
        PrepareEngine(system, slow, &rasterizer, false);
        PrepareEngine(system, fast, &rasterizer, true);
        for (u32 method = 0; method < Maxwell3D::Regs::NUM_REGS; ++method) {
            if (!slow.execution_mask[method] && (method < CbSize || method > CbAddressLow)) {
                state_methods.push_back(method);
            }
        }
    }

    void Call(u32 method, u32 argument, bool is_last) {
        slow.CallMethod(system, method, argument, is_last);
        fast.CallMethod(system, method, argument, is_last);
    }

    void CallMulti(u32 method, const std::vector<u32>& words) {
        const u32 amount = static_cast<u32>(words.size());
        slow.CallMultiMethod(system, method, words.data(), amount, amount);
        fast.CallMultiMethod(system, method, words.data(), amount, amount);
    }

    void Compare() const {
        CompareEngines(fast, slow);
    }
};

u32 RandomArgument(std::mt19937& rng, u32 method, const Maxwell3D& engine) {
    // Mostly values the register already holds or small ones, as the macros write them.
    switch (rng() % 4) {
    case 0:
        return engine.regs.reg_array[method];
    case 1:
        return rng() % 4;
    default:
        break;
    }
    if (method == DrawBegin) {
        // Instance id in bits 26-27: first, subsequent (an instanced loop) or unchanged.
        return (static_cast<u32>(rng() % 3) << 26) | (rng() % 8);
    }
    if (method == Maxwell3D::RootTables::SelectMethod) {
        return (rng() % 8) | ((rng() % 64 * 4) << 8);
    }
    return rng() % 64;
}

// ---- Fast graphics preview: the decoder shortcut against the legacy decoder ----

using Tegra::CommandHeader;
using Tegra::SubmissionMode;
using Words = std::vector<u32>;

constexpr u32 BindGroupBase = MAXWELL3D_REG_INDEX(bind_groups[0].raw_config);
constexpr u32 BindGroupStride = 8; // Regs::BindGroup is 0x20 bytes
constexpr u32 StageCount = 5;
constexpr u32 SlotLimit = Maxwell3D::Regs::MaxConstBuffers;
constexpr u32 InlineLimit = 0x1FFF; // an inline header carries 13 bits
constexpr u32 VertexFirst = MAXWELL3D_REG_INDEX(vertex_buffer.first);
constexpr u32 VertexCount = MAXWELL3D_REG_INDEX(vertex_buffer.count);
constexpr u32 TopologyOverride = MAXWELL3D_REG_INDEX(topology_override);
constexpr u32 InstanceSubsequent = 1u << 26;
constexpr GPUVAddr MapAddress = 0x100000;
constexpr size_t MapSize = 0x3000;
constexpr size_t SnapshotBytes = 16;
constexpr std::array<bool, 2> EngineModes{true, false}; // fast_maxwell3d

// Switches the preview's inputs the test changes back after it: the renderer must be Vulkan for a
// bind to be taken.
struct PreviewSettings {
    const bool previous_preview = Settings::values.nx2_fast_graphics_preview.GetValue();
    const Settings::RendererBackend previous_backend = Settings::values.renderer_backend.GetValue();
    const bool previous_ampere = Tegra::g_nx2_ampere.load();
    const bool previous_replay = VideoCore::Replay::ReplayGpuRunning();

    PreviewSettings() {
        Settings::values.renderer_backend.SetValue(Settings::RendererBackend::Vulkan);
    }

    ~PreviewSettings() {
        Settings::values.nx2_fast_graphics_preview.SetValue(previous_preview);
        Settings::values.renderer_backend.SetValue(previous_backend);
        Tegra::g_nx2_ampere.store(previous_ampere);
        VideoCore::Replay::SetReplayGpuRunning(previous_replay);
    }
};

u32 Header(SubmissionMode mode, u32 method, u32 count, u32 subchannel = 0) {
    CommandHeader header{};
    header.method.Assign(method);
    header.subchannel.Assign(subchannel);
    header.arg_count.Assign(count);
    header.mode.Assign(mode);
    return header.argument;
}

std::vector<CommandHeader> ToHeaders(const Words& words) {
    std::vector<CommandHeader> result(words.size());
    for (size_t i = 0; i < words.size(); ++i) {
        result[i].argument = words[i];
    }
    return result;
}

Words Concat(std::initializer_list<Words> parts) {
    Words result;
    for (const Words& part : parts) {
        result.insert(result.end(), part.begin(), part.end());
    }
    return result;
}

// A uniform buffer bind as NVN writes it: size, address high, address low from const_buffer.size,
// then the write to bind_groups[stage].raw_config, inline or as one increasing word.
struct BindSpec {
    u32 stage{};
    u32 slot{};
    bool enabled{true};
    u32 size{0x100};
    u32 address_high{0};
    u32 address_low{static_cast<u32>(MapAddress)};
    u32 extra_config_bits{}; ///< Bits the register does not define
    bool inline_form{true};
    u32 subchannel{0}; ///< Of the bind write

    [[nodiscard]] u32 Config() const {
        return (enabled ? 1u : 0u) | (slot << 4) | extra_config_bits;
    }
};

Words BindWords(const BindSpec& spec) {
    Words words{Header(SubmissionMode::Increasing, CbSize, 3), spec.size, spec.address_high,
                spec.address_low};
    const u32 method = BindGroupBase + spec.stage * BindGroupStride;
    if (spec.inline_form) {
        REQUIRE(spec.Config() <= InlineLimit);
        words.push_back(Header(SubmissionMode::Inline, method, spec.Config(), spec.subchannel));
    } else {
        words.push_back(Header(SubmissionMode::Increasing, method, 1, spec.subchannel));
        words.push_back(spec.Config());
    }
    return words;
}

// Four inline headers: begin, first, count, end. `begin` carries the instance id when it is
// 0x2000 or more, so that form is one increasing word.
Words DrawWords(u32 begin = 0) {
    Words words;
    if (begin <= InlineLimit) {
        words.push_back(Header(SubmissionMode::Inline, DrawBegin, begin));
    } else {
        words.push_back(Header(SubmissionMode::Increasing, DrawBegin, 1));
        words.push_back(begin);
    }
    words.push_back(Header(SubmissionMode::Inline, VertexFirst, 0));
    words.push_back(Header(SubmissionMode::Inline, VertexCount, 3));
    words.push_back(Header(SubmissionMode::Inline, DrawEnd, 0));
    return words;
}

Words StateWrite(u32 method, u32 value, bool inline_form = false) {
    if (inline_form) {
        REQUIRE(value <= InlineLimit);
        return {Header(SubmissionMode::Inline, method, value)};
    }
    return {Header(SubmissionMode::Increasing, method, 1), value};
}

// The rasterizer calls the bind and the draw make, in order, and what a draw sees in the buffers
// bound at that point (the first SnapshotBytes of each, read at the draw as the buffer cache
// reads them).
struct Event {
    enum class Kind { Bind, Disable, Draw };
    Kind kind{};
    size_t stage{};
    u32 slot{};
    GPUVAddr address{};
    u32 size{};
    std::vector<u8> bytes;

    bool operator==(const Event&) const = default;
};

class Recorder final : public VideoCore::RasterizerInterface {
public:
    Recorder(Tegra::MemoryManager& memory_, Null::RasterizerNull& inner_)
        : memory{memory_}, inner{inner_} {}

    std::vector<Event> events;

    void Draw(bool is_indexed, u32 instance_count) override {
        Event event{.kind = Event::Kind::Draw};
        for (const auto& [key, buffer] : bound) {
            event.bytes.push_back(static_cast<u8>(key.first));
            event.bytes.push_back(static_cast<u8>(key.second));
            std::array<u8, SnapshotBytes> data{};
            const size_t amount = (std::min<size_t>)(buffer.second, SnapshotBytes);
            if (amount != 0 && memory.GpuToCpuAddress(buffer.first, amount)) {
                memory.ReadBlockUnsafe(buffer.first, data.data(), amount);
            }
            event.bytes.insert(event.bytes.end(), data.begin(), data.end());
        }
        events.push_back(std::move(event));
        inner.Draw(is_indexed, instance_count);
    }
    void BindGraphicsUniformBuffer(size_t stage, u32 index, GPUVAddr gpu_addr, u32 size) override {
        bound[{stage, index}] = {gpu_addr, size};
        events.push_back(Event{.kind = Event::Kind::Bind,
                               .stage = stage,
                               .slot = index,
                               .address = gpu_addr,
                               .size = size});
        inner.BindGraphicsUniformBuffer(stage, index, gpu_addr, size);
    }
    void DisableGraphicsUniformBuffer(size_t stage, u32 index) override {
        bound.erase(std::pair<size_t, u32>{stage, index});
        events.push_back(Event{.kind = Event::Kind::Disable, .stage = stage, .slot = index});
        inner.DisableGraphicsUniformBuffer(stage, index);
    }

    void DrawTexture() override {
        inner.DrawTexture();
    }
    void Clear(u32 layer_count) override {
        inner.Clear(layer_count);
    }
    void DispatchCompute() override {
        inner.DispatchCompute();
    }
    void ResetCounter(VideoCommon::QueryType type) override {
        inner.ResetCounter(type);
    }
    void Query(GPUVAddr gpu_addr, VideoCommon::QueryType type,
               VideoCommon::QueryPropertiesFlags flags, u32 payload, u32 subreport) override {
        inner.Query(gpu_addr, type, flags, payload, subreport);
    }
    void SignalFence(std::function<void()>&& func, bool force_completion) override {
        inner.SignalFence(std::move(func), force_completion);
    }
    void SyncOperation(std::function<void()>&& func) override {
        inner.SyncOperation(std::move(func));
    }
    void SignalSyncPoint(u32 value) override {
        inner.SignalSyncPoint(value);
    }
    void SignalReference() override {
        inner.SignalReference();
    }
    void ReleaseFences(bool force) override {
        inner.ReleaseFences(force);
    }
    void FlushAll() override {
        inner.FlushAll();
    }
    void FlushRegion(DAddr addr, u64 size, VideoCommon::CacheType which) override {
        inner.FlushRegion(addr, size, which);
    }
    bool MustFlushRegion(DAddr addr, u64 size, VideoCommon::CacheType which) override {
        return inner.MustFlushRegion(addr, size, which);
    }
    VideoCore::RasterizerDownloadArea GetFlushArea(DAddr addr, u64 size) override {
        return inner.GetFlushArea(addr, size);
    }
    void InvalidateRegion(DAddr addr, u64 size, VideoCommon::CacheType which) override {
        inner.InvalidateRegion(addr, size, which);
    }
    void OnCacheInvalidation(PAddr addr, u64 size) override {
        inner.OnCacheInvalidation(addr, size);
    }
    bool OnCPUWrite(PAddr addr, u64 size) override {
        return inner.OnCPUWrite(addr, size);
    }
    void InvalidateGPUCache() override {
        inner.InvalidateGPUCache();
    }
    void UnmapMemory(DAddr addr, u64 size) override {
        inner.UnmapMemory(addr, size);
    }
    void ModifyGPUMemory(size_t as_id, GPUVAddr addr, u64 size) override {
        inner.ModifyGPUMemory(as_id, addr, size);
    }
    void FlushAndInvalidateRegion(DAddr addr, u64 size, VideoCommon::CacheType which) override {
        inner.FlushAndInvalidateRegion(addr, size, which);
    }
    void WaitForIdle() override {
        inner.WaitForIdle();
    }
    void FragmentBarrier() override {
        inner.FragmentBarrier();
    }
    void TiledCacheBarrier() override {
        inner.TiledCacheBarrier();
    }
    void FlushCommands() override {
        inner.FlushCommands();
    }
    void TickFrame() override {
        inner.TickFrame();
    }
    Tegra::Engines::AccelerateDMAInterface& AccessAccelerateDMA() override {
        return inner.AccessAccelerateDMA();
    }
    void AccelerateInlineToMemory(GPUVAddr address, size_t copy_size,
                                  std::span<const u8> memory_) override {
        inner.AccelerateInlineToMemory(address, copy_size, memory_);
    }

private:
    Tegra::MemoryManager& memory;
    Null::RasterizerNull& inner;
    std::map<std::pair<size_t, u32>, std::pair<GPUVAddr, u32>> bound;
};

// One channel: a Maxwell3D on subchannel 0 behind its own decoder and recorder.
struct Arm {
    Tegra::Control::ChannelState channel;
    Recorder recorder;
    Maxwell3D engine;
    Tegra::DmaPusher pusher;

    Arm(Fixture& f, bool fast_engine, s32 bind_id)
        : channel{bind_id}, recorder{f.memory, f.rasterizer}, engine{f.memory},
          pusher{f.system, f.memory, channel} {
        engine.current_dma_segment = 0; // the engine leaves it unset
        PrepareEngine(f.system, engine, &recorder, fast_engine);
        pusher.BindRasterizer(&recorder);
        pusher.BindSubchannel(&engine, 0, Tegra::Engines::EngineTypes::Maxwell3D);
        pusher.dma_state.dma_get = 0x4000;
    }

    // The setting is read at every call, so it is the one the caller names for this call.
    size_t Run(const std::vector<CommandHeader>& commands, size_t word_offset, bool preview) {
        Settings::values.nx2_fast_graphics_preview.SetValue(preview);
        return Tegra::DmaPusherTestAccess::Process(pusher, commands, word_offset);
    }
};

struct Expected {
    u64 accepted{};
    u64 incomplete_span{};
    u64 shape_subchannel{};
    u64 invalid_binding{};
    u64 backend_engine{};
    u64 ordering{};
    u64 observer{};
    std::optional<u64> other_opcode{}; ///< Every other idle header counts: unset when not wanted
};

void RequireCounters(const Tegra::DmaPusher& pusher, const Expected& expected) {
    const auto& counters = pusher.FastGraphicsPreviewCounters();
    REQUIRE(counters.accepted == expected.accepted);
    REQUIRE(counters.incomplete_span == expected.incomplete_span);
    REQUIRE(counters.shape_subchannel == expected.shape_subchannel);
    REQUIRE(counters.invalid_binding == expected.invalid_binding);
    REQUIRE(counters.backend_engine == expected.backend_engine);
    REQUIRE(counters.ordering == expected.ordering);
    REQUIRE(counters.observer == expected.observer);
    if (expected.other_opcode) {
        REQUIRE(counters.other_opcode == *expected.other_opcode);
    }
}

void CompareArms(const Arm& legacy, const Arm& preview) {
    CompareEngines(preview.engine, legacy.engine);
    REQUIRE(preview.engine.current_dma_segment == legacy.engine.current_dma_segment);
    REQUIRE(preview.engine.method_sink == legacy.engine.method_sink);
    REQUIRE(preview.engine.draw_manager.HasDeferredDraw() ==
            legacy.engine.draw_manager.HasDeferredDraw());
    REQUIRE(preview.recorder.events == legacy.recorder.events);
    const auto& a = preview.pusher.dma_state;
    const auto& b = legacy.pusher.dma_state;
    REQUIRE(a.method == b.method);
    REQUIRE(a.subchannel == b.subchannel);
    REQUIRE(a.method_count == b.method_count);
    REQUIRE(a.length_pending == b.length_pending);
    REQUIRE(a.dma_get == b.dma_get);
    REQUIRE(a.dma_word_offset == b.dma_word_offset);
    REQUIRE(a.non_incrementing == b.non_incrementing);
    REQUIRE(a.is_last_call == b.is_last_call);
    REQUIRE(preview.pusher.dma_increment_once == legacy.pusher.dma_increment_once);
    REQUIRE(preview.pusher.HasPendingAcquire() == legacy.pusher.HasPendingAcquire());
    // The legacy decoder counts nothing, whatever the setting.
    REQUIRE(legacy.pusher.FastGraphicsPreviewCounters() == Tegra::FastGraphicsPreviewStats{});
}

// The same words through the legacy decoder (setting off) and the preview (setting on), compared
// after every call.
struct Channel {
    Arm legacy;
    Arm preview;
    bool preview_on = true;

    Channel(Fixture& f, bool fast_engine, s32 bind_id = 0)
        : legacy{f, fast_engine, bind_id}, preview{f, fast_engine, bind_id} {}

    // One decoder call over `words`, which start `offset` words into the entry. Returns the words
    // both consumed.
    size_t Feed(const Words& words, size_t offset = 0) {
        const auto commands = ToHeaders(words);
        const size_t expected = legacy.Run(commands, offset, false);
        const size_t actual = preview.Run(commands, offset, preview_on);
        REQUIRE(actual == expected);
        CompareArms(legacy, preview);
        return expected;
    }

    // Calls until every word is consumed, as when a pending acquire returns after each word.
    void FeedAll(const Words& words) {
        size_t offset = 0;
        while (offset < words.size()) {
            const size_t consumed = Feed(Words(words.begin() + offset, words.end()), offset);
            REQUIRE(consumed > 0);
            offset += consumed;
        }
    }
};

// Three state methods that are neither the constant buffer, the shadow control, nor executable.
std::array<u32, 3> PriorStateMethods(const Fixture& f) {
    std::array<u32, 3> methods{};
    size_t found = 0;
    for (size_t i = 7; i < f.state_methods.size() && found < methods.size(); i += 97) {
        if (f.state_methods[i] != ShadowControl) {
            methods[found++] = f.state_methods[i];
        }
    }
    REQUIRE(found == methods.size());
    return methods;
}

// A GPU address range of three pages that can be remapped over another device range, as the
// memory the buffer cache reads at a draw.
struct MappedMemory {
    static constexpr DAddr DeviceA = 0x10000;
    static constexpr DAddr DeviceB = 0x20000;

    Fixture& f;
    Kernel::KProcess process;
    decltype(std::declval<Tegra::MaxwellDeviceMemoryManager&>().RegisterProcess(nullptr)) asid{};

    static Kernel::KernelCore& InitializeKernel(Fixture& fixture) {
        fixture.system.Initialize();
        return fixture.system.Kernel();
    }

    explicit MappedMemory(Fixture& fixture) : f{fixture}, process{InitializeKernel(fixture)} {
        auto& table = process.GetPageTable().GetImpl();
        table.Resize(20, 12);
        for (u64 page = 0; page < 6; ++page) {
            const VAddr virtual_address = 0x4000 + page * 0x1000;
            auto* backing = f.physical.GetPointerFromRaw<u8>(0x10000 + page * 0x8000);
            table.entries.GetAndFault(virtual_address >> 12)
                .Store(false, Common::PageType::Memory, 1,
                       reinterpret_cast<uintptr_t>(backing) - virtual_address);
        }
        process.GetMemory().SetCurrentPageTable(process);
        asid = f.device_memory.RegisterProcess(&process.GetMemory());
        f.device_memory.BindInterface(&f.rasterizer);
        f.device_memory.Map(DeviceA, 0x4000, MapSize, asid);
        f.device_memory.Map(DeviceB, 0x7000, MapSize, asid);
        f.memory.BindRasterizer(&f.rasterizer);
        f.memory.Map(MapAddress, DeviceA, MapSize, Tegra::PTEKind::PITCH, false);
    }

    ~MappedMemory() {
        f.device_memory.Unmap(DeviceA, MapSize);
        f.device_memory.Unmap(DeviceB, MapSize);
        f.device_memory.UnregisterProcess(asid);
        f.system.Kernel().UnregisterKernelObject(&process);
    }

    MappedMemory(const MappedMemory&) = delete;
    MappedMemory& operator=(const MappedMemory&) = delete;

    void Write(const std::array<u8, SnapshotBytes>& bytes, GPUVAddr address = MapAddress) {
        f.memory.WriteBlockUnsafe(address, bytes.data(), bytes.size());
    }

    // The same GPU addresses now read the other device range, as when the guest remaps them.
    void Remap(DAddr device) {
        f.memory.Unmap(MapAddress, MapSize);
        f.memory.Map(MapAddress, device, MapSize, Tegra::PTEKind::PITCH, false);
    }
};

std::array<u8, SnapshotBytes> Pattern(u8 seed) {
    std::array<u8, SnapshotBytes> bytes{};
    for (size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<u8>(seed + i);
    }
    return bytes;
}

// What a draw records for one bound buffer: stage, slot, then its bytes.
std::vector<u8> DrawBytes(u8 stage, u8 slot, const std::array<u8, SnapshotBytes>& bytes) {
    std::vector<u8> result{stage, slot};
    result.insert(result.end(), bytes.begin(), bytes.end());
    return result;
}

} // Anonymous namespace

TEST_CASE("Maxwell3D fast method dispatch matches CallMethod on random method streams",
          "[video_core][maxwell_fast_method]") {
    Fixture f;
    for (const u32 seed : {1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u}) {
        CAPTURE(seed);
        std::mt19937 rng{seed};
        for (u32 step = 0; step < 20000; ++step) {
            CAPTURE(step);
            const u32 kind = rng() % 100;
            if (kind < 45) {
                const u32 method = f.state_methods[rng() % f.state_methods.size()];
                f.Call(method, RandomArgument(rng, method, f.slow), rng() % 2 == 0);
            } else if (kind < 75) {
                const u32 method = executable_methods[rng() % executable_methods.size()];
                f.Call(method, RandomArgument(rng, method, f.slow), rng() % 2 == 0);
            } else if (kind < 85) {
                f.Call(CbData + rng() % 16, rng(), true);
            } else if (kind < 88) {
                f.Call(CbOffset, (rng() % 0x4000) * 4, true);
            } else if (kind < 92) {
                std::vector<u32> words(1 + rng() % 8);
                for (u32& word : words) {
                    word = rng() % 3 == 0 ? rng() : rng() % 4;
                }
                const u32 method = rng() % 2 == 0
                                       ? CbData + rng() % 16
                                       : f.state_methods[rng() % f.state_methods.size()];
                f.CallMulti(method, words);
            } else if (kind < 94) {
                // Track, track with filter, passthrough or replay.
                f.Call(ShadowControl, rng() % 4, true);
            } else if (kind < 97) {
                for (auto* engine : {&f.slow, &f.fast}) {
                    engine->dirty.flags.reset();
                }
            } else {
                for (auto* engine : {&f.slow, &f.fast}) {
                    engine->draw_manager.DrawDeferred(*engine);
                }
            }
            f.Compare();
        }
    }
}

TEST_CASE("Maxwell3D fast method dispatch issues deferred instanced draws as CallMethod does",
          "[video_core][maxwell_fast_method]") {
    Fixture f;
    constexpr u32 subsequent = 1u << 26;
    const auto draw = [&f](u32 begin) {
        f.Call(DrawBegin, begin, true);
        f.Call(MAXWELL3D_REG_INDEX(vertex_buffer.first), 0, true);
        f.Call(MAXWELL3D_REG_INDEX(vertex_buffer.count), 3, true);
        f.Call(DrawEnd, 0, true);
        f.Compare();
    };
    draw(0);
    draw(subsequent);
    draw(subsequent);
    REQUIRE(f.fast.draw_manager.HasDeferredDraw());
    // A constant buffer upload, a bind and state changes each end the loop.
    f.Call(CbData, 7, true);
    f.Compare();
    REQUIRE_FALSE(f.fast.draw_manager.HasDeferredDraw());
    draw(subsequent);
    f.Call(MAXWELL3D_REG_INDEX(bind_groups[4].raw_config), 1 | (3 << 4), true);
    f.Compare();
    draw(subsequent);
    f.Call(MAXWELL3D_REG_INDEX(topology_override), 5, true);
    f.Compare();
    draw(subsequent);
    f.Call(MAXWELL3D_REG_INDEX(global_base_instance_index), 9, true);
    f.Compare();
    REQUIRE_FALSE(f.fast.draw_manager.HasDeferredDraw());
}

TEST_CASE("Maxwell3D constant buffer word runs match single CallMethod writes",
          "[video_core][maxwell_fast_method]") {
    Fixture f;
    std::mt19937 rng{11};
    constexpr u32 subsequent = 1u << 26;
    for (u32 step = 0; step < 20000; ++step) {
        CAPTURE(step);
        switch (rng() % 8) {
        case 0:
            f.Call(ShadowControl, rng() % 4, true);
            break;
        case 1:
            // Near the end too, where words are dropped.
            f.Call(CbSize, rng() % 2 == 0 ? 0x10000 : 0x100 + rng() % 0x40, true);
            break;
        case 2:
            f.Call(CbOffset, rng() % 3 == 0 ? 0xF0 + (rng() % 8) * 4 : (rng() % 0x400) * 4, true);
            break;
        case 3:
            // An instanced loop the first word ends.
            f.Call(DrawBegin, rng() % 2 == 0 ? 0 : subsequent, true);
            f.Call(DrawEnd, 0, true);
            break;
        default: {
            const u32 count = 1 + rng() % 64;
            std::vector<u32> methods(count);
            std::vector<u32> arguments(count);
            const u32 first = CbData + rng() % 16;
            for (u32 i = 0; i < count; ++i) {
                methods[i] = rng() % 2 == 0 ? first : CbData + (first - CbData + i) % 16;
                arguments[i] = rng() % 2 == 0 ? rng() : rng() % 4;
            }
            for (u32 i = 0; i < count; ++i) {
                f.slow.CallMethod(f.system, methods[i], arguments[i], true);
            }
            f.fast.CallConstBufferWords(f.system, methods.data(), arguments.data(), count);
            break;
        }
        }
        f.Compare();
    }
}

TEST_CASE("Maxwell3D state method runs match single CallMethod writes",
          "[video_core][maxwell_fast_method]") {
    Fixture f;
    std::mt19937 rng{19};
    constexpr u64 kind_method = 2;
    constexpr u64 is_last = 1ULL << 4;
    constexpr u64 has_segment = 1ULL << 5;
    constexpr u64 direct = 1ULL << 6;
    for (u32 step = 0; step < 5000; ++step) {
        CAPTURE(step);
        if (rng() % 6 == 0) {
            f.Call(ShadowControl, rng() % 4, true);
            f.Compare();
            continue;
        }
        const size_t run_len = 1 + rng() % 32;
        std::vector<u64> ring(run_len + 1);
        for (size_t i = 0; i < run_len; ++i) {
            const u32 method = f.state_methods[rng() % f.state_methods.size()];
            const u32 arg = RandomArgument(rng, method, f.slow);
            const u64 flags = (rng() % 2 == 0 ? is_last : 0) | (rng() % 2 == 0 ? direct : 0);
            ring[i] = kind_method | flags | (u64{method} << 8) | (u64{arg} << 32);
            f.slow.CallMethod(f.system, method, arg, (flags & is_last) != 0);
        }
        // Append a stopper entry (executable method, HasSegment, or non-Method kind).
        switch (rng() % 3) {
        case 0:
            ring[run_len] = kind_method | (u64{DrawBegin} << 8);
            break;
        case 1:
            ring[run_len] =
                kind_method | has_segment | (u64{f.state_methods[0]} << 8) | (u64{1} << 32);
            break;
        default:
            ring[run_len] = 3; // Kind::Multi
            break;
        }
        const size_t consumed = f.fast.CallStateMethodRun(ring.data(), ring.size());
        REQUIRE(consumed == run_len);
        f.Compare();
    }
}

TEST_CASE("GpuSplit CB batching respects device remaps within GPU big pages",
          "[video_core][maxwell_fast_method][gpu_split_dispatch]") {
    using Access = VideoCommon::GpuSplitTestAccess;
    Fixture f;
    f.system.Initialize();
    Kernel::KProcess process{f.system.Kernel()};
    SCOPE_EXIT { f.system.Kernel().UnregisterKernelObject(&process); };
    auto& table = process.GetPageTable().GetImpl();
    table.Resize(20, 12);
    auto* backing = f.physical.GetPointerFromRaw<u8>(0x10000);
    auto* replacement = f.physical.GetPointerFromRaw<u8>(0x30000);
    for (u64 page = 0; page < 16; ++page) {
        table.entries.GetAndFault(4 + page)
            .Store(false, Common::PageType::Memory, 1,
                   reinterpret_cast<uintptr_t>(backing) - 0x4000);
    }
    table.entries.GetAndFault(0x20).Store(false, Common::PageType::Memory, 1,
                                        reinterpret_cast<uintptr_t>(replacement) - 0x20000);
    process.GetMemory().SetCurrentPageTable(process);
    const auto asid = f.device_memory.RegisterProcess(&process.GetMemory());
    f.device_memory.BindInterface(&f.rasterizer);
    SCOPE_EXIT {
        f.device_memory.Unmap(0x10000, 0x10000);
        f.device_memory.UnregisterProcess(asid);
    };
    f.device_memory.Map(0x10000, 0x4000, 0x10000, asid);
    f.memory.BindRasterizer(&f.rasterizer);
    f.memory.Map(0x100000, 0x10000, 0x10000, Tegra::PTEKind::PITCH, true);
    f.device_memory.Unmap(0x11000, 0x1000);
    f.device_memory.Map(0x11000, 0x20000, 0x1000, asid);
    REQUIRE(f.memory.GetPointer(0x100ffc) == backing + 0xffc);
    REQUIRE(f.memory.GetPointer(0x101000) == replacement);

    for (auto* engine : {&f.slow, &f.fast}) {
        std::memset(backing, 0, 0x10000);
        std::memset(replacement, 0, 0x1000);
        VideoCommon::GpuSplit split;
        Access::Initialize(split, f.system);
        split.MacroWrite(engine, CbAddressHigh, 0);
        split.MacroWrite(engine, CbAddressLow, 0x100ffc);
        split.MacroWrite(engine, CbSize, 8);
        split.MacroWrite(engine, CbOffset, 0);
        split.MacroWrite(engine, CbData, 0x12345678);
        split.MacroWrite(engine, CbData + 1, 0x87654321);
        u64 position{};
        while (position < Access::End(split)) {
            position = Access::Execute(split, position, Access::End(split));
        }
        u32 first{}, second{}, stale{};
        std::memcpy(&first, backing + 0xffc, sizeof(first));
        std::memcpy(&second, replacement, sizeof(second));
        std::memcpy(&stale, backing + 0x1000, sizeof(stale));
        CAPTURE(engine == &f.fast);
        CAPTURE(stale);
        REQUIRE(first == 0x12345678);
        REQUIRE(second == 0x87654321);
        REQUIRE(stale == 0);
        REQUIRE(engine->regs.const_buffer.offset == 8);
    }
    f.Compare();
}

TEST_CASE("GpuSplit dispatch preserves mapped uploads and macro boundaries",
          "[video_core][maxwell_fast_method][gpu_split_dispatch]") {
    using Access = VideoCommon::GpuSplitTestAccess;
    Fixture f;
    f.system.Initialize();
    Kernel::KProcess process{f.system.Kernel()};
    SCOPE_EXIT { f.system.Kernel().UnregisterKernelObject(&process); };
    auto& table = process.GetPageTable().GetImpl();
    table.Resize(20, 12);
    constexpr GPUVAddr gpu_address = 0x100000;
    constexpr DAddr device_address = 0x10000;
    for (u64 page = 0; page < 3; ++page) {
        const VAddr virtual_address = 0x4000 + page * 0x1000;
        auto* backing = f.physical.GetPointerFromRaw<u8>(0x10000 + page * 0x8000);
        table.entries.GetAndFault(virtual_address >> 12)
            .Store(false, Common::PageType::Memory, 1,
                   reinterpret_cast<uintptr_t>(backing) - virtual_address);
    }
    process.GetMemory().SetCurrentPageTable(process);
    const auto asid = f.device_memory.RegisterProcess(&process.GetMemory());
    f.device_memory.BindInterface(&f.rasterizer);
    SCOPE_EXIT {
        f.device_memory.Unmap(device_address, 0x3000);
        f.device_memory.UnregisterProcess(asid);
    };
    f.device_memory.Map(device_address, 0x4000, 0x3000, asid);
    f.memory.BindRasterizer(&f.rasterizer);
    f.memory.Map(gpu_address, device_address, 0x3000, Tegra::PTEKind::PITCH, false);

    struct Snapshot {
        std::array<u8, 0x3000> bytes{};
        std::array<u32, Maxwell3D::Regs::NUM_REGS> regs{};
        std::array<u32, Maxwell3D::Regs::NUM_REGS> shadow{};
        Maxwell3D::DirtyState::Flags dirty{};
        GPUVAddr segment{};
    };
    const auto snapshot = [&](const Maxwell3D& engine) {
        Snapshot result;
        f.memory.ReadBlockUnsafe(gpu_address, result.bytes.data(), result.bytes.size());
        result.regs = engine.regs.reg_array;
        result.shadow = engine.shadow_state.reg_array;
        result.dirty = engine.dirty.flags;
        result.segment = engine.current_dma_segment;
        return result;
    };

    for (const bool wrap : {false, true}) {
        CAPTURE(wrap);
        std::vector<Snapshot> expected;
        for (auto* engine : {&f.slow, &f.fast}) {
            VideoCommon::GpuSplit split;
            struct ProbeEngine final : Tegra::Engines::EngineInterface {
                u32 writes{};
                void CallMethod(Core::System&, u32, u32, bool) override {
                    ++writes;
                }
                void CallMultiMethod(Core::System&, u32, const u32*, u32 amount, u32) override {
                    writes += amount;
                }
            } probe;
            const u64 start = wrap ? Access::NearWrap() : 0;
            Access::Initialize(split, f.system, start);
            std::vector<u64> boundaries;
            std::array<u8, 0x3000> cleared{};
            f.memory.WriteBlockUnsafe(gpu_address, cleared.data(), cleared.size());
            engine->current_dma_segment = 0;
            engine->dirty.flags.reset();
            const auto state = [&](u32 method, u32 value) {
                split.Method(engine, method, value, true, nullptr);
                boundaries.push_back(Access::End(split));
            };
            const auto direct = [&](u32 method, u32 value) {
                split.MacroWrite(engine, method, value);
                boundaries.push_back(Access::End(split));
            };
            size_t observed{};
            const auto inspect = [&] {
                split.Defer([&, engine] {
                    auto actual = snapshot(*engine);
                    if (engine == &f.slow) {
                        expected.push_back(std::move(actual));
                    } else {
                        REQUIRE(observed < expected.size());
                        const auto& reference = expected[observed];
                        REQUIRE(actual.bytes == reference.bytes);
                        REQUIRE(actual.regs == reference.regs);
                        REQUIRE(actual.shadow == reference.shadow);
                        REQUIRE(actual.dirty == reference.dirty);
                        REQUIRE(actual.segment == reference.segment);
                    }
                    ++observed;
                });
                boundaries.push_back(Access::End(split));
            };
            state(ShadowControl, 0);
            state(CbAddressHigh, 0);
            state(CbAddressLow, static_cast<u32>(gpu_address));
            state(CbSize, 0x3000);
            state(CbOffset, 0xffb);
            direct(MAXWELL3D_REG_INDEX(global_base_instance_index), 17);
            inspect();
            for (u32 i = 0; i < 129; ++i) {
                if (i % 2 == 0) {
                    direct(CbData + i % 16, 0x12340000 + i);
                } else {
                    state(CbData + i % 16, 0x56780000 + i);
                }
            }
            inspect();
            state(ShadowControl, 3);
            state(CbOffset, 0x1ffd);
            for (u32 i = 0; i < 33; ++i) {
                direct(CbData + i % 16, 0xdeadbeef);
            }
            inspect();
            state(ShadowControl, 2);
            state(CbOffset, 0x1ffd);
            direct(DrawBegin, 0);
            direct(DrawEnd, 0);
            direct(DrawBegin, 1u << 26);
            direct(DrawEnd, 0);
            direct(CbData, 0x11223344);
            direct(CbData + 1, 0xaabbccdd);
            const GPUVAddr segment = 0x12345678;
            split.Method(engine, CbData + 2, 0x87654321, false, &segment);
            boundaries.push_back(Access::End(split));
            direct(CbData + 3, 0x76543210);
            split.MacroWrite(&probe, 0x10, 42);
            boundaries.push_back(Access::End(split));
            direct(CbData + 4, 0x65432100);
            const std::array<u32, 3> multi{19, 20, 21};
            split.MultiMethod(engine, CbData, multi.data(), 3, 3, &segment);
            boundaries.push_back(Access::End(split));
            inspect();
            state(CbSize, 7);
            state(CbOffset, 0);
            direct(CbData, 0x99aabbcc);
            direct(CbData + 1, 0xddeeff00);
            inspect();

            u64 position = start;
            if (wrap) {
                for (const u64 available : boundaries) {
                    while (position < available) {
                        position = Access::Execute(split, position, available);
                        REQUIRE(position <= available);
                    }
                }
            } else {
                while (position < Access::End(split)) {
                    position = Access::Execute(split, position, Access::End(split));
                    REQUIRE(position <= Access::End(split));
                }
            }
            REQUIRE(observed == expected.size());
            REQUIRE(probe.writes == 1);
        }
        f.Compare();
    }
}

TEST_CASE("GpuSplit CB batching preserves words at the GPU address space end",
          "[video_core][maxwell_fast_method][gpu_split_dispatch]") {
    using Access = VideoCommon::GpuSplitTestAccess;
    Fixture f;
    f.system.Initialize();
    Kernel::KProcess process{f.system.Kernel()};
    SCOPE_EXIT { f.system.Kernel().UnregisterKernelObject(&process); };
    auto& table = process.GetPageTable().GetImpl();
    table.Resize(20, 12);
    auto* backing = f.physical.GetPointerFromRaw<u8>(0x10000);
    table.entries.GetAndFault(4).Store(false, Common::PageType::Memory, 1,
                                     reinterpret_cast<uintptr_t>(backing) - 0x4000);
    process.GetMemory().SetCurrentPageTable(process);
    const auto asid = f.device_memory.RegisterProcess(&process.GetMemory());
    f.device_memory.BindInterface(&f.rasterizer);
    SCOPE_EXIT {
        f.device_memory.Unmap(0x10000, 0x1000);
        f.device_memory.UnregisterProcess(asid);
    };
    f.device_memory.Map(0x10000, 0x4000, 0x1000, asid);
    f.memory.BindRasterizer(&f.rasterizer);
    f.memory.Map(0xfffff000, 0x10000, 0x1000, Tegra::PTEKind::PITCH, false);
    for (auto* engine : {&f.slow, &f.fast}) {
        std::memset(backing, 0, 0x1000);
        VideoCommon::GpuSplit split;
        Access::Initialize(split, f.system);
        split.MacroWrite(engine, CbAddressHigh, 0);
        split.MacroWrite(engine, CbAddressLow, 0xfffffffcu);
        split.MacroWrite(engine, CbSize, 8);
        split.MacroWrite(engine, CbOffset, 0);
        split.MacroWrite(engine, CbData, 0x12345678);
        split.MacroWrite(engine, CbData + 1, 0x87654321);
        u64 position{};
        while (position < Access::End(split)) {
            position = Access::Execute(split, position, Access::End(split));
        }
        u32 word{};
        std::memcpy(&word, backing + 0xffc, sizeof(word));
        CAPTURE(engine == &f.fast);
        REQUIRE(word == 0x12345678);
        REQUIRE(engine->regs.const_buffer.offset == 8);
    }
    f.Compare();
}

TEST_CASE("GpuSplit draw.begin repeat counts match separate entries",
          "[video_core][split_writes][draw_begin_coalesce]") {
    using Access = VideoCommon::GpuSplitTestAccess;
    constexpr auto None = VideoCommon::GpuSplit::Effect::None;
    constexpr u32 first = 4;                  // InstanceId::First (bits 26-27), triangles
    constexpr u32 subsequent = (1u << 26) | 4; // InstanceId::Subsequent
    for (const u32 repeats : {0u, 1u, 254u, 255u, 256u, 600u}) {
        for (const bool counted_on_fast : {true, false}) {
            CAPTURE(repeats, counted_on_fast);
            Fixture f;
            Maxwell3D& counted_engine = counted_on_fast ? f.fast : f.slow;
            Maxwell3D& separate_engine = counted_on_fast ? f.slow : f.fast;
            VideoCommon::GpuSplit counted;
            VideoCommon::GpuSplit separate;
            Access::Initialize(counted, f.system);
            Access::Initialize(separate, f.system);
            counted.MacroWrite(&counted_engine, DrawBegin, first);
            separate.MacroWrite(&separate_engine, DrawBegin, first);
            for (u32 i = 0; i <= repeats; ++i) {
                REQUIRE(counted.RepeatableMethod(&counted_engine, DrawBegin, subsequent, true,
                                                 true, None, false) == (i % 256 != 0));
                separate.MacroWrite(&separate_engine, DrawBegin, subsequent);
            }
            // The engine entry, the first begin, then one entry per 256 subsequent ones, each
            // counting up to 255 repeats.
            const u64 entries = repeats / 256 + 1;
            REQUIRE(Access::End(counted) == 3 + entries);
            for (u64 i = 0; i < entries; ++i) {
                const u64 word = Access::Word(counted, 3 + i);
                REQUIRE(((word >> 24) & 0xff) == (std::min)(repeats - i * 256, u64{255}));
                REQUIRE(((word >> 8) & 0xffff) == DrawBegin);
                REQUIRE(static_cast<u32>(word >> 32) == subsequent);
            }
            for (auto* split : {&counted, &separate}) {
                u64 position{};
                while (position < Access::End(*split)) {
                    position = Access::Execute(*split, position, Access::End(*split));
                }
            }
            REQUIRE(counted_engine.draw_manager.draw_state.instance_count == repeats + 1);
            REQUIRE(counted_engine.draw_manager.draw_state.draw_mode ==
                    Maxwell3D::DrawManager::DrawMode::Instance);
            f.Compare();
        }
    }
}

TEST_CASE("GpuSplit counts a draw.begin repeat only into the unpublished entry before it",
          "[video_core][split_writes][draw_begin_coalesce]") {
    using Access = VideoCommon::GpuSplitTestAccess;
    constexpr auto None = VideoCommon::GpuSplit::Effect::None;
    constexpr u32 subsequent = (1u << 26) | 4;
    Fixture f;
    VideoCommon::GpuSplit split;
    Access::Initialize(split, f.system);
    const auto repeat = [&](u32 argument, bool verify) {
        return split.RepeatableMethod(&f.fast, DrawBegin, argument, true, true, None, verify);
    };
    REQUIRE(!repeat(subsequent, false));
    REQUIRE(repeat(subsequent, false));
    REQUIRE(Access::End(split) == 3);
    split.Publish();
    REQUIRE(!repeat(subsequent, false));
    REQUIRE(Access::End(split) == 4);
    split.MacroWrite(&f.fast, DrawEnd, 0);
    REQUIRE(!repeat(subsequent, false));
    REQUIRE(!repeat(subsequent | 1, false));
    // A different call kind (not from a macro) is another word.
    REQUIRE(!split.RepeatableMethod(&f.fast, DrawBegin, subsequent | 1, true, false, None, false));
    REQUIRE(Access::End(split) == 8);
    // Verify: sent, flagged (bit 7), not counted.
    REQUIRE(!repeat(subsequent, true));
    REQUIRE(repeat(subsequent, true));
    REQUIRE(repeat(subsequent, true));
    REQUIRE(Access::End(split) == 11);
    REQUIRE(((Access::Word(split, 8) >> 7) & 1) == 0);
    for (const u64 position : {9ULL, 10ULL}) {
        const u64 word = Access::Word(split, position);
        REQUIRE(((word >> 7) & 1) == 1);
        REQUIRE(((word >> 24) & 0xff) == 0);
    }
    u64 position{};
    while (position < Access::End(split)) {
        position = Access::Execute(split, position, Access::End(split));
    }
    REQUIRE(f.fast.draw_manager.draw_state.instance_count == 9);
}

TEST_CASE("Maxwell3D fast method dispatch cost", "[.][maxwell_fast_method_bench]") {
    Fixture f;
    // Per draw, as the back thread sees Ocarina's: state writes the front did not drop, a
    // constant buffer update, three binds and a draw.
    std::vector<std::pair<u32, u32>> stream;
    std::mt19937 rng{1};
    for (u32 draw = 0; draw < 4096; ++draw) {
        for (u32 i = 0; i < 12; ++i) {
            stream.emplace_back(f.state_methods[rng() % 512], rng() % 8);
        }
        stream.emplace_back(CbOffset, 0x640);
        stream.emplace_back(CbData, draw);
        stream.emplace_back(CbData + 1, 0);
        stream.emplace_back(CbData + 2, draw * 3);
        for (const u32 stage : {0u, 2u, 4u}) {
            stream.emplace_back(static_cast<u32>(MAXWELL3D_REG_INDEX(bind_groups[0].raw_config)) + stage * 8,
                                1 | (rng() % 8) << 4);
        }
        stream.emplace_back(DrawBegin, 0);
        stream.emplace_back(static_cast<u32>(MAXWELL3D_REG_INDEX(vertex_buffer.first)), rng() % 4);
        stream.emplace_back(static_cast<u32>(MAXWELL3D_REG_INDEX(vertex_buffer.count)), 3 + rng() % 4);
        stream.emplace_back(DrawEnd, 0);
    }
    const auto run = [&](Maxwell3D& engine) {
        const auto start = std::chrono::steady_clock::now();
        for (u32 round = 0; round < 50; ++round) {
            for (const auto& [method, argument] : stream) {
                engine.CallMethod(f.system, method, argument, true);
            }
        }
        return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start)
                   .count() /
               (50.0 * static_cast<double>(stream.size()));
    };
    run(f.slow);
    run(f.fast);
    const double slow_ns = run(f.slow);
    const double fast_ns = run(f.fast);
    WARN("CallMethod: " << slow_ns << " ns per method, fast path " << fast_ns << " ns");

    // Three constant buffer words (a draw macro's c[0][0x640..0x648]) one by one or as a run.
    constexpr std::array<u32, 3> methods{CbData, CbData + 1, CbData + 2};
    constexpr std::array<u32, 3> arguments{1, 2, 3};
    const auto words = [&](bool as_run) {
        const auto start = std::chrono::steady_clock::now();
        for (u32 i = 0; i < 1'000'000; ++i) {
            f.fast.CallMethod(f.system, CbOffset, 0x640, true);
            if (as_run) {
                f.fast.CallConstBufferWords(f.system, methods.data(), arguments.data(), 3);
            } else {
                for (u32 w = 0; w < 3; ++w) {
                    f.fast.CallMethod(f.system, methods[w], arguments[w], true);
                }
            }
        }
        return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start)
                   .count() /
               1'000'000.0;
    };
    words(false);
    words(true);
    const double single_ns = words(false);
    const double run_ns = words(true);
    WARN("Three constant buffer words: " << single_ns << " ns one by one, " << run_ns
                                         << " ns as a run (unmapped buffer)");
}

TEST_CASE("Fast graphics preview takes both bind encodings on every stage",
          "[video_core][nx2_fast_graphics]") {
    PreviewSettings settings;
    Fixture f;
    for (const bool fast_engine : EngineModes) {
        for (const bool inline_form : {true, false}) {
            CAPTURE(fast_engine, inline_form);
            Channel channel{f, fast_engine};
            u64 binds = 0;
            for (u32 stage = 0; stage < StageCount; ++stage) {
                CAPTURE(stage);
                const BindSpec spec{.stage = stage,
                                    .slot = stage + 1,
                                    .size = 0x100 * (stage + 1),
                                    .address_low = static_cast<u32>(MapAddress + stage * 0x100),
                                    .inline_form = inline_form};
                const Words words = BindWords(spec);
                REQUIRE(channel.Feed(words) == words.size());
                ++binds;
                RequireCounters(channel.preview.pusher, {.accepted = binds});
                const auto& events = channel.preview.recorder.events;
                REQUIRE(events.size() == binds);
                REQUIRE(events.back().kind == Event::Kind::Bind);
                REQUIRE(events.back().stage == stage);
                REQUIRE(events.back().slot == stage + 1);
                REQUIRE(events.back().address == spec.address_low);
                REQUIRE(events.back().size == spec.size);
            }
            // The decoder carries on with the next header, in the same call and in the next.
            channel.Feed(DrawWords());
            REQUIRE(channel.preview.recorder.events.back().kind == Event::Kind::Draw);
            RequireCounters(channel.preview.pusher, {.accepted = binds, .other_opcode = 4});
            channel.Feed(Concat({BindWords({.stage = 2, .slot = 5, .inline_form = inline_form}),
                                 DrawWords()}));
            REQUIRE(channel.preview.pusher.FastGraphicsPreviewCounters().accepted == binds + 1);
        }
    }
}

TEST_CASE("Fast graphics preview binds every time, whatever the arguments",
          "[video_core][nx2_fast_graphics]") {
    PreviewSettings settings;
    Fixture f;
    for (const bool fast_engine : EngineModes) {
        CAPTURE(fast_engine);
        Channel channel{f, fast_engine};
        const BindSpec same{.stage = 1, .slot = 3};
        // Three identical binds in one call, then in three calls, then the other encoding.
        channel.Feed(Concat({BindWords(same), BindWords(same), BindWords(same)}));
        for (u32 i = 0; i < 3; ++i) {
            channel.Feed(BindWords(same));
        }
        BindSpec single = same;
        single.inline_form = false;
        channel.Feed(Concat({BindWords(single), BindWords(same), BindWords(single)}));
        // The same slot of another stage, and another address of the same slot.
        channel.Feed(BindWords({.stage = 4, .slot = 3}));
        channel.Feed(BindWords({.stage = 1, .slot = 3, .address_low = static_cast<u32>(MapAddress + 0x40)}));
        RequireCounters(channel.preview.pusher, {.accepted = 11});
        // One rasterizer bind each, in order.
        const auto& events = channel.preview.recorder.events;
        REQUIRE(events.size() == 11);
        for (const Event& event : events) {
            REQUIRE(event.kind == Event::Kind::Bind);
        }
        REQUIRE(events[0] == events[1]);
        REQUIRE(events[1] == events[2]);
        REQUIRE(events[9].stage == 4);
        REQUIRE(events[10].address == MapAddress + 0x40);
    }
}

TEST_CASE("Fast graphics preview drains earlier queued writes once",
          "[video_core][nx2_fast_graphics]") {
    PreviewSettings settings;
    Fixture f;
    const auto methods = PriorStateMethods(f);
    for (const bool fast_engine : EngineModes) {
        for (const bool inline_form : {true, false}) {
            CAPTURE(fast_engine, inline_form);
            Channel channel{f, fast_engine};
            const BindSpec spec{.stage = 3, .slot = 4, .inline_form = inline_form};

            // State writes queue in the engine; the call before the bind leaves them there.
            channel.Feed(Concat({StateWrite(methods[0], 5), StateWrite(methods[1], 6, true),
                                 StateWrite(methods[2], 7)}));
            REQUIRE_FALSE(channel.preview.engine.method_sink.empty());
            channel.Feed(BindWords(spec));
            REQUIRE(channel.preview.engine.method_sink.empty());
            RequireCounters(channel.preview.pusher, {.accepted = 1});

            // The same in one call, with a queued write to a bind register among them: the queue
            // runs first, the three words of the bind after it.
            channel.Feed(Concat({StateWrite(methods[0], 1), StateWrite(CbAddressHigh, 7),
                                 StateWrite(methods[1], 2), BindWords(spec)}));
            REQUIRE(channel.preview.engine.method_sink.empty());
            REQUIRE(channel.preview.engine.regs.const_buffer.address_high == 0);
            RequireCounters(channel.preview.pusher, {.accepted = 2});

            // After an executable write, and with writes queued after the bind.
            channel.Feed(Concat({DrawWords(), StateWrite(methods[0], 3), BindWords(spec),
                                 StateWrite(methods[1], 4)}));
            REQUIRE_FALSE(channel.preview.engine.method_sink.empty());
            channel.Feed(BindWords(spec));
            REQUIRE(channel.preview.engine.method_sink.empty());
            RequireCounters(channel.preview.pusher, {.accepted = 4});
        }
    }
}

TEST_CASE("Fast graphics preview falls back on every truncation of a bind",
          "[video_core][nx2_fast_graphics]") {
    PreviewSettings settings;
    Fixture f;
    for (const bool fast_engine : EngineModes) {
        for (const bool inline_form : {true, false}) {
            const Words all = BindWords({.stage = 2, .slot = 6, .inline_form = inline_form});
            for (size_t cut = 1; cut < all.size(); ++cut) {
                CAPTURE(fast_engine, inline_form, cut);
                Channel channel{f, fast_engine};
                // The first call ends inside the bind: nothing is applied early, the legacy path
                // processes the header, the next call carries on from where it left.
                REQUIRE(channel.Feed(Words(all.begin(), all.begin() + cut)) == cut);
                RequireCounters(channel.preview.pusher, {.incomplete_span = 1});
                channel.Feed(Words(all.begin() + cut, all.end()), cut);
                REQUIRE(channel.preview.pusher.FastGraphicsPreviewCounters().accepted == 0);
                REQUIRE(channel.preview.pusher.FastGraphicsPreviewCounters().incomplete_span == 1);
                REQUIRE(channel.preview.recorder.events.size() == 1);
                // The decoder is where the legacy one is: the next whole bind is taken.
                channel.Feed(all);
                REQUIRE(channel.preview.pusher.FastGraphicsPreviewCounters().accepted == 1);
            }
        }
    }
}

TEST_CASE("Fast graphics preview hands invalid and disabled binds to the legacy path",
          "[video_core][nx2_fast_graphics]") {
    PreviewSettings settings;
    Fixture f;
    struct Case {
        const char* name;
        BindSpec spec;
        bool single_only;
    };
    const std::vector<Case> cases{
        {"slot 18", {.slot = SlotLimit}, false},
        {"slot 31", {.slot = 31}, false},
        {"disabled", {.slot = 2, .enabled = false}, false},
        {"reserved bit 1", {.slot = 2, .extra_config_bits = 0x2}, false},
        {"reserved bit 3", {.slot = 2, .extra_config_bits = 0x8}, false},
        {"reserved bit 9", {.slot = 2, .extra_config_bits = 0x200}, false},
        {"reserved bit 12", {.slot = 2, .extra_config_bits = 0x1000}, false},
        {"reserved bit 16", {.slot = 2, .extra_config_bits = 0x10000}, true},
        {"reserved bit 31", {.slot = 2, .extra_config_bits = 0x80000000u}, true},
        {"zero size", {.slot = 2, .size = 0}, false},
        {"zero address", {.slot = 2, .address_high = 0, .address_low = 0}, false},
        {"address wraps",
         {.slot = 2, .size = 0x100, .address_high = 0xFFFFFFFFu, .address_low = 0xFFFFFFF0u},
         false},
    };
    for (const bool fast_engine : EngineModes) {
        for (const Case& test : cases) {
            for (const bool inline_form : {true, false}) {
                if (inline_form && test.single_only) {
                    continue;
                }
                CAPTURE(fast_engine, test.name, inline_form);
                for (u32 stage = 0; stage < StageCount; stage += 4) {
                    CAPTURE(stage);
                    Channel channel{f, fast_engine};
                    BindSpec spec = test.spec;
                    spec.stage = stage;
                    spec.inline_form = inline_form;
                    channel.Feed(BindWords(spec));
                    RequireCounters(channel.preview.pusher, {.invalid_binding = 1});
                    // The legacy path ran the header once: the same registers, one bind or none.
                    channel.Feed(BindWords({.stage = stage, .slot = 1, .inline_form = inline_form}));
                    RequireCounters(channel.preview.pusher, {.accepted = 1, .invalid_binding = 1});
                }
            }
        }
    }
}

TEST_CASE("Fast graphics preview hands other packet shapes to the legacy path",
          "[video_core][nx2_fast_graphics]") {
    PreviewSettings settings;
    Fixture f;
    const BindSpec spec{.stage = 1, .slot = 2};
    const u32 bind_method = BindGroupBase + BindGroupStride;
    for (const bool fast_engine : EngineModes) {
        CAPTURE(fast_engine);
        const auto run = [&](const char* name, const Words& words, const Expected& expected) {
            CAPTURE(name);
            Channel channel{f, fast_engine};
            channel.Feed(words);
            RequireCounters(channel.preview.pusher, expected);
            channel.Feed(BindWords(spec));
            REQUIRE(channel.preview.pusher.FastGraphicsPreviewCounters().accepted == 1);
        };
        const Words inline_bind{Header(SubmissionMode::Inline, bind_method, spec.Config())};
        // Two or four words from const_buffer.size.
        run("two words",
            Concat({{Header(SubmissionMode::Increasing, CbSize, 2), 0x100, 0}, inline_bind}),
            {.shape_subchannel = 1});
        run("four words",
            Concat({{Header(SubmissionMode::Increasing, CbSize, 4), 0x100, 0,
                     static_cast<u32>(MapAddress), 0},
                    inline_bind}),
            {.shape_subchannel = 1});
        // Not an increasing write: the first header counts, and the bind header after it.
        run("non-increasing",
            Concat({{Header(SubmissionMode::NonIncreasing, CbSize, 3), 0x100, 0,
                     static_cast<u32>(MapAddress)},
                    inline_bind}),
            {.other_opcode = 2});
        run("increase once",
            Concat({{Header(SubmissionMode::IncreaseOnce, CbSize, 3), 0x100, 0,
                     static_cast<u32>(MapAddress)},
                    inline_bind}),
            {.other_opcode = 2});
        run("starts at address high",
            Concat({{Header(SubmissionMode::Increasing, CbAddressHigh, 2), 0,
                     static_cast<u32>(MapAddress)},
                    inline_bind}),
            {.other_opcode = 2});
        // The write after the three is not a bind write.
        run("topology write after",
            Concat({{Header(SubmissionMode::Increasing, CbSize, 3), 0x100, 0,
                     static_cast<u32>(MapAddress)},
                    {Header(SubmissionMode::Inline, TopologyOverride, 2)}}),
            {.shape_subchannel = 1});
        run("bind of two words",
            Concat({{Header(SubmissionMode::Increasing, CbSize, 3), 0x100, 0,
                     static_cast<u32>(MapAddress), Header(SubmissionMode::Increasing, bind_method, 2),
                     spec.Config(), 0}}),
            {.shape_subchannel = 1});
        // The bind write on another subchannel, which is not bound.
        run("bind on subchannel 1", BindWords({.stage = 1, .slot = 2, .subchannel = 1}),
            {.shape_subchannel = 1});
        // The whole bind on a subchannel with no engine: the legacy path drops it.
        run("unbound subchannel",
            Concat({{Header(SubmissionMode::Increasing, CbSize, 3, 1), 0x100, 0,
                     static_cast<u32>(MapAddress)},
                    {Header(SubmissionMode::Inline, bind_method, spec.Config(), 1)}}),
            {.shape_subchannel = 1});
    }
}

TEST_CASE("Fast graphics preview counts every other header as another opcode",
          "[video_core][nx2_fast_graphics]") {
    PreviewSettings settings;
    Fixture f;
    const auto methods = PriorStateMethods(f);
    Channel channel{f, true};
    channel.Feed(Concat({StateWrite(methods[0], 1), DrawWords(), StateWrite(methods[1], 2, true)}));
    // Six headers: the state write, four of the draw, the inline state write.
    RequireCounters(channel.preview.pusher, {.other_opcode = 6});
    channel.preview.pusher.ResetFastGraphicsPreviewCounters();
    RequireCounters(channel.preview.pusher, {.other_opcode = 0});
}

TEST_CASE("Fast graphics preview does nothing with the setting off",
          "[video_core][nx2_fast_graphics]") {
    PreviewSettings settings;
    Fixture f;
    const auto methods = PriorStateMethods(f);
    for (const bool fast_engine : EngineModes) {
        CAPTURE(fast_engine);
        Channel channel{f, fast_engine};
        channel.preview_on = false;
        channel.Feed(Concat({StateWrite(methods[0], 1), BindWords({.stage = 0, .slot = 1}),
                             BindWords({.stage = 4, .slot = 2, .inline_form = false}),
                             DrawWords()}));
        RequireCounters(channel.preview.pusher, {.other_opcode = 0});
        REQUIRE(channel.preview.recorder.events.size() == 3);

        // The setting is read at every call: on takes the bind, off after it does not.
        channel.preview_on = true;
        channel.Feed(BindWords({.stage = 1, .slot = 1}));
        channel.preview_on = false;
        channel.Feed(BindWords({.stage = 2, .slot = 1}));
        channel.preview_on = true;
        channel.Feed(BindWords({.stage = 3, .slot = 1}));
        RequireCounters(channel.preview.pusher, {.accepted = 2});
    }
}

TEST_CASE("Fast graphics preview leaves an engine in a state it cannot reproduce to the legacy path",
          "[video_core][nx2_fast_graphics]") {
    PreviewSettings settings;
    Fixture f;
    const BindSpec spec{.stage = 0, .slot = 7};
    for (const bool fast_engine : EngineModes) {
        CAPTURE(fast_engine);
        SECTION("a deferred instanced draw") {
            Channel channel{f, fast_engine};
            channel.Feed(DrawWords());
            channel.Feed(DrawWords(InstanceSubsequent));
            channel.Feed(DrawWords(InstanceSubsequent));
            REQUIRE(channel.legacy.engine.draw_manager.HasDeferredDraw());
            REQUIRE(channel.preview.engine.draw_manager.HasDeferredDraw());
            channel.Feed(BindWords(spec));
            RequireCounters(channel.preview.pusher, {.ordering = 1});
            // The bind ended the loop; the next one is taken.
            REQUIRE_FALSE(channel.preview.engine.draw_manager.HasDeferredDraw());
            channel.Feed(BindWords(spec));
            RequireCounters(channel.preview.pusher, {.accepted = 1, .ordering = 1});
        }
        SECTION("shadow RAM modes") {
            for (const u32 mode : {0u, 1u, 2u, 3u}) {
                CAPTURE(mode);
                Channel channel{f, fast_engine};
                for (Arm* arm : {&channel.legacy, &channel.preview}) {
                    arm->engine.CallMethod(f.system, ShadowControl, mode, true);
                }
                channel.Feed(BindWords(spec));
                channel.Feed(BindWords({.stage = 2, .slot = 1, .inline_form = false}));
                const auto& counters = channel.preview.pusher.FastGraphicsPreviewCounters();
                if (mode == 3) {
                    // Replay: the shadow RAM supplies the values.
                    RequireCounters(channel.preview.pusher, {.ordering = 2});
                } else {
                    REQUIRE(counters.accepted + counters.ordering == 2);
                }
                if (mode == 0 || mode == 2) {
                    REQUIRE(counters.accepted == 2);
                }
            }
        }
    }
}

TEST_CASE("Fast graphics preview waits behind a pending semaphore acquire",
          "[video_core][nx2_fast_graphics]") {
    PreviewSettings settings;
    Fixture f;
    MappedMemory mapped{f};
    Tegra::g_nx2_ampere.store(true);
    constexpr GPUVAddr semaphore = MapAddress + 0x2ff0;
    constexpr u32 acquire_equal = 1; // Puller::GpuSemaphoreOperation::AcquireEqual
    mapped.Write({}, semaphore);
    for (const bool inline_form : {true, false}) {
        CAPTURE(inline_form);
        Channel channel{f, true};
        // SEM_ADDR_HI, SEM_ADDR_LO, payload, trigger: the word holds 0, the acquire wants 1.
        channel.Feed({Header(SubmissionMode::Increasing, 4, 4), 0, static_cast<u32>(semaphore), 1,
                      acquire_equal});
        REQUIRE(channel.legacy.pusher.HasPendingAcquire());
        REQUIRE(channel.preview.pusher.HasPendingAcquire());
        // A suspended channel returns after each word; the legacy path then does the bind
        // alone, word by word, and the preview must not have done it first.
        channel.FeedAll(Concat({BindWords({.stage = 1, .slot = 2, .inline_form = inline_form}),
                                BindWords({.stage = 3, .slot = 1, .inline_form = inline_form})}));
        // Each of the two binds is looked at once, while the acquire waits.
        RequireCounters(channel.preview.pusher, {.ordering = 2});
        REQUIRE(channel.preview.recorder.events.size() == 2);
    }
}

TEST_CASE("Fast graphics preview leaves a split GPU thread to the legacy path",
          "[video_core][nx2_fast_graphics]") {
    using Access = VideoCommon::GpuSplitTestAccess;
    PreviewSettings settings;
    Fixture f;
    auto& split = VideoCommon::GpuSplit::Get();
    SCOPE_EXIT { Access::Reset(split); };
    for (const bool inline_form : {true, false}) {
        CAPTURE(inline_form);
        const auto commands = ToHeaders(
            Concat({BindWords({.stage = 1, .slot = 2, .inline_form = inline_form}),
                    BindWords({.stage = 2, .slot = 3, .inline_form = inline_form})}));
        Arm legacy{f, true, 0};
        Arm preview{f, true, 0};
        std::array<std::vector<u64>, 2> rings;
        std::array<size_t, 2> consumed{};
        size_t index = 0;
        for (Arm* arm : {&legacy, &preview}) {
            Access::Initialize(split, f.system);
            Access::SetActive(split, true);
            consumed[index] = arm->Run(commands, 0, arm == &preview);
            Access::SetActive(split, false);
            for (u64 position = 0; position < Access::End(split); ++position) {
                rings[index].push_back(Access::Word(split, position));
            }
            ++index;
        }
        // Every write went to the split's ring, the same ones in the same order.
        REQUIRE(consumed[0] == consumed[1]);
        REQUIRE_FALSE(rings[0].empty());
        REQUIRE(rings[0] == rings[1]);
        CompareArms(legacy, preview);
        // Both binds were looked at, and neither taken.
        RequireCounters(preview.pusher, {.ordering = 2});
    }
}

TEST_CASE("Fast graphics preview leaves replay and other renderers to the legacy path",
          "[video_core][nx2_fast_graphics]") {
    PreviewSettings settings;
    Fixture f;
    SECTION("a replay watches the stream") {
        VideoCore::Replay::SetReplayGpuRunning(true);
        Channel channel{f, true};
        channel.Feed(BindWords({.stage = 1, .slot = 2}));
        RequireCounters(channel.preview.pusher, {.observer = 1});
        channel.Feed(BindWords({.stage = 1, .slot = 2, .inline_form = false}));
        RequireCounters(channel.preview.pusher, {.observer = 2});
        VideoCore::Replay::SetReplayGpuRunning(false);
        channel.Feed(BindWords({.stage = 1, .slot = 2}));
        RequireCounters(channel.preview.pusher, {.accepted = 1, .observer = 2});
    }
    SECTION("not the Vulkan renderer") {
        for (const auto backend :
             {Settings::RendererBackend::OpenGL_GLSL, Settings::RendererBackend::Null}) {
            Settings::values.renderer_backend.SetValue(backend);
            Channel channel{f, true};
            channel.Feed(BindWords({.stage = 1, .slot = 2}));
            RequireCounters(channel.preview.pusher, {.backend_engine = 1});
        }
    }
}

TEST_CASE("Fast graphics preview keeps interleaved channels apart",
          "[video_core][nx2_fast_graphics]") {
    PreviewSettings settings;
    Fixture f;
    const auto methods = PriorStateMethods(f);
    Channel a{f, true, 0};
    Channel b{f, true, 1};
    const Words bind_a = BindWords({.stage = 0, .slot = 1, .size = 0x40});
    const Words bind_b = BindWords({.stage = 3, .slot = 9, .size = 0x80, .inline_form = false});

    // A queues a write and is cut inside its bind while B binds twice.
    a.Feed(StateWrite(methods[0], 3));
    a.Feed(Words(bind_a.begin(), bind_a.begin() + 3));
    b.Feed(bind_b);
    b.Feed(BindWords({.stage = 4, .slot = 2}));
    RequireCounters(b.preview.pusher, {.accepted = 2});
    RequireCounters(a.preview.pusher, {.incomplete_span = 1});
    REQUIRE(b.preview.recorder.events.size() == 2);
    REQUIRE(a.preview.recorder.events.empty());
    a.Feed(Words(bind_a.begin() + 3, bind_a.end()), 3);
    REQUIRE(a.preview.recorder.events.size() == 1);
    REQUIRE(a.preview.recorder.events[0].size == 0x40);

    // A's queued write waits for A's own bind, whatever B did meanwhile.
    a.Feed(StateWrite(methods[1], 9));
    b.Feed(bind_b);
    REQUIRE_FALSE(a.preview.engine.method_sink.empty());
    REQUIRE(b.preview.engine.method_sink.empty());
    a.Feed(bind_a);
    REQUIRE(a.preview.engine.method_sink.empty());
    REQUIRE(a.preview.engine.regs.const_buffer.size == 0x40);
    RequireCounters(a.preview.pusher, {.accepted = 1, .incomplete_span = 1});
    RequireCounters(b.preview.pusher, {.accepted = 3});
    REQUIRE(a.preview.recorder.events.size() == 2);
    REQUIRE(b.preview.recorder.events.size() == 3);
}

TEST_CASE("Fast graphics preview shows a write to the bound memory at the next draw",
          "[video_core][nx2_fast_graphics]") {
    PreviewSettings settings;
    Fixture f;
    MappedMemory mapped{f};
    // The other device range, with its own bytes.
    mapped.Remap(MappedMemory::DeviceB);
    mapped.Write(Pattern(0x80));
    mapped.Remap(MappedMemory::DeviceA);
    for (const bool inline_form : {true, false}) {
        CAPTURE(inline_form);
        mapped.Write(Pattern(0x10));
        Channel channel{f, true};
        channel.Feed(BindWords({.stage = 1, .slot = 2, .inline_form = inline_form}));
        RequireCounters(channel.preview.pusher, {.accepted = 1});

        // Bytes written after the bind and before the draw are the ones the draw sees.
        channel.Feed(DrawWords());
        REQUIRE(channel.preview.recorder.events.back().bytes == DrawBytes(1, 2, Pattern(0x10)));
        mapped.Write(Pattern(0x40));
        channel.Feed(DrawWords());
        REQUIRE(channel.preview.recorder.events.back().bytes == DrawBytes(1, 2, Pattern(0x40)));
        REQUIRE(channel.legacy.recorder.events.back().bytes == DrawBytes(1, 2, Pattern(0x40)));

        // The same GPU address over other device memory, remapped after the bind.
        mapped.Remap(MappedMemory::DeviceB);
        channel.Feed(DrawWords());
        REQUIRE(channel.preview.recorder.events.back().bytes == DrawBytes(1, 2, Pattern(0x80)));
        REQUIRE(channel.legacy.recorder.events.back().bytes == DrawBytes(1, 2, Pattern(0x80)));

        // A bind made while remapped, then back.
        channel.Feed(BindWords({.stage = 4, .slot = 5, .inline_form = inline_form}));
        mapped.Remap(MappedMemory::DeviceA);
        channel.Feed(DrawWords());
        const auto& bytes = channel.preview.recorder.events.back().bytes;
        REQUIRE(bytes == [&] {
            std::vector<u8> expected = DrawBytes(1, 2, Pattern(0x40));
            const auto second = DrawBytes(4, 5, Pattern(0x40));
            expected.insert(expected.end(), second.begin(), second.end());
            return expected;
        }());
        RequireCounters(channel.preview.pusher, {.accepted = 2});
    }
}
