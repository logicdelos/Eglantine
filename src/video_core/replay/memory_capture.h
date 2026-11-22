// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Owned by slice B: memory provenance bookkeeping of the capture, and the declarations of the
// capture's hooks inside files no 0.5 slice owns (dma_pusher.cpp, memory_manager.cpp,
// nvdisp_disp0.cpp). Contract rules that bind it live in events.h (allocation model, provenance).
//
// MemoryTracker is pure range bookkeeping with no knowledge of the engine: it sees device
// addresses (the address space the GPU's mappings resolve to), a MemorySource that reads bytes at
// the moment it is asked, and the events a capture feeds it (map, unmap, CPU write, GPU write,
// external write, read). It answers with the MAP / UNMAP / MEMORY_BLOB / EXTERNAL_WRITE payloads
// the capture must append, in the order they must be appended.
//
// Model
//   * The device address space is cut into fixed windows (default 1 MiB). A window that is
//     mapped by at least one GPU range is an allocation: dense nonzero alloc_id, a generation that
//     increases whenever the window lost its last mapped page and is mapped again, and an
//     allocation size equal to the window size. A GPU range that crosses windows becomes one MAP
//     entry per window; two GPU ranges over the same device memory name the same allocation
//     (an alias), and mapping a GPU range that is already mapped is a MapRemap.
//   * Per window three byte-exact interval sets: `captured` (the replayer's copy holds the right
//     bytes), `derived` (the GPU wrote them; replay recomputes, never stored) and `dirty` (the
//     CPU wrote them since the last flush). A byte of a mapped window that is in neither
//     `captured` nor `derived` is Undefined: reading it is unknown provenance, counted once per
//     disjoint range in `unresolved_resources` and never guessed.
//   * A page that loses its last GPU mapping is forgotten: its content can change without any
//     notification, so a later mapping captures it again.
//   * Content is captured when bytes become mapped (Initial phase before the first recorded
//     frame, Delta after), when the CPU wrote them (the dirty set is read at Flush, so the bytes
//     are the ones the GPU will see), and when an external device wrote them (read at once).
//     Whole pages are deduplicated by a 128-bit digest: content equal to what the replayer holds
//     for the page is not written, an all-zero page becomes Zero. Edges of a range that do not
//     cover a whole page are always written as data. The tracker never emits a Ref range: the
//     replayer accepts one only for bytes unchanged since the chunk it names, and content that
//     returns to an older value has been overwritten in between, so it is stored again.
//
// Not thread safe: the capture serialises calls with one mutex.

#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "common/common_types.h"
#include "video_core/replay/capture.h"
#include "video_core/replay/effect.h"
#include "video_core/replay/events.h"
#include "video_core/replay/format.h"
#include "video_core/replay/format_io.h"

namespace VideoCore::Replay {

inline constexpr u64 TrackerPageBytes = 4096;
inline constexpr u64 TrackerWindowBytes = 1ULL << 20;

struct ByteRange {
    u64 begin = 0;  // inclusive
    u64 end = 0;    // exclusive

    [[nodiscard]] u64 Size() const noexcept {
        return end - begin;
    }
    friend bool operator==(const ByteRange&, const ByteRange&) = default;
};

// A set of byte ranges kept sorted, disjoint and merged (touching ranges are one range).
class IntervalSet {
public:
    void Add(u64 begin, u64 end);
    void Remove(u64 begin, u64 end);
    void Clear() noexcept {
        ranges.clear();
    }
    // The whole of [begin, end) is in the set (an empty query is contained).
    [[nodiscard]] bool Contains(u64 begin, u64 end) const;
    [[nodiscard]] bool Intersects(u64 begin, u64 end) const;
    // Parts of [begin, end) that are in the set, ascending.
    [[nodiscard]] std::vector<ByteRange> Intersection(u64 begin, u64 end) const;
    // Parts of [begin, end) that are not in the set, ascending.
    [[nodiscard]] std::vector<ByteRange> Gaps(u64 begin, u64 end) const;
    [[nodiscard]] bool Empty() const noexcept {
        return ranges.empty();
    }
    [[nodiscard]] u64 TotalBytes() const noexcept;
    [[nodiscard]] const std::vector<ByteRange>& Ranges() const noexcept {
        return ranges;
    }

private:
    std::vector<ByteRange> ranges;
};

// 128 bit content digest of a page. Not cryptographic; it only has to make an accidental
// collision between two different pages of one capture negligible.
struct PageDigest {
    u64 a = 0;
    u64 b = 0;
    friend bool operator==(const PageDigest&, const PageDigest&) = default;
};
[[nodiscard]] PageDigest DigestBytes(std::span<const u8> bytes) noexcept;
[[nodiscard]] bool IsAllZero(std::span<const u8> bytes) noexcept;

// Reads bytes of the device address space at the moment of the call. False when any byte of the
// range cannot be read; the tracker then leaves the range Undefined and counts the failure.
class MemorySource {
public:
    virtual ~MemorySource() = default;
    [[nodiscard]] virtual bool ReadDevice(u64 device_address, std::span<u8> out) = 0;
};

enum class TrackerPhase : u32 { Initial, Delta };

struct TrackerConfig {
    u64 window_bytes = TrackerWindowBytes;  // power of two, at least TrackerPageBytes
    u64 max_initial_backing_bytes = MaxInitialBackingBytes;
    u64 max_new_payload_bytes_per_frame = MaxNewPayloadBytesPerFrame;
    u64 max_chunk_bytes = MaxChunkBytes;
};

struct TrackerStats {
    u64 initial_backing_bytes = 0;  // Data and Zero bytes captured in the Initial phase
    u64 delta_bytes = 0;            // Data bytes captured in the Delta phase and external writes
    u64 dedup_bytes = 0;            // bytes not written because the page content was known
    u64 external_bytes = 0;         // subset of delta_bytes written by external devices
    u32 unresolved_resources = 0;   // disjoint ranges read while Undefined
    u32 read_failures = 0;          // MemorySource refusals
    bool limit_initial_backing = false;
    bool limit_new_payload = false;
    bool limit_chunk = false;       // one range or record alone exceeds max_chunk_bytes
};

struct BlobChunk {
    ChunkType type = ChunkType::MemoryBlob;  // MemoryBlob or ExternalWrite
    u32 ordinal = 0;
    MemoryBlob blob;
};

// What the capture must append, in this order: unmaps, blobs, maps.
//
// With a `sink` set, a blob chunk is handed to it as soon as the next one is opened, so a
// producer holds at most one chunk (plus the source read of one window) instead of every chunk
// of a mapping or flush. The sink gets the chunks in order; it must emit `unmaps` first.
struct MemoryOutput {
    std::vector<UnmapEntry> unmaps;
    std::vector<BlobChunk> blobs;
    std::vector<MapEntry> maps;
    std::function<void(BlobChunk&&)> sink;

    [[nodiscard]] bool Empty() const noexcept {
        return unmaps.empty() && blobs.empty() && maps.empty();
    }
    void Clear() {
        unmaps.clear();
        blobs.clear();
        maps.clear();
    }
};

class MemoryTracker {
public:
    MemoryTracker(MemorySource& source, const TrackerConfig& config);
    ~MemoryTracker();
    MemoryTracker(const MemoryTracker&) = delete;
    MemoryTracker& operator=(const MemoryTracker&) = delete;

    void SetPhase(TrackerPhase new_phase) noexcept {
        phase = new_phase;
    }
    // Resets the per-frame new payload budget. Called at every frame start.
    void BeginFrame() noexcept {
        frame_payload_bytes = 0;
    }

    // GPU range [gpu_va, gpu_va + size) of an address space now maps device memory at `device`.
    // `flags` are MapFlag bits; MapRemap is added when the range replaced a mapping.
    void Map(u32 address_space, u64 gpu_va, u64 size, u64 device, u32 flags, u32 pte_kind,
             MemoryOutput& out);
    // A range without backing (sparse or reserved): no allocation, alloc_id 0.
    void MapSparse(u32 address_space, u64 gpu_va, u64 size, u32 flags, MemoryOutput& out);
    void Unmap(u32 address_space, u64 gpu_va, u64 size, MemoryOutput& out);

    // The guest CPU wrote the device range (reported before or after the write; the bytes are
    // read at Flush). Writes to bytes that are not mapped are ignored.
    void NoteCpuWrite(u64 device, u64 size);
    // GPU work wrote the device range: never stored, recomputed by replay; supersedes pending CPU
    // writes of the same bytes.
    void NoteGpuWrite(u64 device, u64 size);
    // Another device wrote the device range: the bytes are read now and become an EXTERNAL_WRITE.
    void NoteExternalWrite(u64 device, u64 size, MemoryOutput& out);
    // Something reads the device range. Returns the number of NEW unresolved ranges found.
    u32 NoteRead(u64 device, u64 size);

    // Reads the pending CPU writes into delta blobs.
    void Flush(MemoryOutput& out);
    [[nodiscard]] bool HasPendingWrites() const noexcept {
        return !dirty_windows.empty();
    }

    [[nodiscard]] const TrackerStats& Stats() const noexcept {
        return stats;
    }
    [[nodiscard]] u32 NextOrdinal() const noexcept {
        return next_ordinal;
    }
    // Number of allocations (windows ever mapped) and the generation of the one at `device`
    // (0 when the window was never mapped); introspection for tests and the report.
    [[nodiscard]] u64 AllocationCount() const noexcept {
        return windows.size();
    }
    [[nodiscard]] u32 GenerationAt(u64 device) const noexcept;
    [[nodiscard]] u64 AllocationIdAt(u64 device) const noexcept;
    // True when [device, device + size) is known to the replayer: captured or derived.
    [[nodiscard]] bool IsResolved(u64 device, u64 size) const;
    // Names the byte at `device` as (allocation id, generation, offset inside the allocation).
    // False when the window was never mapped or `size` bytes from there leave the allocation.
    struct Location {
        u64 alloc_id = 0;
        u32 generation = 0;
        u64 offset = 0;
    };
    [[nodiscard]] bool Locate(u64 device, u64 size, Location& out) const;

private:
    struct PageInfo {
        PageDigest current;
        bool current_valid = false;
    };

    struct Window {
        u64 alloc_id = 0;
        u32 generation = 0;
        u64 base = 0;
        u32 mapped_pages = 0;
        bool declared = false;  // the first blob of this generation was produced
        std::vector<u32> page_refs;
        std::vector<PageInfo> pages;
        IntervalSet captured;
        IntervalSet derived;
        IntervalSet dirty;
    };

    struct MapRecord {
        u64 end = 0;
        u64 device = 0;
        bool sparse = false;
    };
    using AddressSpace = std::map<u64, MapRecord>;

    [[nodiscard]] Window* FindWindow(u64 device) noexcept;
    [[nodiscard]] const Window* FindWindow(u64 device) const noexcept;
    // The window at `window_index`: created on first use, a new generation when it was unmapped.
    [[nodiscard]] Window& AcquireWindow(u64 window_index);
    // Counts the pages of one device range (inside one window) as mapped, declares the window
    // when needed and captures the bytes the replayer does not hold yet.
    Window& ReferencePages(u64 device, u64 end, MemoryOutput& out);
    // Drops the mapped pages of one released device range; a page that loses its last mapping is
    // forgotten.
    void ReleasePages(u64 device, u64 end);
    // Carves [begin, end) out of every record of the space that overlaps it, calling `visit` on
    // each overlapped piece (gpu begin, gpu end, record), and keeps the remainders.
    template <typename Visit>
    bool CarveRange(AddressSpace& space, u64 begin, u64 end, Visit&& visit);
    // Captures `ranges` (window-relative offsets) of `window` into blobs of `type`.
    void CaptureRanges(Window& window, std::span<const ByteRange> ranges, ChunkType type,
                       WriterKind writer, MemoryOutput& out);

    MemorySource& source;
    TrackerConfig config;
    TrackerPhase phase = TrackerPhase::Initial;
    TrackerStats stats;
    u64 frame_payload_bytes = 0;
    u32 next_ordinal = 0;
    u64 next_alloc_id = 1;
    std::unordered_map<u64, Window> windows;  // key: window index (device / window_bytes)
    std::set<u64> dirty_windows;
    std::unordered_map<u32, AddressSpace> spaces;
    IntervalSet unresolved_seen;  // device ranges already counted as unresolved
};

// Adds what the tracker saw to a COMPLETE record: its byte counters, the unresolved resources
// (reads of Undefined bytes and refused source reads), and the matching IncompleteReason bits.
// A limit that was hit adds one overflow event and IncompleteLimitExceeded.
void ApplyTrackerStats(const TrackerStats& stats, CompleteRecord& record) noexcept;

// ---- staging sink -----------------------------------------------------------------------------
//
// Where the capture hands finished chunks: a bounded queue drained by one writer thread that
// calls CaptureWriter::Append. Producers never touch the file and never wait (Stage refuses a
// chunk that does not fit, counts it and marks the sink unhealthy). Every bound is enforced
// here, because CaptureWriter only enforces the format maxima: the session's max_file_bytes
// (with room kept for INDEX and COMPLETE), max_chunk_bytes, max_staging_bytes and
// MaxChunkCount. A sink that dropped a chunk or failed to write can still be finished, but only
// as .partial.

struct StagingStats {
    u64 chunks_staged = 0;
    u64 chunks_written = 0;
    u64 dropped_chunks = 0;
    u64 overflow_events = 0;      // bounds hit (chunk, staging, file size, chunk count)
    u64 peak_queued_bytes = 0;
    u64 bytes_staged = 0;         // payload bytes of the chunks accepted
    bool write_failed = false;
    std::string error;            // the first failure, empty when healthy
};

class StagingWriter {
public:
    explicit StagingWriter(const SessionLimits& limits);
    ~StagingWriter();
    StagingWriter(const StagingWriter&) = delete;
    StagingWriter& operator=(const StagingWriter&) = delete;

    // Opens "<final_path>.partial" through CaptureWriter and starts the writer thread.
    [[nodiscard]] bool Open(const std::string& final_path, const FileHeader& header,
                            std::string& error);
    // Queues one chunk. False when it was dropped (bound hit or sink already failed). With
    // `may_wait` a full queue is waited out (bounded by a timeout) instead of refused: only the
    // one-time initial snapshot uses it, never a hook on the guest or GPU thread.
    bool Stage(ChunkType type, u64 frame_id, std::vector<u8>&& payload, bool may_wait = false);
    // The sequence the next accepted chunk will get.
    [[nodiscard]] u64 NextSequence() const;
    // Nothing was dropped and no write failed.
    [[nodiscard]] bool Healthy() const;
    [[nodiscard]] StagingStats Stats() const;
    // Waits for the queue to drain and stops the thread. Folds the sink's own failures into
    // `complete` (overflow, dropped events, write failure; status drops to Incomplete). With
    // `finalize` and a healthy sink, finalizes the file (renaming .partial to the final name when
    // CompleteIsClean); otherwise abandons it. True only when the file was finalized.
    bool Finish(bool finalize, const FileHeader& header, CompleteRecord& complete,
                std::string& error);
    [[nodiscard]] u64 BytesWritten() const;
    [[nodiscard]] u64 ChunkCount() const;

private:
    struct Item {
        ChunkType type = ChunkType::Session;
        u64 frame_id = 0;
        std::vector<u8> payload;
    };
    void Run();
    void Stop();

    SessionLimits limits;
    CaptureWriter writer;
    mutable std::mutex mutex;
    std::condition_variable work;   // the writer waits for items; Stage(may_wait) waits for room
    std::condition_variable room;
    std::deque<Item> queue;
    u64 queued_bytes = 0;
    u64 next_sequence = 0;
    u64 reserved_bytes = 0;         // file bytes accounted for accepted chunks (headers included)
    bool opened = false;
    bool stopping = false;
    bool finished = false;
    StagingStats stats;
    std::thread thread;
    std::atomic<u64> written_bytes{0};
    std::atomic<u64> written_chunks{0};
};

// ---- host seams -------------------------------------------------------------------------------
//
// What the capture needs from the renderer and settings that no engine API exposes. The
// integrator registers them once at start (renderer_vulkan.cpp or the launch code); a missing
// seam is a missing fact, never a guess: without `read_composed_picture` the selected frames have
// no live picture and the capture stays .partial; without `device_identity` the SESSION device is
// all zero.

struct CaptureHostHooks {
    // Reads back the picture the emulator displayed for accepted present `present_id`, after its
    // composite finished, as B8G8R8A8Unorm. Called on the thread that composited. False when the
    // picture is unavailable.
    bool (*read_composed_picture)(Core::System& system, u64 present_id, EndPicture& out) = nullptr;
    bool (*device_identity)(Core::System& system, DeviceIdentity& out) = nullptr;
    // The version of the running title (the application's version number). False when the host
    // cannot read it: the header then records the version as unavailable.
    bool (*title_version)(Core::System& system, u32& version) = nullptr;
};
void SetCaptureHostHooks(const CaptureHostHooks& hooks) noexcept;

// ---- engine hooks private to slice B ------------------------------------------------------
//
// Each is a relaxed load while no capture runs (CaptureActive) and a call into capture.cpp
// otherwise. Called from memory_manager.cpp and dma_pusher.cpp (this slice's), and from the
// frame-completion and channel-creation sites the integrator patches (see the slice report).

// The flags of a native sparse mapping (MemoryManager::MapSparse), at the cut and at the hook.
// Only MapSparse: the replayer re-creates the mapping with MemoryManager::MapSparse, while a
// MapReserved range has no replay representation and is refused (MapFlagsReplayable).
[[nodiscard]] constexpr u32 NativeSparseFlags(bool big_pages) noexcept {
    return static_cast<u32>(MapSparse) | (big_pages ? static_cast<u32>(MapBigPages) : 0U);
}

// MemoryManager::Map / MapSparse / Unmap after the page table changed. `address_space` is
// MemoryManager::GetID(); `flags` are MapFlag bits.
void NoteGpuMapSlow(u32 address_space, u64 gpu_va, u64 size, u64 device, u32 flags, u32 pte_kind);
void NoteGpuMapSparseSlow(u32 address_space, u64 gpu_va, u64 size, u32 flags);
void NoteGpuUnmapSlow(u32 address_space, u64 gpu_va, u64 size);
// Bytes the GPU side wrote through the MemoryManager (derived: replay recomputes them).
void NoteGpuWriteSlow(u64 device, u64 size);
// Bytes the GPU side read through the MemoryManager (unknown provenance is counted).
void NoteGpuReadSlow(u64 device, u64 size);
// Another device wrote the range (video decode and the like): becomes an EXTERNAL_WRITE.
void NoteExternalWriteSlow(u64 device, u64 size);
// DmaPusher: words consumed from one entry. `entry_index` is the index in the submitted list,
// `word_offset` the words of the entry consumed before, `flags` ConsumeFlag bits.
void NoteConsumeSlow(s32 channel, u32 entry_index, u64 gpu_va, u32 word_offset, u32 flags,
                     u32 epoch, std::span<const u32> words);
void NoteChannelCreateSlow(s32 channel, u32 address_space_id, u32 flags);
void NoteChannelReleaseSlow(s32 channel);
// After the composite of accepted present `present_id` finished on the render thread.
void NoteFrameComposedSlow(u64 present_id);

inline void NoteGpuMap(u32 address_space, u64 gpu_va, u64 size, u64 device, u32 flags,
                       u32 pte_kind) {
    if (CaptureActive()) {
        NoteGpuMapSlow(address_space, gpu_va, size, device, flags, pte_kind);
    }
}
inline void NoteGpuMapSparse(u32 address_space, u64 gpu_va, u64 size, u32 flags) {
    if (CaptureActive()) {
        NoteGpuMapSparseSlow(address_space, gpu_va, size, flags);
    }
}
inline void NoteGpuUnmap(u32 address_space, u64 gpu_va, u64 size) {
    if (CaptureActive()) {
        NoteGpuUnmapSlow(address_space, gpu_va, size);
    }
}
inline void NoteGpuWrite(u64 device, u64 size) {
    if (CaptureActive()) {
        NoteGpuWriteSlow(device, size);
    }
}
inline void NoteGpuRead(u64 device, u64 size) {
    if (CaptureActive()) {
        NoteGpuReadSlow(device, size);
    }
}
inline void NoteExternalWrite(u64 device, u64 size) {
    if (CaptureActive()) {
        NoteExternalWriteSlow(device, size);
    }
}
inline void NoteConsume(s32 channel, u32 entry_index, u64 gpu_va, u32 word_offset, u32 flags,
                        u32 epoch, std::span<const u32> words) {
    if (CaptureActive()) {
        NoteConsumeSlow(channel, entry_index, gpu_va, word_offset, flags, epoch, words);
    }
}
inline void NoteChannelCreate(s32 channel, u32 address_space_id, u32 flags) {
    if (CaptureActive()) {
        NoteChannelCreateSlow(channel, address_space_id, flags);
    }
}
inline void NoteChannelRelease(s32 channel) {
    if (CaptureActive()) {
        NoteChannelReleaseSlow(channel);
    }
}
// ---- fences ---------------------------------------------------------------------------------
//
// Producer: something made a syncpoint or a memory fence reach a value. `kind` is FenceKind;
// `address_space` is MemoryManager::GetID() of the memory the value lives in (ignored for a
// syncpoint); `channel` is a GPU channel (bind_id) that uses that address space, 0 when the site
// does not know one (the capture then picks the channel it recorded for the address space). For a
// syncpoint `id` is the syncpoint index and `value` the value after the raise; for a memory fence
// `id` is the GPU virtual address and `value` the value written (`aux` 4 or 8 overrides the width
// of the kind: semaphore 4, query and timestamp 8, else 0).
//   external == true : a device or thread outside the recorded commands produced it. It becomes a
//                      FENCE chunk the replayer applies, at its position among the events.
//   external == false: the recorded commands themselves produced it (a GPU semaphore release or
//                      query write, a syncpoint raised by a GPU list). Replay recomputes it: no
//                      chunk, but the value is a producer for later waits and, for a memory
//                      fence, a semantic output of the frame (Timestamp values are counted as not
//                      comparable).
void NoteFenceProducedSlow(FenceKind kind, s32 channel, u32 address_space, u64 id, u64 value,
                           u64 aux, bool external);
// Wait released: a recorded command stopped waiting on a fence. `blocked` is true when the wait
// had to suspend or spin (the producer ran during the capture), false when the value was already
// there at the check. `value` is the syncpoint threshold, or for a memory fence the value read
// from the word when the wait released; `device` is the device address of that word, 0 when
// unknown (it lets a guest CPU write count as the producer).
void NoteFenceWaitSlow(FenceKind kind, s32 channel, u32 address_space, u64 id, u64 value,
                       u64 device, bool blocked);
inline LiveEffect ProducedEffect(FenceKind kind, u64 id, u64 value) {
    return kind == FenceKind::SyncpointRaised ? LiveEffect{EffectKind::SyncpointIncrement, id, 1, true}
           : LiveEffect{EffectKind::FenceSignal, id, value, kind != FenceKind::Timestamp};
}
void NoteEngineEffectSlow(const LiveEffect& effect);
inline void NoteEngineEffect(EffectKind kind) {
    const LiveEffect effect{kind, 0, 0, true};
    if (CaptureActive()) {
        NoteEngineEffectSlow(effect);
    } else {
        ObserveReplayEffect(effect);
    }
}
inline void NoteFenceProduced(FenceKind kind, s32 channel, u32 address_space, u64 id, u64 value,
                              u64 aux, bool external) {
    if (CaptureActive()) {
        NoteFenceProducedSlow(kind, channel, address_space, id, value, aux, external);
    } else if (!external) {
        ObserveReplayEffect(ProducedEffect(kind, id, value));
    }
}
inline void NoteFenceWait(FenceKind kind, s32 channel, u32 address_space, u64 id, u64 value,
                          u64 device, bool blocked) {
    if (CaptureActive()) {
        NoteFenceWaitSlow(kind, channel, address_space, id, value, device, blocked);
    }
}

// ---- semantic outputs --------------------------------------------------------------------------
//
// FRAME_END carries the outputs the replay compares with what it observes:
//   SemaphoreValue  a GPU-written semaphore word (id = GPU address, value_lo = the u32);
//   QueryResult     a GPU-written 64-bit query value (id = GPU address, value_lo = the u64);
//   ReadbackChecksum  a host readback of GPU memory (NoteHostReadback): id = allocation id,
//                   value_lo = FNV-1a 64 of bytes [0, value_hi) of the allocation, taken at FRAME_END.
// Both semaphore and query outputs come from NoteFenceProduced(..., external = false); the last
// write to an address in a frame wins. Timestamps are not comparable. Syncpoint values are not
// recorded: the replay cannot restore the absolute value (see the slice report).
//
// A host readback of `size` bytes at device address `device` (a guest read of GPU-written memory:
// flush, readback to the guest, screenshot). Recorded only when the range starts an allocation the
// capture knows completely.
void NoteHostReadbackSlow(u64 device, u64 size);
inline void NoteHostReadback(u64 device, u64 size) {
    // Idle: one relaxed load (ObserveReplayReadback checks the origin itself). The thread-local
    // origin is read only while a capture records.
    if (CaptureActive()) {
        if (detail::readback_origin == detail::ReadbackOrigin::EmulatedWork) {
            NoteHostReadbackSlow(device, size);
        }
    } else {
        ObserveReplayReadback();
    }
}

// A semantic output the capture cannot compare was produced by the recorded commands: a counter
// query (anything but a payload), or a reduction release whose value is computed when the fence
// signals. Replay recomputes it; the capture only counts it (`not_comparable`) so a clean file
// does not hide that the output is missing.
void NoteOutputNotComparableSlow();
inline void NoteOutputNotComparable() {
    if (CaptureActive()) {
        NoteOutputNotComparableSlow();
    } else {
        ObserveReplayEffect({EffectKind::FenceSignal, 0, 0, false});
    }
}

// What the last capture could and could not record as outputs. `not_comparable` counts memory
// productions and readbacks that have no comparison convention, `syncpoints_skipped` the GPU
// syncpoint raises that are not recorded, `overflowed` the outputs refused by MaxSemanticOutputs.
struct CaptureOutputStats {
    u64 outputs_recorded = 0;
    u64 readbacks_recorded = 0;
    u64 not_comparable = 0;
    u64 syncpoints_skipped = 0;
    u64 overflowed = 0;
    u64 pins_unknown = 0;  // session pins the host could not read; written as SessionPinUnknown
};
// The stats of the capture that ended last; zero before the first capture ends.
[[nodiscard]] CaptureOutputStats LastCaptureOutputStats() noexcept;

inline void NoteFrameComposed(u64 present_id) {
    if (CaptureActive()) {
        NoteFrameComposedSlow(present_id);
    }
}

} // namespace VideoCore::Replay
