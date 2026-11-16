// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstring>
#include <span>
#include <tuple>

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

Scheduler::Scheduler(const CacheEnvelope::PrepareOptions& options_, WorkIdSource work_ids_)
    : options{options_}, work_ids{work_ids_}, window{(std::max)(MinWindow, options_.workers * 4)} {}

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
    ram_paused_ = paused_ = false;
    counters_ = {};
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

Scheduler::Task Scheduler::Next(u64 generation, u64 available_ram_bytes) {
    std::scoped_lock lock{mutex_};
    Task task;
    task.generation = generation;
    task.epoch = epoch_;
    if (!running_ || generation != generation_) {
        return task; // Done
    }
    const bool ram_low = RamPaused(available_ram_bytes);
    const bool cap_reached = u64{ready_} + building_ >= options.ready_cap;
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
    if (!ram_low && !cap_reached && !queued_.empty()) {
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
        Release(*entry);
        Reserve(*entry, ReadyBookkeepingBytes);
        entry->result = std::move(result);
        entry->state = State::Ready;
        ++ready_;
        ++counters_.prepared;
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
    bool known = false;
    EntryId building = NoEntry;
    std::vector<EntryId> queued;
    const auto [first, last] = identity_.equal_range(hash);
    for (auto it = first; it != last; ++it) {
        Entry& entry = entries_[it->second];
        if (!SameKey(entry.key, key)) {
            continue;
        }
        known = true;
        switch (entry.state) {
        case State::Ready:
            Release(entry);
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
        default:
            break;
        }
    }
    demand.generation = generation_;
    if (!demand.results.empty()) {
        demand.kind = DemandKind::Ready;
        Changed(); // a ready slot is free again
    } else if (building != NoEntry) {
        demand.kind = DemandKind::Building;
        demand.id = building;
        demand.work = entries_[building].work;
    } else if (!queued.empty()) {
        for (const EntryId id : queued) {
            Entry& entry = entries_[id];
            queued_.erase(entry.rank);
            Finish(entry, State::Claimed);
            ++counters_.demand_promoted;
        }
        demand.kind = DemandKind::Claimed;
        Changed();
    } else if (known) {
        demand.kind = DemandKind::Unavailable;
    } else {
        auto& seen = demanded_[hash];
        if (std::ranges::none_of(seen, [&](const std::vector<u8>& other) {
                return SameKey(other, key);
            })) {
            seen.emplace_back(key.begin(), key.end());
        }
    }
    return demand;
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
    stats.prepared = counters_.prepared;
    stats.adopted = counters_.adopted;
    stats.demand_promoted = counters_.demand_promoted;
    stats.prepare_failures = counters_.prepare_failures;
    stats.queue_high_water = counters_.queue_high_water;
    stats.memory_high_water_bytes = counters_.memory_high_water;
    stats.budget_pauses = counters_.budget_pauses;
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
    if (ReadLe32(blob.data()) < HeaderSize || ReadLe32(blob.data() + 4) != HeaderVersionOne) {
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

} // namespace VideoCommon::PipelinePrepare
