// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <span>
#include <unordered_map>
#include <vector>

#include "common/common_types.h"
#include "video_core/cache_envelope.h"

// Preparation of the recorded pipelines ahead of demand: the state machine only. It holds no
// Vulkan object and no thread; the pipeline cache runs its workers against it. An entry is one
// record of an accepted cache file:
//
//   Indexed -> Loading -> Queued -> Building -> Ready -> Adopted
//
// with the terminal states Skipped (not buildable here, or the same key is already known),
// Failed, Claimed (a draw took a queued record, the normal path builds it) and Cancelled.
// Workers take tasks with Next and report them back; a draw asks with Request. A draw is never
// denied anything: the budget, the ready cap and the RAM pause only hold back preparation.
//
// Everything is guarded by one mutex (`mutex_`); the scheduler calls nothing while holding it
// except destructors of the opaque payload and result objects (decoded records, never Vulkan
// objects: Cancel hands the ready results back to be destroyed outside the lock).
namespace VideoCommon::PipelinePrepare {

using EntryId = u32;
inline constexpr EntryId NoEntry = ~EntryId{0};

enum class State : u8 {
    Indexed,
    Loading,
    Queued,
    Building,
    Ready,
    Adopted,
    Claimed,
    Failed,
    Skipped,
    Cancelled,
    Evicted, ///< Ready but unused while the ready cap held back others: released, built when drawn
};

/// What the scheduler knows of one record before reading it: the index entry.
struct Candidate {
    CacheEnvelope::RecordMeta meta;
    u32 length{};
};

/// Memory a record holds from Loading to the end of its build: the serialized size decoded into
/// environments (counted twice, the code is copied) plus a fixed bookkeeping share.
[[nodiscard]] constexpr u64 RecordCost(u32 length) noexcept {
    return u64{length} * 2 + 2048;
}
/// What a Ready entry still holds until it is adopted or dropped.
inline constexpr u64 ReadyBookkeepingBytes = 1024;

class Scheduler {
public:
    using WorkIdSource = u64 (*)();

    enum class TaskKind : u8 {
        Load,  ///< Read and decode record `id`, then call Loaded, LoadSkipped or LoadFailed
        Build, ///< Build `payload`, then call BuildFinished
        Wait,  ///< Nothing may be taken now: WaitForChange(epoch, ...) and ask again
        Done,  ///< Nothing is left to prepare, or the generation was cancelled
    };

    struct Task {
        TaskKind kind{TaskKind::Done};
        EntryId id{NoEntry};
        u64 generation{};
        u64 work{};  ///< Attribution id of this entry's work
        u64 epoch{}; ///< For WaitForChange
        std::shared_ptr<void> payload; ///< Build: what Loaded stored
        /// Ready object released to make room under the ready cap (oldest unused first): the
        /// caller destroys it, outside the scheduler's lock.
        std::shared_ptr<void> evicted;
    };

    enum class LoadOutcome : u8 {
        Queued,
        Duplicate, ///< The key is already known (queued, built or demanded): entry skipped
        Stale,     ///< Another generation or the entry is not loading: drop the record
    };

    enum class DemandKind : u8 {
        Miss,        ///< Not known: build it as usual (the key is remembered as demanded)
        Claimed,     ///< It was queued: the caller owns it, preparation will not build it
        Building,    ///< In progress: Await, then Request again
        Ready,       ///< `results` hold the adopted objects
        Unavailable, ///< Failed, skipped, cancelled or taken: build it as usual
    };

    struct Demand {
        DemandKind kind{DemandKind::Miss};
        EntryId id{NoEntry};
        u64 generation{};
        u64 work{};
        std::vector<std::shared_ptr<void>> results;
    };

    Scheduler(const CacheEnvelope::PrepareOptions& options, WorkIdSource work_ids);

    /// Starts a generation over `candidates` (EntryId = position). Any earlier generation must
    /// have been cancelled. Returns the generation.
    u64 Begin(std::span<const Candidate> candidates);

    /// Ends the generation: queued records are dropped, in-flight tasks become stale and their
    /// completions are refused. Returns the ready results for the caller to destroy.
    [[nodiscard]] std::vector<std::shared_ptr<void>> Cancel();

    // ---- workers ----
    /// `hold_builds` keeps queued records queued (a draw can still claim them) while the caller
    /// waits out a burst of runtime compilation; loads and evictions go on.
    [[nodiscard]] Task Next(u64 generation, u64 available_ram_bytes, bool hold_builds = false);
    void WaitForChange(u64 epoch, std::chrono::milliseconds timeout);
    LoadOutcome Loaded(EntryId id, u64 generation, u64 hash, std::span<const u8> key,
                       std::shared_ptr<void> payload, bool variants_allowed);
    bool LoadSkipped(EntryId id, u64 generation);
    bool LoadFailed(EntryId id, u64 generation);
    /// False when the generation ended meanwhile: the caller destroys `result` itself.
    bool BuildFinished(EntryId id, u64 generation, bool ok, std::shared_ptr<void> result);

    // ---- demand ----
    [[nodiscard]] Demand Request(u64 hash, std::span<const u8> key);
    /// Blocks while entry `id` builds. Returns when it is no longer building or the generation
    /// ended; the caller then asks again.
    void Await(EntryId id, u64 generation);

    // ---- observation ----
    [[nodiscard]] State StateOf(EntryId id) const;
    /// Entries loading, queued or building: work in progress that is not paused.
    [[nodiscard]] size_t InFlight() const;
    [[nodiscard]] size_t ReadyCount() const;
    [[nodiscard]] u64 UsedBytes() const;
    [[nodiscard]] bool Running() const;
    [[nodiscard]] u64 Generation() const;
    /// Fills prepared, adopted, demand_promoted, prepare_failures, queue_high_water,
    /// memory_high_water_bytes, budget_pauses and evicted.
    void FillStats(CacheEnvelope::CacheStats& stats) const;

private:
    struct Entry {
        Candidate candidate;
        State state{State::Indexed};
        u32 rank{};
        u64 hash{};
        u64 reserved{};
        u64 work{};
        std::vector<u8> key;
        std::shared_ptr<void> payload;
        std::shared_ptr<void> result;
        u64 ready_seq{}; ///< Order in which entries became Ready (eviction: smallest first)
    };
    struct Counters {
        u64 prepared{};
        u64 adopted{};
        u64 demand_promoted{};
        u64 prepare_failures{};
        u64 queue_high_water{};
        u64 memory_high_water{};
        u64 budget_pauses{};
        u64 evicted{};
    };

    [[nodiscard]] bool IsCurrent(EntryId id, u64 generation, State expected) const;
    void Reserve(Entry& entry, u64 bytes);
    void Release(Entry& entry);
    void Finish(Entry& entry, State state);
    void Changed();
    void SetPause(bool blocked);
    void NoteInFlight();
    [[nodiscard]] bool RamPaused(u64 available_ram_bytes);
    [[nodiscard]] Entry* Find(EntryId id, u64 generation, State expected);

    const CacheEnvelope::PrepareOptions options;
    const WorkIdSource work_ids;
    const u32 window;

    mutable std::mutex mutex_;
    std::condition_variable changed_;
    u64 generation_{};
    bool running_{};
    u64 epoch_{};
    std::vector<Entry> entries_;
    std::vector<EntryId> order_; ///< entry ids by priority; rank = position
    size_t cursor_{};            ///< next rank to load
    std::set<u32> queued_;       ///< ranks of Queued entries
    std::unordered_multimap<u64, EntryId> identity_;
    std::unordered_map<u64, std::vector<std::vector<u8>>> demanded_;
    u64 index_bytes_{};
    u64 used_{};
    u32 loading_{};
    u32 building_{};
    u32 ready_{};
    u64 ready_seq_{};
    std::map<u64, EntryId> ready_order_; ///< Ready entries by ready_seq
    bool ram_paused_{};
    bool paused_{};
    Counters counters_;
};

// ---- driver blob ----
// The driver's pipeline cache blob is one logical payload larger than a record may be, so it is
// stored as a run of records, each led by {u32 chunk index, u32 chunk count}: the index keeps
// equal chunks from being merged by the writer's duplicate check.

inline constexpr size_t DriverBlobChunkBytes = size_t{8} << 20;
inline constexpr size_t DriverBlobChunkHeaderBytes = 8;

/// Calls `append` with each chunk of `blob` in order; false as soon as one call returns false.
bool ForEachDriverBlobChunk(std::span<const u8> blob,
                            const std::function<bool(std::span<const u8>)>& append);

/// Checks the Vulkan pipeline cache header at the start of `blob` against the device:
/// truncated (shorter than the header), bad_record (header size or version), device (vendor or
/// device id), pipeline_uuid. None when it is the device's own.
[[nodiscard]] CacheEnvelope::RejectReason ValidateDriverBlobHeader(
    std::span<const u8> blob, const CacheEnvelope::DeviceIdentity& device);

/// Reassembles `record_count` chunk records read through `read_record` into `blob`, verifying
/// the chunk numbering and the header. bad_record for missing, repeated or misnumbered chunks and
/// for a blob over MaxDriverBlobBytes; otherwise what the read or the header check returns.
[[nodiscard]] CacheEnvelope::RejectReason JoinDriverBlob(
    size_t record_count,
    const std::function<CacheEnvelope::RejectReason(size_t, std::vector<u8>&)>& read_record,
    const CacheEnvelope::DeviceIdentity& device, std::vector<u8>& blob);

/// Physical memory available to the process now, in bytes; the maximum when the host cannot say
/// (preparation then never pauses for memory).
[[nodiscard]] u64 AvailablePhysicalMemoryBytes();

// ---- checkpoints ----
// New records are committed as a generation of the file before the run ends, so a kill or a crash
// loses at most the last interval. Each commit rewrites the whole file (the writer copies the
// previous generation first), so the interval grows with the file: one second per MiB, never
// under 20 s, which keeps the copying below about 1 MiB/s on average. The end of the first two
// minutes of play, when most of the pipelines of a route exist, commits at once.

inline constexpr u64 CheckpointMinIntervalNs = u64{20} * 1'000'000'000;
inline constexpr u64 CheckpointNsPerByte = 1'000'000'000 / (u64{1} << 20); ///< 1 s per MiB
inline constexpr u64 FirstWindowNs = u64{120} * 1'000'000'000;

/// Whether a checkpoint is due: something is uncommitted and either the interval for a file of
/// `committed_bytes` has passed since the last commit or `first_window_ended` (once per run).
[[nodiscard]] constexpr bool CheckpointDue(u64 uncommitted_records, u64 since_last_commit_ns,
                                           u64 committed_bytes, bool first_window_ended) noexcept {
    if (uncommitted_records == 0) {
        return false;
    }
    const u64 scaled = committed_bytes > ~u64{0} / CheckpointNsPerByte
                           ? ~u64{0}
                           : committed_bytes * CheckpointNsPerByte;
    return first_window_ended ||
           since_last_commit_ns >= (std::max)(CheckpointMinIntervalNs, scaled);
}

/// The size of the committed cache file at `path`, the value `CheckpointDue` scales its interval
/// by. A file accepted at load counts from the start, not only after this run commits. 0 when the
/// file cannot be read.
[[nodiscard]] u64 CommittedFileBytes(const std::filesystem::path& path);

inline constexpr u32 NoRecord = ~u32{0};

/// For each entry of `before`, the position of the same record (kind, length, checksum) in
/// `after`, or NoRecord when the new generation does not hold it. A committed generation keeps
/// the order of the one it was seeded from unless a record was dropped, so this is the identity
/// map in the usual case; it is not assumed.
[[nodiscard]] std::vector<u32> RemapRecords(std::span<const CacheEnvelope::IndexEntry> before,
                                            std::span<const CacheEnvelope::IndexEntry> after);

} // namespace VideoCommon::PipelinePrepare
