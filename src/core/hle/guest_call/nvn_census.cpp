// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "common/logging.h"
#include "common/run_outcome.h"
#include "common/scm_rev.h"
#include "common/settings.h"
#include "core/arm/arm_interface.h"
#include "core/arm/debug.h"
#include "core/arm/symbols.h"
#include "core/hle/guest_call/nvn_census.h"
#include "core/hle/guest_call/nvn_epoch_slot.h"
#include "core/hle/guest_call/nvn_census_verdict.h"
#include "core/hle/kernel/k_process.h"
#include "core/memory.h"
#include "dynarmic/interface/A64/config.h"
#include "video_core/replay/capture.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace Core::NvnCensus {

namespace detail {
std::atomic<bool> g_active{false};
} // namespace detail

namespace {

using Dynarmic::A64::BranchKind;
using Dynarmic::A64::InstructionProbe;
using Dynarmic::A64::InstructionProbeSet;
using Dynarmic::A64::VAddr;
using Arg = InstructionProbe::Arg;
using namespace std::chrono_literals;

constexpr std::size_t MaxTargets = 2048;
constexpr std::size_t MaxShards = 16;
constexpr std::size_t Epochs = 4; ///< Frame counters kept per shard; see NotePresentSlow.
constexpr std::size_t ProbeSlots = 16384; // power of two, kept under half full
constexpr std::size_t MaxPending = 256;
constexpr std::size_t MaxGuestThreads = 64; // power of two, per shard
constexpr std::size_t MaxUnknown = 256;     // power of two, per shard
constexpr std::size_t MaxReturnSites = 1024; // power of two, per shard
constexpr std::size_t MaxStreams = 16;
constexpr std::size_t MaxFrameRows = 8192;
constexpr std::size_t MaxFrameTotals = std::size_t{1} << 18;
constexpr std::size_t MaxReportedUnknown = 64;
constexpr std::size_t MaxReportedMismatches = 64;
constexpr u64 MaxHashBytes = u64{64} << 20;
constexpr auto WindowLength = 10s;
constexpr auto WriteInterval = 30s;
constexpr auto TickInterval = 1s;
constexpr auto ModuleCheckInterval = 5s;
constexpr auto RendezvousTimeout = 200ms;
constexpr std::size_t MaxNameLength = 160;

constexpr u32 FromExport = 1;   ///< A function the library exports.
constexpr u32 FromResolver = 2; ///< An address the game got back from a lookup.
constexpr u32 FromAudit = 4;    ///< An entry point only a branch from outside revealed; it has no name.

static_assert(MaxGuestThreads == 64 && MaxUnknown == 256, "the hashes below assume these sizes");

enum class Role : u32 {
    BootstrapEntry = 1, ///< nvnBootstrapLoader(name): x0 = name
    ProcEntry,          ///< nvnDeviceGetProcAddress(device, name): x1 = name
    Return,             ///< the instruction after a call to one of the two above: x0 = result
    Count,              ///< first instruction of an NVN function: one counter per target
    AuditEntry,         ///< an entry point the audit found: the entry probe counts calls; the branch independently audits them
};

struct Slot {
    std::atomic<u64> pc{0}; ///< Published last; fields below are final once it is non-zero.
    Role role{};
    u32 index{};
};

struct GuestThreadCount {
    std::atomic<u64> key{0}; ///< Guest thread id + 1; 0 = free.
    std::atomic<u64> calls{0};
};

struct UnknownEntry {
    std::atomic<u64> target{0}; ///< 0 = free.
    std::atomic<u64> calls{0};
};

/// Everything counted by one host thread, so cores never share a cache line of counters.
struct Shard {
    /// Calls per frame in flight: `frame[epoch % Epochs][target]`.
    std::array<std::array<std::atomic<u64>, MaxTargets>, Epochs> frame{};
    /// Calls per target over the run, counted independently of the per-frame counters.
    std::array<std::atomic<u64>, MaxTargets> total{};
    /// Calls whose return address lies outside the library (the audit's side of the check).
    std::array<std::atomic<u64>, MaxTargets> from_outside{};
    /// Branches from outside the library that landed on this target.
    std::array<std::atomic<u64>, MaxTargets> audited{};
    std::array<GuestThreadCount, MaxGuestThreads> guest{};
    std::array<UnknownEntry, MaxUnknown> unknown{};
    /// Branches from outside that were RETs landing just after a call in the library (the library
    /// called out and the callee returned), by target; not entries, so not counted as calls.
    std::array<UnknownEntry, MaxReturnSites> returns{};
    std::atomic<u64> guest_overflow{0};
    std::atomic<u64> unknown_overflow{0};
    std::atomic<u64> audit_branches{0}; ///< All branches into the library from outside.
    std::atomic<u64> audit_other{0};    ///< ... that landed on an export that is not NVN.
    /// Writers between reserving and committing an epoch slot. The present waits for zero.
    std::array<std::atomic<u64>, Epochs> inflight{};
    /// Calls whose slot was recycled before they committed, so they count as late.
    std::array<std::atomic<u64>, MaxTargets> late{};
};

struct Pending {
    u64 guest_thread;
    u64 sp;
    u64 lr;
    bool bootstrap;
    std::string name;
};

struct Target {
    u64 address{};
    std::vector<std::string> names;
    u32 provenance{};
    bool acked{}; ///< Its probe is installed and no core can still run code translated without it.
    u64 startup{};
    u64 frame_sum{};
    u64 frame_max{};
    u64 frames_called{};
    s64 first_frame{-1};
    u64 late{};
};

struct Request {
    u64 count{};
    u64 address{};
};

struct Window {
    u64 calls{};
    u64 presents{};
};

struct ModuleInfo {
    std::string name;
    u64 base{};
    u64 extent{};
    u64 hash{};
    u64 hashed{};
};

struct FrameRow {
    u64 frame{};
    std::vector<std::pair<u32, u64>> calls; ///< (target, calls), sparse
};

struct StreamInfo {
    u64 key{};
    PresentPath path{};
    /// First client-buffer token seen on this stream. 0 on the legacy queue, which has no token.
    u64 token{};
    /// Distinct tokens observed, saturating at 2. Two means a second producer shared this stream.
    u32 tokens{};
    /// A second token appeared, or the stream key / nx2 token was 0 (identity discarded).
    bool ambiguous{};
    u64 presents{};
    u64 last_id{};
};

/// Counters bumped under the lock; copied whole into a snapshot.
struct Counters {
    u64 bootstrap_calls{};
    u64 proc_calls{};
    u64 null_results{};
    u64 role_conflicts{};
    u64 probe_table_full{};
    u64 target_table_full{};
    u64 pending_overflow{};
    u64 rendezvous_timeouts{};
    u64 late_calls{};
    u64 late_modules{};
    u64 id_gaps{};
    u64 id_duplicates{};
    u64 stream_overflow{};
    u64 epoch_reservation_timeouts{};
    u64 code_changes{};
    u64 unaudited_entries{};
    u64 ambiguous_pending{}; ///< A resolver entry whose (thread, sp, lr) key a live entry already has.
};

struct State final : Dynarmic::A64::InstructionProbes {
    State() : slots(std::make_unique<Slot[]>(ProbeSlots)) {}

    std::size_t Find(VAddr pc, InstructionProbeSet& out) const override;
    bool FindBranch(VAddr pc, BranchKind kind, u64 target_or_reg, InstructionProbe& out) const override;

    std::mutex m;
    std::condition_variable cv;
    std::filesystem::path out_path;
    bool audit_enabled{};
    u64 min_frames{};
    std::atomic<Core::Memory::Memory*> memory{};
    std::atomic<Kernel::KProcess*> process{};
    u64 program_id{};
    std::string cpu_backend;
    std::chrono::steady_clock::time_point start;
    std::thread reporter;
    bool stop{};
    bool started{};

    std::unique_ptr<Slot[]> slots;
    std::atomic<std::size_t> slot_count{0};

    std::array<std::atomic<Shard*>, MaxShards> shards{};
    std::atomic<std::size_t> shard_count{0};

    /// The frame being collected; bumped by each accepted present.
    std::atomic<u64> epoch{0};
    /// The epoch each frame slot is open for, or SlotClosed while it is being recycled.
    std::array<std::atomic<u64>, Epochs> slot_open{};
    /// The library's extent while auditing; zero otherwise. Written before any guest code runs.
    std::atomic<u64> audit_lo{0};
    std::atomic<u64> audit_hi{0};
    /// Conditional branches into the library: audited only if taken, so they fail coverage.
    mutable std::atomic<u64> unaudited_entries{0};
    /// Executable-map generation when the census armed. A later change means new code.
    u64 code_generation_baseline{};
    std::string library_module;
    std::vector<u64> other_exports; ///< Sorted; exports of the library that are not NVN.

    std::vector<ModuleInfo> modules;
    std::vector<u64> bootstrap_sites;
    std::vector<Target> targets;
    std::unordered_map<u64, u32> target_by_address;
    std::map<std::string, Request> requests;
    std::map<std::string, u64> nulls;
    std::map<std::string, u64> bootstrap_results;
    std::vector<Pending> pending;
    std::atomic<u32> pending_count{0};
    std::atomic<u64> unmatched_returns{0};
    std::atomic<u64> name_read_failures{0};
    Counters c;

    u64 presents{};          ///< every present the streams saw, accepted or not
    u64 accepted_presents{}; ///< presents that reached the accepted-present seam; each closes a frame
    /// Per epoch slot: the id of the accepted present that closed that epoch (its frame id).
    std::array<u64, Epochs> close_id{};
    std::array<u64, 2> presents_by_path{};
    std::vector<StreamInfo> streams;
    u64 next_close{}; ///< The lowest epoch not yet folded into the per-frame figures.
    u64 frames_closed{};
    u64 startup_total{};
    u64 frame_total_sum{};
    u64 frame_total_max{};
    std::vector<u64> frame_totals;
    std::vector<FrameRow> frame_rows;
    u64 frame_rows_dropped{};

    std::vector<Window> windows;
    u64 last_grand{};
    u64 last_presents{};
};

State& S() {
    static State state;
    return state;
}

std::mutex g_options_mutex;
std::optional<Options> g_options;

Options EffectiveOptions() {
    {
        std::scoped_lock lock{g_options_mutex};
        if (g_options) {
            return *g_options;
        }
    }
    Options options;
    if (const char* file = std::getenv("EDEN_NX2_NVN_CENSUS"); file != nullptr && file[0] != '\0') {
        options.out = file;
    }
    if (const char* audit = std::getenv("EDEN_NX2_NVN_CENSUS_AUDIT");
        audit != nullptr && audit[0] != '\0' && audit[0] != '0') {
        options.audit = true;
    }
    return options;
}

thread_local Shard* t_shard = nullptr;

Shard* AcquireShard(State& s) {
    std::scoped_lock lock{s.m};
    const std::size_t count = s.shard_count.load(std::memory_order_relaxed);
    if (count < MaxShards) {
        auto* shard = new Shard{};
        s.shards[count].store(shard, std::memory_order_release);
        s.shard_count.store(count + 1, std::memory_order_release);
        return shard;
    }
    return s.shards[0].load(std::memory_order_acquire);
}

Shard& ThisShard(State& s) {
    Shard* shard = t_shard;
    if (shard == nullptr) {
        shard = t_shard = AcquireShard(s);
    }
    return *shard;
}

u64 Total(const State& s, std::size_t index) {
    u64 total = 0;
    const std::size_t count = s.shard_count.load(std::memory_order_acquire);
    for (std::size_t i = 0; i < count; ++i) {
        total += s.shards[i].load(std::memory_order_acquire)->total[index].load(std::memory_order_relaxed);
    }
    return total;
}

std::size_t SlotHash(u64 pc) {
    return static_cast<std::size_t>(((pc >> 2) * 0x9E3779B97F4A7C15ULL) >> 40) & (ProbeSlots - 1);
}

enum class AddResult { Added, Exists, Rejected };

/// Caller holds s.m. One address may carry one probe of each role; the same role twice with a
/// different index is a conflict.
AddResult AddProbe(State& s, u64 pc, Role role, u32 index) {
    if (pc == 0) {
        return AddResult::Rejected;
    }
    std::size_t i = SlotHash(pc);
    for (std::size_t n = 0; n < ProbeSlots; ++n, i = (i + 1) & (ProbeSlots - 1)) {
        Slot& slot = s.slots[i];
        const u64 existing = slot.pc.load(std::memory_order_relaxed);
        if (existing == 0) {
            if (s.slot_count.load(std::memory_order_relaxed) >= ProbeSlots / 2) {
                ++s.c.probe_table_full;
                return AddResult::Rejected;
            }
            slot.role = role;
            slot.index = index;
            slot.pc.store(pc, std::memory_order_release);
            s.slot_count.fetch_add(1, std::memory_order_release);
            return AddResult::Added;
        }
        if (existing == pc && slot.role == role) {
            if (slot.index == index) {
                return AddResult::Exists;
            }
            ++s.c.role_conflicts;
            return AddResult::Rejected;
        }
    }
    ++s.c.probe_table_full;
    return AddResult::Rejected;
}

struct EntryHit {
    u32 index{};
    Role role{};
};

/// The target a Count or AuditEntry probe at `pc` counts for, if there is one. Lock-free.
std::optional<EntryHit> LookupEntry(const State& s, u64 pc) {
    std::size_t i = SlotHash(pc);
    for (std::size_t n = 0; n < ProbeSlots; ++n, i = (i + 1) & (ProbeSlots - 1)) {
        const Slot& slot = s.slots[i];
        const u64 existing = slot.pc.load(std::memory_order_acquire);
        if (existing == 0) {
            return std::nullopt;
        }
        if (existing == pc && (slot.role == Role::Count || slot.role == Role::AuditEntry)) {
            return EntryHit{slot.index, slot.role};
        }
    }
    return std::nullopt;
}

/// Invalidates [address, address + size) on every core and waits until no other core can still be
/// running code it translated before. False if that did not happen in time.
bool InvalidateAndWait(u64 address, u64 size) {
    auto* process = S().process.load(std::memory_order_acquire);
    if (process == nullptr) {
        return false;
    }
    return Core::InvalidateInstructionCacheRangeAndWait(process, address, size, RendezvousTimeout);
}

bool InvalidateAndWait(const std::vector<u64>& addresses) {
    bool acknowledged = true;
    for (const u64 address : addresses) {
        if (!InvalidateAndWait(address, sizeof(u32))) {
            acknowledged = false;
        }
    }
    return acknowledged;
}

std::string ReadName(u64 address) {
    auto* memory = S().memory.load(std::memory_order_acquire);
    if (memory == nullptr || address == 0) {
        return {};
    }
    return memory->ReadCString(address, MaxNameLength);
}

/// Caller holds s.m. Returns the index of the target at address, creating it on first sight.
std::optional<u32> TargetFor(State& s, u64 address, const std::string& name, u32 provenance) {
    auto [it, inserted] = s.target_by_address.try_emplace(address, 0);
    if (inserted) {
        if (s.targets.size() >= MaxTargets) {
            s.target_by_address.erase(it);
            ++s.c.target_table_full;
            return std::nullopt;
        }
        it->second = static_cast<u32>(s.targets.size());
        Target target;
        target.address = address;
        s.targets.push_back(std::move(target));
    }
    Target& target = s.targets[it->second];
    target.provenance |= provenance;
    if (!name.empty() && std::find(target.names.begin(), target.names.end(), name) == target.names.end()) {
        target.names.push_back(name);
    }
    return it->second;
}

void NoteGuestThread(Shard& shard, u64 guest_thread) {
    const u64 key = guest_thread + 1;
    std::size_t i = static_cast<std::size_t>((key * 0x9E3779B97F4A7C15ULL) >> 58);
    for (std::size_t n = 0; n < MaxGuestThreads; ++n, i = (i + 1) & (MaxGuestThreads - 1)) {
        GuestThreadCount& entry = shard.guest[i];
        u64 existing = entry.key.load(std::memory_order_relaxed);
        if (existing == 0 && entry.key.compare_exchange_strong(existing, key, std::memory_order_relaxed)) {
            existing = key;
        }
        if (existing == key) {
            entry.calls.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    shard.guest_overflow.fetch_add(1, std::memory_order_relaxed);
}

constexpr unsigned Log2(std::size_t n) {
    unsigned bits = 0;
    for (; n > 1; n >>= 1) {
        ++bits;
    }
    return bits;
}

template <std::size_t N>
std::size_t TableStart(u64 target) {
    static_assert((N & (N - 1)) == 0, "a table of branch targets has a power-of-two size");
    return static_cast<std::size_t>(((target >> 2) * 0x9E3779B97F4A7C15ULL) >> (64 - Log2(N)));
}

/// Counts one more branch to `target` if this shard's table already has it. Open addressing, 0 = free.
template <std::size_t N>
bool BumpTarget(std::array<UnknownEntry, N>& table, u64 target) {
    std::size_t i = TableStart<N>(target);
    for (std::size_t n = 0; n < N; ++n, i = (i + 1) & (N - 1)) {
        UnknownEntry& entry = table[i];
        const u64 existing = entry.target.load(std::memory_order_relaxed);
        if (existing == 0) {
            return false;
        }
        if (existing == target) {
            entry.calls.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
    }
    return false;
}

/// Counts a branch to `target`, adding it to the table on first sight. False when the table is full.
template <std::size_t N>
bool NoteTarget(std::array<UnknownEntry, N>& table, u64 target) {
    std::size_t i = TableStart<N>(target);
    for (std::size_t n = 0; n < N; ++n, i = (i + 1) & (N - 1)) {
        UnknownEntry& entry = table[i];
        u64 existing = entry.target.load(std::memory_order_relaxed);
        if (existing == 0 && entry.target.compare_exchange_strong(existing, target, std::memory_order_relaxed)) {
            existing = target;
        }
        if (existing == target) {
            entry.calls.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
    }
    return false;
}

void NoteUnknown(Shard& shard, u64 target) {
    if (!NoteTarget(shard.unknown, target)) {
        shard.unknown_overflow.fetch_add(1, std::memory_order_relaxed);
    }
}

/// One call of target `index`, counted in the frame in progress. The frame is the one in progress
/// when the call starts.
void CountCall(State& s, Shard& shard, u64 index, bool from_outside) {
    // Reserve the slot, then check it is still open for the epoch just read: the recycler closes
    // the slot before it looks at reservations, so one of us sees the other (nvn_epoch_slot.h).
    const u64 epoch = s.epoch.load(std::memory_order_acquire);
    const std::size_t slot = static_cast<std::size_t>(epoch % Epochs);
    if (ReserveSlotWrite(shard.inflight[slot], s.slot_open[slot], epoch)) {
        shard.frame[slot][index].fetch_add(1, std::memory_order_relaxed);
    } else {
        // The slot was recycled under us: late, and never written into the new epoch.
        shard.late[index].fetch_add(1, std::memory_order_relaxed);
    }
    CommitSlotWrite(shard.inflight[slot]);
    shard.total[index].fetch_add(1, std::memory_order_relaxed);
    if (from_outside) {
        shard.from_outside[index].fetch_add(1, std::memory_order_relaxed);
    }
    NoteGuestThread(shard, CurrentGuestThreadId());
}

/// First instruction of an NVN function. The return address says where the caller was: outside the
/// library for a call from the game, inside it when the library entered the function itself (and
/// for a tail branch from the game, which leaves the link register as the game's caller set it).
void OnCall(u64 index, u64 lr, u64) {
    State& s = S();
    const u64 hi = s.audit_hi.load(std::memory_order_relaxed);
    const bool outside = hi != 0 && (lr < s.audit_lo.load(std::memory_order_relaxed) || lr >= hi);
    CountCall(s, ThisShard(s), index, outside);
    // One observed NVN entry: the capture's FRAME_END call_count counts the same population.
    VideoCore::Replay::NoteGraphicsCall();
}

/// Makes `target`, an address in the library that a branch from outside reached and nothing knew, an
/// entry point of its own: it gets a target (no name) and a probe at its first instruction like a named
/// function. Audit observes the branch; only the entry probe records a call.
std::optional<EntryHit> DiscoverEntry(State& s, u64 target) {
    u32 index = 0;
    {
        std::scoped_lock lock{s.m};
        if (!detail::g_active.load(std::memory_order_relaxed)) {
            return std::nullopt;
        }
        if (const std::optional<EntryHit> hit = LookupEntry(s, target)) {
            return hit; // another thread got here first, or a resolver returned it meanwhile
        }
        if (s.target_by_address.find(target) != s.target_by_address.end()) {
            return std::nullopt; // known, but its probe could not be placed
        }
        const std::optional<u32> created = TargetFor(s, target, std::string{}, FromAudit);
        if (!created) {
            return std::nullopt;
        }
        index = *created;
        if (AddProbe(s, target, Role::AuditEntry, index) == AddResult::Rejected) {
            s.target_by_address.erase(target);
            s.targets.pop_back();
            return std::nullopt;
        }
    }
    const bool acknowledged = InvalidateAndWait(std::vector<u64>{target});
    std::scoped_lock lock{s.m};
    if (acknowledged) {
        s.targets[index].acked = true;
    } else {
        ++s.c.rendezvous_timeouts;
    }
    return EntryHit{index, Role::AuditEntry};
}

/// Whether a RET that landed at `target` came back from a call: the instruction before it, read
/// from the library's own code, is a call.
EntryClass ClassifyReturnAt(const State& s, u64 target) {
    auto* memory = s.memory.load(std::memory_order_acquire);
    if (memory == nullptr || target < sizeof(u32) || !memory->IsValidVirtualAddressRange(target - sizeof(u32), sizeof(u32))) {
        return EntryClass::Entry;
    }
    return ClassifyReturn(memory->Read32(target - sizeof(u32)));
}

/// A branch from outside the library, translated because it goes (or may go) into it. `kind` is
/// the BranchKind of the branch. Every such branch ends in exactly one place: a counted entry
/// point (named, or found here), an export that is not NVN, a return site (the library called out
/// and the callee came back), or the unknown table, which fails coverage.
void OnAuditBranch(u64 target, u64, u64 kind) {
    State& s = S();
    if (target < s.audit_lo.load(std::memory_order_relaxed) || target >= s.audit_hi.load(std::memory_order_relaxed)) {
        return;
    }
    Shard& shard = ThisShard(s);
    shard.audit_branches.fetch_add(1, std::memory_order_relaxed);
    const bool is_return = kind == static_cast<u64>(BranchKind::Return);
    if (is_return && BumpTarget(shard.returns, target)) {
        return;
    }
    const auto entered = [&](const EntryHit& hit) {
        shard.audited[hit.index].fetch_add(1, std::memory_order_relaxed);
    };
    if (const std::optional<EntryHit> hit = LookupEntry(s, target)) {
        entered(*hit);
        return;
    }
    if (std::binary_search(s.other_exports.begin(), s.other_exports.end(), target)) {
        shard.audit_other.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (is_return && ClassifyReturnAt(s, target) == EntryClass::ReturnSite) {
        if (!NoteTarget(shard.returns, target)) {
            shard.unknown_overflow.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }
    if (const std::optional<EntryHit> hit = DiscoverEntry(s, target)) {
        entered(*hit);
        return;
    }
    NoteUnknown(shard, target);
}

void OnEntry(u64 name_address, u64 lr, u64 sp, bool bootstrap) {
    State& s = S();
    if (!detail::g_active.load(std::memory_order_relaxed)) {
        return;
    }
    std::string name = ReadName(name_address);
    if (name.empty()) {
        s.name_read_failures.fetch_add(1, std::memory_order_relaxed);
        name = "<unreadable>";
    }
    const u64 guest_thread = CurrentGuestThreadId();
    std::vector<u64> install;
    {
        std::scoped_lock lock{s.m};
        ++(bootstrap ? s.c.bootstrap_calls : s.c.proc_calls);
        ++s.requests[name].count;
        // A live entry with the same key makes the return ambiguous: whichever of the two calls the
        // return belongs to, one is matched to a return that was not its own. Latch it.
        if (std::any_of(s.pending.begin(), s.pending.end(), [&](const Pending& p) {
                return p.guest_thread == guest_thread && p.sp == sp && p.lr == lr;
            })) {
            ++s.c.ambiguous_pending;
        }
        if (s.pending.size() >= MaxPending) {
            s.pending.erase(s.pending.begin());
            ++s.c.pending_overflow;
        }
        s.pending.push_back(Pending{guest_thread, sp, lr, bootstrap, std::move(name)});
        s.pending_count.store(static_cast<u32>(s.pending.size()), std::memory_order_release);
        if (AddProbe(s, lr, Role::Return, 0) == AddResult::Added) {
            install.push_back(lr);
        }
    }
    if (!InvalidateAndWait(install)) {
        std::scoped_lock lock{s.m};
        ++s.c.rendezvous_timeouts;
    }
}

void OnBootstrapEntry(u64 name_address, u64 lr, u64 sp) {
    OnEntry(name_address, lr, sp, true);
}

void OnProcEntry(u64 name_address, u64 lr, u64 sp) {
    OnEntry(name_address, lr, sp, false);
}

void OnReturn(u64 result, u64 sp, u64 pc) {
    State& s = S();
    if (s.pending_count.load(std::memory_order_acquire) == 0) {
        s.unmatched_returns.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const u64 guest_thread = CurrentGuestThreadId();
    std::vector<u64> install;
    std::vector<u32> armed;
    {
        std::scoped_lock lock{s.m};
        const auto it = std::find_if(s.pending.begin(), s.pending.end(), [&](const Pending& p) {
            return p.guest_thread == guest_thread && p.sp == sp && p.lr == pc;
        });
        if (it == s.pending.end()) {
            s.unmatched_returns.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        Pending call = std::move(*it);
        s.pending.erase(it);
        s.pending_count.store(static_cast<u32>(s.pending.size()), std::memory_order_release);

        if (result == 0) {
            ++s.c.null_results;
            ++s.nulls[call.name];
            return;
        }
        if (call.bootstrap) {
            s.bootstrap_results[call.name] = result;
        }
        if (const auto index = TargetFor(s, result, call.name, FromResolver)) {
            s.requests[call.name].address = result;
            // An audit-found address already has its entry probe (role AuditEntry): a second one from
            // the resolver would count every call twice. The name is added to the target; the probe stays.
            const std::optional<EntryHit> counted = LookupEntry(s, result);
            if ((!counted || counted->role != Role::AuditEntry) &&
                AddProbe(s, result, Role::Count, *index) == AddResult::Added) {
                install.push_back(result);
                armed.push_back(*index);
            }
        }
        if (call.bootstrap && call.name == "nvnDeviceGetProcAddress") {
            if (AddProbe(s, result, Role::ProcEntry, 0) == AddResult::Added &&
                std::find(install.begin(), install.end(), result) == install.end()) {
                install.push_back(result);
            }
        }
    }
    const bool acknowledged = InvalidateAndWait(install);
    std::scoped_lock lock{s.m};
    if (!acknowledged) {
        ++s.c.rendezvous_timeouts;
        return;
    }
    for (const u32 index : armed) {
        s.targets[index].acked = true;
    }
}

/// Caller holds s.m; no reporter thread runs and no guest code of an earlier run is alive. Wipes
/// everything a run accumulates so a second run in the same emulator process starts clean. The
/// shards are zeroed, not freed: host threads keep their thread_local pointers across runs.
void Reset(State& s) {
    for (std::size_t i = 0; i < ProbeSlots; ++i) {
        Slot& slot = s.slots[i];
        slot.pc.store(0, std::memory_order_release);
        slot.role = Role{};
        slot.index = 0;
    }
    s.slot_count.store(0, std::memory_order_release);
    const std::size_t shard_count = s.shard_count.load(std::memory_order_acquire);
    for (std::size_t t = 0; t < shard_count; ++t) {
        Shard& shard = *s.shards[t].load(std::memory_order_acquire);
        for (auto& epoch : shard.frame) {
            for (auto& calls : epoch) {
                calls.store(0, std::memory_order_relaxed);
            }
        }
        for (std::size_t i = 0; i < MaxTargets; ++i) {
            shard.total[i].store(0, std::memory_order_relaxed);
            shard.from_outside[i].store(0, std::memory_order_relaxed);
            shard.audited[i].store(0, std::memory_order_relaxed);
        }
        for (auto& entry : shard.guest) {
            entry.key.store(0, std::memory_order_relaxed);
            entry.calls.store(0, std::memory_order_relaxed);
        }
        for (auto& entry : shard.unknown) {
            entry.target.store(0, std::memory_order_relaxed);
            entry.calls.store(0, std::memory_order_relaxed);
        }
        for (auto& entry : shard.returns) {
            entry.target.store(0, std::memory_order_relaxed);
            entry.calls.store(0, std::memory_order_relaxed);
        }
        shard.guest_overflow.store(0, std::memory_order_relaxed);
        shard.unknown_overflow.store(0, std::memory_order_relaxed);
        shard.audit_branches.store(0, std::memory_order_relaxed);
        shard.audit_other.store(0, std::memory_order_relaxed);
        for (auto& count : shard.inflight) {
            count.store(0, std::memory_order_relaxed);
        }
        for (auto& count : shard.late) {
            count.store(0, std::memory_order_relaxed);
        }
    }
    s.epoch.store(0, std::memory_order_release);
    // Only epoch 0 is open; each present opens the slot of the epoch it starts.
    for (std::size_t slot = 0; slot < Epochs; ++slot) {
        s.slot_open[slot].store(slot == 0 ? 0 : SlotClosed, std::memory_order_seq_cst);
    }
    s.audit_lo.store(0, std::memory_order_release);
    s.audit_hi.store(0, std::memory_order_release);
    s.unaudited_entries.store(0, std::memory_order_relaxed);
    s.code_generation_baseline = 0;
    s.library_module.clear();
    s.other_exports.clear();
    s.modules.clear();
    s.bootstrap_sites.clear();
    s.targets.clear();
    s.target_by_address.clear();
    s.requests.clear();
    s.nulls.clear();
    s.bootstrap_results.clear();
    s.pending.clear();
    s.pending_count.store(0, std::memory_order_release);
    s.unmatched_returns.store(0, std::memory_order_relaxed);
    s.name_read_failures.store(0, std::memory_order_relaxed);
    s.c = Counters{};
    s.presents = 0;
    s.accepted_presents = 0;
    s.close_id = {};
    s.presents_by_path = {};
    s.streams.clear();
    s.next_close = 0;
    s.frames_closed = 0;
    s.startup_total = 0;
    s.frame_total_sum = 0;
    s.frame_total_max = 0;
    s.frame_totals.clear();
    s.frame_rows.clear();
    s.frame_rows_dropped = 0;
    s.windows.clear();
    s.last_grand = 0;
    s.last_presents = 0;
    s.stop = false;
}

/// Caller holds s.m. Takes the calls counted for frame `epoch` out of every shard, per target.
std::vector<u64> DrainEpoch(State& s, u64 epoch) {
    std::vector<u64> row(s.targets.size(), 0);
    const std::size_t shard_count = s.shard_count.load(std::memory_order_acquire);
    for (std::size_t t = 0; t < shard_count; ++t) {
        auto& cells = s.shards[t].load(std::memory_order_acquire)->frame[epoch % Epochs];
        for (std::size_t i = 0; i < row.size(); ++i) {
            if (cells[i].load(std::memory_order_relaxed) != 0) {
                row[i] += cells[i].exchange(0, std::memory_order_relaxed);
            }
        }
    }
    return row;
}

/// Caller holds s.m. Folds a finished epoch into the per-function and per-frame figures. Its frame
/// id is the id of the accepted present that closed it, as the capture numbers FRAME_END: epoch 0,
/// everything before the first present (start-up), is frame 1. Start-up keeps its own totals and is
/// not a closed frame, but its row is kept so per-frame calls line up with the capture.
void CloseEpoch(State& s, u64 epoch) {
    const std::vector<u64> row = DrainEpoch(s, epoch);
    const u64 frame = s.close_id[epoch % Epochs];
    u64 frame_total = 0;
    FrameRow sparse;
    sparse.frame = frame;
    for (std::size_t i = 0; i < row.size(); ++i) {
        const u64 calls = row[i];
        frame_total += calls;
        if (calls == 0) {
            continue;
        }
        sparse.calls.emplace_back(static_cast<u32>(i), calls);
        Target& target = s.targets[i];
        if (epoch == 0) {
            target.startup = calls;
            continue;
        }
        target.frame_sum += calls;
        target.frame_max = std::max(target.frame_max, calls);
        ++target.frames_called;
        if (target.first_frame < 0) {
            target.first_frame = static_cast<s64>(frame);
        }
    }
    if (s.frame_rows.size() < MaxFrameRows) {
        s.frame_rows.push_back(std::move(sparse));
    } else {
        ++s.frame_rows_dropped;
    }
    if (epoch == 0) {
        s.startup_total = frame_total;
        return;
    }
    ++s.frames_closed;
    s.frame_total_sum += frame_total;
    s.frame_total_max = std::max(s.frame_total_max, frame_total);
    if (s.frame_totals.size() < MaxFrameTotals) {
        s.frame_totals.push_back(frame_total);
    }
}

/// Caller holds s.m. The counters epoch `next` is about to reuse must be empty: whatever is in
/// them was counted for a frame closed long ago by a call that was still running when it closed.
/// Caller holds s.m. Writers never take it, so waiting here cannot deadlock. On expiry the
/// stuck writer is recorded late by its own re-check; the timeout only refuses completeness.
bool WaitSlotIdle(State& s, std::size_t slot) {
    const auto deadline = std::chrono::steady_clock::now() + RendezvousTimeout;
    const std::size_t shard_count = s.shard_count.load(std::memory_order_acquire);
    for (;;) {
        u64 inflight = 0;
        for (std::size_t t = 0; t < shard_count; ++t) {
            inflight += SlotReservations(s.shards[t].load(std::memory_order_acquire)->inflight[slot]);
        }
        if (inflight == 0) {
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            ++s.c.epoch_reservation_timeouts;
            return false;
        }
        std::this_thread::yield();
    }
}

void SweepLate(State& s, u64 next) {
    const std::vector<u64> row = DrainEpoch(s, next);
    for (std::size_t i = 0; i < row.size(); ++i) {
        s.targets[i].late += row[i];
        s.c.late_calls += row[i];
    }
}

/// Caller holds s.m. Folds the calls made since the last fold into the 10 s windows.
void Tick(State& s) {
    u64 grand = 0;
    for (std::size_t i = 0; i < s.targets.size(); ++i) {
        grand += Total(s, i);
    }
    const auto elapsed = std::chrono::steady_clock::now() - s.start;
    const std::size_t window = static_cast<std::size_t>(elapsed / WindowLength);
    if (s.windows.size() <= window) {
        s.windows.resize(window + 1);
    }
    s.windows[window].calls += grand - s.last_grand;
    s.windows[window].presents += s.presents - s.last_presents;
    s.last_grand = grand;
    s.last_presents = s.presents;
}

/// Everything a report needs, copied out under s.m so the JSON is built and written without it.
struct Snapshot {
    bool is_last{};
    bool run_complete{};
    std::string stop_reason;
    /// A replay capture of the run failed: its own result, kept out of how the run ended.
    bool capture_failed{};
    std::string capture_detail;
    std::filesystem::path out_path;
    bool audit_enabled{};
    u64 min_frames{};
    u64 program_id{};
    std::string cpu_backend;
    double elapsed_s{};
    std::vector<ModuleInfo> modules;
    std::string library_module;
    u64 audit_lo{};
    u64 audit_hi{};
    std::size_t bootstrap_sites{};
    std::size_t pending{};
    Counters c;
    u64 unmatched_returns{};
    u64 name_read_failures{};
    std::vector<Target> targets;
    std::vector<u64> totals;
    std::vector<u64> tail;                    ///< Per target, calls of the frame still open.
    std::vector<u64> from_outside;            ///< Per target, calls with a return address outside the library.
    std::vector<u64> audited;                 ///< Per target, branches from outside that landed on it.
    std::vector<u64> writer_late;             ///< Per target, calls that found their slot recycled.
    std::vector<std::vector<u64>> by_thread;  ///< Per target, per host thread.
    std::size_t threads{};
    std::map<u64, u64> guest_threads;         ///< Guest thread id -> calls.
    u64 guest_overflow{};
    std::map<u64, u64> unknown;               ///< Entry address -> branches, for targets nobody knew.
    std::map<u64, u64> returns;               ///< Return-site address -> RETs from outside that landed there.
    u64 unknown_overflow{};
    u64 audit_branches{};
    u64 audit_other{};
    std::size_t other_exports{};
    std::map<std::string, Request> requests;
    std::map<std::string, u64> nulls;
    std::map<std::string, u64> bootstrap_results;
    u64 presents{};
    u64 accepted_presents{};
    std::array<u64, 2> presents_by_path{};
    std::vector<StreamInfo> streams;
    u64 frames_closed{};
    u64 startup_total{};
    u64 frame_total_sum{};
    u64 frame_total_max{};
    std::vector<u64> frame_totals;
    std::vector<FrameRow> frame_rows;
    u64 frame_rows_dropped{};
    std::vector<Window> windows;
};

/// Caller holds s.m. Copies, never formats or sorts: this is the only part of a report the
/// guest-facing paths wait for. A final snapshot also closes every frame that is over.
Snapshot TakeSnapshot(State& s, bool is_last, bool run_complete, std::string stop_reason,
                      bool capture_failed = false, std::string capture_detail = {}) {
    Tick(s);
    if (is_last) {
        // Cores are quiescent, but drain any reservation so no commit lands after the read.
        for (std::size_t slot = 0; slot < Epochs; ++slot) {
            CloseSlot(s.slot_open[slot]);
            WaitSlotIdle(s, slot);
        }
        const u64 current = s.epoch.load(std::memory_order_acquire);
        while (s.next_close < current) {
            CloseEpoch(s, s.next_close++);
        }
    }
    Snapshot snap;
    snap.is_last = is_last;
    snap.run_complete = is_last && run_complete;
    snap.stop_reason = std::move(stop_reason);
    snap.capture_failed = is_last && capture_failed;
    snap.capture_detail = snap.capture_failed ? std::move(capture_detail) : std::string{};
    snap.out_path = s.out_path;
    snap.audit_enabled = s.audit_enabled;
    snap.min_frames = s.min_frames;
    snap.program_id = s.program_id;
    snap.cpu_backend = s.cpu_backend;
    snap.elapsed_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - s.start).count();
    snap.modules = s.modules;
    snap.library_module = s.library_module;
    snap.audit_lo = s.audit_lo.load(std::memory_order_relaxed);
    snap.audit_hi = s.audit_hi.load(std::memory_order_relaxed);
    snap.bootstrap_sites = s.bootstrap_sites.size();
    snap.pending = s.pending.size();
    s.c.unaudited_entries = s.unaudited_entries.load(std::memory_order_relaxed);
    snap.c = s.c;
    snap.unmatched_returns = s.unmatched_returns.load(std::memory_order_relaxed);
    snap.name_read_failures = s.name_read_failures.load(std::memory_order_relaxed);
    snap.targets = s.targets;
    snap.threads = s.shard_count.load(std::memory_order_acquire);
    snap.totals.assign(s.targets.size(), 0);
    snap.tail.assign(s.targets.size(), 0);
    snap.from_outside.assign(s.targets.size(), 0);
    snap.audited.assign(s.targets.size(), 0);
    snap.writer_late.assign(s.targets.size(), 0);
    snap.by_thread.assign(s.targets.size(), std::vector<u64>(snap.threads, 0));
    const u64 open_slot = s.epoch.load(std::memory_order_acquire) % Epochs;
    for (std::size_t t = 0; t < snap.threads; ++t) {
        const Shard& shard = *s.shards[t].load(std::memory_order_acquire);
        for (std::size_t i = 0; i < s.targets.size(); ++i) {
            const u64 calls = shard.total[i].load(std::memory_order_relaxed);
            snap.by_thread[i][t] = calls;
            snap.totals[i] += calls;
            snap.tail[i] += shard.frame[open_slot][i].load(std::memory_order_relaxed);
            snap.from_outside[i] += shard.from_outside[i].load(std::memory_order_relaxed);
            snap.audited[i] += shard.audited[i].load(std::memory_order_relaxed);
            snap.writer_late[i] += shard.late[i].load(std::memory_order_relaxed);
        }
        for (const GuestThreadCount& entry : shard.guest) {
            if (const u64 key = entry.key.load(std::memory_order_relaxed); key != 0) {
                snap.guest_threads[key - 1] += entry.calls.load(std::memory_order_relaxed);
            }
        }
        for (const UnknownEntry& entry : shard.unknown) {
            if (const u64 target = entry.target.load(std::memory_order_relaxed); target != 0) {
                snap.unknown[target] += entry.calls.load(std::memory_order_relaxed);
            }
        }
        for (const UnknownEntry& entry : shard.returns) {
            if (const u64 target = entry.target.load(std::memory_order_relaxed); target != 0) {
                snap.returns[target] += entry.calls.load(std::memory_order_relaxed);
            }
        }
        snap.guest_overflow += shard.guest_overflow.load(std::memory_order_relaxed);
        snap.unknown_overflow += shard.unknown_overflow.load(std::memory_order_relaxed);
        snap.audit_branches += shard.audit_branches.load(std::memory_order_relaxed);
        snap.audit_other += shard.audit_other.load(std::memory_order_relaxed);
    }
    snap.other_exports = s.other_exports.size();
    snap.requests = s.requests;
    snap.nulls = s.nulls;
    snap.bootstrap_results = s.bootstrap_results;
    snap.presents = s.presents;
    snap.accepted_presents = s.accepted_presents;
    snap.presents_by_path = s.presents_by_path;
    snap.streams = s.streams;
    snap.frames_closed = s.frames_closed;
    snap.startup_total = s.startup_total;
    snap.frame_total_sum = s.frame_total_sum;
    snap.frame_total_max = s.frame_total_max;
    snap.frame_totals = s.frame_totals;
    snap.frame_rows = s.frame_rows;
    snap.frame_rows_dropped = s.frame_rows_dropped;
    snap.windows = s.windows;
    return snap;
}

std::string Hex(u64 value) {
    return fmt::format("{:#x}", value);
}

/// Every call, once: over the run, calls = start-up + closed frames + late + the open tail. Exact
/// only in a final snapshot, when no guest code runs and every closed frame has been folded in.
struct Accounting {
    u64 executed{};
    u64 startup{};
    u64 frames{};
    u64 late{};
    u64 tail{};
    std::vector<std::size_t> mismatched;
    bool exact{};
};

Accounting Account(const Snapshot& s) {
    Accounting a;
    for (std::size_t i = 0; i < s.targets.size(); ++i) {
        const Target& target = s.targets[i];
        a.executed += s.totals[i];
        a.startup += target.startup;
        a.frames += target.frame_sum;
        a.late += target.late + s.writer_late[i];
        a.tail += s.tail[i];
        if (s.totals[i] != target.startup + target.frame_sum + target.late + s.writer_late[i] + s.tail[i]) {
            a.mismatched.push_back(i);
        }
    }
    a.exact = s.is_last && a.mismatched.empty() && a.executed == a.startup + a.frames + a.late + a.tail;
    return a;
}

nlohmann::json BuildReport(const Snapshot& s) {
    using nlohmann::json;
    const Accounting accounting = Account(s);

    // The branches into the library that the census could not attribute, those it attributed to a
    // return (the library called out and the callee came back), and what the two ways of counting
    // a function, at its first instruction and at the branch that reached it, saw of each target.
    u64 unknown_calls = 0;
    for (const auto& [target, calls] : s.unknown) {
        unknown_calls += calls;
    }
    u64 return_entries = 0;
    for (const auto& [target, calls] : s.returns) {
        return_entries += calls;
    }
    u64 audited_total = 0;
    u64 audit_shortfall_calls = 0;
    u64 caller_outside_not_audited = 0; // lr outside the library, no audited branch: entered from inside
    u64 audited_caller_inside = 0;      // audited branch, lr inside the library: a tail branch from outside
    u64 named_targets = 0;
    u64 audit_entry_counted = 0;
    u64 audit_targets = 0;
    std::vector<std::size_t> audit_short;
    for (std::size_t i = 0; i < s.targets.size(); ++i) {
        audited_total += s.audited[i];
        // Every audited branch must have been counted by the function's own counter. The other way
        // round is not an error: a function is also entered from inside the library.
        if (s.audited[i] > s.totals[i]) {
            audit_shortfall_calls += s.audited[i] - s.totals[i];
            audit_short.push_back(i);
        }
        if (s.from_outside[i] > s.audited[i]) {
            caller_outside_not_audited += s.from_outside[i] - s.audited[i];
        } else {
            audited_caller_inside += s.audited[i] - s.from_outside[i];
        }
        ((s.targets[i].provenance & FromAudit) != 0 ? audit_targets : named_targets) += 1;
        // `acked` of an audit-found target is set only once its entry probe is acknowledged.
        if ((s.targets[i].provenance & FromAudit) != 0 && s.targets[i].acked) {
            ++audit_entry_counted;
        }
    }
    const std::size_t known_targets = s.targets.size();
    const std::size_t discovered = known_targets + s.unknown.size();
    const std::size_t acked = static_cast<std::size_t>(
        std::count_if(s.targets.begin(), s.targets.end(), [](const Target& t) { return t.acked; }));
    const std::size_t paths_used = (s.presents_by_path[0] != 0 ? 1 : 0) + (s.presents_by_path[1] != 0 ? 1 : 0);
    const std::array<std::pair<const char*, u64>, 19> error_counters{{
        {"frame_rows_dropped", s.frame_rows_dropped},
        {"role_conflicts", s.c.role_conflicts},
        {"probe_table_full", s.c.probe_table_full},
        {"target_table_full", s.c.target_table_full},
        {"pending_overflow", s.c.pending_overflow},
        {"returns_not_observed", s.pending},
        {"rendezvous_timeouts", s.c.rendezvous_timeouts},
        {"name_read_failures", s.name_read_failures},
        {"late_modules", s.c.late_modules},
        {"stream_overflow", s.c.stream_overflow},
        {"guest_thread_overflow", s.guest_overflow},
        {"unknown_overflow", s.unknown_overflow},
        {"late_calls", s.c.late_calls},
        {"id_gaps", s.c.id_gaps},
        {"id_duplicates", s.c.id_duplicates},
        {"epoch_reservation_timeouts", s.c.epoch_reservation_timeouts},
        {"code_changes", s.c.code_changes},
        {"unaudited_entries", s.c.unaudited_entries},
        {"ambiguous_pending", s.c.ambiguous_pending},
    }};

    const bool identity_ok = s.streams.size() == 1 && s.streams[0].key != 0 && s.streams[0].tokens == 1 &&
                             !s.streams[0].ambiguous &&
                             (s.streams[0].path != PresentPath::Nx2Vi2 || s.streams[0].token != 0);
    u64 error_sum = 0;
    std::string error_detail;
    for (const auto& [name, value] : error_counters) {
        // Present id problems and late calls have conditions of their own; keep them out of this one.
        if (std::string_view{name} == "id_gaps" || std::string_view{name} == "id_duplicates" ||
            std::string_view{name} == "late_calls") {
            continue;
        }
        error_sum += value;
        if (value != 0) {
            error_detail += fmt::format("{}={} ", name, value);
        }
    }

    CoverageInputs inputs;
    inputs.final_report = s.is_last;
    inputs.run_complete = s.run_complete;
    inputs.audit_enabled = s.audit_enabled;
    inputs.audit_region_known = s.audit_hi != 0;
    inputs.bootstrap_sites = s.bootstrap_sites;
    inputs.bootstrap_calls = s.c.bootstrap_calls;
    inputs.late_modules = s.c.late_modules;
    inputs.code_changes = s.c.code_changes;
    inputs.named_targets = named_targets;
    inputs.audit_targets = audit_targets;
    inputs.audit_targets_entry_counted = audit_entry_counted;
    inputs.unknown_targets = s.unknown.size();
    inputs.unknown_calls = unknown_calls;
    inputs.unknown_overflow = s.unknown_overflow;
    inputs.unaudited_entries = s.c.unaudited_entries;
    inputs.instrumented_targets = acked;
    inputs.audited_entries = audited_total;
    inputs.audit_shortfall_functions = audit_short.size();
    inputs.audit_shortfall_calls = audit_shortfall_calls;
    inputs.accounting_exact = accounting.exact;
    inputs.accounting_late = accounting.late;
    inputs.executed = accounting.executed;
    inputs.startup = accounting.startup;
    inputs.frames = accounting.frames;
    inputs.tail = accounting.tail;
    inputs.accounting_mismatched = accounting.mismatched.size();
    inputs.paths_used = paths_used;
    inputs.streams = s.streams.size();
    inputs.stream_ambiguous = !s.streams.empty() && s.streams[0].ambiguous;
    inputs.stream_tokens = s.streams.empty() ? 0 : s.streams[0].tokens;
    inputs.identity_ok = identity_ok;
    inputs.presents = s.accepted_presents;
    inputs.frames_closed = s.frames_closed;
    inputs.min_frames = s.min_frames;
    inputs.id_gaps = s.c.id_gaps;
    inputs.id_duplicates = s.c.id_duplicates;
    inputs.lost_events = error_sum;
    inputs.lost_detail = error_detail;
    const Verdict verdict = EvaluateCoverage(inputs);
    const double resolved_share = verdict.resolved_share;
    const double instrumented_share = verdict.instrumented_share;
    const bool coverage_complete = verdict.complete;

    json coverage_conditions = json::object();
    for (const Condition& condition : verdict.conditions) {
        coverage_conditions[condition.name] = {{"ok", condition.ok}, {"detail", condition.detail}};
    }
    json failed = json::array();
    for (const std::string& name : verdict.failed) {
        failed.push_back(name);
    }
    json reasons = json::array();
    for (const std::string& reason : verdict.reasons) {
        reasons.push_back(reason);
    }

    json report;
    report["schema"] = 3;
    // frame_rows[*][0] is the id of the accepted present closing that frame (FRAME_END frame_id).
    report["frame_id_basis"] = "closing_accepted_present";
    report["kind"] = "nvn_call_census";
    report["final"] = s.is_last;
    report["run_complete"] = s.run_complete;
    report["coverage_complete"] = coverage_complete;
    // True only once the report carries a capture of the arguments and results of each call.
    report["library_replay_ready"] = false;
    report["capture"] = {{"present", false}, {"reason", "argument capture is not implemented"}};
    report["stop"] = {{"reason", s.stop_reason}, {"clean", s.run_complete}};
    // The result of a replay capture of this run, apart from how the run ended: a failed capture
    // ends the run as `capture_failed` for the run report but not for the census.
    report["replay_capture"] = {{"failed", s.capture_failed}, {"detail", s.capture_detail}};

    json modules = json::array();
    for (const ModuleInfo& module : s.modules) {
        modules.push_back({{"name", module.name},
                           {"base", Hex(module.base)},
                           {"extent", Hex(module.extent)},
                           {"hash_fnv1a64", Hex(module.hash)},
                           {"hashed_bytes", module.hashed}});
    }
    report["provenance"] = {
        {"emulator", {{"version", std::string(Common::g_build_version)},
                      {"scm_rev", std::string(Common::g_scm_rev)},
                      {"build_id", std::string(Common::g_build_id)}}},
        {"title_id", fmt::format("{:016X}", s.program_id)},
        {"cpu_backend", s.cpu_backend},
        {"modules", std::move(modules)},
        {"library", {{"module", s.library_module},
                     {"base", Hex(s.audit_lo)},
                     {"end", Hex(s.audit_hi)},
                     {"discovery", "dynsym export nvnBootstrapLoader"}}},
        {"sdk_version", nullptr},
        {"audit", s.audit_enabled},
        {"min_frames", s.min_frames},
    };
    report["duration_s"] = s.elapsed_s;

    json bootstrap;
    bootstrap["found"] = s.bootstrap_sites != 0;
    bootstrap["sites"] = s.bootstrap_sites;
    bootstrap["bootstrap_calls"] = s.c.bootstrap_calls;
    bootstrap["proc_address_calls"] = s.c.proc_calls;
    bootstrap["null_results"] = s.c.null_results;
    bootstrap["returns_not_observed"] = s.pending;
    bootstrap["unmatched_returns"] = s.unmatched_returns;
    json results = json::object();
    for (const auto& [name, address] : s.bootstrap_results) {
        results[name] = Hex(address);
    }
    bootstrap["results"] = std::move(results);
    report["bootstrap"] = std::move(bootstrap);

    const auto locate = [&](u64 address) -> std::pair<std::string, u64> {
        const ModuleInfo* best = nullptr;
        for (const ModuleInfo& module : s.modules) {
            if (module.base <= address && (best == nullptr || module.base > best->base)) {
                best = &module;
            }
        }
        return best != nullptr ? std::pair{best->name, address - best->base} : std::pair{std::string{}, address};
    };

    std::vector<std::size_t> order(s.targets.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return s.totals[a] > s.totals[b]; });
    const u64 frames = s.frames_closed;
    json functions = json::array();
    for (const std::size_t i : order) {
        const Target& target = s.targets[i];
        const auto [module, offset] = locate(target.address);
        json provenance = json::array();
        if ((target.provenance & FromExport) != 0) {
            provenance.push_back("export");
        }
        if ((target.provenance & FromResolver) != 0) {
            provenance.push_back("resolver");
        }
        if ((target.provenance & FromAudit) != 0) {
            provenance.push_back("audit");
        }
        // An entry point only the audit found has no name; the report names it by where it is.
        std::vector<std::string> names = target.names;
        if (names.empty()) {
            names.push_back(fmt::format("unnamed:{}+{}", module, Hex(offset)));
        }
        json entry;
        entry["id"] = i;
        entry["names"] = names;
        entry["named"] = !target.names.empty();
        entry["counted_by"] = target.acked ? "probe_at_entry" : "entry_probe_unconfirmed";
        entry["alias_group"] = target.names.size();
        entry["target"] = Hex(target.address);
        entry["module"] = module;
        entry["offset"] = Hex(offset);
        entry["provenance"] = std::move(provenance);
        entry["acked"] = target.acked;
        entry["calls"] = s.totals[i];
        entry["startup_calls"] = target.startup;
        entry["frame_calls"] = target.frame_sum;
        entry["frame_mean"] = frames != 0 ? static_cast<double>(target.frame_sum) / static_cast<double>(frames) : 0.0;
        entry["frame_max"] = target.frame_max;
        entry["frames_called"] = target.frames_called;
        entry["first_frame"] = target.first_frame;
        entry["late_calls"] = target.late + s.writer_late[i];
        entry["open_tail_calls"] = s.tail[i];
        entry["from_outside"] = s.from_outside[i];
        entry["audited_entries"] = s.audited[i];
        entry["by_thread"] = s.by_thread[i];
        functions.push_back(std::move(entry));
    }
    report["functions"] = std::move(functions);
    report["threads"] = s.threads;

    json guest_threads = json::array();
    for (const auto& [id, calls] : s.guest_threads) {
        guest_threads.push_back({{"id", id}, {"calls", calls}});
    }
    report["guest_threads"] = std::move(guest_threads);

    json requests = json::array();
    for (const auto& [name, request] : s.requests) {
        requests.push_back({{"name", name},
                            {"count", request.count},
                            {"target", request.address != 0 ? Hex(request.address) : std::string{}}});
    }
    report["requests"] = std::move(requests);
    json nulls = json::array();
    for (const auto& [name, count] : s.nulls) {
        nulls.push_back({{"name", name}, {"count", count}});
    }
    report["null_results"] = std::move(nulls);

    std::vector<u64> sorted = s.frame_totals;
    std::sort(sorted.begin(), sorted.end());
    const auto percentile = [&](double p) -> u64 {
        return sorted.empty() ? 0 : sorted[static_cast<std::size_t>(p * static_cast<double>(sorted.size() - 1))];
    };
    json streams = json::array();
    for (const StreamInfo& stream : s.streams) {
        streams.push_back({{"path", stream.path == PresentPath::Nx2Vi2 ? "nx2_vi2" : "buffer_queue"},
                           {"stream", Hex(stream.key)},
                           {"token", Hex(stream.token)},
                           {"tokens", stream.tokens},
                           {"ambiguous", stream.ambiguous},
                           {"presents", stream.presents},
                           {"last_id", stream.last_id}});
    }
    json frame_info;
    frame_info["presents"] = s.presents;
    frame_info["accepted_presents"] = s.accepted_presents;
    frame_info["presents_by_path"] = {{"nx2_vi2", s.presents_by_path[0]}, {"buffer_queue", s.presents_by_path[1]}};
    frame_info["streams"] = std::move(streams);
    frame_info["closed"] = frames;
    frame_info["startup_calls"] = s.startup_total;
    frame_info["calls_mean"] = frames != 0 ? static_cast<double>(s.frame_total_sum) / static_cast<double>(frames) : 0.0;
    frame_info["calls_median"] = percentile(0.5);
    frame_info["calls_p95"] = percentile(0.95);
    frame_info["calls_max"] = s.frame_total_max;
    frame_info["calls_recorded"] = sorted.size();
    frame_info["rows_recorded"] = s.frame_rows.size();
    frame_info["rows_dropped"] = s.frame_rows_dropped;
    // Per-frame rows join the capture's FRAME_END ids only when one unambiguous stream produced
    // every present and each of them reached the accepted-present seam; otherwise a verifier must
    // refuse per-frame qualification rather than guess which present closed which row.
    std::string unjoinable;
    if (s.streams.size() != 1) {
        unjoinable = fmt::format("{} present streams", s.streams.size());
    } else if (s.streams[0].ambiguous) {
        unjoinable = "the present stream is ambiguous";
    } else if (s.streams[0].presents != s.accepted_presents) {
        unjoinable = fmt::format("{} stream presents but {} accepted presents",
                                 s.streams[0].presents, s.accepted_presents);
    } else if (s.frame_rows_dropped != 0) {
        unjoinable = fmt::format("{} frame rows dropped", s.frame_rows_dropped);
    }
    frame_info["rows_joinable"] = unjoinable.empty();
    frame_info["rows_unjoinable_reason"] = unjoinable.empty() ? json(nullptr) : json(unjoinable);
    report["frames"] = std::move(frame_info);

    // Per frame, sparse: [frame, [[function id, calls], ...]].
    json rows = json::array();
    if (s.is_last) {
        for (const FrameRow& row : s.frame_rows) {
            json calls = json::array();
            for (const auto& [id, count] : row.calls) {
                calls.push_back(json::array({id, count}));
            }
            rows.push_back(json::array({row.frame, std::move(calls)}));
        }
    }
    report["frame_rows"] = std::move(rows);

    json mismatches = json::array();
    for (std::size_t n = 0; n < accounting.mismatched.size() && n < MaxReportedMismatches; ++n) {
        const std::size_t i = accounting.mismatched[n];
        const Target& target = s.targets[i];
        mismatches.push_back({{"id", i},
                              {"calls", s.totals[i]},
                              {"accounted", target.startup + target.frame_sum + target.late + s.tail[i]}});
    }
    report["accounting"] = {{"final", s.is_last},
                            {"executed_total", accounting.executed},
                            {"startup_calls", accounting.startup},
                            {"closed_frame_calls", accounting.frames},
                            {"late_calls", accounting.late},
                            {"open_tail_calls", accounting.tail},
                            {"mismatched_functions", accounting.mismatched.size()},
                            {"mismatches", std::move(mismatches)},
                            {"exact", accounting.exact}};

    json unknown = json::array();
    std::vector<std::pair<u64, u64>> unknown_sorted(s.unknown.begin(), s.unknown.end());
    std::sort(unknown_sorted.begin(), unknown_sorted.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    for (std::size_t n = 0; n < unknown_sorted.size() && n < MaxReportedUnknown; ++n) {
        const auto [module, offset] = locate(unknown_sorted[n].first);
        unknown.push_back({{"target", Hex(unknown_sorted[n].first)},
                           {"module", module},
                           {"offset", Hex(offset)},
                           {"calls", unknown_sorted[n].second}});
    }
    json audit_mismatches = json::array();
    for (std::size_t n = 0; n < audit_short.size() && n < MaxReportedMismatches; ++n) {
        const std::size_t i = audit_short[n];
        audit_mismatches.push_back({{"id", i},
                                    {"audited_entries", s.audited[i]},
                                    {"calls", s.totals[i]},
                                    {"from_outside", s.from_outside[i]}});
    }
    json return_sites = json::array();
    std::vector<std::pair<u64, u64>> returns_sorted(s.returns.begin(), s.returns.end());
    std::sort(returns_sorted.begin(), returns_sorted.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    for (std::size_t n = 0; n < returns_sorted.size() && n < MaxReportedUnknown; ++n) {
        const auto [module, offset] = locate(returns_sorted[n].first);
        return_sites.push_back({{"target", Hex(returns_sorted[n].first)},
                                {"module", module},
                                {"offset", Hex(offset)},
                                {"calls", returns_sorted[n].second}});
    }
    report["audit"] = {{"enabled", s.audit_enabled},
                       {"region_known", s.audit_hi != 0},
                       {"branches_into_library", s.audit_branches},
                       // Branches that landed on a counted entry point, named or found by the audit.
                       {"known_entries", audited_total},
                       {"other_export_entries", s.audit_other},
                       {"other_exports", s.other_exports},
                       // RETs from outside that came back to a place after a call in the library: the
                       // library called out and the callee returned. They enter nothing.
                       {"return_site_entries", return_entries},
                       {"return_sites", s.returns.size()},
                       {"return_site_list", std::move(return_sites)},
                       {"unknown_targets", s.unknown.size()},
                       {"unknown_calls", unknown_calls},
                       {"unknown_overflow", s.unknown_overflow},
                       {"unknown", std::move(unknown)},
                       // Entries the audit saw that the function's own counter did not: the failure.
                       {"mismatched_functions", audit_short.size()},
                       {"mismatched_calls", audit_shortfall_calls},
                       {"mismatches", std::move(audit_mismatches)},
                       // Calls whose return address lies outside the library but that no audited branch
                       // reached: the function was entered from inside (a tail branch of another library
                       // function leaves the caller's return address), so the two counts differ legitimately.
                       {"calls_entered_from_inside_with_outside_caller", caller_outside_not_audited},
                       // Audited branches whose callee saw a return address inside the library: a tail
                       // branch (B, BR) from outside, which does not set the link register.
                       {"audited_entries_with_inside_caller", audited_caller_inside}};

    json windows = json::array();
    for (std::size_t w = 0; w < s.windows.size(); ++w) {
        const double begin = static_cast<double>(w) * 10.0;
        const double length = std::clamp(s.elapsed_s - begin, 0.0, 10.0);
        windows.push_back({{"start_s", begin},
                           {"seconds", length},
                           {"calls", s.windows[w].calls},
                           {"calls_per_s", length > 0.0 ? static_cast<double>(s.windows[w].calls) / length : 0.0},
                           {"presents", s.windows[w].presents}});
    }
    report["windows"] = std::move(windows);

    report["executed_total"] = accounting.executed;
    json errors = json::object();
    for (const auto& [name, value] : error_counters) {
        errors[name] = value;
    }
    errors["unmatched_returns"] = s.unmatched_returns; // diagnostic: a probed return that matched no call
    report["errors"] = std::move(errors);
    report["coverage"] = {{"verdict", coverage_complete ? "COMPLETE" : "INCOMPLETE"},
                          {"conditions", std::move(coverage_conditions)},
                          {"failed", std::move(failed)},
                          {"reasons", std::move(reasons)},
                          {"resolved_target_share", resolved_share},
                          {"instrumented_target_share", instrumented_share},
                          {"targets", known_targets},
                          {"named_targets", named_targets},
                          {"audit_found_targets", audit_targets},
                          {"audit_found_counted_at_entry", audit_entry_counted},
                          {"discovered_targets", discovered},
                          {"scope", "the frames of the workload this run covered; not any other scene"}};
    return report;
}

/// Moves from over to, replacing it in one step; to is never removed first, so a reader sees the
/// old report or the new one, and a failure leaves the old one in place.
std::error_code ReplaceFile(const std::filesystem::path& from, const std::filesystem::path& to) {
#ifdef _WIN32
    if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0) {
        return {};
    }
    return std::error_code{static_cast<int>(GetLastError()), std::system_category()};
#else
    std::error_code ec;
    std::filesystem::rename(from, to, ec); // POSIX rename replaces an existing file atomically
    return ec;
#endif
}

/// Builds, serialises and writes a report. Takes no lock.
void WriteReport(const Snapshot& snapshot) {
    const std::string text = BuildReport(snapshot).dump();
    auto tmp = snapshot.out_path;
    tmp += ".tmp";
    {
        std::ofstream file{tmp, std::ios::binary | std::ios::trunc};
        file << text;
        if (!file) {
            LOG_ERROR(Core, "NVN census: cannot write {}", tmp.string());
            return;
        }
    }
    if (const std::error_code ec = ReplaceFile(tmp, snapshot.out_path)) {
        LOG_ERROR(Core, "NVN census: cannot replace {}: {}", snapshot.out_path.string(), ec.message());
        std::error_code ignored;
        std::filesystem::remove(tmp, ignored);
    }
}

/// How many executable modules exist now that did not exist when the census started.
u64 CountLateModules(const State& s, Kernel::KProcess* process) {
    u64 late = 0;
    for (const auto& [base, name] : Core::FindModules(process)) {
        const bool known = std::any_of(s.modules.begin(), s.modules.end(),
                                       [module_base = base](const ModuleInfo& module) { return module.base == module_base; });
        if (!known) {
            ++late;
        }
    }
    return late;
}

void ReporterLoop(State& s) {
    std::unique_lock lock{s.m};
    const auto begin = std::chrono::steady_clock::now();
    auto next_write = begin + WriteInterval;
    auto next_modules = begin + ModuleCheckInterval;
    while (!s.stop) {
        s.cv.wait_for(lock, TickInterval);
        if (s.stop) {
            break;
        }
        Tick(s);
        if (std::chrono::steady_clock::now() >= next_modules) {
            next_modules += ModuleCheckInterval;
            // Stop joins this thread before it clears the process, so the pointer stays valid here.
            Kernel::KProcess* process = s.process.load(std::memory_order_acquire);
            if (process != nullptr) {
                lock.unlock();
                const u64 late = CountLateModules(s, process);
                lock.lock();
                s.c.late_modules = std::max(s.c.late_modules, late);
                if (s.stop) {
                    break;
                }
            }
        }
        if (std::chrono::steady_clock::now() >= next_write) {
            next_write += WriteInterval;
            const Snapshot snapshot = TakeSnapshot(s, false, false, "running");
            lock.unlock();
            WriteReport(snapshot);
            lock.lock();
        }
    }
}

constexpr InstructionProbe::Operand Op(Arg kind, u64 imm = 0) {
    InstructionProbe::Operand operand;
    operand.kind = kind;
    operand.imm = imm;
    return operand;
}

std::size_t State::Find(VAddr pc, InstructionProbeSet& out) const {
    if (slot_count.load(std::memory_order_acquire) == 0) {
        return out.count;
    }
    std::size_t i = SlotHash(pc);
    for (std::size_t n = 0; n < ProbeSlots; ++n, i = (i + 1) & (ProbeSlots - 1)) {
        const Slot& slot = slots[i];
        const u64 existing = slot.pc.load(std::memory_order_acquire);
        if (existing == 0) {
            break;
        }
        if (existing != pc || out.count >= out.probes.size()) {
            continue;
        }
        InstructionProbe& probe = out.probes[out.count];
        switch (slot.role) {
        case Role::BootstrapEntry:
            probe.fn = OnBootstrapEntry;
            probe.operands = {Op(Arg::X0), Op(Arg::X30), Op(Arg::SP)};
            probe.end_block_after = true;
            break;
        case Role::ProcEntry:
            probe.fn = OnProcEntry;
            probe.operands = {Op(Arg::X1), Op(Arg::X30), Op(Arg::SP)};
            probe.end_block_after = true;
            break;
        case Role::Return:
            probe.fn = OnReturn;
            probe.operands = {Op(Arg::X0), Op(Arg::SP), Op(Arg::Imm, pc)};
            probe.end_block_after = true;
            break;
        case Role::Count:
        case Role::AuditEntry:
            probe.fn = OnCall;
            probe.operands = {Op(Arg::Imm, slot.index), Op(Arg::X30), Op(Arg::Imm, 0)};
            probe.end_block_after = false;
            break;
        }
        ++out.count;
    }
    return out.count;
}

bool State::FindBranch(VAddr pc, BranchKind kind, u64 target_or_reg, InstructionProbe& out) const {
    const u64 hi = audit_hi.load(std::memory_order_acquire);
    if (hi == 0) {
        return false;
    }
    const u64 lo = audit_lo.load(std::memory_order_relaxed);
    if (pc >= lo && pc < hi) {
        return false; // a branch inside the library is not a call into it
    }
    if (kind == BranchKind::Conditional) {
        // The target is known now, but the branch is taken only sometimes, so a probe on the taken
        // edge would count branches that never ran. A target inside the library fails coverage.
        if (target_or_reg >= lo && target_or_reg < hi) {
            unaudited_entries.fetch_add(1, std::memory_order_relaxed);
        }
        return false;
    }
    if (kind == BranchKind::Direct && (target_or_reg < lo || target_or_reg >= hi)) {
        return false;
    }
    out.fn = OnAuditBranch;
    out.operands = {kind == BranchKind::Direct ? Op(Arg::Imm, target_or_reg) : Op(Arg::XReg, target_or_reg),
                    Op(Arg::Imm, pc), Op(Arg::Imm, static_cast<u64>(kind))};
    // Discovery may have invalidated a cached target. Service it before entering the callee.
    out.end_block_after = true;
    return true;
}

u64 HashImage(Core::Memory::Memory& memory, u64 base, u64 size, u64& hashed) {
    constexpr u64 Prime = 0x100000001b3ULL;
    u64 hash = 0xcbf29ce484222325ULL;
    hashed = 0;
    std::vector<u8> buffer(std::size_t{1} << 18);
    for (u64 offset = 0; offset < size && hashed < MaxHashBytes; offset += buffer.size()) {
        const std::size_t length = static_cast<std::size_t>(std::min<u64>(buffer.size(), size - offset));
        if (!memory.IsValidVirtualAddressRange(base + offset, length) ||
            !memory.ReadBlockUnsafe(base + offset, buffer.data(), length)) {
            break;
        }
        for (std::size_t i = 0; i < length; ++i) {
            hash = (hash ^ buffer[i]) * Prime;
        }
        hashed += length;
    }
    return hash;
}

bool StartsWithNvn(const std::string& name) {
    return name.size() > 3 && name.compare(0, 3, "nvn") == 0;
}

std::string_view StopReasonName(Common::RunOutcome::Reason reason) {
    return Common::RunOutcome::ReasonName(reason);
}

} // namespace

void Configure(Options options) {
    std::scoped_lock lock{g_options_mutex};
    g_options = std::move(options);
}

const Dynarmic::A64::InstructionProbes* Probes() {
    return EffectiveOptions().out.empty() ? nullptr : &S();
}

void Start(Kernel::KProcess& process, u64 program_id) {
    const Options options = EffectiveOptions();
    if (options.out.empty()) {
        return;
    }
    State& s = S();
    bool acknowledged = true;
    u64 install_lo = ~u64{0};
    u64 install_hi = 0;
    {
        std::scoped_lock lock{s.m};
        if (s.started) {
            return;
        }
        Reset(s);
        s.started = true;
        s.out_path = std::filesystem::path{options.out};
        s.audit_enabled = options.audit;
        s.min_frames = options.min_frames;
        s.program_id = program_id;
        s.start = std::chrono::steady_clock::now();
        s.memory.store(std::addressof(process.GetMemory()), std::memory_order_release);
        s.process.store(std::addressof(process), std::memory_order_release);
        const bool nce = Settings::values.cpu_backend.GetValue() == Settings::CpuBackend::Nce;
        s.cpu_backend = nce ? "nce" : "dynarmic";
        if (nce) {
            LOG_ERROR(Core, "NVN census: the NCE CPU backend emits no probes; nothing will be counted");
        }

        const auto note_install = [&](u64 address) {
            install_lo = std::min(install_lo, address);
            install_hi = std::max(install_hi, address + sizeof(u32));
        };
        const bool is_64 = process.Is64Bit();
        bool library_found = false;
        for (const auto& [base, name] : Core::FindModules(std::addressof(process))) {
            ModuleInfo info;
            info.name = std::string{name};
            info.base = base;
            info.extent = GetInteger(Core::GetModuleEnd(std::addressof(process), base)) + 1 - base;
            info.hash = HashImage(process.GetMemory(), base, info.extent, info.hashed);
            s.modules.push_back(info);
            if (library_found) {
                continue;
            }
            const auto symbols = Core::Symbols::GetSymbols(base, process.GetMemory(), is_64);
            const auto bootstrap = symbols.find("nvnBootstrapLoader");
            if (bootstrap == symbols.end() || bootstrap->second.first == 0) {
                continue;
            }
            const u64 entry = base + bootstrap->second.first;
            if (!process.GetMemory().IsValidVirtualAddressRange(entry, sizeof(u32))) {
                continue;
            }
            // This module is the library: arm its bootstrap and every NVN function it exports, and
            // remember its other exports so a branch to one of them is not mistaken for an NVN call.
            library_found = true;
            s.library_module = info.name;
            if (AddProbe(s, entry, Role::BootstrapEntry, 0) != AddResult::Rejected) {
                s.bootstrap_sites.push_back(entry);
                note_install(entry);
            }
            for (const auto& [symbol, location] : symbols) {
                if (location.first == 0) {
                    continue;
                }
                const u64 address = base + location.first;
                if (!StartsWithNvn(symbol)) {
                    s.other_exports.push_back(address);
                    continue;
                }
                const auto index = TargetFor(s, address, symbol, FromExport);
                if (index && AddProbe(s, address, Role::Count, *index) != AddResult::Rejected) {
                    note_install(address);
                }
            }
            std::sort(s.other_exports.begin(), s.other_exports.end());
            if (options.audit) {
                s.audit_lo.store(base, std::memory_order_release);
                s.audit_hi.store(base + info.extent, std::memory_order_release);
            }
        }
        detail::g_active.store(true, std::memory_order_release);
        s.reporter = std::thread{ReporterLoop, std::ref(s)};
    }
    if (install_hi != 0) {
        // Nothing has run yet in the normal case; if some core has, wait until it has left.
        acknowledged = InvalidateAndWait(install_lo, install_hi - install_lo);
    }
    {
        std::scoped_lock lock{s.m};
        if (acknowledged) {
            for (Target& target : s.targets) {
                target.acked = true;
            }
        } else {
            ++s.c.rendezvous_timeouts;
        }
        s.code_generation_baseline = process.CodeGeneration();
    }
    if (s.bootstrap_sites.empty()) {
        LOG_WARNING(Core, "NVN census: no nvnBootstrapLoader export in any loaded module; the census cannot be complete");
    } else {
        LOG_INFO(Core, "NVN census on ({} functions armed, audit {}), writing {}", s.targets.size(),
                 options.audit ? "on" : "off", s.out_path.string());
    }
}

void Stop() {
    if (EffectiveOptions().out.empty()) {
        return; // as in Start: off means no state, and S() would allocate the slot table
    }
    State& s = S();
    {
        std::scoped_lock lock{s.m};
        if (!s.started) {
            return;
        }
        s.stop = true;
    }
    s.cv.notify_all();
    if (s.reporter.joinable()) {
        s.reporter.join();
    }
    const StopFacts ended = ClassifyStop(Common::RunOutcome::CurrentRecord());
    const std::string capture_detail = ended.capture_failed ? Common::RunOutcome::Detail() : std::string{};
    Snapshot snapshot;
    {
        std::scoped_lock lock{s.m};
        if (Kernel::KProcess* process = s.process.load(std::memory_order_acquire);
            process != nullptr && process->CodeGeneration() != s.code_generation_baseline) {
            s.c.code_changes = process->CodeGeneration() - s.code_generation_baseline;
        }
        detail::g_active.store(false, std::memory_order_release);
        s.memory.store(nullptr, std::memory_order_release);
        s.process.store(nullptr, std::memory_order_release);
        snapshot = TakeSnapshot(s, true, ended.clean, std::string{StopReasonName(ended.ended_by)},
                                ended.capture_failed, capture_detail);
        s.started = false;
        s.stop = false;
    }
    WriteReport(snapshot);
}

void detail::NotePresentSlow(PresentPath path, u64 stream, u64 token, u64 id) {
    State& s = S();
    std::scoped_lock lock{s.m};
    if (!s.started) {
        return;
    }
    ++s.presents;
    ++s.presents_by_path[static_cast<std::size_t>(path)];
    auto it = std::find_if(s.streams.begin(), s.streams.end(),
                           [&](const StreamInfo& info) { return info.key == stream && info.path == path; });
    if (it == s.streams.end()) {
        if (s.streams.size() < MaxStreams) {
            // A zero key, or an nx2 present with no token, is an erased identity. Keep the stream
            // and fail it closed; do not invent a producer from the session-wide present count.
            StreamInfo info;
            info.key = stream;
            info.path = path;
            info.token = token;
            info.tokens = 1;
            info.ambiguous = stream == 0 || (path == PresentPath::Nx2Vi2 && token == 0);
            info.last_id = id - 1;
            s.streams.push_back(info);
            it = s.streams.end() - 1;
        } else {
            ++s.c.stream_overflow;
        }
    } else if (it->token != token) {
        // Two client-buffer tokens on one session are two producers, or one queue whose token
        // rotated. Either way this is not one stream: mark it ambiguous instead of splitting the
        // rotating tokens apart or collapsing them into one consecutive id sequence.
        it->ambiguous = true;
        if (it->tokens < 2) {
            it->tokens = 2;
        }
    }
    if (it != s.streams.end()) {
        ObservePresentId(it->last_id, id, s.c.id_duplicates, s.c.id_gaps);
        ++it->presents;
        it->last_id = id;
    }

}

namespace {

/// Caller holds s.m. Accepted present `present_id` ends the frame in progress, epoch e, and starts
/// epoch e + 1: calls that start after the store below count for the new frame. Epoch e - 1 is
/// folded in now, a frame late, so a call that started in it and was still running has landed its
/// count by then; one that lands after that is counted as late rather than lost.
void AdvanceEpoch(State& s, u64 present_id) {
    ++s.accepted_presents;
    const u64 next = s.epoch.load(std::memory_order_acquire) + 1;
    s.close_id[(next - 1) % Epochs] = present_id;
    // The slot `next` reuses held epoch next-Epochs. Close it to new reservations first, so a
    // writer still holding that older epoch cannot reserve after the drain below has looked, then
    // wait out the writers that reserved before the close. A writer that comes late finds the slot
    // closed or open for another epoch, and counts itself late instead of writing.
    const std::size_t next_slot = static_cast<std::size_t>(next % Epochs);
    CloseSlot(s.slot_open[next_slot]);
    WaitSlotIdle(s, next_slot);
    if (Kernel::KProcess* process = s.process.load(std::memory_order_acquire);
        process != nullptr && process->CodeGeneration() != s.code_generation_baseline) {
        s.c.code_changes = process->CodeGeneration() - s.code_generation_baseline;
    }
    SweepLate(s, next);
    OpenSlot(s.slot_open[next_slot], next);
    s.epoch.store(next, std::memory_order_seq_cst);
    while (s.next_close + 2 <= next) {
        CloseEpoch(s, s.next_close++);
    }
}

} // namespace

void detail::NoteAcceptedPresentSlow(u64 present_id) {
    State& s = S();
    std::scoped_lock lock{s.m};
    if (!s.started) {
        return;
    }
    AdvanceEpoch(s, present_id);
}

namespace testing {

void Begin(std::size_t targets) {
    State& s = S();
    std::scoped_lock lock{s.m};
    Reset(s);
    s.targets.resize(targets);
    s.start = std::chrono::steady_clock::now();
    s.audit_enabled = true;
    s.audit_lo = sizeof(u32);
    s.audit_hi = (targets + 1) * sizeof(u32);
    for (u32 i = 0; i < targets; ++i) {
        auto& target = s.targets[i];
        target.address = (u64{i} + 1) * sizeof(u32);
        target.acked = AddProbe(s, target.address, Role::Count, i) == AddResult::Added;
    }
    s.started = true;
    detail::g_active.store(true, std::memory_order_release);
}

void Call(u64 index) {
    State& s = S();
    CountCall(s, ThisShard(s), index, false);
}

void AuditBranch(u64 index) {
    OnAuditBranch(S().targets.at(index).address, 0, static_cast<u64>(BranchKind::Direct));
}

static Snapshot Finish() {
    State& s = S();
    std::scoped_lock lock{s.m};
    detail::g_active.store(false, std::memory_order_release);
    auto snap = TakeSnapshot(s, true, true, "frame_limit");
    Reset(s);
    s.started = false;
    return snap;
}

std::string EndReport() {
    return BuildReport(Finish()).dump();
}

FrameRows End() {
    const Snapshot s = Finish();
    FrameRows out;
    for (const FrameRow& row : s.frame_rows) {
        u64 total = 0;
        for (const auto& [id, count] : row.calls) {
            total += count;
        }
        out.rows.emplace_back(row.frame, total);
    }
    out.dropped = s.frame_rows_dropped;
    out.closed = s.frames_closed;
    out.accepted_presents = s.accepted_presents;
    return out;
}

} // namespace testing

} // namespace Core::NvnCensus
