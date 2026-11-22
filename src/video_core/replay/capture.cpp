// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// The capture recorder. Contract: capture.h, events.h, format.h, checkpoint.h,
// library_call.h. The memory model lives in memory_capture.h/.cpp, the state checkpoint in
// checkpoint.cpp, the file codec in format_io.cpp.
//
// Threads. Hooks run on the guest threads (map, CPU write, submit, present), the GPU thread
// (consume, GPU writes and reads) and the render thread (frame composed). They all take the one
// mutex of the capture; nothing under it does file I/O. Chunks go to a StagingWriter (bounded
// queue plus one writer thread), so a slow disk costs memory up to max_staging_bytes and then a
// recorded overflow, never a stalled guest. The one exception is the initial snapshot at the
// checkpoint cut, which may wait for the writer to free staging room (see StagingWriter::Stage).
//
// Order. Events are staged in the order the hooks observe them. A PRESENT closes the frame's
// events; the frame ends (END_PICTURE, FRAME_END) only when its composite finished on the render
// thread, which can be after the next frame's events were observed. Those events are held in
// memory and staged behind FRAME_END, so the file keeps the grammar of format.h.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <nlohmann/json.hpp>
#include <deque>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <tuple>
#include <utility>
#include <vector>

#include "common/logging.h"
#include "common/run_outcome.h"
#include "common/scm_rev.h"
#include "video_core/nx2.h" // Tegra::g_nx2_ampere
#include "video_core/replay/capture.h"
#include "video_core/replay/capture_host.h"
#include "video_core/replay/checkpoint.h"
#include "video_core/replay/events.h"
#include "video_core/replay/format.h"
#include "video_core/replay/format_io.h"
#include "video_core/replay/library_call.h"
#include "video_core/replay/memory_capture.h"
#include "video_core/replay/options.h"
#include "video_core/replay/present_extent.h"
#include "video_core/replay/result.h"
#include "video_core/replay/run_state.h"

namespace VideoCore::Replay {

nlohmann::json EffectRow(u64 frame, u32 ordinal, const LiveEffect& effect) {
    constexpr std::array names{"submit", "consume", "draw", "compute_dispatch", "syncpoint_increment",
                               "fence_signal", "readback_visibility", "present_compose"};
    nlohmann::json row{{"schema", 1}, {"frame_id", frame}, {"ordinal", ordinal},
                       {"kind", names.at(static_cast<size_t>(effect.kind))}};
    if (effect.observable) {
        row["classification"] = "supported";
        row["payload"] = {{"id", effect.id}, {"value", effect.value}};
        row["outcome"] = "observed";
    } else {
        row["classification"] = "unobservable";
        row["reason"] = effect.kind == EffectKind::ReadbackVisibility
            ? "the cache download hook does not expose the bytes visible to the guest at this instant"
            : "the fence result is device-timed or calculated when it signals";
    }
    return row;
}

// ---- registries and host seams ----------------------------------------------------------------

namespace {
std::atomic<LibraryCallSource*> g_library_source{nullptr};
std::atomic<u64> g_call_seq{0};
std::atomic<u64> g_open_frame{0};
std::mutex g_output_stats_mutex;
CaptureOutputStats g_output_stats;

} // namespace

void RegisterLibrarySource(LibraryCallSource* source) noexcept {
    g_library_source.store(source, std::memory_order_release);
}

LibraryCallSource* RegisteredLibrarySource() noexcept {
    return g_library_source.load(std::memory_order_acquire);
}

// ---- the capture ----------------------------------------------------------------------------

namespace {

constexpr u64 NoSequence = ~u64{0};
constexpr u32 NoOrdinal = ~u32{0};
constexpr size_t BatchEventLimit = 4096;
constexpr u64 BatchByteLimit = 1ULL << 20;
constexpr u32 LibraryBatchRecords = 4096;
constexpr u64 LibraryBatchBytes = 1ULL << 20;
constexpr u64 TrackerWindowBytesForCapture = 512ULL << 20;

// Cutting: the checkpoint is being taken; hooks are staged (see Capture::Staging).
enum class State { WaitingCut, Cutting, Recording, Finished, Failed };

// A chunk that waits behind a frame whose composite has not finished. Blobs are kept unencoded
// because their Ref ordinals are translated to file sequences when they are staged.
struct HeldItem {
    ChunkType type = ChunkType::GpuEvent;
    u64 frame = 0;
    std::vector<u8> payload;
    bool is_blob = false;
    BlobChunk blob;
    u64 weight = 0;
};

struct SubmitInfo {
    u64 submit_id = 0;
    u32 last_entry = 0;  // index of the last entry that carries words
};

// A host readback waiting for FRAME_END, where its checksum is taken.
struct ReadbackTarget {
    u64 alloc_id = 0;
    u64 device = 0;
    u64 length = 0;
};

constexpr u64 ReadbackChunkBytes = 1ULL << 20;
constexpr u64 MaxReadbackBytes = 64ULL << 20;

// Hooks that arrive while the checkpoint is cut, kept as closures until it is in the file.
constexpr u64 MaxCutStageBytes = 64ULL << 20;
constexpr size_t MaxCutStageEntries = 1U << 20;
thread_local bool t_draining = false;  // this thread is replaying the staged hooks

struct AwaitingFrame {
    u64 id = 0;
    bool preroll = false;
    bool present_staged = false;
    u64 gpu_words = 0;
    u64 calls = 0;
    std::vector<LiveEffect> effects;
    u64 first_sequence = ~u64{0};
    std::vector<SemanticOutput> outputs;  // sorted by (kind, id)
    std::vector<ReadbackTarget> readbacks;
};

// Collects the library checkpoint records into one LIBRARY_CHECKPOINT payload.
class LibrarySink final : public RecordSink {
public:
    explicit LibrarySink(u64 max_bytes_) : max_bytes{max_bytes_} {}

    bool Append(const CallRecordView& record) override {
        const size_t before = buffer.size();
        if (!Encode(record, buffer) || buffer.size() > max_bytes) {
            buffer.resize(before);
            ++dropped;
            return false;
        }
        ++count;
        return true;
    }

    std::vector<u8> buffer;
    u32 count = 0;
    u64 dropped = 0;
    u64 max_bytes = 0;
};

void PutU32(std::vector<u8>& out, u32 value) {
    for (u32 shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<u8>((value >> shift) & 0xFFU));
    }
}

[[nodiscard]] u32 Saturate32(u64 value) noexcept {
    return static_cast<u32>((std::min)(value, u64{std::numeric_limits<u32>::max()}));
}

[[nodiscard]] u64 NonZeroRandom() {
    std::random_device device;
    std::mt19937_64 engine{(u64{device()} << 32) ^ device() ^
                           static_cast<u64>(std::chrono::steady_clock::now().time_since_epoch().count())};
    u64 value = 0;
    while (value == 0) {
        value = engine();
    }
    return value;
}

[[nodiscard]] u64 HashText(const char* text) noexcept {
    return Fnv1a64(std::span<const u8>{reinterpret_cast<const u8*>(text), std::strlen(text)});
}

// A fence hook call kept as data, so the checkpoint cut can stage it (see Capture::Cutting).
struct FenceNote {
    FenceKind kind = FenceKind::SyncpointRaised;
    s32 channel = 0;
    u32 space = 0;
    u64 id = 0;
    u64 value = 0;
    u64 aux = 0;
    u64 device = 0;
    bool external = false;  // producer: a device outside the recorded commands made it
    bool blocked = false;   // wait: the command had to suspend or spin
};

constexpr u32 SyncpointCount = 192;
constexpr u64 GpuAddressBound = 1ULL << 40;
constexpr u32 MaxSyncpointRaise = 1U << 20;  // the same limit as ReplaySession::RaiseSyncpoint

// Wrap-safe "have has reached want" for 32-bit syncpoint values.
[[nodiscard]] bool WrapAtLeast(u64 have, u64 want) noexcept {
    return static_cast<s32>(static_cast<u32>(have) - static_cast<u32>(want)) >= 0;
}

// Bytes a memory fence value occupies: semaphore 4, query and timestamp 8, `aux` 4 or 8 overrides.
[[nodiscard]] u32 FenceWidth(const FenceNote& note) noexcept {
    if (note.kind == FenceKind::SyncpointRaised) {
        return 0;
    }
    if (note.aux == 4 || note.aux == 8) {
        return static_cast<u32>(note.aux);
    }
    return note.kind == FenceKind::SemaphoreWrite ? 4U : 8U;
}

[[nodiscard]] const char* FenceName(FenceKind kind) noexcept {
    if (kind == FenceKind::SyncpointRaised) {
        return "syncpoint";
    }
    if (kind == FenceKind::SemaphoreWrite) {
        return "semaphore";
    }
    if (kind == FenceKind::QueryValue) {
        return "query";
    }
    if (kind == FenceKind::Timestamp) {
        return "timestamp";
    }
    return "unknown fence";
}

// False with a named reason when the note cannot be stored in a FENCE entry.
[[nodiscard]] bool FenceNoteValid(const FenceNote& note, std::string& why) {
    if (note.kind == FenceKind::SyncpointRaised) {
        if (note.id >= SyncpointCount || note.value > 0xFFFFFFFFULL) {
            why = "a syncpoint fence is outside the syncpoint table or wider than 32 bits";
            return false;
        }
        return true;
    }
    if (note.kind != FenceKind::SemaphoreWrite && note.kind != FenceKind::QueryValue &&
        note.kind != FenceKind::Timestamp) {
        why = "a fence of unknown kind was reported";
        return false;
    }
    if (note.id >= GpuAddressBound || (FenceWidth(note) == 4 && note.value > 0xFFFFFFFFULL)) {
        why = "a memory fence is outside the GPU address space or its value is wider than its word";
        return false;
    }
    return true;
}

class Capture {
public:
    Capture(std::unique_ptr<CaptureHost> host_, const CaptureConfig& config_)
        : host{std::move(host_)}, config{config_}, sink{config_.limits} {}

    [[nodiscard]] bool Begin(std::string& error);
    CaptureReport End(bool run_ended_normally);

    void AcceptedPresent(u64 id, PresentPath path, std::span<const Tegra::FramebufferConfig> layers,
                         std::span<const Service::Nvidia::NvFence> fences);
    void FrameComposed(u64 id);
    void GraphicsCall();
    void EngineEffect(const LiveEffect& effect);
    void DrainFinal();
    void ReleasePins();
    void Submit(s32 channel, const Tegra::CommandList& list);
    void Consume(s32 channel, u32 entry_index, u64 gpu_va, u32 word_offset, u32 flags, u32 epoch,
                 std::span<const u32> words);
    void GuestWrite(u64 device, u64 size);
    void GpuMap(u32 space, u64 gpu_va, u64 size, u64 device, u32 flags, u32 pte_kind);
    void GpuMapSparse(u32 space, u64 gpu_va, u64 size, u32 flags);
    void GpuUnmap(u32 space, u64 gpu_va, u64 size);
    void GpuWrite(u64 device, u64 size);
    void GpuRead(u64 device, u64 size);
    void ExternalWrite(u64 device, u64 size);
    void ChannelCreate(s32 channel, u32 space, u32 flags);
    void ChannelRelease(s32 channel);
    void FenceProduced(const FenceNote& note);
    void FenceWait(const FenceNote& note);
    void HostReadback(u64 device, u64 size);
    void OutputNotComparable();
    [[nodiscard]] bool LibraryRecord(const CallTicket& ticket, const CallRecordView& record);

private:
    [[nodiscard]] bool TryCut(u64 present_id, std::string& error);
    [[nodiscard]] CutOutcome CutBody(u64 present_id, std::string& error);
    void Fail(u32 reason, const std::string& message);
    void Deactivate();
    [[nodiscard]] bool Staging() const;
    void Stage(u64 bytes, std::function<void()>&& replay, bool is_consume = false);
    void BeginCutLocked();
    void AbandonCutLocked();
    void AbandonCut();
    void DrainStage();
    void SubmitBuilt(s32 channel, SubmitEvent&& submit, SubmitInfo info);
    void FinishComposedLocked(u64 id, bool have_picture, const EndPicture& picture);
    bool LibraryStoreLocked(u64 frame, std::span<const u8> data, bool encoded);
    void ApplyPins(std::vector<std::pair<DAddr, size_t>>&& runs);
    void RecordPresentLocked(u64 id, PresentPath path, std::span<const Tegra::FramebufferConfig> layers,
                             std::span<const Service::Nvidia::NvFence> fences);
    void SealPresentLocked(u64 id);

    // The helpers below run with `mu` held.
    void Emit(ChunkType type, u64 frame, std::vector<u8>&& payload);
    void EmitBlob(BlobChunk&& chunk, u64 frame);
    void EmitMemory(MemoryOutput& out);
    void StreamBlobs(MemoryOutput& out, u64 frame);
    void FlushBatch();
    void FlushEventBatch();
    void FlushFenceBatch();
    void AddFence(const FenceEntry& entry);
    void AppendEffect(std::vector<LiveEffect>& destination, const LiveEffect& effect);
    bool WriteEffects(const std::filesystem::path& path, std::string& error) const;
    void FenceProducedLocked(const FenceNote& note);
    void FenceWaitLocked(const FenceNote& note);
    void NoteUnresolved(const std::string& message);
    [[nodiscard]] u32 ResolveChannel(s32 channel, u32 space) const;
    void NoteOutputProduced(const FenceNote& note);
    void BindOutputs();
    void NoteOutputOverflow();
    void HostReadbackLocked(u64 device, u64 size);
    void FlushLibrary();
    void FlushTracker();
    void AddEvent(GpuEvent&& event, u64 bytes);
    void Route(HeldItem&& item);
    void StageRouted(HeldItem& item);
    bool StageItem(HeldItem& item, bool wait);
    void FinishFrame(u64 id, const EndPicture* picture);
    void NoteDrop(u32 reasons, const char* message);
    [[nodiscard]] SessionRecord BuildSession(const CutState& cut) const;

    template <typename Entry>
    void EmitList(ChunkType type, u64 frame, std::span<const Entry> entries, size_t entry_bytes);

    std::unique_ptr<CaptureHost> host;
    const CaptureConfig config;
    StagingWriter sink;
    std::unique_ptr<MemoryTracker> tracker;
    FileHeader header;

    mutable std::mutex mu;
    State state = State::WaitingCut;
    bool window_closed = false;  // the last selected present was accepted
    bool final_pending = false;  // the last selected present was accepted, its PRESENT is not yet recorded
    struct PendingPresent {
        u64 id = 0;
        u64 calls = 0;
        PresentPath path = PresentPath::Nx2;
        std::vector<Tegra::FramebufferConfig> layers;
        std::vector<Service::Nvidia::NvFence> fences;
    };
    std::deque<PendingPresent> pending_presents;
    std::map<u64, std::vector<std::vector<u8>>> future_library;
    size_t future_library_bytes = 0;
    std::string error_text;
    u32 reasons = 0;
    u32 cap_dropped = 0;
    u32 cap_overflow = 0;
    u32 cap_unresolved = 0;

    u64 first_frame = 0;
    u64 last_frame = 0;
    u64 start_frame = 0;  // first frame stored in the file; set at the cut
    u64 open_frame = 0;
    u32 selected_finished = 0;
    u32 missing_pictures = 0;
    u32 pictures = 0;
    u32 unfinished_frames = 0;

    std::vector<GpuEvent> batch;
    u64 batch_bytes = 0;
    u64 batch_frame = 0;
    u64 next_submit_id = 1;
    u64 next_consume_id = 1;
    std::unordered_map<s32, std::deque<SubmitInfo>> submits;
    std::vector<FenceEntry> fence_batch;
    u64 fence_batch_frame = 0;
    std::map<u32, u64> seen_syncpoints;               // syncpoint -> latest value a producer raised
    std::map<std::pair<u32, u64>, std::unordered_set<u64>> seen_memory;   // (address space, gpu va) -> seen values
    std::unordered_map<s32, u32> channel_space;       // live channel -> address space
    std::set<u32> checkpoint_spaces;                  // address spaces the checkpoint lists
    void NoteSpaceUnknown(u32 space, const char* what);
    std::map<u32, u32> recorded_syncpoints;           // syncpoint -> last absolute value recorded
    [[nodiscard]] bool SyncpointRaiseFits(u32 id, u64 absolute);
    std::set<u32> external_syncpoints;                // syncpoints a recorded FENCE sets absolutely
    std::map<u32, u64> gpu_syncpoint_raises;          // syncpoint -> raises by the recorded commands
    void PushPendingOutput(const SemanticOutput& output);
    IntervalSet cpu_written;                          // device ranges guest or external writes touched
    u64 fences_recorded = 0;
    u64 fences_synthesized = 0;
    std::vector<SemanticOutput> pending_outputs;  // written since the last consume event
    // (kind, address space tag, id) of the open frame: the same address in two spaces is two outputs
    std::map<std::tuple<u32, u32, u64>, SemanticOutput> open_outputs;
    std::vector<ReadbackTarget> open_readbacks;
    CaptureOutputStats output_stats;

    // Hooks staged while the checkpoint is cut (state Cutting) and until they are replayed.
    std::vector<std::function<void()>> cut_stage;
    u64 cut_stage_bytes = 0;
    bool cut_stage_overflow = false;
    bool cut_consumed = false;  // GPU work executed during the cut: the checkpoint is torn
    bool draining = false;
    u64 frame_calls = 0;
    size_t total_effects = 0;
    std::vector<LiveEffect> open_effects;
    std::vector<std::vector<LiveEffect>> finished_effects;
    u64 frame_cmd_bytes = 0;
    u64 frame_gpu_words = 0;
    u64 total_gpu_words = 0;

    std::deque<HeldItem> held;
    u64 held_bytes = 0;
    bool hold_active = false;
    std::deque<AwaitingFrame> awaiting;
    std::vector<u32> ordinal_sequence;  // blob ordinal -> file sequence
    u64 last_stage_frame = 0;
    u64 frame_first_seq = NoSequence;
    bool initial_phase = false;

    bool has_library = false;
    LibrarySchema schema;
    LibraryCallSource* library = nullptr;
    std::vector<u8> library_buffer;
    u32 library_count = 0;
    u64 library_frame = 0;
    u64 library_calls = 0;
    u64 library_dropped = 0;
    CheckpointStatus library_checkpoint;
    bool library_checkpoint_overflow = false;

    std::vector<std::pair<DAddr, size_t>> all_pins;
    std::mutex pin_mutex;

};

std::mutex g_lifecycle_mutex;
std::mutex g_registry_mutex;
std::condition_variable g_hooks_retired;
size_t g_active_hooks = 0;
std::shared_ptr<Capture> g_capture;

// A hook can leave the recorder mutex to pin pages, cut the GPU, or read a picture. Keep
// the entire operation admitted until it returns, not merely the lifetime of its object.
class CaptureLease {
public:
    CaptureLease() {
        std::lock_guard lock{g_registry_mutex};
        owner = g_capture.get();
        if (owner) ++g_active_hooks;
    }
    CaptureLease(const CaptureLease&) = delete;
    ~CaptureLease() {
        if (!owner) return;
        std::lock_guard lock{g_registry_mutex};
        if (--g_active_hooks == 0) g_hooks_retired.notify_all();
    }
    explicit operator bool() const { return owner != nullptr; }
    Capture* operator->() const { return owner; }
private:
    Capture* owner;
};

[[nodiscard]] CaptureLease Current() { return {}; }

std::shared_ptr<Capture> DetachAndDrainHooks() {
    std::unique_lock lock{g_registry_mutex};
    auto owner = std::move(g_capture);
    detail::g_capture_active.store(false, std::memory_order_relaxed);
    detail::g_library_capture_active.store(false, std::memory_order_relaxed);
    g_hooks_retired.wait(lock, [] { return g_active_hooks == 0; });
    return owner;
}

void Capture::Deactivate() {
    detail::g_capture_active.store(false, std::memory_order_relaxed);
    detail::g_library_capture_active.store(false, std::memory_order_relaxed);
}

void Capture::NoteDrop(u32 more_reasons, const char* message) {
    reasons |= more_reasons;
    if ((more_reasons & IncompleteLimitExceeded) != 0) {
        ++cap_overflow;
    }
    if ((more_reasons & IncompleteDroppedEvents) != 0) {
        ++cap_dropped;
    }
    if (error_text.empty()) {
        error_text = message;
    }
}

void Capture::Fail(u32 reason, const std::string& message) {
    // Callers hold `mu`.
    state = State::Failed;
    reasons |= reason;
    if (error_text.empty()) {
        error_text = message;
    }
    Deactivate();
}

void Capture::ApplyPins(std::vector<std::pair<DAddr, size_t>>&& runs) {
    if (runs.empty()) {
        return;
    }
    // Cached pages trap guest CPU writes (Memory::HandleRasterizerWrite), which is how the
    // tracker learns that guest memory the GPU may read has changed. Not exercised without a
    // run: see the slice report.
    host->PinPages(runs, 1);
    std::lock_guard lock{pin_mutex};
    all_pins.insert(all_pins.end(), runs.begin(), runs.end());
}

// ---- emission (mu held) ---------------------------------------------------------------------

bool Capture::StageItem(HeldItem& item, bool wait) {
    const bool frameless = ChunkTypeIsFrameless(item.type);
    const u64 frame = frameless ? 0 : (std::max)(item.frame, last_stage_frame);
    std::vector<u8> payload;
    if (item.is_blob) {
        for (MemoryRange& range : item.blob.blob.ranges) {
            if (range.kind != RangeKind::Ref) {
                continue;
            }
            const u32 ordinal = range.ref_sequence;
            if (ordinal >= ordinal_sequence.size() || ordinal_sequence[ordinal] == NoOrdinal) {
                NoteDrop(IncompleteDroppedEvents, "a memory blob refers to a blob that was dropped");
                return false;
            }
            range.ref_sequence = ordinal_sequence[ordinal];
        }
        Encode(item.blob.blob, payload);
    } else {
        payload = std::move(item.payload);
    }
    const u64 sequence = sink.NextSequence();
    if (!sink.Stage(item.type, frame, std::move(payload), wait)) {
        return false;
    }
    if (item.is_blob) {
        if (ordinal_sequence.size() <= item.blob.ordinal) {
            ordinal_sequence.resize(static_cast<size_t>(item.blob.ordinal) + 1, NoOrdinal);
        }
        ordinal_sequence[item.blob.ordinal] = static_cast<u32>(sequence);
    }
    last_stage_frame = (std::max)(last_stage_frame, frame);
    if (!frameless && item.type != ChunkType::EndPicture && item.type != ChunkType::FrameEnd &&
        frame_first_seq == NoSequence) {
        frame_first_seq = sequence;
    }
    return true;
}

// Stages one chunk and arms the frame wait when it is a PRESENT.
void Capture::StageRouted(HeldItem& item) {
    const ChunkType type = item.type;
    StageItem(item, initial_phase);
    if (type == ChunkType::Present) {
        for (AwaitingFrame& frame : awaiting) {
            if (!frame.present_staged) {
                frame.present_staged = true;
                frame.first_sequence = frame_first_seq == NoSequence ? sink.NextSequence() : frame_first_seq;
                break;
            }
        }
        frame_first_seq = NoSequence;
        hold_active = true;
    }
}

void Capture::Route(HeldItem&& item) {
    if (hold_active) {
        if (held_bytes + item.weight > config.limits.max_staging_bytes) {
            NoteDrop(IncompleteLimitExceeded | IncompleteDroppedEvents,
                     "max_staging_bytes was reached while a frame awaited its composite");
            return;
        }
        held_bytes += item.weight;
        held.push_back(std::move(item));
        return;
    }
    StageRouted(item);
}

void Capture::Emit(ChunkType type, u64 frame, std::vector<u8>&& payload) {
    HeldItem item;
    item.type = type;
    item.frame = frame;
    item.weight = payload.size();
    item.payload = std::move(payload);
    Route(std::move(item));
}

void Capture::EmitBlob(BlobChunk&& chunk, u64 frame) {
    HeldItem item;
    item.type = chunk.type;
    item.frame = frame;
    item.is_blob = true;
    item.weight = chunk.blob.data.size() + 64 * chunk.blob.ranges.size();
    item.blob = std::move(chunk);
    Route(std::move(item));
}

template <typename Entry>
void Capture::EmitList(ChunkType type, u64 frame, std::span<const Entry> entries,
                       size_t entry_bytes) {
    const u64 budget = config.limits.max_chunk_bytes > 64 ? config.limits.max_chunk_bytes - 64 : 1;
    const size_t per_chunk = (std::max<size_t>)(
        1, (std::min)(size_t{MaxEventsPerChunk}, static_cast<size_t>(budget / entry_bytes)));
    for (size_t at = 0; at < entries.size(); at += per_chunk) {
        const size_t count = (std::min)(per_chunk, entries.size() - at);
        std::vector<u8> payload;
        Encode(entries.subspan(at, count), payload);
        Emit(type, frame, std::move(payload));
    }
}

void Capture::EmitMemory(MemoryOutput& out) {
    if (out.Empty()) {
        return;
    }
    FlushBatch();
    const u64 frame = open_frame;
    EmitList<UnmapEntry>(ChunkType::Unmap, frame, out.unmaps, 32);
    for (BlobChunk& chunk : out.blobs) {
        EmitBlob(std::move(chunk), frame);
    }
    EmitList<MapEntry>(ChunkType::Map, frame, out.maps, 64);
    out.Clear();
}

// Hands every blob chunk the tracker completes to the capture at once (see MemoryOutput::sink), so
// a mapping or flush of any size costs one chunk of producer memory, not its whole payload. The
// unmaps of the output go out ahead of the first chunk, as EmitMemory orders them.
void Capture::StreamBlobs(MemoryOutput& out, u64 frame) {
    out.sink = [this, &out, frame](BlobChunk&& chunk) {
        FlushBatch();
        EmitList<UnmapEntry>(ChunkType::Unmap, frame, out.unmaps, 32);
        out.unmaps.clear();
        EmitBlob(std::move(chunk), frame);
    };
}

void Capture::FlushEventBatch() {
    if (batch.empty()) {
        return;
    }
    std::vector<u8> payload;
    Encode(std::span<const GpuEvent>{batch}, payload);
    Emit(ChunkType::GpuEvent, batch_frame, std::move(payload));
    batch.clear();
    batch_bytes = 0;
}

void Capture::FlushFenceBatch() {
    if (fence_batch.empty()) {
        return;
    }
    EmitList<FenceEntry>(ChunkType::Fence, fence_batch_frame, fence_batch, 40);
    fence_batch.clear();
}

// GPU events and fences share one order: the batch that is open when the other kind arrives is
// flushed first, so a FENCE chunk always sits between the events that surround it in time.
void Capture::FlushBatch() {
    FlushEventBatch();
    FlushFenceBatch();
}

void Capture::GraphicsCall() {
    std::lock_guard lock{mu};
    if (Staging()) {
        Stage(16, [this] { GraphicsCall(); });
    } else if (state == State::Recording && !window_closed && !final_pending) {
        ++frame_calls;
    }
}

void Capture::EngineEffect(const LiveEffect& effect) {
    std::lock_guard lock{mu};
    if (Staging()) {
        Stage(sizeof(effect), [this, effect] { EngineEffect(effect); }, true);
    } else if (state == State::Recording && !window_closed) {
        AppendEffect(open_effects, effect);
    }
}

void Capture::AppendEffect(std::vector<LiveEffect>& destination, const LiveEffect& effect) {
    if (total_effects >= config.limits.max_staging_bytes / sizeof(LiveEffect)) {
        NoteDrop(IncompleteLimitExceeded | IncompleteDroppedEvents, "ordered effect storage exhausted");
        return;
    }
    destination.push_back(effect);
    ++total_effects;
}

void Capture::AddFence(const FenceEntry& entry) {
    if (entry.kind != FenceKind::SyncpointRaised) {
        FlushTracker();
    }
    AppendEffect(open_effects, {EffectKind::FenceSignal, entry.id, entry.value, true});
    FlushEventBatch();
    if (fence_batch.empty()) {
        fence_batch_frame = open_frame;
    }
    fence_batch.push_back(entry);
    ++fences_recorded;
    if (fence_batch.size() >= BatchEventLimit) {
        FlushFenceBatch();
    }
}

void Capture::AddEvent(GpuEvent&& event, u64 bytes) {
    FlushFenceBatch();
    if (batch.empty()) {
        batch_frame = open_frame;
    }
    batch.push_back(std::move(event));
    batch_bytes += bytes;
    if (batch.size() >= BatchEventLimit ||
        batch_bytes >= (std::min)(BatchByteLimit, config.limits.max_chunk_bytes / 2)) {
        FlushEventBatch();
    }
}

void Capture::FlushLibrary() {
    if (library_count == 0) {
        return;
    }
    std::vector<u8> payload;
    payload.reserve(4 + library_buffer.size());
    PutU32(payload, library_count);
    payload.insert(payload.end(), library_buffer.begin(), library_buffer.end());
    Emit(ChunkType::LibraryEvent, library_frame, std::move(payload));
    library_buffer.clear();
    library_count = 0;
}

void Capture::FlushTracker() {
    MemoryOutput out;
    StreamBlobs(out, open_frame);
    tracker->Flush(out);
    EmitMemory(out);
}

void Capture::FinishFrame(u64 id, const EndPicture* picture) {
    AwaitingFrame frame = std::move(awaiting.front());
    awaiting.pop_front();
    if (picture != nullptr) {
        HeldItem item;
        item.type = ChunkType::EndPicture;
        item.frame = id;
        Encode(*picture, item.payload);
        if (StageItem(item, false)) {
            ++pictures;
        }
    } else if (!frame.preroll) {
        ++missing_pictures;
    }
    FrameEndRecord record;
    record.frame_id = id;
    record.flags = FrameComplete | (frame.preroll ? FramePreroll : 0U) |
                   (picture != nullptr ? FrameHasLivePicture : 0U);
    record.gpu_words = frame.gpu_words;
    record.first_sequence = frame.first_sequence == NoSequence ? sink.NextSequence() : frame.first_sequence;
    record.last_sequence = sink.NextSequence();
    record.output_id = id;
    AppendEffect(frame.effects, {EffectKind::PresentCompose, id, record.output_id, true});
    record.call_count = frame.calls;
    record.effect_count = static_cast<u32>(frame.effects.size());
    finished_effects.push_back(std::move(frame.effects));
    output_stats.outputs_recorded += frame.outputs.size();
    record.outputs = std::move(frame.outputs);
    for (const ReadbackTarget& target : frame.readbacks) {
        u64 hash = Fnv1aOffset;
        bool readable = true;
        std::vector<u8> buffer(static_cast<size_t>((std::min)(target.length, ReadbackChunkBytes)));
        for (u64 at = 0; at < target.length && readable; at += buffer.size()) {
            const size_t count =
                static_cast<size_t>((std::min)(static_cast<u64>(buffer.size()), target.length - at));
            readable = host->Memory().ReadDevice(target.device + at, std::span<u8>{buffer.data(), count});
            hash = Fnv1a64(std::span<const u8>{buffer.data(), count}, hash);
        }
        if (!readable) {
            ++output_stats.not_comparable;
            continue;
        }
        SemanticOutput checksum;
        checksum.kind = SemanticKind::ReadbackChecksum;
        checksum.id = target.alloc_id;
        checksum.value_lo = hash;
        checksum.value_hi = target.length;
        record.outputs.push_back(checksum);
        ++output_stats.readbacks_recorded;
    }
    std::sort(record.outputs.begin(), record.outputs.end(),
              [](const SemanticOutput& a, const SemanticOutput& b) {
                  if (a.kind != b.kind) {
                      return a.kind < b.kind;
                  }
                  return a.id != b.id ? a.id < b.id : a.space_tag < b.space_tag;
              });
    HeldItem end;
    end.type = ChunkType::FrameEnd;
    end.frame = id;
    Encode(record, end.payload);
    StageItem(end, false);
    hold_active = false;
    if (!frame.preroll) {
        ++selected_finished;
    }
    while (!held.empty() && !hold_active) {
        HeldItem next = std::move(held.front());
        held.pop_front();
        held_bytes -= next.weight;
        StageRouted(next);
    }
    if (selected_finished == config.frame_count) {
        state = State::Finished;
        Deactivate();
    }
}

SessionRecord Capture::BuildSession(const CutState& cut) const {
    SessionRecord record;
    record.limits = config.limits;
    record.device = cut.device;
    record.pins = cut.pins;
    for (const u32 key : cut.unknown_pins) {
        record.pins.push_back(SessionPin{key, SessionPinUnknown});
    }
    return record;
}

// ---- the checkpoint cut -----------------------------------------------------------------------

// While the checkpoint is cut (state Cutting) and while the hooks staged during the cut are
// replayed (draining), a hook from any thread but the draining one is staged as a closure that
// repeats the call. Nothing a recorded frame depends on is dropped: the staged calls run in
// arrival order once the checkpoint is in the file.
bool Capture::Staging() const {
    return state == State::Cutting || (draining && !t_draining);
}

void Capture::Stage(u64 bytes, std::function<void()>&& replay, bool is_consume) {
    if (is_consume && state == State::Cutting) {
        cut_consumed = true;
    }
    if (cut_stage_bytes + bytes > MaxCutStageBytes || cut_stage.size() >= MaxCutStageEntries) {
        if (state == State::Cutting) {
            cut_stage_overflow = true;  // the cut is abandoned, so nothing is lost silently
        } else {
            NoteDrop(IncompleteLimitExceeded | IncompleteDroppedEvents,
                     "hooks that arrived while the cut was applied exceeded the staging limit");
        }
        return;
    }
    cut_stage_bytes += bytes;
    cut_stage.push_back(std::move(replay));
}

void Capture::BeginCutLocked() {
    state = State::Cutting;
    cut_stage.clear();
    cut_stage_bytes = 0;
    cut_stage_overflow = false;
    cut_consumed = false;
    draining = false;
}

// The cut did not produce a usable checkpoint. The staged hooks belong to frames that precede the
// next cut (which then covers them), so they are discarded and the capture waits for the next
// present. The callers fail the capture when no later present can still start the file.
void Capture::AbandonCutLocked() {
    cut_stage.clear();
    cut_stage_bytes = 0;
    cut_stage_overflow = false;
    cut_consumed = false;
    draining = false;
    state = State::WaitingCut;
    if (has_library) {
        detail::g_library_capture_active.store(false, std::memory_order_relaxed);
    }
}

void Capture::AbandonCut() {
    std::lock_guard lock{mu};
    AbandonCutLocked();
}

void Capture::DrainStage() {
    t_draining = true;
    for (;;) {
        std::vector<std::function<void()>> staged;
        {
            std::lock_guard lock{mu};
            if (cut_stage.empty()) {
                draining = false;
                cut_stage_bytes = 0;
                break;
            }
            staged.swap(cut_stage);
            cut_stage_bytes = 0;
        }
        for (const std::function<void()>& replay : staged) {
            replay();
        }
    }
    t_draining = false;
}

// Cuts the checkpoint with the GPU at rest. The host runs CutBody between two command lists of the
// GPU thread and holds that thread until it returns, so no command is consumed while the state and
// the memory are read; the hooks of the game's own threads that arrive meanwhile are staged and
// replayed by DrainStage once the GPU runs again.
bool Capture::TryCut(u64 present_id, std::string& error) {
    const bool cut = host->RunQuiescent(
        present_id,
        [this, present_id](std::string& why) { return CutBody(present_id, why); }, error);
    if (!cut) {
        AbandonCut();
        return false;
    }
    DrainStage();
    return true;
}

CutOutcome Capture::CutBody(u64 present_id, std::string& error) {
    {
        std::lock_guard lock{mu};
        BeginCutLocked();
    }
    CutState cut;
    if (!host->Cut(present_id, cut, error)) {
        AbandonCut();
        return cut.retry_later ? CutOutcome::Retry : CutOutcome::Failed;
    }
    for (const auto& pin : cut.pins) {
        if (pin.key == static_cast<u32>(SettingKey::VSync) && pin.value != 0) {
            error = "capture requires Immediate VSync";
            AbandonCut();
            return CutOutcome::Failed;
        }
    }
    LibrarySink library_sink{config.limits.max_chunk_bytes - 64};
    CheckpointStatus library_status;
    if (has_library) {
        // Calls from here on are staged (state Cutting) and replayed after the checkpoint.
        detail::g_library_capture_active.store(true, std::memory_order_relaxed);
        library_status = library->EmitCheckpoint(library_sink);
    }

    // The source pages are pinned before the first byte is read from them: a pinned page reports
    // the guest CPU writes that follow (staged while the state is Cutting and replayed after the
    // snapshot), so a store that lands after the read cannot go unseen. The pins are released by
    // End. Pinning takes guest memory locks, so it runs outside `mu`.
    {
        std::vector<std::pair<DAddr, size_t>> pins;
        for (const CutMapping& entry : cut.mappings) {
            if (!entry.sparse) {
                pins.emplace_back(static_cast<DAddr>(entry.device), static_cast<size_t>(entry.size));
            }
        }
        ApplyPins(std::move(pins));
    }
    {
        std::lock_guard lock{mu};
        if (cut_consumed || cut_stage_overflow) {
            error = cut_consumed ? "GPU commands or effects executed while the checkpoint was cut"
                                 : "more hooks arrived during the cut than the stage holds";
            AbandonCutLocked();
            return CutOutcome::Failed;
        }
        start_frame = present_id + 1;
        open_frame = start_frame;
        g_open_frame.store(open_frame, std::memory_order_relaxed);
        initial_phase = true;
        const u64 frame = start_frame;
        channel_space.clear();
        for (const ChannelEntry& entry : cut.checkpoint.channels) {
            channel_space[static_cast<s32>(entry.channel_id)] = entry.address_space_id;
        }
        checkpoint_spaces.clear();
        for (const AddressSpaceEntry& entry : cut.checkpoint.address_spaces) {
            checkpoint_spaces.insert(entry.address_space_id);
        }
        if (cut.title_version_known) {
            header.title_version = cut.title_version;
            header.title_version_status = static_cast<u32>(VersionStatus::Known);
        }
        header.gpu_architecture = static_cast<u32>(
            Tegra::g_nx2_ampere.load(std::memory_order_relaxed) ? GpuArchitecture::Ampere
                                                                : GpuArchitecture::Maxwell);
        output_stats.pins_unknown = cut.unknown_pins.size();

        const SessionRecord session = BuildSession(cut);
        {
            std::vector<u8> payload;
            Encode(session, payload);
            Emit(ChunkType::Session, 0, std::move(payload));
        }
        {
            u64 hash = Fnv1aOffset;
            for (const SessionPin& entry : session.pins) {
                const u64 words[2] = {entry.key, entry.value};
                hash = Fnv1a64(std::span<const u8>{reinterpret_cast<const u8*>(words), sizeof(words)}, hash);
            }
            header.settings_hash = hash;
        }
        if (has_library) {
            std::vector<u8> payload;
            Encode(schema, payload);
            Emit(ChunkType::LibrarySchema, 0, std::move(payload));
        }
        {
            std::vector<u8> payload;
            Encode(cut.checkpoint, payload);
            Emit(ChunkType::GpuCheckpoint, frame, std::move(payload));
        }
        if (has_library) {
            library_checkpoint = library_status;
            std::vector<u8> payload;
            PutU32(payload, library_sink.count);
            payload.insert(payload.end(), library_sink.buffer.begin(), library_sink.buffer.end());
            if (library_sink.dropped != 0) {
                library_dropped += library_sink.dropped;
                library_checkpoint_overflow = true;
            }
            Emit(ChunkType::LibraryCheckpoint, frame, std::move(payload));
        }

        // The initial snapshot streams blob by blob (it can be gigabytes); maps follow all blobs.
        tracker->SetPhase(TrackerPhase::Initial);
        tracker->BeginFrame();
        std::vector<MapEntry> maps;
        for (const CutMapping& entry : cut.mappings) {
            MemoryOutput out;
            StreamBlobs(out, frame);
            const u32 big = entry.big_pages ? static_cast<u32>(MapBigPages) : 0U;
            if (entry.sparse) {
                tracker->MapSparse(entry.space, entry.gpu_addr, entry.size,
                                   NativeSparseFlags(entry.big_pages), out);
            } else {
                tracker->Map(entry.space, entry.gpu_addr, entry.size, entry.device, big, entry.kind,
                             out);
            }
            for (BlobChunk& chunk : out.blobs) {
                EmitBlob(std::move(chunk), frame);
            }
            maps.insert(maps.end(), out.maps.begin(), out.maps.end());
        }
        EmitList<MapEntry>(ChunkType::Map, frame, maps, 64);
        initial_phase = false;
        tracker->SetPhase(TrackerPhase::Delta);
        tracker->BeginFrame();
        state = State::Recording;
        draining = true;
    }
    return CutOutcome::Done;
}

// ---- begin and end ----------------------------------------------------------------------------

bool Capture::Begin(std::string& error) {
    const SessionLimits& limits = config.limits;
    if (config.path.empty()) {
        error = "no capture path";
        return false;
    }
    if (config.frame_count == 0 || config.frame_count > MaxSelectedFrames) {
        error = "frame_count must be 1.." + std::to_string(MaxSelectedFrames);
        return false;
    }
    if (config.first_frame == 0 || config.preroll_frames >= config.first_frame) {
        error = "first_frame must be at least 1 and larger than preroll_frames";
        return false;
    }
    if (u64{config.preroll_frames} + config.frame_count > MaxTotalFrames) {
        error = "preroll_frames + frame_count exceeds " + std::to_string(MaxTotalFrames);
        return false;
    }
    if (limits.max_chunk_bytes < 4096 || limits.max_chunk_bytes > MaxChunkBytes ||
        limits.max_staging_bytes < limits.max_chunk_bytes || limits.max_staging_bytes > MaxStagingBytes ||
        limits.max_file_bytes < (1ULL << 20) || limits.max_file_bytes > MaxFileBytes ||
        limits.max_initial_backing_bytes == 0 || limits.max_command_bytes_per_frame == 0 ||
        limits.max_new_payload_bytes_per_frame == 0) {
        error = "the capture limits are out of range";
        return false;
    }
    const std::filesystem::path path{config.path};
    if (path.filename() == "expected_effects.jsonl") {
        error = "the capture cannot use its effect sidecar name";
        return false;
    }
    if (!CheckPrivateOutput(path, OutputKind::File, error) ||
        !CheckPrivateOutput(path.parent_path() / "expected_effects.jsonl", OutputKind::File, error)) {
        return false;
    }
    const std::filesystem::path directory = path.parent_path().empty() ? std::filesystem::path{"."}
                                                                        : path.parent_path();
    if (!MarkDirectoryPrivate(directory, error)) {
        return false;
    }
    first_frame = config.first_frame;
    last_frame = config.first_frame + config.frame_count - 1;
    const u64 floor = first_frame - config.preroll_frames - 1;  // the present the cut may follow
    const u64 last_accepted = LastAcceptedPresentId();
    if (last_frame < first_frame) {
        error = "capture frame range overflows";
        return false;
    }
    if (last_accepted > first_frame - 1) {
        error = "the selected frames have already started";
        return false;
    }

    library = RegisteredLibrarySource();
    if (library != nullptr) {
        std::string schema_error;
        has_library = library->BuildSchema(schema, schema_error);
        if (!has_library) {
            library = nullptr;
        }
    }

    header.format_major = FormatMajor;
    header.format_minor = FormatMinor;
    header.first_frame = first_frame;
    header.requested_frames = config.frame_count;
    header.preroll_frames = config.preroll_frames;
    header.title_id = config.title_id;
    header.session_id = NonZeroRandom();
    header.build_id = HashText(Common::g_build_id);
    header.gpu_architecture = static_cast<u32>(Tegra::g_nx2_ampere.load(std::memory_order_relaxed)
                                                    ? GpuArchitecture::Ampere
                                                    : GpuArchitecture::Maxwell);
#if defined(__clang__) && defined(_MSC_VER)
    header.compiler = static_cast<u32>(CompilerId::ClangMsvc);
#elif defined(__clang__)
    header.compiler = static_cast<u32>(CompilerId::ClangMinGw);
#elif defined(__GNUC__)
    header.compiler = static_cast<u32>(CompilerId::Gcc);
#elif defined(_MSC_VER)
    header.compiler = static_cast<u32>(CompilerId::Msvc);
#endif
    {
        const char* const revision = Common::g_scm_rev;
        for (size_t i = 0; i < header.revision.size() && revision[i] != '\0'; ++i) {
            const char c = revision[i];
            header.revision[i] = (c >= 0x20 && c < 0x7F) ? c : '?';
        }
    }
    if (has_library) {
        header.optional_features |= OptionalLibrarySection;
    }
    header.optional_features |= OptionalLivePictures;

    TrackerConfig tracker_config;
    tracker_config.window_bytes = TrackerWindowBytesForCapture;
    tracker_config.max_initial_backing_bytes = limits.max_initial_backing_bytes;
    tracker_config.max_new_payload_bytes_per_frame = limits.max_new_payload_bytes_per_frame;
    tracker_config.max_chunk_bytes = limits.max_chunk_bytes;
    tracker = std::make_unique<MemoryTracker>(host->Memory(), tracker_config);

    if (!sink.Open(config.path, header, error)) {
        return false;
    }

    // The cut waits for the present the preroll starts after; when that present has passed, the
    // cut is taken now with a shorter preroll.
    if (last_accepted >= floor) {
        std::string cut_error;
        if (!TryCut(last_accepted, cut_error)) {
            if (last_accepted + 1 >= first_frame) {
                error = "the checkpoint could not be cut: " + cut_error;
                reasons |= IncompleteCheckpointUnavailable;
                error_text = error;
                return false;
            }
        }
    }
    return true;
}

void Capture::AcceptedPresent(u64 id, PresentPath path,
                              std::span<const Tegra::FramebufferConfig> layers,
                              std::span<const Service::Nvidia::NvFence> fences) {
    bool cut = false;
    {
        std::lock_guard lock{mu};
        if (Staging()) {
            std::vector<Tegra::FramebufferConfig> layer_copy{layers.begin(), layers.end()};
            std::vector<Service::Nvidia::NvFence> fence_copy{fences.begin(), fences.end()};
            const u64 staged_bytes = 64 + layer_copy.size() * sizeof(Tegra::FramebufferConfig) +
                                     fence_copy.size() * sizeof(Service::Nvidia::NvFence);
            Stage(staged_bytes, [this, id, path, stashed_layers = std::move(layer_copy),
                                 stashed_fences = std::move(fence_copy)] {
                AcceptedPresent(id, path, stashed_layers, stashed_fences);
            });
            return;
        }
        if (state == State::WaitingCut) {
            if (id + 1 < first_frame - config.preroll_frames) {
                return;
            }
            if (id + 1 > first_frame) {
                Fail(IncompleteCheckpointUnavailable,
                     "the selected frames started before the checkpoint was cut");
                return;
            }
            cut = true;
        }
    }
    if (cut) {
        std::string cut_error;
        if (!TryCut(id, cut_error)) {
            std::lock_guard lock{mu};
            if (id + 1 >= first_frame) {
                Fail(IncompleteCheckpointUnavailable, "the checkpoint could not be cut: " + cut_error);
            }
        }
        // Frame `id` precedes the first recorded frame: its present is not part of the file.
        return;
    }

    std::lock_guard lock{mu};
    if (state != State::Recording || window_closed || final_pending || id < open_frame) {
        return;
    }
    if (id != open_frame + pending_presents.size()) {
        Fail(IncompleteDroppedEvents, "an accepted present was not observed in order");
        return;
    }
    if (layers.size() > MaxLayersPerPresent) {
        Fail(IncompleteLimitExceeded, "the pending present exceeds the capture layer limit");
        return;
    }
    if (fences.size() > 1 || (!fences.empty() &&
        static_cast<u32>(fences.front().id) >= Service::Nvidia::MaxSyncPoints)) {
        Fail(IncompleteUnresolvedResource, "PRESENT cannot encode multiple or invalid syncpoint fences");
        return;
    }
    // NVN call counts use the guest acceptance boundary. GPU words and effects keep belonging
    // to the open GPU frame until the renderer actually composites it.
    auto& present = pending_presents.emplace_back();
    present.id = id;
    present.path = path;
    present.calls = std::exchange(frame_calls, 0);
    present.layers.assign(layers.begin(), layers.end());
    if (!fences.empty()) present.fences.push_back(fences.front());
    g_open_frame.store(id + 1, std::memory_order_relaxed);
    final_pending = id == last_frame;
}

void Capture::RecordPresentLocked(u64 id, PresentPath path,
                                  std::span<const Tegra::FramebufferConfig> layers,
                                  std::span<const Service::Nvidia::NvFence> fences) {
    FlushTracker();
    FlushBatch();
    FlushLibrary();

    PresentRecord record;
    record.path = path;
    record.present_id = id;
    record.output_id = id;
    for (const auto& fence : fences) {
        if (fence.id < 0 || static_cast<u32>(fence.id) >= SyncpointCount) {
            continue;
        }
        record.fence_id = static_cast<u32>(fence.id);
        record.fence_value = fence.value;
        if (recorded_syncpoints[static_cast<u32>(record.fence_id)] < record.fence_value) {
            NoteUnresolved("the PRESENT syncpoint threshold is not established by recorded producers");
        }
        break;
    }
    for (size_t index = 0; index < layers.size(); ++index) {
        if (index >= MaxLayersPerPresent) {
            NoteDrop(IncompleteLimitExceeded | IncompleteDroppedEvents,
                     "a present has more layers than the format allows");
            break;
        }
        const Tegra::FramebufferConfig& source_layer = layers[index];
        PresentLayer layer;
        const auto bytes = PresentBackingBytes(source_layer.width, source_layer.height,
                                               source_layer.stride, static_cast<u32>(source_layer.pixel_format));
        const u64 available = std::numeric_limits<u64>::max() - source_layer.address;
        MemoryTracker::Location location;
        const bool range_ok = bytes && source_layer.offset <= available && *bytes <= available - source_layer.offset;
        const bool located = range_ok && tracker->Locate(source_layer.address, source_layer.offset + *bytes, location);
        const bool resolved = located && tracker->IsResolved(source_layer.address + source_layer.offset, *bytes);
        if (resolved) {
            layer.alloc_id = location.alloc_id;
            layer.alloc_offset = location.offset;
            layer.generation = location.generation;
        } else {
            ++cap_unresolved;
            reasons |= IncompleteUnresolvedResource;
            LOG_WARNING(HW_GPU,
                        "Replay capture unresolved: present layer addr={:#x} offset={:#x} bytes={} w={} h={} stride={} fmt={} range_ok={} located={} resolved={}",
                        source_layer.address, source_layer.offset, bytes.value_or(0), source_layer.width,
                        source_layer.height, source_layer.stride, static_cast<u32>(source_layer.pixel_format),
                        range_ok, located, resolved);
            if (error_text.empty()) {
                error_text = "a present layer is not backed by one captured allocation";
            }
        }
        layer.framebuffer_offset = source_layer.offset;
        layer.width = source_layer.width;
        layer.height = source_layer.height;
        layer.stride = source_layer.stride;
        layer.pixel_format = static_cast<u32>(source_layer.pixel_format);
        layer.transform_flags = static_cast<u32>(source_layer.transform_flags);
        layer.blending = static_cast<u32>(source_layer.blending);
        layer.layer_stack_mask = source_layer.layer_stack_mask;
        layer.flags = source_layer.native_tiling ? static_cast<u32>(LayerNativeTiling) : 0U;
        layer.crop_left = source_layer.crop_rect.left;
        layer.crop_top = source_layer.crop_rect.top;
        layer.crop_right = source_layer.crop_rect.right;
        layer.crop_bottom = source_layer.crop_rect.bottom;
        record.layers.push_back(layer);
    }
    std::vector<u8> payload;
    Encode(record, payload);
    AwaitingFrame awaited;
    awaited.id = id;
    awaited.preroll = id < first_frame;
    awaited.gpu_words = frame_gpu_words;
    BindOutputs();
    awaited.outputs.reserve(open_outputs.size());
    for (const auto& [key, output] : open_outputs) {
        awaited.outputs.push_back(output);
    }
    open_outputs.clear();
    awaited.readbacks.swap(open_readbacks);
    awaited.calls = pending_presents.front().calls;
    awaited.effects.swap(open_effects);
    awaiting.push_back(std::move(awaited));
    frame_gpu_words = 0;
    Emit(ChunkType::Present, id, std::move(payload));
    open_frame = id + 1;
    if (auto next = future_library.extract(open_frame)) {
        for (const auto& data : next.mapped()) {
            future_library_bytes -= sizeof(data) + data.size();
            LibraryStoreLocked(open_frame, data, true);
        }
    }
    tracker->BeginFrame();
    frame_cmd_bytes = 0;
    if (id == last_frame) {
        window_closed = true;
        library_buffer.clear();
        library_count = 0;
        batch.clear();
        batch_bytes = 0;
        fence_batch.clear();
    }
}

// Called at the GPU boundary, never at guest-side queue acceptance.
void Capture::SealPresentLocked(u64 id) {
    if (state != State::Recording || id < open_frame) return;
    if (pending_presents.empty() || pending_presents.front().id != id) {
        Fail(IncompleteDroppedEvents, "a GPU composite did not match the next accepted present");
        return;
    }
    if (id == last_frame) {
        final_pending = false;
        for (const auto& [channel, queue] : submits) {
            if (!queue.empty()) {
                NoteDrop(IncompleteDroppedEvents, "a submit was not fully consumed at the last GPU composite");
                break;
            }
        }
    }
    const auto& present = pending_presents.front();
    RecordPresentLocked(id, present.path, present.layers, present.fences);
    pending_presents.pop_front();
}

void Capture::FrameComposed(u64 id) {
    bool need_picture = false;
    {
        std::lock_guard lock{mu};
        const bool staging = Staging();
        if (!staging) {
            SealPresentLocked(id);
        }
        if (!staging && (state != State::Recording || awaiting.empty() || awaiting.front().id != id)) {
            return;
        }
        // While staging the frame is not known yet: the picture is read now, because it is the
        // one displayed for this present and a later read could see another.
        need_picture = staging || !awaiting.front().preroll;
    }
    EndPicture picture;
    bool have_picture = false;
    if (need_picture) {
        have_picture = host->ReadComposedPicture(id, picture);
        if (have_picture) {
            const u64 expected = u64{picture.stride_bytes} * picture.height;
            have_picture = picture.frame_id == id && picture.width != 0 && picture.height != 0 &&
                           u64{picture.stride_bytes} >= u64{picture.width} * 4 &&
                           picture.pixels.size() == expected &&
                           expected + 128 <= config.limits.max_chunk_bytes;
            if (have_picture) {
                picture.kind = PictureKind::Composed;
                picture.format = PictureFormat::B8G8R8A8Unorm;
                picture.color = ColorEncoding::SdrSrgb;
                picture.pixel_checksum = Fnv1a64(picture.pixels);
            }
        }
    }
    std::lock_guard lock{mu};
    if (Staging()) {
        const u64 staged_bytes = have_picture ? picture.pixels.size() + 128 : 64;
        Stage(staged_bytes,
              [this, id, have_picture, stashed_picture = std::move(picture)] {
                  std::lock_guard replay_lock{mu};
                  SealPresentLocked(id);
                  FinishComposedLocked(id, have_picture, stashed_picture);
              });
        return;
    }
    FinishComposedLocked(id, have_picture, picture);
}

void Capture::FinishComposedLocked(u64 id, bool have_picture, const EndPicture& picture) {
    if (state != State::Recording || awaiting.empty() || awaiting.front().id != id) {
        return;
    }
    FinishFrame(id, have_picture && !awaiting.front().preroll ? &picture : nullptr);
}

void Capture::Submit(s32 channel, const Tegra::CommandList& list) {
    SubmitEvent submit;
    submit.channel_id = static_cast<u32>(channel);
    SubmitInfo info;
    if (!list.prefetch_command_list.empty()) {
        SubmitEntry entry;
        entry.word_count = static_cast<u32>(list.prefetch_command_list.size());
        submit.entries.push_back(entry);
        info.last_entry = 0;
    } else {
        for (size_t index = 0; index < list.command_lists.size(); ++index) {
            const Tegra::CommandListHeader& header_word = list.command_lists[index];
            SubmitEntry entry;
            entry.gpu_va = header_word.addr;
            entry.word_count = static_cast<u32>(header_word.size);
            entry.header_flags = (header_word.allow_flush ? 1U : 0U) |
                                 (header_word.is_push_buffer ? 2U : 0U) |
                                 (header_word.sync ? 4U : 0U);
            if (entry.word_count != 0) {
                info.last_entry = static_cast<u32>(index);
            }
            submit.entries.push_back(entry);
        }
    }
    {
        std::lock_guard lock{mu};
        if (Staging()) {
            const u64 staged_bytes = 64 + 16 * submit.entries.size();
            Stage(staged_bytes, [this, channel, submit, info]() mutable {
                SubmitBuilt(channel, std::move(submit), info);
            });
            return;
        }
    }
    SubmitBuilt(channel, std::move(submit), info);
}

void Capture::SubmitBuilt(s32 channel, SubmitEvent&& submit, SubmitInfo info) {
    std::lock_guard lock{mu};
    if (state != State::Recording || window_closed) {
        return;
    }
    FlushTracker();
    if (submit.entries.size() > MaxEntriesPerSubmit) {
        NoteDrop(IncompleteLimitExceeded | IncompleteDroppedEvents,
                 "a submit has more entries than the format allows");
        return;
    }
    bool carries_words = false;
    for (const SubmitEntry& entry : submit.entries) {
        carries_words = carries_words || entry.word_count != 0;
    }
    AppendEffect(open_effects, {EffectKind::Submit, submit.channel_id, submit.entries.size(), true});
    submit.submit_id = next_submit_id++;
    info.submit_id = submit.submit_id;
    if (carries_words) {
        submits[channel].push_back(info);
    }
    const u64 bytes = 32 + 16 * submit.entries.size();
    GpuEvent event;
    event.kind = GpuEventKind::Submit;
    event.submit = std::move(submit);
    AddEvent(std::move(event), bytes);
}

void Capture::Consume(s32 channel, u32 entry_index, u64 gpu_va, u32 word_offset, u32 flags, u32 epoch,
                      std::span<const u32> words) {
    std::lock_guard lock{mu};
    if (Staging()) {
        Stage(u64{words.size()} * 4 + 64,
              [this, channel, entry_index, gpu_va, word_offset, flags, epoch,
               copy = std::vector<u32>(words.begin(), words.end())] {
                  Consume(channel, entry_index, gpu_va, word_offset, flags, epoch, copy);
              },
              true);
        return;
    }
    if (state != State::Recording || window_closed) {
        return;
    }
    BindOutputs();
    const auto found = submits.find(channel);
    if (found == submits.end() || found->second.empty()) {
        // After the last present the GPU may consume commands of the next frame, which no
        // submit of the file describes: those words are not part of the capture.
        NoteDrop(IncompleteDroppedEvents, "words were consumed from a channel with no recorded submit");
        return;
    }
    // The words ran before this hook: bytes the guest CPU changed since the last flush are the
    // input they read, so they are in the file ahead of the consume (see GpuRead, GpuWrite).
    FlushTracker();
    const SubmitInfo head = found->second.front();
    const size_t piece_words = (std::max<size_t>)(
        1, (std::min)(size_t{MaxWordsPerConsume}, static_cast<size_t>(config.limits.max_chunk_bytes / 8)));
    for (size_t at = 0; at < words.size();) {
        const size_t count = (std::min)(piece_words, words.size() - at);
        const u64 bytes = u64{count} * 4 + 64;
        if (frame_cmd_bytes + bytes > config.limits.max_command_bytes_per_frame) {
            NoteDrop(IncompleteLimitExceeded | IncompleteDroppedEvents,
                     "max_command_bytes_per_frame was reached");
            break;
        }
        const bool last_piece = at + count == words.size();
        GpuEvent event;
        event.kind = GpuEventKind::Consume;
        ConsumeEvent& consume = event.consume;
        consume.channel_id = static_cast<u32>(channel);
        consume.entry_index = entry_index;
        consume.submit_id = head.submit_id;
        consume.consume_id = next_consume_id++;
        consume.word_offset = word_offset + static_cast<u32>(at);
        // The address of the first word it carries: the entry's address is what the pusher passes.
        consume.gpu_va = gpu_va + u64{consume.word_offset} * 4U;
        consume.flags = last_piece ? flags : (flags & ~static_cast<u32>(ConsumeEntryEnd));
        consume.epoch = epoch;
        consume.words.assign(words.begin() + static_cast<std::ptrdiff_t>(at),
                             words.begin() + static_cast<std::ptrdiff_t>(at + count));
        AppendEffect(open_effects, {EffectKind::Consume, consume.channel_id, count, true});
        frame_cmd_bytes += bytes;
        frame_gpu_words += count;
        total_gpu_words += count;
        AddEvent(std::move(event), bytes);
        at += count;
    }
    if ((flags & ConsumeEntryEnd) != 0 && entry_index >= head.last_entry) {
        found->second.pop_front();
    }
}

void Capture::GuestWrite(u64 device, u64 size) {
    std::lock_guard lock{mu};
    if (Staging()) {
        Stage(32, [this, device, size] { GuestWrite(device, size); });
        return;
    }
    if (state == State::Recording && !window_closed) {
        if (size != 0 && device + size > device) {
            cpu_written.Add(device, device + size);
        }
        tracker->NoteCpuWrite(device, size);
    }
}

void Capture::GpuMap(u32 space, u64 gpu_va, u64 size, u64 device, u32 flags, u32 pte_kind) {
    const auto stage_again = [&] {
        Stage(64, [this, space, gpu_va, size, device, flags, pte_kind] {
            GpuMap(space, gpu_va, size, device, flags, pte_kind);
        });
    };
    {
        std::lock_guard lock{mu};
        if (Staging()) {
            stage_again();
            return;
        }
        if (state != State::Recording || window_closed) {
            return;
        }
    }
    // Pinned before the tracker reads the bytes (see TryCut); outside `mu` like there.
    std::vector<std::pair<DAddr, size_t>> pins;
    pins.emplace_back(static_cast<DAddr>(device), static_cast<size_t>(size));
    ApplyPins(std::move(pins));
    std::lock_guard lock{mu};
    if (Staging()) {
        stage_again();
        return;
    }
    if (state != State::Recording || window_closed) {
        return;
    }
    if (checkpoint_spaces.find(space) == checkpoint_spaces.end()) {
        return;
    }
    MemoryOutput out;
    StreamBlobs(out, open_frame);
    tracker->Map(space, gpu_va, size, device, flags, pte_kind, out);
    EmitMemory(out);
}

void Capture::GpuMapSparse(u32 space, u64 gpu_va, u64 size, u32 flags) {
    std::lock_guard lock{mu};
    if (Staging()) {
        Stage(48, [this, space, gpu_va, size, flags] { GpuMapSparse(space, gpu_va, size, flags); });
        return;
    }
    if (state != State::Recording || window_closed) {
        return;
    }
    if (checkpoint_spaces.find(space) == checkpoint_spaces.end()) {
        return;
    }
    MemoryOutput out;
    tracker->MapSparse(space, gpu_va, size, flags, out);
    EmitMemory(out);
}

void Capture::GpuUnmap(u32 space, u64 gpu_va, u64 size) {
    std::lock_guard lock{mu};
    if (Staging()) {
        Stage(48, [this, space, gpu_va, size] { GpuUnmap(space, gpu_va, size); });
        return;
    }
    if (state != State::Recording || window_closed) {
        return;
    }
    if (checkpoint_spaces.find(space) == checkpoint_spaces.end()) {
        return;
    }
    MemoryOutput out;
    tracker->Unmap(space, gpu_va, size, out);
    EmitMemory(out);
}

void Capture::GpuWrite(u64 device, u64 size) {
    std::lock_guard lock{mu};
    if (Staging()) {
        Stage(32, [this, device, size] { GpuWrite(device, size); });
        return;
    }
    if (state == State::Recording && !window_closed) {
        // A command that reads and then writes a range leaves no trace of its input once the
        // range is derived: what the CPU wrote there is snapshotted before the write clears it.
        if (tracker->HasPendingWrites()) {
            FlushTracker();
        }
        tracker->NoteGpuWrite(device, size);
    }
}

void Capture::GpuRead(u64 device, u64 size) {
    std::lock_guard lock{mu};
    if (Staging()) {
        Stage(32, [this, device, size] { GpuRead(device, size); });
        return;
    }
    if (state == State::Recording && !window_closed) {
        if (tracker->HasPendingWrites()) {
            FlushTracker();
        }
        tracker->NoteRead(device, size);
    }
}

void Capture::ExternalWrite(u64 device, u64 size) {
    std::lock_guard lock{mu};
    if (Staging()) {
        Stage(32, [this, device, size] { ExternalWrite(device, size); });
        return;
    }
    if (state != State::Recording || window_closed) {
        return;
    }
    if (size != 0 && device + size > device) {
        cpu_written.Add(device, device + size);
    }
    MemoryOutput out;
    StreamBlobs(out, open_frame);
    tracker->NoteExternalWrite(device, size, out);
    EmitMemory(out);
}

void Capture::ChannelCreate(s32 channel, u32 space, u32 flags) {
    std::lock_guard lock{mu};
    if (Staging()) {
        Stage(32, [this, channel, space, flags] { ChannelCreate(channel, space, flags); });
        return;
    }
    if (state != State::Recording || window_closed) {
        return;
    }
    NoteSpaceUnknown(space, "a channel");
    channel_space[channel] = space;
    GpuEvent event;
    event.kind = GpuEventKind::ChannelCreate;
    event.channel_create.channel_id = static_cast<u32>(channel);
    event.channel_create.address_space_id = space;
    event.channel_create.flags = flags;
    AddEvent(std::move(event), 32);
}

void Capture::ChannelRelease(s32 channel) {
    std::lock_guard lock{mu};
    if (Staging()) {
        Stage(32, [this, channel] { ChannelRelease(channel); });
        return;
    }
    if (state != State::Recording || window_closed) {
        return;
    }
    submits.erase(channel);
    channel_space.erase(channel);
    GpuEvent event;
    event.kind = GpuEventKind::ChannelRelease;
    event.channel_release.channel_id = static_cast<u32>(channel);
    AddEvent(std::move(event), 32);
}

void Capture::NoteUnresolved(const std::string& message) {
    ++cap_unresolved;
    reasons |= IncompleteMissingFence;
    if (error_text.empty()) {
        error_text = message;
    }
}

// A replay can only create the address spaces its checkpoint lists, so a channel or mapping in
// another one cannot be reproduced. Called with `mu` held.
void Capture::NoteSpaceUnknown(u32 space, const char* what) {
    if (checkpoint_spaces.find(space) != checkpoint_spaces.end()) {
        return;
    }
    ++cap_unresolved;
    reasons |= IncompleteUnresolvedResource;
    LOG_WARNING(HW_GPU, "Replay capture unresolved: {} uses unknown address space {}", what, space);
    if (error_text.empty()) {
        error_text = std::string{what} + " uses address space " + std::to_string(space) +
                     ", which the checkpoint does not list: a replay cannot create it";
    }
}

// The channel a memory fence is applied through: `channel` when the capture knows it, else the
// lowest channel recorded in `space`. 0 when there is none.
u32 Capture::ResolveChannel(s32 channel, u32 space) const {
    if (channel > 0 && channel_space.find(channel) != channel_space.end()) {
        return static_cast<u32>(channel);
    }
    u32 best = 0;
    for (const auto& [live, live_space] : channel_space) {
        if (live_space == space && live > 0 && (best == 0 || static_cast<u32>(live) < best)) {
            best = static_cast<u32>(live);
        }
    }
    return best;
}

// A replay raises a syncpoint to a FENCE's absolute value one increment at a time, from its own
// value, and refuses a jump over MaxSyncpointRaise. The capture checks the same rule here, with
// the replay's value standing in as the last value recorded for the id (0 at the start), so a
// capture the replayer would refuse is incomplete at record time.
bool Capture::SyncpointRaiseFits(u32 id, u64 absolute) {
    const u32 value = static_cast<u32>(absolute);
    if (external_syncpoints.insert(id).second) {
        // The replay sets it from the file, so its delta is not comparable: not only the raises
        // that follow, but the delta the open frame already queued for this id is dropped.
        constexpr u32 DeltaKind = static_cast<u32>(SemanticKind::SyncpointDelta);
        const size_t queued = std::erase_if(pending_outputs, [id](const SemanticOutput& output) {
            return output.kind == SemanticKind::SyncpointDelta && output.id == id;
        });
        const size_t open = std::erase_if(open_outputs, [id](const auto& entry) {
            return std::get<0>(entry.first) == DeltaKind && std::get<2>(entry.first) == id;
        });
        output_stats.syncpoints_skipped += static_cast<decltype(output_stats.syncpoints_skipped)>(queued + open);
    }
    u32& last = recorded_syncpoints[id];
    if (value > last && value - last > MaxSyncpointRaise) {
        NoteDrop(IncompleteLimitExceeded,
                 "a syncpoint fence is too far from the replay's syncpoint value");
        return false;
    }
    if (value > last) {
        last = value;
    }
    return true;
}

void Capture::FenceProducedLocked(const FenceNote& note) {
    std::string why;
    if (!FenceNoteValid(note, why)) {
        NoteDrop(IncompleteDroppedEvents, why.c_str());
        return;
    }
    const bool syncpoint = note.kind == FenceKind::SyncpointRaised;
    if (syncpoint) {
        const auto [at, inserted] = seen_syncpoints.try_emplace(static_cast<u32>(note.id), note.value);
        if (!inserted && WrapAtLeast(note.value, at->second)) {
            at->second = note.value;
        }
    } else {
        auto& vals = seen_memory[{note.space, note.id}];
        if (vals.size() >= 256) {
            vals.clear();
        }
        vals.insert(note.value);
    }
    if (!note.external) {
        if (syncpoint) {
            const u32 sp_id = static_cast<u32>(note.id);
            if (recorded_syncpoints[sp_id] == 0 && note.value > 1) {
                const u32 pre_cut = static_cast<u32>(note.value) - 1;
                if (SyncpointRaiseFits(sp_id, pre_cut)) {
                    FenceEntry pre_entry;
                    pre_entry.kind = FenceKind::SyncpointRaised;
                    pre_entry.id = note.id;
                    pre_entry.value = pre_cut;
                    AddFence(pre_entry);
                    ++fences_synthesized;
                }
            }
        }
        AppendEffect(open_effects, ProducedEffect(note.kind, note.id, note.value));
        if (syncpoint) {
            // A raise by the recorded commands is an output: the number of raises since the cut,
            // which the replay compares with its own count since its start. A syncpoint that a
            // FENCE also sets to an absolute value is not comparable and is skipped.
            u64& raises = gpu_syncpoint_raises[static_cast<u32>(note.id)];
            ++raises;
            ++recorded_syncpoints[static_cast<u32>(note.id)];
            if (external_syncpoints.find(static_cast<u32>(note.id)) != external_syncpoints.end()) {
                ++output_stats.syncpoints_skipped;
            } else {
                SemanticOutput output;
                output.kind = SemanticKind::SyncpointDelta;
                output.id = note.id;
                output.value_lo = raises & 0xFFFFFFFFULL;
                PushPendingOutput(output);
            }
        } else {
            NoteOutputProduced(note);
        }
        return;  // the recorded commands produce it again in a replay
    }
    FenceEntry entry;
    entry.kind = note.kind;
    entry.id = note.id;
    entry.value = note.value;
    entry.aux = (note.aux == 4 || note.aux == 8) ? note.aux : 0;
    if (syncpoint && !SyncpointRaiseFits(static_cast<u32>(entry.id), entry.value)) {
        return;
    }
    if (!syncpoint) {
        entry.channel_id = ResolveChannel(note.channel, note.space);
        if (entry.channel_id == 0) {
            NoteUnresolved("an external memory fence was written in an address space no recorded channel uses");
            return;
        }
    }
    AddFence(entry);
}

void Capture::FenceWaitLocked(const FenceNote& note) {
    std::string why;
    if (!FenceNoteValid(note, why)) {
        NoteDrop(IncompleteDroppedEvents, why.c_str());
        return;
    }
    const bool syncpoint = note.kind == FenceKind::SyncpointRaised;
    bool produced = false;
    if (syncpoint) {
        const auto seen = seen_syncpoints.find(static_cast<u32>(note.id));
        produced = seen != seen_syncpoints.end() && WrapAtLeast(seen->second, note.value);
    } else {
        const auto seen = seen_memory.find({note.space, note.id});
        produced = seen != seen_memory.end() && seen->second.contains(note.value);
        if (!produced && note.device != 0 &&
            cpu_written.Intersects(note.device, note.device + FenceWidth(note))) {
            produced = true;  // a guest or device write, carried by the memory deltas
        }
    }
    if (produced) {
        return;
    }
    if (!note.blocked || !syncpoint) {
        // The value was already there when the wait checked it: its producer ran before the
        // capture window. The replay starts from a checkpoint that does not restore syncpoint
        // values, so the fence is recorded here, ahead of the events that depend on it.
        FenceEntry entry;
        entry.kind = note.kind;
        entry.id = note.id;
        entry.value = note.value;
        entry.aux = (note.aux == 4 || note.aux == 8) ? note.aux : 0;
        if (!syncpoint) {
            if (note.device == 0) {
                return;
            }
            entry.channel_id = ResolveChannel(note.channel, note.space);
            if (entry.channel_id == 0) {
                NoteUnresolved("a memory fence wait names an address space no recorded channel uses");
                return;
            }
            tracker->NoteRead(note.device, FenceWidth(note));
            auto& vals = seen_memory[{note.space, note.id}];
            if (vals.size() >= 256) {
                vals.clear();
            }
            vals.insert(note.value);
        } else {
            if (!SyncpointRaiseFits(static_cast<u32>(note.id), note.value)) {
                return;
            }
            seen_syncpoints[static_cast<u32>(note.id)] = note.value;
        }
        const auto head = submits.find(note.channel);
        entry.submit_id = head != submits.end() && !head->second.empty() ? head->second.front().submit_id : 0;
        AddFence(entry);
        ++fences_synthesized;
        return;
    }
    NoteUnresolved(std::string{"a wait on "} + FenceName(note.kind) + " " + std::to_string(note.id) +
                   " reached value " + std::to_string(note.value) +
                   " with no producer recorded in the capture");
}

void Capture::FenceProduced(const FenceNote& note) {
    std::lock_guard lock{mu};
    if (Staging()) {
        Stage(sizeof(FenceNote) + 16, [this, note] { FenceProduced(note); }, !note.external);
        return;
    }
    if (state != State::Recording || window_closed) {
        return;
    }
    FenceProducedLocked(note);
}

void Capture::FenceWait(const FenceNote& note) {
    std::lock_guard lock{mu};
    if (Staging()) {
        Stage(sizeof(FenceNote) + 16, [this, note] { FenceWait(note); });
        return;
    }
    if (state != State::Recording || window_closed) {
        return;
    }
    FenceWaitLocked(note);
}

// A memory production of the recorded commands becomes an output of the open frame once the next
// consume event is added (or the frame's present is accepted).
void Capture::NoteOutputProduced(const FenceNote& note) {
    SemanticOutput output;
    output.id = note.id;
    output.space_tag = SpaceTag(note.space);
    const u32 width = FenceWidth(note);
    if (note.kind == FenceKind::SemaphoreWrite && (width == 4 || width == 8)) {
        output.kind = SemanticKind::SemaphoreValue;
        output.value_lo = note.value & 0xFFFFFFFFULL;
    } else if (note.kind == FenceKind::QueryValue && width == 8) {
        output.kind = SemanticKind::QueryResult;
        output.value_lo = note.value;
    } else {
        ++output_stats.not_comparable;  // timestamps, or a word narrower than the replay reads
        return;
    }
    PushPendingOutput(output);
}

void Capture::PushPendingOutput(const SemanticOutput& output) {
    if (pending_outputs.size() >= MaxSemanticOutputs) {
        NoteOutputOverflow();
        return;
    }
    pending_outputs.push_back(output);
}

void Capture::BindOutputs() {
    for (const SemanticOutput& output : pending_outputs) {
        const std::tuple<u32, u32, u64> key{static_cast<u32>(output.kind), output.space_tag,
                                            output.id};
        const auto found = open_outputs.find(key);
        if (found == open_outputs.end()) {
            if (open_outputs.size() + open_readbacks.size() >= MaxSemanticOutputs) {
                NoteOutputOverflow();
                continue;
            }
            open_outputs.emplace(key, output);
        } else {
            found->second = output;
        }
    }
    pending_outputs.clear();
}

void Capture::NoteOutputOverflow() {
    ++output_stats.overflowed;
    NoteDrop(IncompleteLimitExceeded | IncompleteDroppedEvents,
             "a frame has more semantic outputs than the format allows");
}

void Capture::HostReadbackLocked(u64 device, u64 size) {
    MemoryTracker::Location location;
    if (size == 0 || size > MaxReadbackBytes || !tracker->Locate(device, size, location) ||
        location.offset != 0 || !tracker->IsResolved(device, size)) {
        ++output_stats.not_comparable;  // not a prefix of one fully known allocation
        return;
    }
    for (ReadbackTarget& target : open_readbacks) {
        if (target.alloc_id == location.alloc_id) {
            target.device = device;
            target.length = size;
            return;
        }
    }
    if (open_outputs.size() + open_readbacks.size() >= MaxSemanticOutputs) {
        NoteOutputOverflow();
        return;
    }
    open_readbacks.push_back(ReadbackTarget{location.alloc_id, device, size});
}

void Capture::HostReadback(u64 device, u64 size) {
    std::lock_guard lock{mu};
    if (Staging()) {
        Stage(32, [this, device, size] { HostReadback(device, size); });
        return;
    }
    if (state != State::Recording || window_closed) {
        return;
    }
    AppendEffect(open_effects, {EffectKind::ReadbackVisibility, 0, 0, false});
    HostReadbackLocked(device, size);
}

void Capture::OutputNotComparable() {
    std::lock_guard lock{mu};
    if (Staging()) {
        Stage(16, [this] { OutputNotComparable(); });
        return;
    }
    if (state != State::Recording || window_closed) {
        return;
    }
    AppendEffect(open_effects, {EffectKind::FenceSignal, 0, 0, false});
    ++output_stats.not_comparable;
}

bool Capture::LibraryStoreLocked(u64 frame, std::span<const u8> data, bool encoded) {
    if (state != State::Recording || window_closed) {
        return true;  // outside the capture window: the checkpoint or nothing covers it
    }
    if (frame > last_frame) return true;
    if (frame < open_frame || !encoded || data.size() > config.limits.max_chunk_bytes / 2) {
        ++library_dropped;
        NoteDrop(IncompleteDroppedEvents, "a library call record could not be stored");
        return false;
    }
    if (frame > open_frame) {
        const size_t cost = sizeof(std::vector<u8>) + data.size();
        if (cost > config.limits.max_staging_bytes - future_library_bytes) {
            ++library_dropped;
            NoteDrop(IncompleteLimitExceeded, "library calls ahead of GPU composition exhausted staging");
            return false;
        }
        future_library[frame].emplace_back(data.begin(), data.end());
        future_library_bytes += cost;
        return true;
    }
    if (library_count != 0 && frame != library_frame) {
        FlushLibrary();
    }
    if (library_count == 0) {
        library_frame = frame;
    }
    library_buffer.insert(library_buffer.end(), data.begin(), data.end());
    ++library_count;
    ++library_calls;
    if (library_count >= LibraryBatchRecords ||
        library_buffer.size() >= (std::min)(LibraryBatchBytes, config.limits.max_chunk_bytes / 2)) {
        FlushLibrary();
    }
    return true;
}

bool Capture::LibraryRecord(const CallTicket& ticket, const CallRecordView& record) {
    thread_local std::vector<u8> scratch;
    scratch.clear();
    CallRecordView stamped = record;
    if (stamped.call_seq == 0) {
        stamped.call_seq = ticket.call_seq;
    }
    if (stamped.frame_id == 0) {
        stamped.frame_id = ticket.frame_id;
    }
    const bool encoded = Encode(stamped, scratch);
    std::lock_guard lock{mu};
    if (Staging()) {
        Stage(scratch.size() + 64, [this, frame = ticket.frame_id, data = std::vector<u8>(scratch), encoded] {
            std::lock_guard replay_lock{mu};
            LibraryStoreLocked(frame, data, encoded);
        });
        return true;
    }
    return LibraryStoreLocked(ticket.frame_id, scratch, encoded);
}

bool Capture::WriteEffects(const std::filesystem::path& path, std::string& error) const {
#ifdef _WIN32
    std::FILE* file = _wfopen(path.c_str(), L"wbx");
#else
    std::FILE* file = std::fopen(path.c_str(), "wbx");
#endif
    if (!file) {
        error = "could not exclusively create expected_effects.jsonl";
        return false;
    }
    bool ok = true;
    u64 frame = start_frame;
    for (const auto& events : finished_effects) {
        u32 ordinal = 0;
        for (const LiveEffect& event : events) {
            const auto row = EffectRow(frame, ordinal++, event).dump();
            if (std::fwrite(row.data(), 1, row.size(), file) != row.size() || std::fputc('\n', file) == EOF) {
                ok = false;
                break;
            }
        }
        if (!ok) break;
        ++frame;
    }
    if (std::fclose(file) != 0 || !ok) {
        error = "could not write expected_effects.jsonl";
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        return false;
    }
    return true;
}

void Capture::ReleasePins() {
    std::vector<std::pair<DAddr, size_t>> pins;
    {
        std::lock_guard lock{pin_mutex};
        pins.swap(all_pins);
    }
    if (!pins.empty()) host->PinPages(pins, -1);
}

void Capture::DrainFinal() {
    {
        std::lock_guard lock{mu};
        if (!final_pending || state != State::Recording) return;
    }
    // Keep the live hooks registered until the queued composite actually completes. Never
    // manufacture a frame when the producing fence did not signal before teardown.
    std::string error;
    if (!host->RunQuiescent(last_frame, [this](std::string& why) {
            std::lock_guard lock{mu};
            if (state != State::Recording) return CutOutcome::Done;
            why = "the last accepted present has not composed";
            return CutOutcome::Retry;
        }, error)) {
        std::lock_guard lock{mu};
        if (state == State::Recording) Fail(IncompleteInterrupted, error);
    }
}

CaptureReport Capture::End(bool run_ended_normally) {
    Deactivate();
    CompleteRecord complete;
    FileHeader final_header;
    bool clean = false;
    {
        std::lock_guard lock{mu};
        const bool cut_done = start_frame != 0;
        if (!run_ended_normally || state != State::Finished) {
            reasons |= IncompleteInterrupted;
            if (error_text.empty()) {
                error_text = !cut_done ? "the selected frames were not reached"
                             : run_ended_normally ? "the run ended before the selected frames completed"
                                                  : "the capture was interrupted";
            }
            state = State::Failed;
        }
        if (missing_pictures != 0) {
            cap_unresolved += missing_pictures;
            reasons |= IncompleteUnresolvedResource;
            LOG_WARNING(HW_GPU, "Replay capture unresolved: {} frame(s) missing composed picture", missing_pictures);
            if (error_text.empty()) {
                error_text = "the composed picture was unavailable for " +
                             std::to_string(missing_pictures) + " frame(s)";
            }
        }
        ApplyTrackerStats(tracker->Stats(), complete);
        if (has_library && cut_done) {
            LibraryCallSource* const current = RegisteredLibrarySource();
            if (current == library) {
                const LibraryCoverage coverage = library->Coverage();
                complete.unsupported_schemas += coverage.unsupported_schemas;
                if (coverage.unsupported_schemas != 0) {
                    reasons |= IncompleteUnsupportedSchema;
                }
                if (coverage.unresolved_calls != 0) {
                    cap_unresolved += coverage.unresolved_calls;
                    reasons |= IncompleteUnresolvedResource;
                }
                if (coverage.dropped_records != 0) {
                    library_dropped += coverage.dropped_records;
                }
                if (!coverage.checkpoint_complete || !library_checkpoint.complete) {
                    reasons |= IncompleteClosureFailed;
                }
            } else {
                reasons |= IncompleteClosureFailed;
            }
            if (library_dropped != 0) {
                reasons |= IncompleteDroppedEvents;
            }
            if (library_checkpoint_overflow) {
                reasons |= IncompleteLimitExceeded;
            }
        }
        complete.incomplete_reasons |= reasons;
        complete.dropped_events = Saturate32(u64{complete.dropped_events} + cap_dropped + library_dropped);
        complete.overflow_events = Saturate32(u64{complete.overflow_events} + cap_overflow);
        complete.unresolved_resources = Saturate32(u64{complete.unresolved_resources} + cap_unresolved);
        complete.gpu_words = total_gpu_words;
        complete.library_calls = library_calls;
        clean = complete.incomplete_reasons == 0 && complete.unsupported_schemas == 0 &&
                complete.unresolved_resources == 0 && complete.overflow_events == 0 &&
                complete.dropped_events == 0;
        complete.status = clean ? CompleteStatus::Complete : CompleteStatus::Incomplete;
        complete.dependency_complete = clean ? 1 : 0;
        complete.library_replay_ready = 0;  // the reader's closure decides, never the writer

        header.finalized_frames = selected_finished;
        header.preroll_frames = cut_done ? static_cast<u32>(first_frame - start_frame) : config.preroll_frames;
        header.flags = 0;
        if (clean) {
            header.flags |= HeaderDependencyComplete;
        }
        if (has_library && cut_done) {
            header.flags |= HeaderHasLibrarySection;
        } else {
            header.optional_features &= ~static_cast<u64>(OptionalLibrarySection);
        }
        if (pictures != 0 && missing_pictures == 0) {
            header.flags |= HeaderHasLivePictures;
        } else {
            header.optional_features &= ~static_cast<u64>(OptionalLivePictures);
        }
        final_header = header;
        held.clear();
        held_bytes = 0;
    }
    ReleasePins();
    std::string finish_error;
    const auto sidecar = std::filesystem::path(config.path).parent_path() / "expected_effects.jsonl";
    const bool wrote_effects = clean && WriteEffects(sidecar, error_text);
    if (clean && !wrote_effects) {
        clean = false;
        complete.status = CompleteStatus::Incomplete;
        complete.dependency_complete = 0;
        complete.incomplete_reasons |= IncompleteWriteFailed;
        final_header.flags &= ~HeaderDependencyComplete;
    }
    const bool finalized = sink.Finish(clean, final_header, complete, finish_error);

    if (!finalized && wrote_effects) {
        std::error_code ignored;
        std::filesystem::remove(sidecar, ignored);
    }

    CaptureReport report;
    const u32 failed_bits = IncompleteWriteFailed | IncompleteCheckpointUnavailable;
    report.status = finalized ? SectionStatus::Complete
                              : ((complete.incomplete_reasons & failed_bits) != 0 ? SectionStatus::Failed
                                                                                 : SectionStatus::Incomplete);
    report.run_complete = finalized;
    report.dependency_complete = complete.dependency_complete == 1 && finalized;
    report.library_replay_ready = false;
    report.library_section = (final_header.flags & HeaderHasLibrarySection) != 0;
    report.format_major = FormatMajor;
    report.format_minor = FormatMinor;
    report.first_frame = first_frame;
    report.requested_frames = config.frame_count;
    report.finalized_frames = final_header.finalized_frames;
    report.preroll_frames = final_header.preroll_frames;
    report.file_bytes = sink.BytesWritten();
    report.chunks = sink.ChunkCount();
    report.gpu_words = complete.gpu_words;
    report.library_calls = complete.library_calls;
    report.initial_backing_bytes = complete.initial_backing_bytes;
    report.delta_bytes = complete.delta_bytes;
    report.dedup_bytes = complete.dedup_bytes;
    report.incomplete_reasons = complete.incomplete_reasons;
    report.dropped_events = complete.dropped_events;
    report.unresolved_resources = complete.unresolved_resources;
    report.outputs_recorded = output_stats.outputs_recorded;
    report.readbacks_recorded = output_stats.readbacks_recorded;
    report.outputs_not_comparable = output_stats.not_comparable;
    report.syncpoints_skipped = output_stats.syncpoints_skipped;
    report.outputs_overflowed = output_stats.overflowed;
    report.pins_unknown = output_stats.pins_unknown;
    if (!finalized) {
        report.error = !error_text.empty() ? error_text : finish_error;
        if (report.error.empty()) {
            report.error = "the capture is incomplete";
        }
    }
    {
        std::lock_guard stats_lock{g_output_stats_mutex};
        g_output_stats = output_stats;
    }
    if (!finalized) {
        Common::RunOutcome::Request(Common::RunOutcome::Reason::CaptureFailed, report.error);
    }
    PublishCaptureReport(report);
    return report;
}

} // namespace

// ---- public entry points --------------------------------------------------------------------------

bool BeginCaptureWithHost(std::unique_ptr<CaptureHost> host, const CaptureConfig& config,
                          std::string& error) {
    std::lock_guard lifecycle{g_lifecycle_mutex};
    auto capture = std::make_shared<Capture>(std::move(host), config);
    {
        std::lock_guard lock{g_registry_mutex};
        if (g_capture != nullptr) {
            error = "a capture is already running";
            return false;
        }
        g_capture = capture;
    }
    g_call_seq.store(0, std::memory_order_relaxed);
    g_open_frame.store(config.first_frame > config.preroll_frames ? config.first_frame - config.preroll_frames : 1,
                       std::memory_order_relaxed);
    detail::g_capture_active.store(true, std::memory_order_relaxed);
    if (!capture->Begin(error)) {
        DetachAndDrainHooks();
        capture->ReleasePins();
        Common::RunOutcome::Request(Common::RunOutcome::Reason::CaptureFailed, error);
        return false;
    }
    return true;
}

CaptureReport EndCapture(bool run_ended_normally) {
    const std::lock_guard lifecycle{g_lifecycle_mutex};
    if (run_ended_normally) {
        if (auto pending = Current()) pending->DrainFinal();
    }
    auto capture = DetachAndDrainHooks();
    if (capture == nullptr) {
        return CaptureReport{};
    }
    return capture->End(run_ended_normally);
}

void NoteGraphicsCallSlow() {
    if (const auto capture = Current()) capture->GraphicsCall();
}

void NoteEngineEffectSlow(const LiveEffect& effect) {
    if (const auto capture = Current()) capture->EngineEffect(effect);
}

void NoteSubmitSlow(s32 channel, const Tegra::CommandList& list) {
    if (const auto capture = Current()) {
        capture->Submit(channel, list);
    }
}

void NoteGuestWriteSlow(u64 device_address, u64 size) {
    if (const auto capture = Current()) {
        capture->GuestWrite(device_address, size);
    }
}

void NoteAcceptedPresentSlow(u64 present_id, PresentPath path,
                             std::span<const Tegra::FramebufferConfig> layers,
                             std::span<const Service::Nvidia::NvFence> fences) {
    if (const auto capture = Current()) {
        capture->AcceptedPresent(present_id, path, layers, fences);
    }
}

void NoteGpuMapSlow(u32 address_space, u64 gpu_va, u64 size, u64 device, u32 flags, u32 pte_kind) {
    if (const auto capture = Current()) {
        capture->GpuMap(address_space, gpu_va, size, device, flags, pte_kind);
    }
}

void NoteGpuMapSparseSlow(u32 address_space, u64 gpu_va, u64 size, u32 flags) {
    if (const auto capture = Current()) {
        capture->GpuMapSparse(address_space, gpu_va, size, flags);
    }
}

void NoteGpuUnmapSlow(u32 address_space, u64 gpu_va, u64 size) {
    if (const auto capture = Current()) {
        capture->GpuUnmap(address_space, gpu_va, size);
    }
}

void NoteGpuWriteSlow(u64 device, u64 size) {
    if (const auto capture = Current()) {
        capture->GpuWrite(device, size);
    }
}

void NoteGpuReadSlow(u64 device, u64 size) {
    if (const auto capture = Current()) {
        capture->GpuRead(device, size);
    }
}

void NoteExternalWriteSlow(u64 device, u64 size) {
    if (const auto capture = Current()) {
        capture->ExternalWrite(device, size);
    }
}

void NoteConsumeSlow(s32 channel, u32 entry_index, u64 gpu_va, u32 word_offset, u32 flags, u32 epoch,
                     std::span<const u32> words) {
    if (const auto capture = Current()) {
        capture->Consume(channel, entry_index, gpu_va, word_offset, flags, epoch, words);
    }
}

void NoteChannelCreateSlow(s32 channel, u32 address_space_id, u32 flags) {
    if (const auto capture = Current()) {
        capture->ChannelCreate(channel, address_space_id, flags);
    }
}

void NoteChannelReleaseSlow(s32 channel) {
    if (const auto capture = Current()) {
        capture->ChannelRelease(channel);
    }
}

void NoteFenceProducedSlow(FenceKind kind, s32 channel, u32 address_space, u64 id, u64 value,
                           u64 aux, bool external) {
    if (const auto capture = Current()) {
        FenceNote note;
        note.kind = kind;
        note.channel = channel;
        note.space = address_space;
        note.id = id;
        note.value = value;
        note.aux = aux;
        note.external = external;
        capture->FenceProduced(note);
    }
}

void NoteFenceWaitSlow(FenceKind kind, s32 channel, u32 address_space, u64 id, u64 value,
                       u64 device, bool blocked) {
    if (const auto capture = Current()) {
        FenceNote note;
        note.kind = kind;
        note.channel = channel;
        note.space = address_space;
        note.id = id;
        note.value = value;
        note.device = device;
        note.blocked = blocked;
        capture->FenceWait(note);
    }
}

void NoteHostReadbackSlow(u64 device, u64 size) {
    if (const auto capture = Current()) {
        capture->HostReadback(device, size);
    }
}

void NoteOutputNotComparableSlow() {
    if (const auto capture = Current()) {
        capture->OutputNotComparable();
    }
}

CaptureOutputStats LastCaptureOutputStats() noexcept {
    std::lock_guard lock{g_output_stats_mutex};
    return g_output_stats;
}

void NoteFrameComposedSlow(u64 present_id) {
    if (const auto capture = Current()) {
        capture->FrameComposed(present_id);
    }
}

CallTicket NextCallTicket() noexcept {
    return CallTicket{g_call_seq.fetch_add(1, std::memory_order_relaxed) + 1,
                      g_open_frame.load(std::memory_order_relaxed)};
}

bool RecordLibraryCall(const CallTicket& ticket, const CallRecordView& record) noexcept {
    const auto capture = Current();
    return !capture || capture->LibraryRecord(ticket, record);
}

} // namespace VideoCore::Replay
