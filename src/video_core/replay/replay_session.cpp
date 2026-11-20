// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// The replay of a capture without the game (replay_session.h). Decisions that need no GPU live in
// replay_internal.h and are tested there; this file drives the GPU with them. The seams other
// slices fill in are in replay_host.h; until one is registered, Run fails with its name.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "common/frame_timing_log.h"
#include "common/run_outcome.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "core/core.h"
#include "video_core/control/channel_state.h"
#include "video_core/dma_pusher.h"
#include "video_core/framebuffer_config.h"
#include "video_core/gpu.h"
#include "video_core/host1x/host1x.h"
#include "video_core/host1x/syncpoint_manager.h"
#include "video_core/memory_manager.h"
#include "video_core/nx2.h"
#include "video_core/replay/effect.h"
#include "video_core/textures/decoders.h"
#include "video_core/replay/format_io.h"
#include "video_core/replay/options.h"
#include "video_core/replay/replay_host.h"
#include "video_core/replay/replay_internal.h"
#include "video_core/replay/replay_session.h"
#include "video_core/replay/run_state.h"

namespace VideoCore::Replay {

namespace {

std::mutex g_host_mutex;
ReplayHostFns g_host;

using Clock = std::chrono::steady_clock;

[[nodiscard]] double MillisecondsSince(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

[[nodiscard]] std::string DetailWithName(ReadError error, const std::string& detail) {
    return detail.empty() ? std::string(ReadErrorName(error)) : detail;
}

// A host backing the replay created for one allocation id and generation.
struct Backing {
    DAddr device_address = 0;
    u8* host_bytes = nullptr;
    u64 size = 0;
    RangeWriteLog log;
};

struct ChannelRef {
    std::shared_ptr<Tegra::Control::ChannelState> owner;  // set for channels the replay created
    Tegra::Control::ChannelState* state = nullptr;
    std::shared_ptr<Tegra::MemoryManager> memory;
    u32 space = 0;  // the capture's address space id of `memory`
};

// Everything one Run keeps. A failure sets `error_` and stops the loop; nothing is skipped.
class Replayer {
public:
    Replayer(const ReplayConfig& config, CaptureReader& reader, const FileHeader& header,
             const SessionRecord& session, const FrameSelection& selection,
             const PinDecision& pins, const std::atomic<bool>& stop, ReplayReport& report)
        : config_{config}, reader_{reader}, header_{header}, session_{session},
          selection_{selection}, pins_{pins}, stop_{stop}, report_{report},
          host_{GetReplayHost()} {}

    // Runs the capture. On false, `error_` holds the reason (prefixed when it is not a defect of
    // the capture).
    bool Run(Core::System& system, Core::Frontend::EmuWindow& window) {
        system_ = &system;
        const Clock::time_point start = Clock::now();
        asked_ = FramesAsked(selection_.selected, config_.max_frames);
        report_.frames_expected = asked_;

        if (!CheckSeams()) {
            return false;
        }
        if (!PrepareOutput()) {
            return false;
        }
        report_.replay_digest = config_.benchmark ? 0 : Fnv1aOffset;

        SetAcceptedPresentBase(PresentBaseBeforeFirst(header_.first_frame, header_.preroll_frames));
        SetReplayGpuRunning(true);
        const auto end_replay = ::detail::MakeScopeGuard([] { SetReplayGpuRunning(false); });
        // The checkpoint restore refuses a generation other than the recorded one.
        Tegra::g_nx2_ampere.store(
            header_.gpu_architecture == static_cast<u32>(GpuArchitecture::Ampere),
            std::memory_order_relaxed);

        std::string setup_error;
        if (!host_.setup(system, window, setup_error)) {
            error_ = std::string(StartupPrefix) + DetailWithName(ReadError::None, setup_error);
            return false;
        }
        struct TeardownGuard {
            Core::System& system;
            void (*teardown)(Core::System&);
            ~TeardownGuard() {
                if (teardown != nullptr) {
                    teardown(system);
                }
            }
        } teardown{system, host_.teardown};

        gpu_ = &system.GPU();
        gpu_->Start();
        host_.set_readback(!config_.benchmark);

        {
            std::scoped_lock lock{effect_mutex_};
            active_ = this;
        }
        SetReplayEffectObserver(&ObserveEffect);
        const auto reset_observer = ::detail::MakeScopeGuard([] {
            SetReplayEffectObserver(nullptr);
            std::scoped_lock lock{effect_mutex_};
            active_ = nullptr;
        });
        const bool ok = Replay(start);
        report_.replay_ms = MillisecondsSince(start) - report_.setup_ms;
        if (frames_file_.is_open()) {
            frames_file_.close();
            effects_file_.close();
            if (!frames_file_ || !effects_file_) {
                return Fail(std::string(ReportIoPrefix) + "cannot finalize replay ledgers");
            }
        }
        // Quiesce before the guard stops the GPU thread.
        if (gpu_ != nullptr && gpu_dirty_) {
            gpu_->WaitForIdle();
        }
        return ok;
    }

    [[nodiscard]] const std::string& Error() const noexcept {
        return error_;
    }
    [[nodiscard]] ReadError ErrorKind() const noexcept {
        return error_kind_;
    }
    [[nodiscard]] bool Interrupted() const noexcept {
        return interrupted_;
    }

private:
    bool Fail(std::string message, ReadError kind = ReadError::None) {
        RecordStop(error_, error_kind_, std::move(message), kind);
        return false;
    }

    // A GPU hook can run on another thread. Its rows stay in observation order, including
    // repeated effects. Never populate this ledger from FRAME_END or an expectation file.
    static void ObserveEffect(const LiveEffect& effect) noexcept {
        std::scoped_lock lock{effect_mutex_};
        if (!active_) {
            return;
        }
        auto& run = *active_;
        if (run.effects_failed_ || run.effects_.size() >= MaxStagingBytes / sizeof(LiveEffect)) {
            run.effects_failed_ = true;
            return;
        }
        try {
            run.effects_.push_back(effect);
        } catch (...) {
            run.effects_failed_ = true;
        }
    }

    bool CloseEffects(const FrameEndRecord& end, bool preroll) {
        std::scoped_lock lock{effect_mutex_};
        if (effects_failed_) {
            return Fail("observed effect ledger overflow");
        }
        if (!preroll && effects_file_.is_open()) {
            for (u32 i = 0; i < effects_.size(); ++i) {
                effects_file_ << EffectRow(end.frame_id, i, effects_[i]).dump() << '\n';
            }
            effects_file_.flush();
            if (!effects_file_) {
                return Fail(std::string(ReportIoPrefix) + "cannot write effects.jsonl");
            }
        }
        if (effects_.size() != end.effect_count) {
            return Fail("observed effect count differs from FRAME_END on frame " +
                        std::to_string(end.frame_id));
        }
        effects_.clear();
        return true;
    }

    bool CheckSeams() {
        const auto missing = [this](const char* name) {
            return Fail(std::string(StartupPrefix) + "replay host seam '" + name +
                        "' is not registered");
        };
        if (host_.setup == nullptr) {
            return missing("setup");
        }
        if (host_.channel == nullptr) {
            return missing("channel");
        }
        if (host_.address_space == nullptr) {
            return missing("address_space");
        }
        if (host_.map_backing == nullptr) {
            return missing("map_backing");
        }
        if (host_.set_readback == nullptr) {
            return missing("set_readback");
        }
        if (!config_.benchmark && host_.read_picture == nullptr) {
            return missing("read_picture");
        }
        return true;
    }

    bool PrepareOutput() {
        if (config_.images_dir.empty() || config_.benchmark) {
            return true;
        }
        const std::filesystem::path dir{config_.images_dir};
        std::string error;
        if (!CheckPrivateOutput(dir, OutputKind::Directory, error)) {
            return Fail("pictures directory refused: " + error);
        }
        if (!MarkDirectoryPrivate(dir, error)) {
            return Fail(std::string(ReportIoPrefix) + error);
        }
        effects_file_.open(dir / "effects.jsonl", std::ios::binary | std::ios::trunc);
        if (!effects_file_) {
            return Fail(std::string(ReportIoPrefix) + "cannot create effects.jsonl");
        }
        frames_file_.open(dir / "frames.jsonl", std::ios::binary | std::ios::trunc);
        if (!frames_file_.is_open()) {
            return Fail(std::string(ReportIoPrefix) + "cannot create frames.jsonl");
        }
        return true;
    }

    [[nodiscard]] bool ResultLost() {
        const Common::RunOutcome::Reason reason = Common::RunOutcome::CurrentReason();
        if (reason == Common::RunOutcome::Reason::DeviceLost) {
            Fail("the graphics device was lost");
            return true;
        }
        if (reason == Common::RunOutcome::Reason::ShaderFailure) {
            Fail("a shader failed to build");
            return true;
        }
        return false;
    }

    bool Replay(Clock::time_point start) {
        reader_.Rewind();
        u64 expect_sequence = 0;
        bool setup_done = false;
        while (true) {
            if (stop_.load(std::memory_order_acquire)) {
                interrupted_ = true;
                return Fail(std::string(InterruptedText));
            }
            ChunkView view;
            bool at_end = false;
            std::string detail;
            const ReadError read = reader_.Next(view, at_end, detail);
            if (read != ReadError::None) {
                return Fail("capture read failed: " + DetailWithName(read, detail));
            }
            if (at_end) {
                break;
            }
            if (view.header.sequence != expect_sequence) {
                return Fail("chunk sequence is not contiguous");
            }
            ++expect_sequence;
            if (!Apply(view, setup_done, start)) {
                return false;
            }
            if (ResultLost()) {
                return false;
            }
            if (report_.frames_replayed >= asked_) {
                break;
            }
        }
        if (report_.frames_replayed < asked_) {
            return Fail("capture ended after " + std::to_string(report_.frames_replayed) +
                        " of " + std::to_string(asked_) + " selected frames");
        }
        if (report_.frames_replayed == selection_.selected && !AllSubmitsComplete()) {
            return Fail("capture ends with a submit whose words never arrived");
        }
        return true;
    }

    bool Apply(const ChunkView& view, bool& setup_done, Clock::time_point start) {
        const std::span<const u8> payload{view.payload};
        const u64 frame_id = view.header.frame_id;
        switch (view.type) {
        case ChunkType::Session:
        case ChunkType::LibrarySchema:
        case ChunkType::LibraryCheckpoint:
        case ChunkType::LibraryEvent:
        case ChunkType::Index:
        case ChunkType::Complete:
            return true;
        case ChunkType::GpuCheckpoint: {
            GpuCheckpoint checkpoint;
            const ReadError decoded = Decode(payload, checkpoint);
            if (decoded != ReadError::None) {
                return Fail("checkpoint chunk does not decode");
            }
            std::string error;
            if (!RestoreGpuCheckpoint(*gpu_, checkpoint, error)) {
                // The GPU keeps the channels it made before the refusal and cannot release them:
                // it is unusable after this and the teardown discards it.
                return Fail("checkpoint cannot be restored: " + error +
                                " (the GPU is unusable after a refused restore)",
                            ReadError::UnsupportedState);
            }
            {
                auto& syncpoints = system_->Host1x().GetSyncpointManager();
                for (u32 id = 0; id < SyncpointLimit; ++id) {
                    syncpoint_baseline_[id] = syncpoints.GetHostSyncpointValue(id);
                }
            }
            for (const ChannelEntry& entry : checkpoint.channels) {
                if (!AdoptChannel(entry.channel_id, entry.address_space_id)) {
                    return false;
                }
            }
            if (!setup_done) {
                setup_done = true;
                report_.setup_ms = MillisecondsSince(start);
            }
            return true;
        }
        case ChunkType::MemoryBlob:
        case ChunkType::ExternalWrite: {
            MemoryBlob blob;
            if (Decode(payload, blob) != ReadError::None) {
                return Fail("memory chunk does not decode");
            }
            return ApplyMemory(blob, view.header.sequence, view.type == ChunkType::ExternalWrite);
        }
        case ChunkType::Map: {
            std::vector<MapEntry> maps;
            if (Decode(payload, maps) != ReadError::None) {
                return Fail("map chunk does not decode");
            }
            for (const MapEntry& map : maps) {
                if (!ApplyMap(map)) {
                    return false;
                }
            }
            return true;
        }
        case ChunkType::Unmap: {
            std::vector<UnmapEntry> unmaps;
            if (Decode(payload, unmaps) != ReadError::None) {
                return Fail("unmap chunk does not decode");
            }
            for (const UnmapEntry& unmap : unmaps) {
                if (!ApplyUnmap(unmap)) {
                    return false;
                }
            }
            return true;
        }
        case ChunkType::Fence: {
            std::vector<FenceEntry> fences;
            if (Decode(payload, fences) != ReadError::None) {
                return Fail("fence chunk does not decode");
            }
            return ApplyFences(fences);
        }
        case ChunkType::GpuEvent: {
            std::vector<GpuEvent> events;
            if (Decode(payload, events) != ReadError::None) {
                return Fail("gpu event chunk does not decode");
            }
            for (const GpuEvent& event : events) {
                if (!ApplyEvent(event)) {
                    return false;
                }
            }
            return true;
        }
        case ChunkType::Present: {
            BeginFrame(frame_id);
            PresentRecord present;
            if (Decode(payload, present) != ReadError::None) {
                return Fail("present chunk does not decode");
            }
            return ApplyPresent(present, frame_id);
        }
        case ChunkType::EndPicture: {
            BeginFrame(frame_id);
            EndPicture picture;
            if (Decode(payload, picture) != ReadError::None) {
                return Fail("picture chunk does not decode");
            }
            if (picture.kind == PictureKind::Composed) {
                live_ = std::move(picture);
            }
            return true;
        }
        case ChunkType::FrameEnd: {
            BeginFrame(frame_id);
            FrameEndRecord end;
            if (Decode(payload, end) != ReadError::None) {
                return Fail("frame end chunk does not decode");
            }
            return FinishFrame(end, frame_id);
        }
        }
        return Fail("chunk type is unknown");
    }

    // ---- channels ----------------------------------------------------------------------

    bool AdoptChannel(u32 channel_id, u32 address_space_id) {
        if (channels_.count(channel_id) != 0) {
            return Fail("checkpoint names a channel twice");
        }
        ChannelRef ref;
        ref.state = host_.channel(*system_, channel_id);
        ref.memory = host_.address_space(*system_, address_space_id);
        ref.space = address_space_id;
        if (ref.state == nullptr || ref.memory == nullptr || !ref.state->payload.has_value()) {
            return Fail("the restored checkpoint left no channel " + std::to_string(channel_id));
        }
        channels_.emplace(channel_id, std::move(ref));
        return true;
    }

    bool ApplyEvent(const GpuEvent& event) {
        switch (event.kind) {
        case GpuEventKind::ChannelCreate:
            return CreateChannel(event.channel_create);
        case GpuEventKind::ChannelRelease:
            return ReleaseChannel(event.channel_release.channel_id);
        case GpuEventKind::Submit:
            return QueueSubmit(event.submit);
        case GpuEventKind::Consume:
            return FeedConsume(event.consume);
        }
        return Fail("gpu event kind is unknown");
    }

    bool CreateChannel(const ChannelCreateEvent& create) {
        BeginFrameIfIdle();
        if (channels_.count(create.channel_id) != 0) {
            return Fail("channel " + std::to_string(create.channel_id) + " is created twice");
        }
        auto memory = host_.address_space(*system_, create.address_space_id);
        if (memory == nullptr) {
            return Fail("channel names address space " + std::to_string(create.address_space_id) +
                        " that does not exist");
        }
        Quiesce();
        ChannelRef ref;
        ref.owner = gpu_->AllocateChannel();
        ref.state = ref.owner.get();
        ref.memory = memory;
        ref.space = create.address_space_id;
        gpu_->BindAddressSpace(*ref.state, std::move(memory));
        gpu_->InitChannel(*ref.state, 0);
        if (!ref.state->payload.has_value()) {
            return Fail("a new channel has no pusher");
        }
        channels_.emplace(create.channel_id, std::move(ref));
        return true;
    }

    // The GPU has no way to release a channel (GPU::ReleaseChannel is unimplemented), so the
    // replay forgets it and keeps the object alive until the run ends.
    bool ReleaseChannel(u32 channel_id) {
        const auto it = channels_.find(channel_id);
        if (it == channels_.end()) {
            return Fail("channel " + std::to_string(channel_id) + " is released but never existed");
        }
        const auto queue = pending_.find(channel_id);
        if (queue != pending_.end() && !queue->second.empty()) {
            return Fail("channel is released with an incomplete submit");
        }
        if (it->second.owner != nullptr) {
            retired_.push_back(std::move(it->second.owner));
        }
        channels_.erase(it);
        pending_.erase(channel_id);
        return true;
    }

    // ---- submits -----------------------------------------------------------------------

    bool QueueSubmit(const SubmitEvent& submit) {
        BeginFrameIfIdle();
        if (channels_.count(submit.channel_id) == 0) {
            return Fail("submit names channel " + std::to_string(submit.channel_id) +
                        " that does not exist");
        }
        std::string error;
        if (!QueueSubmitEvent(pending_[submit.channel_id], submit, error)) {
            return Fail(error);
        }
        ObserveEffect({EffectKind::Submit, submit.channel_id, submit.entries.size(), true});
        return true;
    }

    // The words of a consume reach the pusher now, between the events recorded before and after
    // it: a prefix the original consumed ahead of an external write runs before that write.
    bool FeedConsume(const ConsumeEvent& consume) {
        BeginFrameIfIdle();
        const auto queue = pending_.find(consume.channel_id);
        if (queue == pending_.end()) {
            return Fail("consume names a channel with no submit");
        }
        SubmitPiece piece;
        std::string error;
        if (!FeedConsumeEvent(queue->second, consume, piece, error)) {
            return Fail(error);
        }
        PushPiece(channels_.at(consume.channel_id), piece);
        Quiesce(); // Finish engine effects and the actual consume before the next recorded event.
        return true;
    }

    void PushPiece(const ChannelRef& channel, const SubmitPiece& piece) {
        if (piece.words.empty()) {
            return;
        }
        Tegra::CommandList list;
        Tegra::CommandListHeader raw{};
        raw.raw = SubmitHeaderRaw(piece.gpu_va, static_cast<u32>(piece.words.size()),
                                  piece.header_flags);
        list.command_lists.push_back(raw);
        list.submitted_offsets.push_back(0);
        list.submitted_words.reserve(piece.words.size());
        for (const u32 word : piece.words) {
            Tegra::CommandHeader header{};
            header.argument = word;
            list.submitted_words.push_back(header);
        }
        list.snapshot_suspensions =
            channel.state->payload->dma_pusher.suspensions.load(std::memory_order_acquire);
        gpu_->PushGPUEntries(channel.state->bind_id, std::move(list));
        gpu_dirty_ = true;
    }

    [[nodiscard]] bool AllSubmitsComplete() const {
        for (const auto& [channel_id, queue] : pending_) {
            if (!queue.empty()) {
                return false;
            }
        }
        return true;
    }

    // Memory and mappings change only while the GPU is idle: every submit queued before the
    // change finishes first, so two replays see the same order.
    void Quiesce() {
        if (gpu_dirty_) {
            gpu_->WaitForIdle();
            gpu_dirty_ = false;
        }
    }

    // ---- memory ------------------------------------------------------------------------

    bool ApplyMemory(const MemoryBlob& blob, u64 sequence, bool external) {
        BeginFrameIfIdle();
        const std::pair<u64, u32> key{blob.alloc_id, blob.generation};
        auto it = backings_.find(key);
        const bool fresh = it == backings_.end();
        if (fresh) {
            if (external) {
                return Fail("external write names an allocation that has no backing");
            }
            if (blob.allocation_size == 0 ||
                blob.allocation_size > session_.limits.max_initial_backing_bytes) {
                return Fail("allocation size is zero or over the session limit");
            }
            Backing backing;
            backing.size = blob.allocation_size;
            std::string error;
            if (!host_.map_backing(*system_, blob.alloc_id, blob.allocation_size,
                                   backing.device_address, backing.host_bytes, error) ||
                backing.host_bytes == nullptr) {
                return Fail(std::string(StartupPrefix) + "cannot back allocation " +
                            std::to_string(blob.alloc_id) + ": " + error);
            }
            it = backings_.emplace(key, std::move(backing)).first;
        } else {
            Quiesce();
        }
        if (!fresh && std::ranges::any_of(blob.ranges, [](const MemoryRange& range) {
                return range.kind == RangeKind::Undefined;
            })) {
            // Previously mapped GPU addresses could otherwise continue reading stale bytes.
            return Fail("cannot replay an update that invalidates defined backing",
                        ReadError::UnsupportedState);
        }
        Backing& backing = it->second;
        written_.clear();
        std::string error;
        if (!ApplyBlobToBytes(blob, sequence,
                              std::span<u8>{backing.host_bytes, static_cast<size_t>(backing.size)},
                              backing.log, written_, error)) {
            return Fail(error);
        }
        if (!fresh) {
            for (const WrittenRange& range : written_) {
                gpu_->InvalidateRegion(backing.device_address + range.offset, range.length);
            }
            gpu_dirty_ = true;
        }
        return true;
    }

    bool ApplyMap(const MapEntry& map) {
        BeginFrameIfIdle();
        if (map.size == 0 || map.gpu_va >= GpuAddressLimit ||
            map.size > GpuAddressLimit - map.gpu_va || map.pte_kind > 0xFFU) {
            return Fail("map range is empty, outside the GPU address space or has a bad kind");
        }
        if (!MapFlagsReplayable(map.flags)) {
            return Fail("reserved mappings cannot be replayed");
        }
        auto memory = host_.address_space(*system_, map.address_space_id);
        if (memory == nullptr) {
            return Fail("map names address space " + std::to_string(map.address_space_id) +
                        " that does not exist");
        }
        const bool big_pages = (map.flags & MapBigPages) != 0;
        Quiesce();
        if ((map.flags & MapRemap) != 0) {
            memory->PreserveMapping(map.gpu_va, static_cast<size_t>(map.size));
            memory->Unmap(map.gpu_va, static_cast<size_t>(map.size));
        }
        if ((map.flags & MapSparse) != 0) {
            (void)memory->MapSparse(map.gpu_va, static_cast<size_t>(map.size), big_pages);
            return true;
        }
        const auto backing = backings_.find({map.alloc_id, map.generation});
        if (backing == backings_.end()) {
            return Fail("map names an allocation that has no backing yet");
        }
        if (map.alloc_offset > backing->second.size ||
            map.size > backing->second.size - map.alloc_offset) {
            return Fail("map reaches past its allocation");
        }
        if (!backing->second.log.UnchangedSince(map.alloc_offset, map.size, UINT64_MAX)) {
            return Fail("map requires undefined bytes");
        }
        (void)memory->Map(map.gpu_va, backing->second.device_address + map.alloc_offset,
                          static_cast<size_t>(map.size), static_cast<Tegra::PTEKind>(map.pte_kind),
                          big_pages);
        return true;
    }

    bool ApplyUnmap(const UnmapEntry& unmap) {
        BeginFrameIfIdle();
        if (unmap.size == 0 || unmap.gpu_va >= GpuAddressLimit ||
            unmap.size > GpuAddressLimit - unmap.gpu_va) {
            return Fail("unmap range is empty or outside the GPU address space");
        }
        auto memory = host_.address_space(*system_, unmap.address_space_id);
        if (memory == nullptr) {
            return Fail("unmap names address space " + std::to_string(unmap.address_space_id) +
                        " that does not exist");
        }
        Quiesce();
        memory->PreserveMapping(unmap.gpu_va, static_cast<size_t>(unmap.size));
        memory->Unmap(unmap.gpu_va, static_cast<size_t>(unmap.size));
        return true;
    }

    // ---- fences ------------------------------------------------------------------------

    bool ApplyFences(const std::vector<FenceEntry>& fences) {
        BeginFrameIfIdle();
        for (const FenceEntry& fence : fences) {
            std::string error;
            if (!ValidateFence(fence, error)) {
                return Fail(error);
            }
            if (fence.kind == FenceKind::SyncpointRaised) {
                if (!RaiseSyncpoint(static_cast<u32>(fence.id), static_cast<u32>(fence.value))) {
                    return false;
                }
                const u32 observed = system_->Host1x().GetSyncpointManager().GetHostSyncpointValue(
                    static_cast<u32>(fence.id));
                ObserveEffect({EffectKind::FenceSignal, fence.id, observed, true});
                continue;
            }
            const auto channel = channels_.find(fence.channel_id);
            if (channel == channels_.end()) {
                return Fail("fence names channel " + std::to_string(fence.channel_id) +
                            " that does not exist");
            }
            const u32 bytes = FenceWriteBytes(fence);
            Quiesce();
            if (!channel->second.memory->IsFullyMappedRange(fence.id, bytes)) {
                return Fail("memory fence address is not mapped");
            }
            if (bytes == 4) {
                const u32 value = static_cast<u32>(fence.value);
                channel->second.memory->WriteBlock(fence.id, &value, bytes);
            } else {
                channel->second.memory->WriteBlock(fence.id, &fence.value, bytes);
            }
            u64 observed{};
            channel->second.memory->ReadBlock(fence.id, &observed, bytes);
            ObserveEffect({EffectKind::FenceSignal, fence.id, observed, true});
        }
        // Wakes a channel suspended on the value that was just written.
        gpu_->TickWork();
        gpu_dirty_ = true;
        return true;
    }

    bool RaiseSyncpoint(u32 id, u32 value) {
        auto& manager = system_->Host1x().GetSyncpointManager();
        constexpr u32 MaxRaise = 1U << 20;
        const u32 current = manager.GetHostSyncpointValue(id);
        if (value > current && value - current > MaxRaise) {
            return Fail("syncpoint fence is too far from the replay's syncpoint value");
        }
        // The guest value is what the NX2 channel waits read; the producers' fences carry the
        // value the guest saw, so both counters are brought up to it.
        const u32 guest = manager.GetGuestSyncpointValue(id);
        if (value > guest && value - guest > MaxRaise) {
            return Fail("syncpoint fence is too far from the replay's guest syncpoint value");
        }
        while (manager.GetGuestSyncpointValue(id) < value) {
            manager.IncrementGuest(id);
        }
        while (manager.GetHostSyncpointValue(id) < value) {
            manager.IncrementHost(id);
        }
        return true;
    }

    // ---- frames ------------------------------------------------------------------------

    void BeginFrame(u64 frame_id) {
        if (!frame_open_) {
            frame_open_ = true;
            frame_id_ = frame_id;
            frame_start_ = Clock::now();
        }
    }

    void BeginFrameIfIdle() {
        if (!frame_open_) {
            frame_open_ = true;
            frame_start_ = Clock::now();
        }
    }

    bool ApplyPresent(const PresentRecord& present, u64 frame_id) {
        std::string error;
        if (!ValidatePresent(present, frame_id, error)) {
            return Fail(error);
        }
        // The id the live run counted must be the id the replay counts: the ledger agrees.
        const u64 counted = CountAcceptedPresent();
        if (counted != present.present_id) {
            return Fail("accepted present " + std::to_string(counted) + " is not capture present " +
                        std::to_string(present.present_id));
        }
        // The GPU is idle before the compose reads the pictures. Idle is stronger than the
        // producing fence and does not depend on the replay's syncpoint values starting where
        // the live run's did.
        gpu_->WaitForIdle();
        gpu_dirty_ = false;

        std::vector<Tegra::FramebufferConfig> layers;
        layers.reserve(present.layers.size());
        for (const PresentLayer& layer : present.layers) {
            const auto backing = backings_.find({layer.alloc_id, layer.generation});
            if (backing == backings_.end()) {
                return Fail("present layer names an allocation that has no backing");
            }
            using Format = Service::android::PixelFormat;
            const auto format = static_cast<Format>(layer.pixel_format);
            if (format != Format::Rgba8888 && format != Format::Rgbx8888 &&
                format != Format::Bgra8888 && format != Format::Rgb565 && format != Format::Rgba1010102) {
                return Fail("unsupported present pixel format", ReadError::UnsupportedState);
            }
            if (layer.stride < layer.width || layer.stride > MaxPictureDimension ||
                layer.alloc_offset > backing->second.size ||
                layer.framebuffer_offset > backing->second.size - layer.alloc_offset ||
                layer.crop_left < 0 || layer.crop_top < 0 ||
                layer.crop_right > static_cast<s32>(layer.width) ||
                layer.crop_bottom > static_cast<s32>(layer.height)) {
                return Fail("invalid present stride, crop or offset");
            }
            // Layer::UpdateRawImage reads the block-linear padded allocation, not just the
            // visible rows. Undefined padding cannot be silently sourced from the host heap.
            const u64 tiled = Tegra::Texture::CalculateSize(true, format == Format::Rgb565 ? 2 : 4,
                                                            layer.stride, layer.height, 1, 4, 0);
            if (!backing->second.log.UnchangedSince(layer.alloc_offset + layer.framebuffer_offset,
                                                    tiled, UINT64_MAX)) {
                return Fail("present requires undefined tiled backing");
            }
            Tegra::FramebufferConfig config;
            config.address = backing->second.device_address + layer.alloc_offset;
            config.offset = layer.framebuffer_offset;
            config.width = layer.width;
            config.height = layer.height;
            config.stride = layer.stride;
            config.pixel_format = static_cast<Service::android::PixelFormat>(layer.pixel_format);
            config.transform_flags =
                static_cast<Service::android::BufferTransformFlags>(layer.transform_flags);
            config.crop_rect = Common::Rectangle<int>(layer.crop_left, layer.crop_top,
                                                     layer.crop_right, layer.crop_bottom);
            config.blending = static_cast<Tegra::BlendMode>(layer.blending);
            config.native_tiling = (layer.flags & LayerNativeTiling) != 0;
            config.layer_stack_mask = layer.layer_stack_mask;
            layers.push_back(config);
        }
        constexpr u32 default_stack = Service::Nvnflinger::DefaultLayerStackMask;
        if (std::ranges::none_of(layers, [](const auto& layer) {
                return (layer.layer_stack_mask & default_stack) != 0;
            })) {
            return Fail("present has no composable layer on the default stack");
        }
        gpu_->RequestComposite(counted, std::move(layers), {});
        gpu_->WaitForComposite();
        ObserveEffect({EffectKind::PresentCompose, counted, present.output_id, true});
        have_present_ = true;
        return true;
    }

    // Reads `bytes` at a GPU address in the address space the output was recorded in. An output
    // that names no space is read when one space maps it (see ResolveOutputSpace).
    bool ReadGpu(u32 space_tag, u64 gpu_va, u32 bytes, u64& value) {
        std::vector<u32> mapping;
        for (auto& [channel_id, channel] : channels_) {
            if (channel.memory->IsFullyMappedRange(gpu_va, bytes)) {
                mapping.push_back(channel.space);
            }
        }
        u32 space = 0;
        if (!ResolveOutputSpace(space_tag, mapping, space)) {
            return false;
        }
        for (auto& [channel_id, channel] : channels_) {
            if (channel.space == space && channel.memory->IsFullyMappedRange(gpu_va, bytes)) {
                u64 raw = 0;
                channel.memory->ReadBlock(gpu_va, &raw, bytes);
                value = raw;
                return true;
            }
        }
        return false;
    }

    // The outputs the replay observes, for the keys the capture recorded. An output that cannot
    // be observed is left out, which CompareSemantics counts as a mismatch.
    std::vector<SemanticOutput> Observe(const std::vector<SemanticOutput>& expected) {
        std::vector<SemanticOutput> observed;
        observed.reserve(expected.size());
        for (const SemanticOutput& want : expected) {
            SemanticOutput got = want;
            got.value_lo = 0;
            got.value_hi = 0;
            switch (want.kind) {
            case SemanticKind::SemaphoreValue:
                if (!ReadGpu(want.space_tag, want.id, SemaphoreBytes, got.value_lo)) {
                    continue;
                }
                break;
            case SemanticKind::QueryResult:
                if (!ReadGpu(want.space_tag, want.id, QueryBytes, got.value_lo)) {
                    continue;
                }
                break;
            case SemanticKind::SyncpointValue:
                if (want.id >= SyncpointLimit) {
                    continue;
                }
                got.value_lo = system_->Host1x().GetSyncpointManager().GetHostSyncpointValue(
                    static_cast<u32>(want.id));
                break;
            case SemanticKind::SyncpointDelta:
                if (want.id >= SyncpointLimit) {
                    continue;
                }
                got.value_lo = static_cast<u32>(
                    system_->Host1x().GetSyncpointManager().GetHostSyncpointValue(
                        static_cast<u32>(want.id)) -
                    syncpoint_baseline_[static_cast<size_t>(want.id)]);
                break;
            case SemanticKind::ReadbackChecksum: {
                // The newest generation of the allocation, the bytes [0, length).
                auto it = backings_.upper_bound({want.id, UINT32_MAX});
                if (it == backings_.begin()) {
                    continue;
                }
                --it;
                if (it->first.first != want.id || want.value_hi > it->second.size) {
                    continue;
                }
                gpu_->FlushRegion(it->second.device_address, want.value_hi);
                got.value_lo = Fnv1a64(std::span<const u8>{
                    it->second.host_bytes, static_cast<size_t>(want.value_hi)});
                got.value_hi = want.value_hi;
                break;
            }
            default:
                continue;
            }
            observed.push_back(got);
        }
        return observed;
    }

    bool WritePicture(const std::string& name, std::span<const u8> bgra, u32 width, u32 height,
                      u32 stride) {
        std::vector<u8> rgba;
        std::vector<u8> png;
        std::string error;
        if (!SwizzleBgraToRgba(bgra, width, height, stride, rgba) ||
            !EncodeRgbaPng(rgba, width, height, png, error)) {
            return Fail("picture " + name + " cannot be encoded: " + error);
        }
        std::ofstream file{std::filesystem::path{config_.images_dir} / name,
                           std::ios::binary | std::ios::trunc};
        file.write(reinterpret_cast<const char*>(png.data()),
                   static_cast<std::streamsize>(png.size()));
        file.close();
        if (!file) {
            return Fail(std::string(ReportIoPrefix) + "cannot write " + name);
        }
        return true;
    }

    bool FinishFrame(const FrameEndRecord& end, u64 frame_id) {
        if (end.frame_id != frame_id) {
            return Fail("frame end names another frame than its chunk");
        }
        if (!have_present_) {
            return Fail("frame " + std::to_string(frame_id) + " has no present");
        }
        const bool preroll = FrameIsPreroll(end.flags);
        Quiesce();
        gpu_->WaitForIdle();
        const u64 work_ns = static_cast<u64>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - frame_start_)
                .count());
        if (!CloseEffects(end, preroll)) {
            return false;
        }
        if (preroll) {
            ++report_.preroll_frames_executed;
            ResetFrame();
            return true;
        }
        Common::FrameTiming::Record(frame_id);
        ReplayFrameRecord record;
        record.frame_id = frame_id;
        record.replay_ns = work_ns;
        record.call_count = end.call_count;
        record.effect_count = end.effect_count;
        const std::vector<SemanticOutput> observed = Observe(end.outputs);
        record.semantic_mismatches = static_cast<u32>(CompareSemantics(end.outputs, observed));
        report_.semantic_mismatches += record.semantic_mismatches;

        if (!config_.benchmark) {
            if (!live_.has_value()) {
                return Fail("frame " + std::to_string(frame_id) + " has no live picture");
            }
            std::string error;
            if (!ValidateLivePicture(*live_, frame_id, error)) {
                return Fail(error);
            }
            u32 width = 0;
            u32 height = 0;
            std::vector<u8> bgra;
            if (!host_.read_picture(width, height, bgra, error)) {
                return Fail(std::string(StartupPrefix) + "picture readback failed: " + error);
            }
            const ChecksumResult replay = PictureChecksum(bgra, width, height, width * 4U);
            const ChecksumResult live =
                PictureChecksum(live_->pixels, live_->width, live_->height, live_->stride_bytes);
            if (!replay.ok || !live.ok || bgra.size() != u64{width} * height * 4U) {
                return Fail("a picture of frame " + std::to_string(frame_id) +
                            " cannot be checksummed");
            }
            record.width = live_->width;
            record.height = live_->height;
            record.live_checksum = live.hash;
            record.replay_checksum = replay.hash;
            if (!config_.images_dir.empty()) {
                if (!WritePicture(LivePictureName(frame_id), live_->pixels, live_->width,
                                  live_->height, live_->stride_bytes) ||
                    !WritePicture(ReplayPictureName(frame_id), bgra, width, height, width * 4U)) {
                    return false;
                }
                report_.pictures_written += PicturesWritten(1);
                frames_file_ << ReplayFrameLine(record) << '\n';
                frames_file_.flush();
                if (!frames_file_) {
                    return Fail(std::string(ReportIoPrefix) + "cannot write frames.jsonl");
                }
            }
            if (width != live_->width || height != live_->height) {
                return Fail("replay picture of frame " + std::to_string(frame_id) +
                            " has another size than the live picture");
            }
            if (record.replay_checksum == record.live_checksum) {
                ++report_.identical_frames;
            }
            report_.replay_digest = ChainDigest(report_.replay_digest, record.replay_checksum);
        }
        if (record.semantic_mismatches != 0) {
            return Fail("semantic output differs on frame " + std::to_string(frame_id));
        }
        ++report_.frames_replayed;
        ResetFrame();
        return true;
    }

    void ResetFrame() {
        frame_open_ = false;
        have_present_ = false;
        live_.reset();
    }

    const ReplayConfig& config_;
    CaptureReader& reader_;
    const FileHeader& header_;
    const SessionRecord& session_;
    const FrameSelection& selection_;
    const PinDecision& pins_;
    const std::atomic<bool>& stop_;
    ReplayReport& report_;
    const ReplayHostFns host_;

    Core::System* system_ = nullptr;
    Tegra::GPU* gpu_ = nullptr;
    u64 asked_ = 0;
    std::string error_;
    ReadError error_kind_ = ReadError::None;
    bool interrupted_ = false;
    bool gpu_dirty_ = false;
    // Host syncpoint values when the replay started (after the checkpoint is restored), the base
    // of every SyncpointDelta output.
    std::array<u32, SyncpointLimit> syncpoint_baseline_{};

    std::map<std::pair<u64, u32>, Backing> backings_;
    std::map<u32, ChannelRef> channels_;
    std::map<u32, std::deque<PendingSubmit>> pending_;
    std::vector<std::shared_ptr<Tegra::Control::ChannelState>> retired_;

    bool frame_open_ = false;
    bool have_present_ = false;
    u64 frame_id_ = 0;
    Clock::time_point frame_start_{};
    std::optional<EndPicture> live_;
    inline static std::mutex effect_mutex_;
    inline static Replayer* active_ = nullptr;
    std::vector<WrittenRange> written_;
    std::vector<LiveEffect> effects_;
    bool effects_failed_ = false;
    std::ofstream effects_file_;
    std::ofstream frames_file_;
};

[[nodiscard]] SectionStatus StatusOf(bool ok, bool interrupted) {
    if (ok) {
        return SectionStatus::Complete;
    }
    return interrupted ? SectionStatus::Incomplete : SectionStatus::Failed;
}

} // namespace

void SetReplayHost(const ReplayHostFns& fns) {
    std::scoped_lock lock{g_host_mutex};
    g_host = fns;
}

const ReplayHostFns& GetReplayHost() {
    return g_host;
}

struct ReplaySession::Impl {
    ReplayConfig config;
    CaptureReader reader;
    FileHeader header;
    SessionRecord session;
    CompleteRecord complete;
    FrameSelection selection;
    PinDecision pins;
    Phase phase = Phase::Closed;
};

ReplaySession::ReplaySession() : impl{std::make_unique<Impl>()} {}

ReplaySession::~ReplaySession() = default;

ReadError ReplaySession::Open(const ReplayConfig& config, std::string& detail) {
    Impl& s = *impl;
    const PhaseStep step = AdvancePhase(s.phase, PhaseEvent::Opened);
    if (!step.allowed) {
        detail = "the capture is already open";
        return ReadError::BadOrdering;
    }
    const auto fail = [&s, &detail](ReadError error, std::string text) {
        s.phase = Phase::Failed;
        detail = std::move(text);
        return error;
    };
    s.config = config;

    std::array<u8, FileHeaderBytes> head{};
    size_t head_size = 0;
    {
        std::ifstream file{std::filesystem::path{config.capture_path}, std::ios::binary};
        if (!file) {
            return fail(ReadError::Truncated, "cannot open the capture file");
        }
        file.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(head.size()));
        head_size = static_cast<size_t>(file.gcount());
    }
    const PrefixVerdict prefix =
        ClassifyCapturePrefix(config.capture_path, std::span<const u8>{head.data(), head_size});
    if (prefix.error != ReadError::None) {
        return fail(prefix.error, prefix.detail);
    }

    std::string text;
    ReadError error = s.reader.Open(std::filesystem::path{config.capture_path}, text);
    if (error != ReadError::None) {
        return fail(error, DetailWithName(error, text));
    }
    error = s.reader.Validate(text);
    if (error != ReadError::None) {
        return fail(error, DetailWithName(error, text));
    }
    s.header = s.reader.Header();
    s.complete = s.reader.Complete();
    error = RejectUnclean(s.complete, text);
    if (error != ReadError::None) {
        return fail(error, text);
    }
    if ((s.header.flags & HeaderComplete) == 0) {
        return fail(ReadError::NotComplete, "capture header is not marked complete");
    }
    error = SelectFrames(s.header, s.reader.Index(), s.selection, text);
    if (error != ReadError::None) {
        return fail(error, text);
    }
    // Without live pictures there is nothing to compare: only a benchmark can use the capture.
    if (!config.benchmark && (s.header.flags & HeaderHasLivePictures) == 0) {
        return fail(ReadError::UnsupportedState,
                    "capture holds no live pictures; replay it with --replay-benchmark");
    }
    if (!config.benchmark && (s.header.optional_features & OptionalLivePictures) == 0) {
        return fail(ReadError::UnsupportedState, "capture does not declare live pictures");
    }

    s.reader.Rewind();
    ChunkView view;
    bool at_end = false;
    error = s.reader.Next(view, at_end, text);
    if (error != ReadError::None) {
        return fail(error, DetailWithName(error, text));
    }
    if (at_end || view.type != ChunkType::Session) {
        return fail(ReadError::BadOrdering, "the first chunk is not the session");
    }
    SequenceCheck sequence;
    error = RejectDecodedChunk(sequence, view.header, view.payload, text);
    if (error != ReadError::None) {
        return fail(error, text);
    }
    error = Decode(view.payload, s.session);
    if (error != ReadError::None) {
        return fail(error, "session chunk does not decode");
    }
    s.pins = InterpretPins(s.session.pins);
    if (!s.pins.ok) {
        return fail(ReadError::UnsupportedState, s.pins.error);
    }
    s.phase = step.next;
    detail.clear();
    return ReadError::None;
}

const FileHeader& ReplaySession::Header() const noexcept {
    return impl->header;
}

const SessionRecord& ReplaySession::Session() const noexcept {
    return impl->session;
}

bool ReplaySession::ApplyPins(std::string& error) {
    Impl& s = *impl;
    const PhaseStep step = AdvancePhase(s.phase, PhaseEvent::PinsApplied);
    if (!step.allowed) {
        error = "pins are applied once, after the capture is open";
        return false;
    }
    const PinDecision& pins = s.pins;
    Settings::values.renderer_backend.SetValue(Settings::RendererBackend::Vulkan);
    Settings::values.vsync_mode.SetValue(Settings::VSyncMode::Immediate);
    Settings::values.use_asynchronous_gpu_emulation.SetValue(false);
    if (pins.has_docked) {
        Settings::values.use_docked_mode.SetValue(static_cast<Settings::ConsoleMode>(pins.docked));
    }
    if (pins.has_accuracy) {
        const auto accuracy = static_cast<Settings::GpuAccuracy>(pins.accuracy);
        Settings::values.gpu_accuracy.SetValue(accuracy);
        Settings::values.current_gpu_accuracy = accuracy;
    }
    if (pins.has_resolution) {
        Settings::values.resolution_setup.SetValue(
            static_cast<Settings::ResolutionSetup>(pins.resolution));
    }
    if (pins.has_anti_aliasing) {
        Settings::values.anti_aliasing.SetValue(
            static_cast<Settings::AntiAliasing>(pins.anti_aliasing));
    }
    if (pins.has_scaling) {
        Settings::values.scaling_filter.SetValue(
            static_cast<Settings::ScalingFilter>(pins.scaling));
    }
    if (pins.has_astc) {
        Settings::values.astc_recompression.SetValue(
            static_cast<Settings::AstcRecompression>(pins.astc));
    }
    s.phase = step.next;
    error.clear();
    return true;
}

ReplayReport ReplaySession::Run(Core::System& system, Core::Frontend::EmuWindow& window,
                                const std::atomic<bool>& stop) {
    Impl& s = *impl;
    ReplayReport report;
    report.benchmark = s.config.benchmark;
    report.format_major = s.header.format_major;
    report.format_minor = s.header.format_minor;
    report.capture_session_id = s.header.session_id;
    report.capture_build_id = s.header.build_id;
    report.settings_hash = s.header.settings_hash;
    report.dependency_complete = s.complete.dependency_complete == 1;
    report.frames_expected = FramesAsked(s.selection.selected, s.config.max_frames);

    const PhaseStep step = AdvancePhase(s.phase, PhaseEvent::Running);
    if (!step.allowed) {
        report.status = SectionStatus::Failed;
        report.error = "Run needs an open capture with its pins applied";
        PublishReplayReport(report);
        return report;
    }
    s.phase = step.next;

    Replayer replayer{s.config, s.reader, s.header, s.session, s.selection, s.pins, stop, report};
    const bool ok = replayer.Run(system, window);

    report.run_complete = ok && report.frames_replayed >= report.frames_expected;
    report.status = StatusOf(report.run_complete, replayer.Interrupted());
    report.error = report.run_complete ? std::string{} : replayer.Error();
    report.error_kind = report.run_complete ? std::string{} : ErrorKindText(replayer.ErrorKind());
    if (!report.run_complete && report.error.empty()) {
        report.error = "the replay stopped before every requested frame ran";
    }
    s.phase = AdvancePhase(s.phase, report.run_complete ? PhaseEvent::Done : PhaseEvent::Failed)
                  .next;
    PublishReplayReport(report);
    return report;
}

} // namespace VideoCore::Replay
