// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
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
// Workers take tasks with Next and report them back; a draw asks with Request.
//
// Records are prepared in the order the first run reached them (Begin). Ready objects are kept
// for their draws: once ready plus building reach the ready cap, builds are held back until a
// draw adopts one, so the objects needed first are never the ones released. Only when draws have
// gone on for PrepareOptions::stalled_after_ns without adopting any (they match no record, or
// take a record still queued) while the cap holds are the oldest unused ready objects released.
// A draw is never denied anything: the budget, the ready cap and the RAM pause only hold back
// preparation.
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
    Evicted, ///< Ready but unused while the game diverged from the recorded order: released
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
    /// Nanoseconds of any steady clock; null selects std::chrono::steady_clock.
    using ClockSource = u64 (*)();

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
        /// Ready object released because the game diverged from the recorded order: the caller
        /// destroys it, outside the scheduler's lock.
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

    Scheduler(const CacheEnvelope::PrepareOptions& options, WorkIdSource work_ids,
              ClockSource clock = nullptr);

    /// Starts a generation over `candidates` (EntryId = position). Any earlier generation must
    /// have been cancelled. Returns the generation.
    u64 Begin(std::span<const Candidate> candidates);

    /// Ends the generation: queued records are dropped, in-flight tasks become stale and their
    /// completions are refused. Returns the ready results for the caller to destroy.
    [[nodiscard]] std::vector<std::shared_ptr<void>> Cancel();

    // ---- workers ----
    /// `hold_builds` keeps queued records queued (a draw can still claim them) while the caller
    /// waits out a burst of runtime compilation; loads go on, nothing is released.
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
    /// Fills records, prepared, adopted, demand_promoted, demand_building, demand_evicted,
    /// demand_unknown, prepare_failures, queue_high_water, memory_high_water_bytes,
    /// budget_pauses, evicted, held_back, adopted_lead_ns, late_records, late_wait_ns and
    /// late_wait_max_ns.
    void FillStats(CacheEnvelope::CacheStats& stats) const;

    /// How a record that was not ready at its first draw stood then.
    enum class LateOutcome : u8 {
        Building, ///< Being built: the draw waited for it (ready_ns is when it was done)
        Claimed,  ///< Still queued: the draw built it itself
        Released, ///< Ready earlier but released unused (ready_ns is when it had been ready)
    };
    struct LateRecord {
        u32 rank{};
        CacheEnvelope::FirstUse recorded_phase{};
        u64 recorded_first_use_ns{}; ///< As the first run recorded it (since its first present)
        LateOutcome outcome{};
        u64 demanded_ns{}; ///< Clock value of the first draw that asked for it
        u64 ready_ns{};    ///< Clock value it was ready; 0 when it never was
    };
    static constexpr size_t MaxLateRecords = 64;
    /// The first MaxLateRecords records first used before they were ready, in the order asked.
    [[nodiscard]] std::vector<LateRecord> LateRecords() const;

private:
    static constexpr size_t NoLate = ~size_t{0};
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
        u64 ready_seq{}; ///< Order in which entries became Ready (release: smallest first)
        u64 ready_ns{};  ///< Clock value it became Ready
        u64 demanded_ns{}; ///< Clock value of the first draw that found it not ready, 0 if none
        size_t late_slot{NoLate};
    };
    struct Counters {
        u64 records{};
        u64 prepared{};
        u64 adopted{};
        u64 demand_promoted{};
        u64 demand_building{};
        u64 demand_evicted{};
        u64 demand_unknown{};
        u64 prepare_failures{};
        u64 queue_high_water{};
        u64 memory_high_water{};
        u64 budget_pauses{};
        u64 evicted{};
        u64 held_back{};
        u64 adopted_lead_ns{};
        u64 late_records{};
        u64 late_wait_ns{};
        u64 late_wait_max_ns{};
    };

    [[nodiscard]] bool IsCurrent(EntryId id, u64 generation, State expected) const;
    void Reserve(Entry& entry, u64 bytes);
    void Release(Entry& entry);
    void Finish(Entry& entry, State state);
    void Changed();
    void SetPause(bool blocked);
    void NoteInFlight();
    [[nodiscard]] bool RamPaused(u64 available_ram_bytes);
    [[nodiscard]] u64 Now() const;
    void NoteLate(Entry& entry, LateOutcome outcome, u64 now);
    [[nodiscard]] Entry* Find(EntryId id, u64 generation, State expected);

    const CacheEnvelope::PrepareOptions options;
    const WorkIdSource work_ids;
    const ClockSource clock;
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
    bool cap_held_{};            ///< Builds are being held at the ready cap (one count per run)
    /// Clock value of the first draw that adopted nothing while ready objects held the cap, since
    /// the last adoption; 0 when none.
    u64 stall_since_ns_{};
    std::vector<LateRecord> late_;
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

/// The size of each committed cache file of a session and their total: the file sizes as they
/// are on disk now, not the bytes this run has written. `CheckpointDue` scales its interval by
/// the total. Files are counted in the order the session keeps them. The session changes it
/// through OpenCommittedFile and CommitGeneration only.
class CommittedSizes {
public:
    /// The total, published at every change; readable from any thread.
    [[nodiscard]] u64 Total() const noexcept {
        return total.load(std::memory_order_relaxed);
    }
    /// The count of file `index`; 0 for an unknown file. Not safe against a concurrent change.
    [[nodiscard]] u64 FileBytes(size_t index) const noexcept {
        return index < bytes.size() ? bytes[index] : u64{0};
    }

private:
    friend std::unique_ptr<CacheEnvelope::EnvelopeReader> OpenCommittedFile(
        CommittedSizes& sizes, const std::filesystem::path& path,
        const CacheEnvelope::CacheIdentity& expected, CacheEnvelope::CacheLoadResult& load);
    friend bool CommitGeneration(CommittedSizes& sizes, size_t index,
                                 CacheEnvelope::EnvelopeWriter& writer);
    void Publish() noexcept;

    std::vector<u64> bytes;
    std::atomic<u64> total{};
};

/// Opens the cache file at `path` for a session and counts it as file number `sizes` has so far
/// (the session's files are opened in one fixed order). A temporary file of a generation that
/// never committed is removed first. An accepted file counts the size it has on disk from the
/// start; one that is rejected or missing counts nothing (the next generation is not seeded from
/// it) and the result is null, `load` telling why.
[[nodiscard]] std::unique_ptr<CacheEnvelope::EnvelopeReader> OpenCommittedFile(
    CommittedSizes& sizes, const std::filesystem::path& path,
    const CacheEnvelope::CacheIdentity& expected, CacheEnvelope::CacheLoadResult& load);

/// Commits `writer` as the new generation of file number `index`. On success the count of the
/// file becomes the size of the generation (it holds the records of the previous one, so the
/// sizes are not summed); on failure the previous file stays in place and keeps its count.
[[nodiscard]] bool CommitGeneration(CommittedSizes& sizes, size_t index,
                                    CacheEnvelope::EnvelopeWriter& writer);

inline constexpr u32 NoRecord = ~u32{0};

/// For each entry of `before`, the position of the same record (kind, length, checksum) in
/// `after`, or NoRecord when the new generation does not hold it. A committed generation keeps
/// the order of the one it was seeded from unless a record was dropped, so this is the identity
/// map in the usual case; it is not assumed.
[[nodiscard]] std::vector<u32> RemapRecords(std::span<const CacheEnvelope::IndexEntry> before,
                                            std::span<const CacheEnvelope::IndexEntry> after);

} // namespace VideoCommon::PipelinePrepare
