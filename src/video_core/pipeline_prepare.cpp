// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <tuple>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "video_core/pipeline_prepare.h"

namespace VideoCommon::PipelinePrepare {

using CacheEnvelope::RejectReason;

namespace {

constexpr u32 MinWindow = 4;

bool SameKey(const std::vector<u8>& stored, std::span<const u8> key) {
    return stored.size() == key.size() && std::equal(stored.begin(), stored.end(), key.begin());
}

u32 ReadLe32(const u8* data) {
    u32 value{};
    std::memcpy(&value, data, sizeof(value));
    return value;
}

void WriteLe32(u8* data, u32 value) {
    std::memcpy(data, &value, sizeof(value));
}

} // Anonymous namespace

Scheduler::Scheduler(const CacheEnvelope::PrepareOptions& options_, WorkIdSource work_ids_,
                     ClockSource clock_)
    : options{options_}, work_ids{work_ids_}, clock{clock_},
      window{(std::max)(MinWindow, options_.workers * 4)} {}

u64 Scheduler::Begin(std::span<const Candidate> candidates) {
    std::scoped_lock lock{mutex_};
    entries_.clear();
    entries_.resize(candidates.size());
    order_.resize(candidates.size());
    for (size_t i = 0; i < candidates.size(); ++i) {
        entries_[i].candidate = candidates[i];
        order_[i] = static_cast<EntryId>(i);
    }
    // Boot-phase records first, then in the order the first run reached them; among records
    // reached at the same time the one that cost most goes first.
    const auto cost = [&](EntryId id) {
        const auto& meta = entries_[id].candidate.meta;
        return u64{meta.translate_cost_us} + u64{meta.build_cost_us};
    };
    std::ranges::sort(order_, [&](EntryId a, EntryId b) {
        const auto& ma = entries_[a].candidate.meta;
        const auto& mb = entries_[b].candidate.meta;
        const u64 ca = cost(a);
        const u64 cb = cost(b);
        return std::tuple{static_cast<u8>(ma.first_use_phase), ma.first_use_ns, ma.first_use_ordinal,
                          ~ca, a} < std::tuple{static_cast<u8>(mb.first_use_phase), mb.first_use_ns,
                                               mb.first_use_ordinal, ~cb, b};
    });
    for (size_t rank = 0; rank < order_.size(); ++rank) {
        entries_[order_[rank]].rank = static_cast<u32>(rank);
    }
    cursor_ = 0;
    queued_.clear();
    identity_.clear();
    demanded_.clear();
    index_bytes_ = u64{candidates.size()} * sizeof(Entry);
    used_ = index_bytes_;
    loading_ = building_ = ready_ = 0;
    ready_seq_ = 0;
    ready_order_.clear();
    ram_paused_ = paused_ = false;
    cap_held_ = false;
    stall_since_ns_ = 0;
    late_.clear();
    counters_ = {};
    counters_.records = candidates.size();
    counters_.memory_high_water = used_;
    ++generation_;
    running_ = true;
    Changed();
    return generation_;
}

std::vector<std::shared_ptr<void>> Scheduler::Cancel() {
    std::vector<std::shared_ptr<void>> dropped;
    {
        std::scoped_lock lock{mutex_};
        for (Entry& entry : entries_) {
            switch (entry.state) {
            case State::Indexed:
            case State::Loading:
            case State::Queued:
            case State::Building:
            case State::Ready:
                if (entry.result) {
                    dropped.push_back(std::move(entry.result));
                }
                if (entry.payload) {
                    dropped.push_back(std::move(entry.payload));
                }
                entry.state = State::Cancelled;
                break;
            default:
                break;
            }
        }
        // Stale completions find the generation changed and are refused.
        ++generation_;
        running_ = false;
        queued_.clear();
        identity_.clear();
        demanded_.clear();
        used_ = 0;
        loading_ = building_ = ready_ = 0;
        ready_order_.clear();
        cursor_ = order_.size();
        Changed();
    }
    return dropped;
}

bool Scheduler::IsCurrent(EntryId id, u64 generation, State expected) const {
    return running_ && generation == generation_ && id < entries_.size() &&
           entries_[id].state == expected;
}

Scheduler::Entry* Scheduler::Find(EntryId id, u64 generation, State expected) {
    return IsCurrent(id, generation, expected) ? &entries_[id] : nullptr;
}

void Scheduler::Reserve(Entry& entry, u64 bytes) {
    entry.reserved += bytes;
    used_ += bytes;
    counters_.memory_high_water = (std::max)(counters_.memory_high_water, used_);
}

void Scheduler::Release(Entry& entry) {
    used_ -= (std::min)(used_, entry.reserved);
    entry.reserved = 0;
}

void Scheduler::Finish(Entry& entry, State state) {
    Release(entry);
    entry.payload.reset();
    entry.result.reset();
    entry.state = state;
}

void Scheduler::Changed() {
    ++epoch_;
    changed_.notify_all();
}

void Scheduler::SetPause(bool blocked) {
    if (blocked && !paused_) {
        ++counters_.budget_pauses;
    }
    paused_ = blocked;
}

void Scheduler::NoteInFlight() {
    counters_.queue_high_water =
        (std::max)(counters_.queue_high_water, u64{loading_} + queued_.size() + building_);
}

bool Scheduler::RamPaused(u64 available_ram_bytes) {
    // Pause below the minimum, resume only a quarter above it, so a value hovering at the limit
    // does not flip the pause on every call.
    const u64 resume = options.min_available_ram_bytes + options.min_available_ram_bytes / 4;
    if (ram_paused_) {
        ram_paused_ = available_ram_bytes < resume;
    } else {
        ram_paused_ = available_ram_bytes < options.min_available_ram_bytes;
    }
    return ram_paused_;
}

Scheduler::Task Scheduler::Next(u64 generation, u64 available_ram_bytes, bool hold_builds) {
    std::scoped_lock lock{mutex_};
    Task task;
    task.generation = generation;
    task.epoch = epoch_;
    if (!running_ || generation != generation_) {
        return task; // Done
    }
    const bool ram_low = RamPaused(available_ram_bytes);
    bool cap_reached = u64{ready_} + building_ >= options.ready_cap;
    if (!cap_reached) {
        cap_held_ = false;
        stall_since_ns_ = 0;
    }
    bool blocked = ram_low;
    // Decode ahead first, up to the window: a decoded record is known by its key, so a draw that
    // needs it can claim it instead of building it a second time.
    while (!ram_low && cursor_ < order_.size() && loading_ + queued_.size() < window) {
        const EntryId id = order_[cursor_];
        Entry& entry = entries_[id];
        const u64 cost = RecordCost(entry.candidate.length);
        const u64 room = options.memory_budget_bytes > index_bytes_
                             ? options.memory_budget_bytes - index_bytes_
                             : 0;
        if (cost > room) {
            // Over the whole budget on its own, whatever else is released: it is compiled
            // normally when drawn, and must not hold the queue for ever.
            entry.state = State::Skipped;
            ++cursor_;
            continue;
        }
        if (used_ + cost > options.memory_budget_bytes) {
            blocked = true;
            break;
        }
        ++cursor_;
        entry.state = State::Loading;
        entry.work = work_ids != nullptr ? work_ids() : 0;
        Reserve(entry, cost);
        ++loading_;
        SetPause(false);
        task.kind = TaskKind::Load;
        task.id = id;
        task.work = entry.work;
        NoteInFlight();
        return task;
    }
    if (!ram_low && !hold_builds && cap_reached && ready_ != 0 && !queued_.empty()) {
        // The ready objects are the ones the game reaches first (records are prepared in the
        // order it reached them), so they are kept and the builds behind them wait until a draw
        // adopts one. Draws that go on without adopting any (they match no record, or take a
        // record still queued) for stalled_after_ns show the objects at the cap are not the
        // ones this run needs: they would hold the rest back for ever, so the one that became
        // ready first is released (built again if its draw ever comes).
        const u64 now = Now();
        if (stall_since_ns_ != 0 && now - stall_since_ns_ >= options.stalled_after_ns) {
            const auto oldest = ready_order_.begin();
            Entry& victim = entries_[oldest->second];
            ready_order_.erase(oldest);
            Release(victim);
            task.evicted = std::move(victim.result);
            victim.state = State::Evicted;
            --ready_;
            ++counters_.evicted;
            cap_reached = u64{ready_} + building_ >= options.ready_cap;
            if (!cap_reached) {
                cap_held_ = false;
            }
        } else if (!cap_held_) {
            cap_held_ = true;
            ++counters_.held_back;
        }
    }
    if (!ram_low && !hold_builds && !cap_reached && !queued_.empty()) {
        const EntryId id = order_[*queued_.begin()];
        queued_.erase(queued_.begin());
        Entry& entry = entries_[id];
        entry.state = State::Building;
        entry.work = work_ids != nullptr ? work_ids() : 0;
        ++building_;
        SetPause(blocked);
        task.kind = TaskKind::Build;
        task.id = id;
        task.work = entry.work;
        task.payload = std::move(entry.payload);
        NoteInFlight();
        return task;
    }
    const bool work_left = cursor_ < order_.size() || loading_ != 0 || !queued_.empty() ||
                           building_ != 0;
    if (!work_left) {
        SetPause(false);
        return task; // Done
    }
    // Something is left but may not be taken: the RAM, the budget or the ready cap (a full
    // window is only the pipeline being full). One pause is counted per run of such calls.
    SetPause(blocked || (cap_reached && !queued_.empty()));
    task.kind = TaskKind::Wait;
    return task;
}

void Scheduler::WaitForChange(u64 epoch, std::chrono::milliseconds timeout) {
    std::unique_lock lock{mutex_};
    changed_.wait_for(lock, timeout, [&] { return epoch_ != epoch; });
}

Scheduler::LoadOutcome Scheduler::Loaded(EntryId id, u64 generation, u64 hash,
                                         std::span<const u8> key, std::shared_ptr<void> payload,
                                         bool variants_allowed) {
    std::scoped_lock lock{mutex_};
    Entry* const entry = Find(id, generation, State::Loading);
    if (entry == nullptr) {
        return LoadOutcome::Stale;
    }
    --loading_;
    bool duplicate = false;
    if (!variants_allowed) {
        const auto [first, last] = identity_.equal_range(hash);
        for (auto it = first; it != last && !duplicate; ++it) {
            const Entry& other = entries_[it->second];
            duplicate = SameKey(other.key, key) &&
                        (other.state == State::Queued || other.state == State::Building ||
                         other.state == State::Ready || other.state == State::Adopted ||
                         other.state == State::Claimed);
        }
        if (const auto found = demanded_.find(hash); found != demanded_.end() && !duplicate) {
            duplicate = std::ranges::any_of(
                found->second, [&](const std::vector<u8>& seen) { return SameKey(seen, key); });
        }
    }
    if (duplicate) {
        Finish(*entry, State::Skipped);
        Changed();
        return LoadOutcome::Duplicate;
    }
    entry->hash = hash;
    entry->key.assign(key.begin(), key.end());
    entry->payload = std::move(payload);
    entry->state = State::Queued;
    identity_.emplace(hash, id);
    queued_.insert(entry->rank);
    NoteInFlight();
    Changed();
    return LoadOutcome::Queued;
}

bool Scheduler::LoadSkipped(EntryId id, u64 generation) {
    std::scoped_lock lock{mutex_};
    Entry* const entry = Find(id, generation, State::Loading);
    if (entry == nullptr) {
        return false;
    }
    --loading_;
    Finish(*entry, State::Skipped);
    Changed();
    return true;
}

bool Scheduler::LoadFailed(EntryId id, u64 generation) {
    std::scoped_lock lock{mutex_};
    Entry* const entry = Find(id, generation, State::Loading);
    if (entry == nullptr) {
        return false;
    }
    --loading_;
    ++counters_.prepare_failures;
    Finish(*entry, State::Failed);
    Changed();
    return true;
}

bool Scheduler::BuildFinished(EntryId id, u64 generation, bool ok,
                              std::shared_ptr<void> result) {
    std::scoped_lock lock{mutex_};
    Entry* const entry = Find(id, generation, State::Building);
    if (entry == nullptr) {
        return false;
    }
    --building_;
    if (ok) {
        const u64 now = Now();
        Release(*entry);
        Reserve(*entry, ReadyBookkeepingBytes);
        entry->result = std::move(result);
        entry->state = State::Ready;
        entry->ready_seq = ++ready_seq_;
        entry->ready_ns = now;
        ready_order_.emplace(entry->ready_seq, id);
        ++ready_;
        ++counters_.prepared;
        if (entry->demanded_ns != 0) {
            // A draw asked for it while it was building and is waiting for this.
            const u64 waited = now > entry->demanded_ns ? now - entry->demanded_ns : 0;
            counters_.late_wait_ns += waited;
            counters_.late_wait_max_ns = (std::max)(counters_.late_wait_max_ns, waited);
            if (entry->late_slot != NoLate) {
                late_[entry->late_slot].ready_ns = now;
            }
        }
    } else {
        ++counters_.prepare_failures;
        Finish(*entry, State::Failed);
    }
    Changed();
    return true;
}

Scheduler::Demand Scheduler::Request(u64 hash, std::span<const u8> key) {
    Demand demand;
    std::scoped_lock lock{mutex_};
    if (!running_) {
        return demand;
    }
    const u64 now = Now();
    bool known = false;
    EntryId building = NoEntry;
    std::vector<EntryId> queued;
    std::vector<EntryId> released;
    const auto [first, last] = identity_.equal_range(hash);
    for (auto it = first; it != last; ++it) {
        Entry& entry = entries_[it->second];
        if (!SameKey(entry.key, key)) {
            continue;
        }
        known = true;
        switch (entry.state) {
        case State::Ready:
            if (entry.demanded_ns == 0 && now > entry.ready_ns) {
                counters_.adopted_lead_ns += now - entry.ready_ns;
            }
            Release(entry);
            ready_order_.erase(entry.ready_seq);
            demand.results.push_back(std::move(entry.result));
            entry.state = State::Adopted;
            --ready_;
            ++counters_.adopted;
            break;
        case State::Building:
            building = it->second;
            break;
        case State::Queued:
            queued.push_back(it->second);
            break;
        case State::Evicted:
            released.push_back(it->second);
            break;
        default:
            break;
        }
    }
    // The stall clock runs from the first draw that does not adopt while ready objects hold the
    // cap, and stops at the next adoption: what a draw finds queued or unknown frees no ready
    // slot, so it is no sign the objects at the cap are still wanted.
    if (!demand.results.empty()) {
        stall_since_ns_ = 0;
    } else if (stall_since_ns_ == 0 && ready_ != 0 &&
               u64{ready_} + building_ >= options.ready_cap) {
        stall_since_ns_ = now;
    }
    demand.generation = generation_;
    if (!demand.results.empty()) {
        demand.kind = DemandKind::Ready;
        Changed(); // a ready slot is free again
    } else if (building != NoEntry) {
        demand.kind = DemandKind::Building;
        demand.id = building;
        demand.work = entries_[building].work;
        if (entries_[building].demanded_ns == 0) {
            ++counters_.demand_building;
            NoteLate(entries_[building], LateOutcome::Building, now);
        }
    } else if (!queued.empty()) {
        for (const EntryId id : queued) {
            Entry& entry = entries_[id];
            queued_.erase(entry.rank);
            NoteLate(entry, LateOutcome::Claimed, now);
            Finish(entry, State::Claimed);
            ++counters_.demand_promoted;
        }
        demand.kind = DemandKind::Claimed;
        Changed();
    } else if (known) {
        demand.kind = DemandKind::Unavailable;
        for (const EntryId id : released) {
            if (entries_[id].demanded_ns == 0) {
                ++counters_.demand_evicted;
                NoteLate(entries_[id], LateOutcome::Released, now);
            }
        }
    } else {
        ++counters_.demand_unknown;
        auto& seen = demanded_[hash];
        if (std::ranges::none_of(seen, [&](const std::vector<u8>& other) {
                return SameKey(other, key);
            })) {
            seen.emplace_back(key.begin(), key.end());
        }
    }
    return demand;
}

u64 Scheduler::Now() const {
    if (clock != nullptr) {
        return clock();
    }
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
}

void Scheduler::NoteLate(Entry& entry, LateOutcome outcome, u64 now) {
    // Clock values start at 0 only in tests: a first draw at 0 would not be told from none.
    entry.demanded_ns = (std::max)(now, u64{1});
    ++counters_.late_records;
    if (late_.size() < MaxLateRecords) {
        entry.late_slot = late_.size();
        late_.push_back(LateRecord{.rank = entry.rank,
                                   .recorded_phase = entry.candidate.meta.first_use_phase,
                                   .recorded_first_use_ns = entry.candidate.meta.first_use_ns,
                                   .outcome = outcome,
                                   .demanded_ns = now,
                                   .ready_ns = outcome == LateOutcome::Released ? entry.ready_ns
                                                                                : u64{0}});
    }
}

std::vector<Scheduler::LateRecord> Scheduler::LateRecords() const {
    std::scoped_lock lock{mutex_};
    return late_;
}

void Scheduler::Await(EntryId id, u64 generation) {
    std::unique_lock lock{mutex_};
    changed_.wait(lock, [&] {
        return !running_ || generation != generation_ || id >= entries_.size() ||
               entries_[id].state != State::Building;
    });
}

State Scheduler::StateOf(EntryId id) const {
    std::scoped_lock lock{mutex_};
    return id < entries_.size() ? entries_[id].state : State::Cancelled;
}

size_t Scheduler::InFlight() const {
    std::scoped_lock lock{mutex_};
    return u64{loading_} + queued_.size() + building_;
}

size_t Scheduler::ReadyCount() const {
    std::scoped_lock lock{mutex_};
    return ready_;
}

u64 Scheduler::UsedBytes() const {
    std::scoped_lock lock{mutex_};
    return used_;
}

bool Scheduler::Running() const {
    std::scoped_lock lock{mutex_};
    return running_;
}

u64 Scheduler::Generation() const {
    std::scoped_lock lock{mutex_};
    return generation_;
}

void Scheduler::FillStats(CacheEnvelope::CacheStats& stats) const {
    std::scoped_lock lock{mutex_};
    stats.records = counters_.records;
    stats.prepared = counters_.prepared;
    stats.adopted = counters_.adopted;
    stats.demand_promoted = counters_.demand_promoted;
    stats.demand_building = counters_.demand_building;
    stats.demand_evicted = counters_.demand_evicted;
    stats.demand_unknown = counters_.demand_unknown;
    stats.prepare_failures = counters_.prepare_failures;
    stats.queue_high_water = counters_.queue_high_water;
    stats.memory_high_water_bytes = counters_.memory_high_water;
    stats.budget_pauses = counters_.budget_pauses;
    stats.evicted = counters_.evicted;
    stats.held_back = counters_.held_back;
    stats.adopted_lead_ns = counters_.adopted_lead_ns;
    stats.late_records = counters_.late_records;
    stats.late_wait_ns = counters_.late_wait_ns;
    stats.late_wait_max_ns = counters_.late_wait_max_ns;
}

bool ForEachDriverBlobChunk(std::span<const u8> blob,
                            const std::function<bool(std::span<const u8>)>& append) {
    const size_t count = (std::max)(size_t{1}, (blob.size() + DriverBlobChunkBytes - 1) /
                                                   DriverBlobChunkBytes);
    std::vector<u8> chunk;
    for (size_t i = 0; i < count; ++i) {
        const size_t begin = i * DriverBlobChunkBytes;
        const size_t size = (std::min)(DriverBlobChunkBytes, blob.size() - begin);
        chunk.resize(DriverBlobChunkHeaderBytes + size);
        WriteLe32(chunk.data(), static_cast<u32>(i));
        WriteLe32(chunk.data() + 4, static_cast<u32>(count));
        if (size != 0) {
            std::memcpy(chunk.data() + DriverBlobChunkHeaderBytes, blob.data() + begin, size);
        }
        if (!append(chunk)) {
            return false;
        }
    }
    return true;
}

RejectReason ValidateDriverBlobHeader(std::span<const u8> blob,
                                      const CacheEnvelope::DeviceIdentity& device) {
    // VkPipelineCacheHeaderVersionOne: headerSize, headerVersion, vendorID, deviceID, UUID.
    constexpr size_t HeaderSize = 32;
    constexpr u32 HeaderVersionOne = 1;
    if (blob.size() < HeaderSize) {
        return RejectReason::Truncated;
    }
    // The header lies inside the blob: a declared size past its end is not a header of this blob.
    const u32 declared_size = ReadLe32(blob.data());
    if (declared_size < HeaderSize || declared_size > blob.size() ||
        ReadLe32(blob.data() + 4) != HeaderVersionOne) {
        return RejectReason::BadRecord;
    }
    if (ReadLe32(blob.data() + 8) != device.vendor_id ||
        ReadLe32(blob.data() + 12) != device.device_id) {
        return RejectReason::Device;
    }
    if (!std::equal(device.pipeline_cache_uuid.begin(), device.pipeline_cache_uuid.end(),
                    blob.begin() + 16)) {
        return RejectReason::PipelineUuid;
    }
    return RejectReason::None;
}

RejectReason JoinDriverBlob(
    size_t record_count,
    const std::function<RejectReason(size_t, std::vector<u8>&)>& read_record,
    const CacheEnvelope::DeviceIdentity& device, std::vector<u8>& blob) {
    blob.clear();
    const auto join = [&]() {
        if (record_count == 0) {
            return RejectReason::BadRecord;
        }
        std::vector<u8> chunk;
        for (size_t i = 0; i < record_count; ++i) {
            if (const RejectReason reason = read_record(i, chunk); reason != RejectReason::None) {
                return reason;
            }
            if (chunk.size() < DriverBlobChunkHeaderBytes || ReadLe32(chunk.data()) != i ||
                ReadLe32(chunk.data() + 4) != record_count) {
                return RejectReason::BadRecord;
            }
            const size_t size = chunk.size() - DriverBlobChunkHeaderBytes;
            if (blob.size() + size > CacheEnvelope::MaxDriverBlobBytes) {
                return RejectReason::BadRecord;
            }
            blob.insert(blob.end(), chunk.begin() + DriverBlobChunkHeaderBytes, chunk.end());
        }
        return ValidateDriverBlobHeader(blob, device);
    };
    const RejectReason reason = join();
    if (reason != RejectReason::None) {
        blob.clear();
    }
    return reason;
}

u64 AvailablePhysicalMemoryBytes() {
#ifdef _WIN32
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status)) {
        return static_cast<u64>(status.ullAvailPhys);
    }
#elif defined(__linux__)
    if (FILE* const file = std::fopen("/proc/meminfo", "r")) {
        char line[256];
        u64 kilobytes = 0;
        bool found = false;
        while (std::fgets(line, sizeof(line), file) != nullptr) {
            if (std::strncmp(line, "MemAvailable:", 13) == 0) {
                kilobytes = std::strtoull(line + 13, nullptr, 10);
                found = true;
                break;
            }
        }
        std::fclose(file);
        if (found) {
            return kilobytes * 1024;
        }
    }
#endif
    return ~u64{0}; // Unknown: never pause for memory
}

std::vector<u32> RemapRecords(std::span<const CacheEnvelope::IndexEntry> before,
                              std::span<const CacheEnvelope::IndexEntry> after) {
    using Identity = std::tuple<u32, u32, u64>; // kind, length, checksum: the writer's identity
    std::map<Identity, u32> positions;
    for (size_t i = 0; i < after.size(); ++i) {
        positions.try_emplace(
            Identity{static_cast<u32>(after[i].meta.kind), after[i].length, after[i].checksum},
            static_cast<u32>(i));
    }
    std::vector<u32> mapped(before.size(), NoRecord);
    for (size_t i = 0; i < before.size(); ++i) {
        const auto found = positions.find(Identity{static_cast<u32>(before[i].meta.kind),
                                                   before[i].length, before[i].checksum});
        if (found != positions.end()) {
            mapped[i] = found->second;
        }
    }
    return mapped;
}

void CommittedSizes::Publish() noexcept {
    u64 sum = 0;
    for (const u64 size : bytes) {
        sum += size;
    }
    total.store(sum, std::memory_order_relaxed);
}

std::unique_ptr<CacheEnvelope::EnvelopeReader> OpenCommittedFile(
    CommittedSizes& sizes, const std::filesystem::path& path,
    const CacheEnvelope::CacheIdentity& expected, CacheEnvelope::CacheLoadResult& load) {
    CacheEnvelope::RemoveStaleTemp(path);
    auto reader = CacheEnvelope::EnvelopeReader::Open(path, expected, load);
    u64 size = 0;
    if (reader) {
        std::error_code error;
        const auto on_disk = std::filesystem::file_size(path, error);
        size = error ? u64{0} : static_cast<u64>(on_disk);
    }
    sizes.bytes.push_back(size);
    sizes.Publish();
    return reader;
}

bool CommitGeneration(CommittedSizes& sizes, size_t index, CacheEnvelope::EnvelopeWriter& writer) {
    const u64 generation_bytes = writer.Bytes();
    const bool ok = writer.Commit();
    if (ok && index < sizes.bytes.size()) {
        sizes.bytes[index] = generation_bytes;
    }
    sizes.Publish();
    return ok;
}

} // namespace VideoCommon::PipelinePrepare
