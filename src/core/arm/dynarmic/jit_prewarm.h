// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Second-launch code translation ahead of the guest.
//
// Early demand translations record their image-relative location, instruction count and hash.
// The list is written next to the title's pipeline caches. On the next launch of the same program
// in the same build, each idle core snapshots each recorded span and translates it only if the
// snapshot matches. A list that is absent, corrupt, obsolete or for another build or program is
// ignored and replaced. EDEN_NX2_JIT_PREWARM=0 turns it off.
//
// Threading: a core's JIT is only ever used by that core's host thread, so the blocks of a core
// are translated by it and by nothing else. The one helper thread reads and writes the file; it
// never touches a JIT.

#pragma once

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <vector>
#include <utility>

#include "common/common_types.h"

namespace Core::JitPrewarm {

/// The PC field is a byte offset from the initial process image; translation flags are unchanged.
struct Entry {
    u64 location{};
    u64 guest_hash{};
    u32 word_count{};

    friend bool operator==(const Entry&, const Entry&) = default;
};

/// Initial process image, not the page table's (larger) code allocation region.
struct CodeImage {
    u64 base;
    u64 size;

    [[nodiscard]] std::optional<u64> ToRelative(u64 location, u32 word_count) const noexcept;
    [[nodiscard]] std::optional<u64> ToAbsolute(u64 location, u32 word_count) const noexcept;
};

/// What a list is valid for. The emulator build and the program (content stamp, title, version) are
/// the identity; core_count is the number of per-core lists the file holds.
struct Identity {
    u64 build_id{};
    u64 content_stamp{};
    u64 title_id{};
    u32 game_version{};
    u32 core_count{};

    friend bool operator==(const Identity&, const Identity&) = default;
};

constexpr u32 FormatVersion = 3; // v1 and v2 stored absolute PCs and cannot be rebased.
constexpr u32 MaxCores = 16;
/// Per core. At 20 bytes per entry on disk, the largest list is 5 MiB a core.
constexpr std::size_t MaxEntriesPerCore = std::size_t{1} << 18;
constexpr std::size_t HeaderBytes = 80;

enum class Reject : u8 {
    None,
    TooShort,
    BadMagic,
    BadVersion,
    HeaderChecksum,
    Build,
    Content,
    Title,
    GameVersion,
    CoreCount,
    BodySize,
    BodyChecksum,
    TooManyEntries,
    InvalidEntry,
};

/// Stable lower-case token for logs and tests.
[[nodiscard]] const char* RejectName(Reject reject) noexcept;

/// The file for `per_core.size()` cores (identity.core_count is overwritten with it). Each list is
/// written as given: callers keep it within MaxEntriesPerCore (MergeForWrite does).
[[nodiscard]] std::vector<u8> Serialize(Identity identity,
                                        std::span<const std::vector<Entry>> per_core);

struct Parsed {
    Reject reject{Reject::None};
    std::vector<std::vector<Entry>> per_core; ///< Empty unless reject is None
};

/// Validates `bytes` against `expected` and returns the per-core lists. Nothing is trusted before
/// it passed: magic, version, header checksum, identity, sizes, body checksum, entry counts.
[[nodiscard]] Parsed Parse(std::span<const u8> bytes, const Identity& expected);

/// The list to write at the end of a run or at a checkpoint: the entries loaded at the start except
/// those found stale (ascending indices into `loaded`), then the entries recorded this run that are
/// not already there, at most `cap` in all.
[[nodiscard]] std::vector<Entry> MergeForWrite(std::span<const Entry> loaded,
                                               std::span<const u32> stale,
                                               std::span<const Entry> recorded, std::size_t cap);

/// Whole-file read (nullopt when it cannot be read or is larger than max_bytes) and replacement:
/// the bytes go to "<path>.tmp" and are renamed over `path`, so a reader sees the old file or the
/// new one. A "<path>.tmp" left by a crash is overwritten.
[[nodiscard]] std::optional<std::vector<u8>> ReadFileBytes(const std::filesystem::path& path,
                                                           std::size_t max_bytes);
[[nodiscard]] bool WriteFileAtomic(const std::filesystem::path& path, std::span<const u8> bytes);

/// How a block's translation ended (the JIT's answer, as the scheduling rule sees it).
enum class Outcome : u8 {
    Compiled,    ///< Translated now
    Present,     ///< Already in the code cache
    Mismatch,    ///< The guest code is not what the list says (yet?)
    NoSpace,     ///< The code cache is too full to translate ahead: stop for good
    Busy,        ///< The JIT cannot take it now: stop this slice, nothing is judged
    Unsupported, ///< This JIT cannot translate ahead: stop for good
};

/// Disjoint outcomes for loaded hints, except mismatch_attempts (includes retries).
struct OutcomeCounts {
    u32 loaded{};
    u32 compiled{};
    u32 already_present{};
    u32 rejected_mismatch{};
    u32 no_space{};
    u32 unsupported{};
    u32 pending{};
    u64 mismatch_attempts{};
};

struct Policy {
    u32 max_sweeps = 3;        ///< Passes over the blocks that mismatched, the first pass included
    u64 retry_gap_ms = 2000;   ///< Between the end of a pass and the next one over its mismatches
};

/// The order and retry rule of one core's list, as a pure state machine (no JIT, no clock):
/// entries in list order; a mismatch is retried in a later pass, not before `retry_gap_ms` after
/// the pass that saw it ended, because the code may not be loaded yet; after `max_sweeps` passes
/// the ones still mismatching are stale. A mismatch never stops the others.
class Cursor {
public:
    enum class Kind : u8 { Translate, Wait, Finished };
    struct Step {
        Kind kind{Kind::Finished};
        u32 index{};              ///< Translate: the entry to translate
        u64 wait_until_ms{};      ///< Wait: the time `now_ms` must reach
    };

    explicit Cursor(u32 count, Policy policy = {});

    /// What to do at `now_ms` (any monotonic millisecond clock). Translate repeats until Report.
    [[nodiscard]] Step Next(u64 now_ms);
    /// The outcome of the Translate step Next last returned.
    void Report(u32 index, Outcome outcome);

    [[nodiscard]] bool Finished() const noexcept {
        return finished_;
    }
    /// Entries that still mismatched after the last pass, ascending; set when Finished by running
    /// out of passes (empty if it was cut short).
    [[nodiscard]] std::span<const u32> Stale() const noexcept {
        return stale_;
    }
    [[nodiscard]] u32 Compiled() const noexcept {
        return compiled_;
    }
    [[nodiscard]] u32 Present() const noexcept {
        return present_;
    }
    [[nodiscard]] OutcomeCounts WarmOutcomes() const noexcept;

private:
    Policy policy_;
    u32 count_;
    u32 pass_{0};                ///< 0-based; pass 0 is the whole list
    u32 pos_{0};
    std::vector<u32> retry_;     ///< Indices of this pass's mismatches, ascending
    std::vector<u32> current_;   ///< Passes after the first iterate this
    std::vector<u32> stale_;
    bool gap_open_{false};
    u64 gap_until_ms_{0};
    bool finished_{false};
    u32 compiled_{0};
    u32 present_{0};
    u64 mismatch_attempts_{0};
    std::optional<Outcome> stopped_by_;
};

enum class RunResult : u8 {
    Finished, ///< Nothing more will ever be translated for this core
    Poll,     ///< Come back later (not published yet, interrupted, waiting to retry, JIT busy)
};

/// One core's share: the list loaded from the file, the scheduling cursor, and the blocks this
/// core's JIT reports it translated. The owner core thread calls Run and Record; the helper
/// thread publishes the list and reads what Record stored.
class CoreState {
public:
    explicit CoreState(Policy policy = {}, std::size_t record_capacity = MaxEntriesPerCore);

    // --- Helper thread ---
    /// Hands over the list loaded from the file (nullopt: there is none to translate). Once.
    void Publish(std::optional<std::vector<Entry>> loaded);
    /// No more entries are recorded after this.
    void CloseRecording() noexcept {
        closed_.store(true, std::memory_order_relaxed);
    }
    [[nodiscard]] std::span<const Entry> Loaded() const noexcept {
        return loaded_;
    }
    /// True and the stale indices when the owner finished its passes.
    [[nodiscard]] bool GetStale(std::vector<u32>& out) const;
    /// The entries recorded so far (a prefix that never changes once visible).
    [[nodiscard]] std::size_t RecordedCount() const noexcept {
        return recorded_count_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::span<const Entry> Recorded() const noexcept {
        return {recorded_.get(), RecordedCount()};
    }

    /// Read only after the CPU owner has stopped; nullopt until the helper publishes its list.
    [[nodiscard]] std::optional<OutcomeCounts> WarmOutcomes() const noexcept;
    void LogWarmOutcomes(std::size_t core_index) const;

    // --- Owner core thread ---
    [[nodiscard]] bool CanRecord() const noexcept {
        return !closed_.load(std::memory_order_relaxed) &&
               recorded_count_.load(std::memory_order_relaxed) < record_capacity_;
    }
    /// The JIT translated a block because the guest reached it.
    void Record(u64 location, u64 guest_hash, u32 word_count) noexcept;

    /// Translates entries until `stop` is set, the list is done or something must wait.
    /// `now()` is a monotonic millisecond clock; `compile(const Entry&)` returns an Outcome.
    template <class Now, class Compile>
    RunResult Run(const std::atomic<bool>& stop, Now&& now, Compile&& compile) {
        const u32 state = state_.load(std::memory_order_acquire);
        if (state == Pending) {
            return RunResult::Poll;
        }
        if (state == NoList || done_) {
            return RunResult::Finished;
        }
        if (!cursor_) {
            cursor_.emplace(static_cast<u32>(loaded_.size()), policy_);
        }
        while (!stop.load(std::memory_order_acquire)) {
            const Cursor::Step step = cursor_->Next(now());
            switch (step.kind) {
            case Cursor::Kind::Translate: {
                const Outcome outcome = compile(loaded_[step.index]);
                if (outcome == Outcome::Busy) {
                    return RunResult::Poll;
                }
                cursor_->Report(step.index, outcome);
                break;
            }
            case Cursor::Kind::Wait:
                return RunResult::Poll;
            case Cursor::Kind::Finished:
                Finish();
                return RunResult::Finished;
            }
        }
        return RunResult::Poll;
    }

private:
    static constexpr u32 Pending = 0;
    static constexpr u32 HasList = 1;
    static constexpr u32 NoList = 2;

    void Finish();

    Policy policy_;
    std::atomic<u32> state_{Pending};
    std::vector<Entry> loaded_;            ///< Written before state_ leaves Pending
    std::optional<Cursor> cursor_;         ///< Owner thread only
    bool done_{false};                     ///< Owner thread only
    std::vector<u32> stale_;               ///< Written before finished_ is set
    std::atomic<bool> finished_{false};

    std::unique_ptr<Entry[]> recorded_;
    std::size_t record_capacity_;
    std::atomic<std::size_t> recorded_count_{0};
    std::atomic<bool> closed_{false};
};

class Session;

/// What an ArmDynarmic64 keeps: its core's state, and with it the session (file I/O, the helper
/// thread) for as long as any core of the process uses it.
class Handle {
public:
    Handle(std::shared_ptr<Session> session, CoreState& core) noexcept
        : session_{std::move(session)}, core_{&core} {}

    [[nodiscard]] CoreState& State() const noexcept {
        return *core_;
    }

private:
    std::shared_ptr<Session> session_;
    CoreState* core_;
};

/// The handle for core `core_index` of the process `process_key` running `program_id`, or nullopt
/// when EDEN_NX2_JIT_PREWARM=0. The first call of a process starts its session.
[[nodiscard]] std::optional<Handle> Attach(u64 program_id, const void* process_key,
                                           std::size_t core_index);

} // namespace Core::JitPrewarm
