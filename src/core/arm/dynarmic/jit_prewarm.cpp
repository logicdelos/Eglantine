// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <stop_token>
#include <system_error>
#include <thread>
#include <unordered_set>

#include "common/env_gate.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "core/arm/dynarmic/jit_prewarm.h"
#include "core/hardware_properties.h"
#include "dynarmic/frontend/A64/a64_location_descriptor.h"
#include "dynarmic/interface/A64/a64.h"
#include "video_core/cache_envelope.h"

namespace Core::JitPrewarm {

namespace {

constexpr char Magic[8] = {'E', 'G', 'L', 'J', 'I', 'T', 'H', '1'};
constexpr std::size_t MagicBytes = sizeof(Magic);
constexpr std::size_t ChecksummedHeaderBytes = HeaderBytes - sizeof(u64);
constexpr std::size_t EntryBytes = 2 * sizeof(u64) + sizeof(u32); // wire size, not host stride

constexpr const char* FileName = "jit_blocks.mythos.bin";
constexpr std::chrono::seconds IdentityWait{60};
constexpr std::chrono::seconds RecordWindow{180};
constexpr std::chrono::seconds CheckpointEvery{15};

u64 Fnv1a(std::span<const u8> bytes) noexcept {
    u64 hash = 0xcbf29ce484222325ULL;
    for (const u8 byte : bytes) {
        hash ^= byte;
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

void Store32(u8* at, u32 value) noexcept {
    for (std::size_t i = 0; i < sizeof(u32); ++i) {
        at[i] = static_cast<u8>(value >> (8 * i));
    }
}

void Store64(u8* at, u64 value) noexcept {
    for (std::size_t i = 0; i < sizeof(u64); ++i) {
        at[i] = static_cast<u8>(value >> (8 * i));
    }
}

u32 Load32(const u8* at) noexcept {
    u32 value = 0;
    for (std::size_t i = sizeof(u32); i-- > 0;) {
        value = (value << 8) | at[i];
    }
    return value;
}

u64 Load64(const u8* at) noexcept {
    u64 value = 0;
    for (std::size_t i = sizeof(u64); i-- > 0;) {
        value = (value << 8) | at[i];
    }
    return value;
}

struct EntryHash {
    std::size_t operator()(const Entry& entry) const noexcept {
        return static_cast<std::size_t>((entry.location * 0x9e3779b97f4a7c15ULL) ^
                                        (entry.guest_hash + 0x7f4a7c15ULL));
    }
};

std::optional<u64> RebaseLocation(u64 location, u32 word_count, u64 from, u64 to, u64 size) noexcept {
    using Descriptor = Dynarmic::A64::LocationDescriptor;
    constexpr u64 AddressLimit = u64{1} << (Descriptor::pc_bit_count - 1);
    const u64 base = (std::max)(from, to);
    const Descriptor descriptor{Dynarmic::IR::LocationDescriptor{location}};
    if (base >= AddressLimit || size > AddressLimit - base || base % sizeof(u32) != 0 ||
        word_count == 0 || word_count > Dynarmic::A64::MaxPrecompileWords ||
        descriptor.SingleStepping() || descriptor.UniqueHash() != location) {
        return std::nullopt;
    }
    const u64 pc = descriptor.PC();
    if (pc < from || pc % sizeof(u32) != 0) {
        return std::nullopt;
    }
    const u64 offset = pc - from;
    if (offset >= size || u64{word_count} * sizeof(u32) > size - offset) {
        return std::nullopt;
    }
    return descriptor.SetPC(to + offset).UniqueHash();
}

} // namespace

std::optional<u64> CodeImage::ToRelative(u64 location, u32 word_count) const noexcept {
    return RebaseLocation(location, word_count, base, 0, size);
}

std::optional<u64> CodeImage::ToAbsolute(u64 location, u32 word_count) const noexcept {
    return RebaseLocation(location, word_count, 0, base, size);
}

const char* RejectName(Reject reject) noexcept {
    switch (reject) {
    case Reject::None:
        return "none";
    case Reject::TooShort:
        return "too_short";
    case Reject::BadMagic:
        return "bad_magic";
    case Reject::BadVersion:
        return "bad_version";
    case Reject::HeaderChecksum:
        return "header_checksum";
    case Reject::Build:
        return "build";
    case Reject::Content:
        return "content";
    case Reject::Title:
        return "title";
    case Reject::GameVersion:
        return "game_version";
    case Reject::CoreCount:
        return "core_count";
    case Reject::BodySize:
        return "body_size";
    case Reject::BodyChecksum:
        return "body_checksum";
    case Reject::TooManyEntries:
        return "too_many_entries";
    case Reject::InvalidEntry:
        return "invalid_entry";
    }
    return "unknown";
}

std::vector<u8> Serialize(Identity identity, std::span<const std::vector<Entry>> per_core) {
    identity.core_count = static_cast<u32>(per_core.size());
    std::size_t body_bytes = 0;
    u64 total = 0;
    for (const auto& list : per_core) {
        body_bytes += sizeof(u64) + list.size() * EntryBytes;
        total += list.size();
    }
    std::vector<u8> out(HeaderBytes + body_bytes);
    u8* cursor = out.data() + HeaderBytes;
    for (const auto& list : per_core) {
        Store64(cursor, list.size());
        cursor += sizeof(u64);
        for (const Entry& entry : list) {
            Store64(cursor, entry.location);
            Store64(cursor + sizeof(u64), entry.guest_hash);
            Store32(cursor + 2 * sizeof(u64), entry.word_count);
            cursor += EntryBytes;
        }
    }
    u8* const header = out.data();
    std::memcpy(header, Magic, MagicBytes);
    Store32(header + 8, FormatVersion);
    Store32(header + 12, identity.core_count);
    Store64(header + 16, identity.build_id);
    Store64(header + 24, identity.content_stamp);
    Store64(header + 32, identity.title_id);
    Store32(header + 40, identity.game_version);
    Store32(header + 44, 0);
    Store64(header + 48, body_bytes);
    Store64(header + 56, Fnv1a(std::span<const u8>{out}.subspan(HeaderBytes)));
    Store64(header + 64, total);
    Store64(header + ChecksummedHeaderBytes, Fnv1a(std::span<const u8>{header, ChecksummedHeaderBytes}));
    return out;
}

Parsed Parse(std::span<const u8> bytes, const Identity& expected) {
    const auto reject = [](Reject why) { return Parsed{why, {}}; };
    if (bytes.size() < HeaderBytes) {
        return reject(Reject::TooShort);
    }
    const u8* const header = bytes.data();
    if (std::memcmp(header, Magic, MagicBytes) != 0) {
        return reject(Reject::BadMagic);
    }
    if (Load32(header + 8) != FormatVersion) {
        return reject(Reject::BadVersion);
    }
    if (Fnv1a(bytes.first(ChecksummedHeaderBytes)) != Load64(header + ChecksummedHeaderBytes)) {
        return reject(Reject::HeaderChecksum);
    }
    if (Load64(header + 16) != expected.build_id) {
        return reject(Reject::Build);
    }
    if (Load64(header + 24) != expected.content_stamp) {
        return reject(Reject::Content);
    }
    if (Load64(header + 32) != expected.title_id) {
        return reject(Reject::Title);
    }
    if (Load32(header + 40) != expected.game_version) {
        return reject(Reject::GameVersion);
    }
    const u32 core_count = Load32(header + 12);
    if (core_count == 0 || core_count > MaxCores || core_count != expected.core_count) {
        return reject(Reject::CoreCount);
    }
    const std::span<const u8> body = bytes.subspan(HeaderBytes);
    if (Load64(header + 48) != body.size()) {
        return reject(Reject::BodySize);
    }
    if (Fnv1a(body) != Load64(header + 56)) {
        return reject(Reject::BodyChecksum);
    }
    Parsed parsed;
    parsed.per_core.resize(core_count);
    std::size_t offset = 0;
    u64 total = 0;
    for (auto& list : parsed.per_core) {
        if (body.size() - offset < sizeof(u64)) {
            return reject(Reject::BodySize);
        }
        const u64 count = Load64(body.data() + offset);
        offset += sizeof(u64);
        if (count > MaxEntriesPerCore) {
            return reject(Reject::TooManyEntries);
        }
        if (count > (body.size() - offset) / EntryBytes) {
            return reject(Reject::BodySize);
        }
        list.resize(static_cast<std::size_t>(count));
        for (Entry& entry : list) {
            entry.location = Load64(body.data() + offset);
            entry.guest_hash = Load64(body.data() + offset + sizeof(u64));
            entry.word_count = Load32(body.data() + offset + 2 * sizeof(u64));
            if (entry.word_count == 0 || entry.word_count > Dynarmic::A64::MaxPrecompileWords) {
                return reject(Reject::InvalidEntry);
            }
            offset += EntryBytes;
        }
        total += count;
    }
    if (offset != body.size() || total != Load64(header + 64)) {
        return reject(Reject::BodySize);
    }
    return parsed;
}

std::vector<Entry> MergeForWrite(std::span<const Entry> loaded, std::span<const u32> stale,
                                 std::span<const Entry> recorded, std::size_t cap) {
    std::vector<Entry> out;
    out.reserve((std::min)(cap, loaded.size() + recorded.size()));
    std::unordered_set<Entry, EntryHash> seen;
    seen.reserve(out.capacity());
    const auto add = [&](const Entry& entry) {
        if (out.size() < cap && seen.insert(entry).second) {
            out.push_back(entry);
        }
    };
    for (std::size_t i = 0; i < loaded.size(); ++i) {
        if (!std::binary_search(stale.begin(), stale.end(), static_cast<u32>(i))) {
            add(loaded[i]);
        }
    }
    for (const Entry& entry : recorded) {
        add(entry);
    }
    return out;
}

std::optional<std::vector<u8>> ReadFileBytes(const std::filesystem::path& path,
                                             std::size_t max_bytes) {
    std::ifstream file{path, std::ios::binary | std::ios::ate};
    if (!file) {
        return std::nullopt;
    }
    const std::streamoff size = file.tellg();
    if (size < 0 || static_cast<u64>(size) > max_bytes) {
        return std::nullopt;
    }
    std::vector<u8> bytes(static_cast<std::size_t>(size));
    file.seekg(0);
    if (!bytes.empty() &&
        !file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
        return std::nullopt;
    }
    return bytes;
}

bool WriteFileAtomic(const std::filesystem::path& path, std::span<const u8> bytes) {
    std::filesystem::path temporary = path;
    temporary += ".tmp";
    {
        std::ofstream file{temporary, std::ios::binary | std::ios::trunc};
        if (!file) {
            return false;
        }
        file.write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
        file.flush();
        if (!file) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            return false;
        }
    }
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        return false;
    }
    return true;
}

// --- Cursor ---

Cursor::Cursor(u32 count, Policy policy) : policy_{policy}, count_{count} {
    policy_.max_sweeps = (std::max)(policy_.max_sweeps, u32{1});
}

Cursor::Step Cursor::Next(u64 now_ms) {
    for (;;) {
        if (finished_) {
            return {Kind::Finished, 0, 0};
        }
        const u32 size = pass_ == 0 ? count_ : static_cast<u32>(current_.size());
        if (pos_ < size) {
            return {Kind::Translate, pass_ == 0 ? pos_ : current_[pos_], 0};
        }
        // The pass is over.
        if (retry_.empty()) {
            finished_ = true;
            continue;
        }
        if (pass_ + 1 >= policy_.max_sweeps) {
            stale_ = std::move(retry_);
            retry_.clear();
            finished_ = true;
            continue;
        }
        if (!gap_open_) {
            gap_open_ = true;
            gap_until_ms_ = now_ms + policy_.retry_gap_ms;
        }
        if (now_ms < gap_until_ms_) {
            return {Kind::Wait, 0, gap_until_ms_};
        }
        gap_open_ = false;
        current_ = std::move(retry_);
        retry_.clear();
        ++pass_;
        pos_ = 0;
    }
}

void Cursor::Report(u32 index, Outcome outcome) {
    switch (outcome) {
    case Outcome::Compiled:
        ++compiled_;
        ++pos_;
        break;
    case Outcome::Present:
        ++present_;
        ++pos_;
        break;
    case Outcome::Mismatch:
        retry_.push_back(index);
        ++mismatch_attempts_;
        ++pos_;
        break;
    case Outcome::NoSpace:
    case Outcome::Unsupported:
        stopped_by_ = outcome;
        finished_ = true;
        break;
    case Outcome::Busy:
        break;
    }
}

OutcomeCounts Cursor::WarmOutcomes() const noexcept {
    // On the final sweep an interrupt can stop us before Next() moves retry_ into stale_.
    const auto final_pass_mismatches = pass_ + 1 == policy_.max_sweeps ? retry_.size() : 0;
    const u32 rejected = static_cast<u32>(stale_.size() + final_pass_mismatches);
    const u32 no_space = stopped_by_ == Outcome::NoSpace;
    const u32 unsupported = stopped_by_ == Outcome::Unsupported;
    return {.loaded = count_,
            .compiled = compiled_,
            .already_present = present_,
            .rejected_mismatch = rejected,
            .no_space = no_space,
            .unsupported = unsupported,
            .pending = count_ - compiled_ - present_ - rejected - no_space - unsupported,
            .mismatch_attempts = mismatch_attempts_};
}

// --- CoreState ---

CoreState::CoreState(Policy policy, std::size_t record_capacity)
    : policy_{policy}, recorded_{std::make_unique_for_overwrite<Entry[]>(record_capacity)},
      record_capacity_{record_capacity} {}

void CoreState::Publish(std::optional<std::vector<Entry>> loaded) {
    if (loaded) {
        loaded_ = std::move(*loaded);
        state_.store(HasList, std::memory_order_release);
    } else {
        state_.store(NoList, std::memory_order_release);
    }
}

bool CoreState::GetStale(std::vector<u32>& out) const {
    if (!finished_.load(std::memory_order_acquire)) {
        return false;
    }
    out = stale_;
    return true;
}

std::optional<OutcomeCounts> CoreState::WarmOutcomes() const noexcept {
    if (state_.load(std::memory_order_acquire) == Pending) {
        return std::nullopt;
    }
    if (cursor_) {
        return cursor_->WarmOutcomes();
    }
    const auto loaded = static_cast<u32>(loaded_.size());
    return OutcomeCounts{.loaded = loaded, .pending = loaded};
}

void CoreState::LogWarmOutcomes(std::size_t core_index) const {
    if (const auto counts = WarmOutcomes()) {
        LOG_INFO(Core_ARM, "JIT prewarm outcomes: core={} loaded={} compiled={} already_present={} "
                 "rejected_mismatch={} no_space={} unsupported={} pending={} mismatch_attempts={}",
                 core_index, counts->loaded, counts->compiled, counts->already_present,
                 counts->rejected_mismatch, counts->no_space, counts->unsupported, counts->pending,
                 counts->mismatch_attempts);
    } else {
        LOG_INFO(Core_ARM, "JIT prewarm outcomes: core={} list=not_published", core_index);
    }
}

void CoreState::Record(u64 location, u64 guest_hash, u32 word_count) noexcept {
    const std::size_t count = recorded_count_.load(std::memory_order_relaxed);
    if (count >= record_capacity_ || closed_.load(std::memory_order_relaxed)) {
        return;
    }
    recorded_[count] = Entry{location, guest_hash, word_count};
    recorded_count_.store(count + 1, std::memory_order_release);
}

void CoreState::Finish() {
    done_ = true;
    const std::span<const u32> stale = cursor_->Stale();
    stale_.assign(stale.begin(), stale.end());
    finished_.store(true, std::memory_order_release);
}

// --- Session ---

namespace {

using Clock = std::chrono::steady_clock;

/// Sleeps until `until` or until a stop is requested; false when it was a stop.
bool SleepUntil(const std::stop_token& stop, Clock::time_point until) {
    std::mutex mutex;
    std::condition_variable_any wake;
    std::unique_lock lock{mutex};
    wake.wait_until(lock, stop, until, [] { return false; });
    return !stop.stop_requested();
}

} // namespace

/// One launch of a program: the per-core states, and the helper thread that waits for the program's
/// identity, loads the list, and writes it back while the run is young.
class Session {
public:
    Session(u64 program_id, const void* process_key)
        : program_id_{program_id}, process_key_{process_key} {
        cores_.reserve(Core::Hardware::NUM_CPU_CORES);
        for (std::size_t i = 0; i < Core::Hardware::NUM_CPU_CORES; ++i) {
            cores_.push_back(std::make_unique<CoreState>());
        }
        helper_ = std::jthread{[this](std::stop_token stop) { HelperMain(stop); }};
    }

    ~Session() {
        helper_.request_stop();
        helper_.join();
        // A run that ends inside the recording window leaves the list as it stands.
        if (have_identity_ && !final_written_) {
            WriteCurrent();
        }
    }

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    [[nodiscard]] bool Matches(const void* process_key) const noexcept {
        return process_key_ == process_key;
    }

    [[nodiscard]] CoreState& CoreAt(std::size_t index) {
        return *cores_[index];
    }

private:
    void HelperMain(const std::stop_token& stop) {
        const auto start = Clock::now();
        const auto give_up = [this] {
            for (auto& core : cores_) {
                core->Publish(std::nullopt);
                core->CloseRecording();
            }
        };
        // The identity is published by the frontend after the guest has started.
        for (;;) {
            const VideoCommon::CacheEnvelope::HostIdentity host =
                VideoCommon::CacheEnvelope::GetHostIdentity();
            if (host.build_id != 0 && host.content_stamp != 0 && host.title_id == program_id_) {
                identity_ = Identity{host.build_id, host.content_stamp, host.title_id,
                                     host.game_version,
                                     static_cast<u32>(cores_.size())};
                break;
            }
            if (Clock::now() - start > IdentityWait ||
                !SleepUntil(stop, Clock::now() + std::chrono::milliseconds{20})) {
                give_up();
                return;
            }
        }
        std::error_code error;
        path_ = Common::FS::GetVulkanPipelineCacheDir(
                    Common::FS::GetEdenPath(Common::FS::EdenPath::ShaderDir), identity_.title_id) /
                FileName;
        std::filesystem::create_directories(path_.parent_path(), error);
        if (error) {
            LOG_WARNING(Core_ARM, "JIT prewarm off: the cache directory cannot be created");
            give_up();
            return;
        }
        have_identity_ = true;
        Load();

        const auto window_end = start + RecordWindow;
        auto next = Clock::now() + CheckpointEvery;
        for (;;) {
            if (!SleepUntil(stop, (std::min)(next, window_end))) {
                return;
            }
            const bool last = Clock::now() >= window_end;
            if (last) {
                for (auto& core : cores_) {
                    core->CloseRecording();
                }
            }
            WriteCurrent();
            if (last) {
                final_written_ = true;
                return;
            }
            next += CheckpointEvery;
        }
    }

    void Load() {
        constexpr std::size_t MaxFileBytes =
            HeaderBytes + MaxCores * (sizeof(u64) + MaxEntriesPerCore * EntryBytes);
        std::optional<std::vector<std::vector<Entry>>> lists;
        if (const auto bytes = ReadFileBytes(path_, MaxFileBytes)) {
            Parsed parsed = Parse(*bytes, identity_);
            if (parsed.reject == Reject::None) {
                std::size_t total = 0;
                for (const auto& list : parsed.per_core) {
                    total += list.size();
                }
                LOG_INFO(Core_ARM, "JIT prewarm: {} blocks to translate ahead", total);
                lists = std::move(parsed.per_core);
            } else {
                LOG_INFO(Core_ARM, "JIT prewarm: the list of an earlier launch is ignored ({})",
                         RejectName(parsed.reject));
            }
        } else {
            LOG_INFO(Core_ARM, "JIT prewarm: no list from an earlier launch");
        }
        for (std::size_t i = 0; i < cores_.size(); ++i) {
            cores_[i]->Publish(lists ? std::optional<std::vector<Entry>>{std::move((*lists)[i])}
                                     : std::nullopt);
        }
    }

    /// Writes the file when something changed since the last write: a block was recorded, or a
    /// core found entries of the list stale.
    void WriteCurrent() {
        std::vector<std::vector<Entry>> lists;
        lists.reserve(cores_.size());
        u64 signature = 0;
        std::vector<u32> stale;
        for (auto& core : cores_) {
            const std::span<const Entry> recorded = core->Recorded();
            const bool finished = core->GetStale(stale);
            if (!finished) {
                stale.clear();
            }
            signature += recorded.size() * 8 + stale.size() * 8 + (finished ? 1 : 0);
            lists.push_back(MergeForWrite(core->Loaded(), stale, recorded, MaxEntriesPerCore));
        }
        if (signature == last_signature_) {
            return;
        }
        if (WriteFileAtomic(path_, Serialize(identity_, lists))) {
            last_signature_ = signature;
        } else {
            LOG_WARNING(Core_ARM, "JIT prewarm: the list cannot be written");
        }
    }

    const u64 program_id_;
    const void* const process_key_;
    std::vector<std::unique_ptr<CoreState>> cores_;

    // Set and used by the helper thread; the destructor reads them after joining it.
    Identity identity_{};
    std::filesystem::path path_;
    bool have_identity_{false};
    bool final_written_{false};
    u64 last_signature_{0};

    std::jthread helper_; ///< Last: everything above exists when it starts
};

std::optional<Handle> Attach(u64 program_id, const void* process_key, std::size_t core_index) {
    if (!Common::EnvGate("EDEN_NX2_JIT_PREWARM") || core_index >= Core::Hardware::NUM_CPU_CORES) {
        return std::nullopt;
    }
    static std::mutex mutex;
    static std::weak_ptr<Session> current;
    std::shared_ptr<Session> session;
    {
        const std::scoped_lock lock{mutex};
        session = current.lock();
        if (!session || !session->Matches(process_key)) {
            session = std::make_shared<Session>(program_id, process_key);
            current = session;
        }
    }
    CoreState& core = session->CoreAt(core_index);
    return Handle{std::move(session), core};
}

} // namespace Core::JitPrewarm
