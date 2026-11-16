// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <set>
#include <system_error>

#include <fmt/format.h>

#include "common/fs/file.h"
#include "common/logging.h"
#include "video_core/cache_envelope.h"

namespace VideoCommon::CacheEnvelope {
namespace {

namespace fs = std::filesystem;

static_assert(std::endian::native == std::endian::little,
              "The envelope is little endian and is read and written without swapping");

constexpr std::array<char, 8> LegacyTransferable{'y', 'u', 'z', 'u', 'c', 'a', 'c', 'h'};
constexpr std::array<char, 8> LegacyDriverBlob{'y', 'u', 'z', 'u', 'v', 'k', 'c', 'h'};

/// Header field offsets (the grammar in cache_envelope.h).
namespace Field {
constexpr size_t Version = 8;
constexpr size_t Length = 12;
constexpr size_t Kind = 16;
constexpr size_t ShaderFormat = 20;
constexpr size_t BuildId = 24;
constexpr size_t Vendor = 32;
constexpr size_t Device = 36;
constexpr size_t DriverId = 40;
constexpr size_t DriverVersion = 44;
constexpr size_t Uuid = 48;
constexpr size_t TitleId = 64;
constexpr size_t GameVersion = 72;
constexpr size_t Content = 80;
constexpr size_t Settings = 88;
constexpr size_t Records = 96;
constexpr size_t PayloadOffset = 104;
constexpr size_t PayloadBytes = 112;
constexpr size_t IndexOffset = 120;
constexpr size_t IndexBytes = 128;
constexpr size_t Algorithm = 136;
constexpr size_t HeaderChecksum = 144;
constexpr size_t IndexChecksum = 152;
} // namespace Field

using HeaderBytesArray = std::array<u8, HeaderBytes>;

template <typename T>
void Put(u8* data, size_t offset, T value) {
    std::memcpy(data + offset, &value, sizeof(T));
}

template <typename T>
T Get(const u8* data, size_t offset) {
    T value;
    std::memcpy(&value, data + offset, sizeof(T));
    return value;
}

u64 FirstEightBytes(const u8* data) {
    return Get<u64>(data, 0);
}

HeaderBytesArray EncodeHeader(const CacheIdentity& identity, u64 record_count, u64 payload_bytes,
                              u64 index_bytes, u64 index_checksum) {
    HeaderBytesArray header{};
    u8* const data = header.data();
    std::memcpy(data, Magic.data(), Magic.size());
    Put<u32>(data, Field::Version, EnvelopeVersion);
    Put<u32>(data, Field::Length, HeaderBytes);
    Put<u32>(data, Field::Kind, static_cast<u32>(identity.kind));
    Put<u32>(data, Field::ShaderFormat, identity.shader_format_version);
    Put<u64>(data, Field::BuildId, identity.host.build_id);
    Put<u32>(data, Field::Vendor, identity.device.vendor_id);
    Put<u32>(data, Field::Device, identity.device.device_id);
    Put<u32>(data, Field::DriverId, identity.device.driver_id);
    Put<u32>(data, Field::DriverVersion, identity.device.driver_version);
    std::memcpy(data + Field::Uuid, identity.device.pipeline_cache_uuid.data(),
                identity.device.pipeline_cache_uuid.size());
    Put<u64>(data, Field::TitleId, identity.host.title_id);
    Put<u32>(data, Field::GameVersion, identity.host.game_version);
    Put<u64>(data, Field::Content, identity.host.content_stamp);
    Put<u64>(data, Field::Settings, identity.host.settings_hash);
    Put<u64>(data, Field::Records, record_count);
    Put<u64>(data, Field::PayloadOffset, HeaderBytes);
    Put<u64>(data, Field::PayloadBytes, payload_bytes);
    Put<u64>(data, Field::IndexOffset, HeaderBytes + payload_bytes);
    Put<u64>(data, Field::IndexBytes, index_bytes);
    Put<u32>(data, Field::Algorithm, ChecksumFnv1a64);
    Put<u64>(data, Field::HeaderChecksum, Checksum(std::span<const u8>(data, Field::HeaderChecksum)));
    Put<u64>(data, Field::IndexChecksum, index_checksum);
    return header;
}

/// The identity fields a header holds.
CacheIdentity DecodeIdentity(const u8* data) {
    CacheIdentity identity;
    identity.kind = static_cast<PayloadKind>(Get<u32>(data, Field::Kind));
    identity.envelope_version = Get<u32>(data, Field::Version);
    identity.shader_format_version = Get<u32>(data, Field::ShaderFormat);
    identity.host.build_id = Get<u64>(data, Field::BuildId);
    identity.device.vendor_id = Get<u32>(data, Field::Vendor);
    identity.device.device_id = Get<u32>(data, Field::Device);
    identity.device.driver_id = Get<u32>(data, Field::DriverId);
    identity.device.driver_version = Get<u32>(data, Field::DriverVersion);
    std::memcpy(identity.device.pipeline_cache_uuid.data(), data + Field::Uuid,
                identity.device.pipeline_cache_uuid.size());
    identity.host.title_id = Get<u64>(data, Field::TitleId);
    identity.host.game_version = Get<u32>(data, Field::GameVersion);
    identity.host.content_stamp = Get<u64>(data, Field::Content);
    identity.host.settings_hash = Get<u64>(data, Field::Settings);
    return identity;
}

CacheLoadResult Rejected(PayloadKind kind, RejectReason reason, u64 expected = 0, u64 found = 0) {
    CacheLoadResult result;
    result.kind = kind;
    result.reason = reason;
    result.expected = expected;
    result.found = found;
    return result;
}

u64 UuidValue(const DeviceIdentity& device) {
    return FirstEightBytes(device.pipeline_cache_uuid.data());
}

bool KnownRecordKind(u32 kind) {
    return kind >= static_cast<u32>(RecordKind::Graphics) &&
           kind <= static_cast<u32>(RecordKind::Opaque);
}

fs::path TemporaryPath(const fs::path& path) {
    fs::path temporary = path;
    temporary += ".tmp";
    return temporary;
}

} // Anonymous namespace

u64 Checksum(std::span<const u8> bytes) {
    u64 hash = 0xcbf29ce484222325ULL;
    for (const u8 byte : bytes) {
        hash ^= byte;
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

CacheLoadResult CheckIdentity(const CacheIdentity& expected, const CacheIdentity& found) {
    const PayloadKind kind = expected.kind;
    if (found.envelope_version != expected.envelope_version) {
        return Rejected(kind, RejectReason::FormatVersion, expected.envelope_version,
                        found.envelope_version);
    }
    if (found.kind != expected.kind) {
        return Rejected(kind, RejectReason::PayloadKind, static_cast<u32>(expected.kind),
                        static_cast<u32>(found.kind));
    }
    if (found.shader_format_version != expected.shader_format_version) {
        return Rejected(kind, RejectReason::ShaderFormat, expected.shader_format_version,
                        found.shader_format_version);
    }
    if (found.host.build_id != expected.host.build_id) {
        return Rejected(kind, RejectReason::BuildId, expected.host.build_id, found.host.build_id);
    }
    if (found.device.vendor_id != expected.device.vendor_id ||
        found.device.device_id != expected.device.device_id) {
        return Rejected(kind, RejectReason::Device,
                        u64{expected.device.vendor_id} << 32 | expected.device.device_id,
                        u64{found.device.vendor_id} << 32 | found.device.device_id);
    }
    if (found.device.driver_id != expected.device.driver_id ||
        found.device.driver_version != expected.device.driver_version) {
        return Rejected(kind, RejectReason::Driver,
                        u64{expected.device.driver_id} << 32 | expected.device.driver_version,
                        u64{found.device.driver_id} << 32 | found.device.driver_version);
    }
    if (found.device.pipeline_cache_uuid != expected.device.pipeline_cache_uuid) {
        return Rejected(kind, RejectReason::PipelineUuid, UuidValue(expected.device),
                        UuidValue(found.device));
    }
    if (found.host.title_id != expected.host.title_id) {
        return Rejected(kind, RejectReason::Title, expected.host.title_id, found.host.title_id);
    }
    if (found.host.game_version != expected.host.game_version) {
        return Rejected(kind, RejectReason::GameVersion, expected.host.game_version,
                        found.host.game_version);
    }
    if (found.host.content_stamp != expected.host.content_stamp) {
        return Rejected(kind, RejectReason::Content, expected.host.content_stamp,
                        found.host.content_stamp);
    }
    if (found.host.settings_hash != expected.host.settings_hash) {
        return Rejected(kind, RejectReason::Settings, expected.host.settings_hash,
                        found.host.settings_hash);
    }
    CacheLoadResult accepted;
    accepted.accepted = true;
    accepted.kind = kind;
    return accepted;
}

std::string CacheRejectLine(const CacheLoadResult& result) {
    if (result.accepted) {
        return {};
    }
    return fmt::format("Nx2Cache reject kind={} reason={} expected={} found={} action={}",
                       PayloadKindName(result.kind), RejectReasonName(result.reason),
                       result.expected, result.found,
                       result.legacy_backed_up ? "backup_legacy" : "ignore");
}

void RemoveStaleTemp(const fs::path& path) {
    std::error_code error;
    fs::remove(TemporaryPath(path), error);
}

bool BackupLegacy(const fs::path& path) {
    fs::path backup = path;
    backup += ".legacy.bak";
    std::error_code error;
    fs::remove(backup, error);
    error.clear();
    fs::rename(path, backup, error);
    return !error;
}

struct EnvelopeReader::Impl {
    fs::path path;
    std::vector<IndexEntry> index;
};

EnvelopeReader::EnvelopeReader(std::unique_ptr<Impl> state) noexcept : impl{std::move(state)} {}

EnvelopeReader::~EnvelopeReader() = default;

std::unique_ptr<EnvelopeReader> EnvelopeReader::Open(const fs::path& path,
                                                     const CacheIdentity& expected,
                                                     CacheLoadResult& result) {
    const PayloadKind kind = expected.kind;
    const auto reject = [&](RejectReason reason, u64 want = 0, u64 got = 0) {
        result = Rejected(kind, reason, want, got);
        return std::unique_ptr<EnvelopeReader>{};
    };

    std::error_code error;
    const bool exists = fs::exists(path, error);
    if (error) {
        return reject(RejectReason::IoError);
    }
    if (!exists) {
        return reject(RejectReason::Missing);
    }
    Common::FS::IOFile file{path, Common::FS::FileAccessMode::Read};
    if (!file.IsOpen()) {
        return reject(RejectReason::IoError);
    }

    // Header first, small. Fewer bytes than a header is read as far as there are any.
    const u64 size = file.GetSize();
    HeaderBytesArray raw{};
    const size_t available = static_cast<size_t>(std::min<u64>(size, HeaderBytes));
    if (file.ReadSpan(std::span<u8>(raw.data(), available)) != available) {
        return reject(RejectReason::IoError);
    }
    const u8* const header = raw.data();

    // Magic. An old unenveloped file is backed up once so it is not retried.
    const size_t magic_bytes = std::min<size_t>(available, Magic.size());
    if (std::memcmp(header, Magic.data(), magic_bytes) != 0) {
        const bool legacy = available >= Magic.size() &&
                            (std::memcmp(header, LegacyTransferable.data(), Magic.size()) == 0 ||
                             std::memcmp(header, LegacyDriverBlob.data(), Magic.size()) == 0);
        const u64 want = FirstEightBytes(reinterpret_cast<const u8*>(Magic.data()));
        const u64 got = available >= Magic.size() ? FirstEightBytes(header) : 0;
        if (!legacy) {
            return reject(RejectReason::BadMagic, want, got);
        }
        file.Close();
        result = Rejected(kind, RejectReason::LegacyFormat, want, got);
        result.legacy_backed_up = BackupLegacy(path);
        return nullptr;
    }
    if (available < HeaderBytes) {
        return reject(RejectReason::Truncated, HeaderBytes, size);
    }

    // The version and the length come before the checksum: a later format is rejected cleanly.
    const u32 version = Get<u32>(header, Field::Version);
    if (version != expected.envelope_version) {
        return reject(RejectReason::FormatVersion, expected.envelope_version, version);
    }
    const u32 header_length = Get<u32>(header, Field::Length);
    if (header_length != HeaderBytes) {
        return reject(RejectReason::HeaderLength, HeaderBytes, header_length);
    }
    const u64 header_checksum = Checksum(std::span<const u8>(header, Field::HeaderChecksum));
    const u64 stored_checksum = Get<u64>(header, Field::HeaderChecksum);
    if (header_checksum != stored_checksum) {
        return reject(RejectReason::BadChecksum, header_checksum, stored_checksum);
    }
    const u32 algorithm = Get<u32>(header, Field::Algorithm);
    if (algorithm != ChecksumFnv1a64) {
        return reject(RejectReason::BadChecksum, ChecksumFnv1a64, algorithm);
    }

    // payload_kind, shader format, build, device, driver, uuid, title, version, content, settings.
    if (const CacheLoadResult identity = CheckIdentity(expected, DecodeIdentity(header));
        !identity.accepted) {
        result = identity;
        return nullptr;
    }

    // Bounds. Nothing is allocated for the payload; the index only once its size is consistent.
    const u64 record_count = Get<u64>(header, Field::Records);
    const u64 payload_offset = Get<u64>(header, Field::PayloadOffset);
    const u64 payload_bytes = Get<u64>(header, Field::PayloadBytes);
    const u64 index_offset = Get<u64>(header, Field::IndexOffset);
    const u64 index_bytes = Get<u64>(header, Field::IndexBytes);
    if (payload_offset != HeaderBytes) {
        return reject(RejectReason::BadIndex, HeaderBytes, payload_offset);
    }
    const u64 max_total = MaxBytes(kind);
    const u64 body = payload_bytes + index_bytes;
    const bool overflow = body < payload_bytes || body + payload_offset < body;
    const u64 total = overflow ? ~u64{} : body + payload_offset;
    if (overflow || total > max_total) {
        return reject(RejectReason::TooLarge, max_total, total);
    }
    if (total != size) {
        return reject(RejectReason::Truncated, total, size);
    }
    if (record_count > MaxIndexBytes / IndexEntryBytes) {
        return reject(RejectReason::BadIndex, MaxIndexBytes / IndexEntryBytes, record_count);
    }
    if (index_bytes > MaxIndexBytes || index_bytes != record_count * IndexEntryBytes) {
        return reject(RejectReason::BadIndex, record_count * IndexEntryBytes, index_bytes);
    }
    if (index_offset != payload_offset + payload_bytes) {
        return reject(RejectReason::BadIndex, payload_offset + payload_bytes, index_offset);
    }

    std::vector<u8> raw_index(static_cast<size_t>(index_bytes));
    if (!file.Seek(static_cast<s64>(index_offset)) ||
        file.ReadSpan(std::span<u8>(raw_index)) != raw_index.size()) {
        return reject(RejectReason::IoError);
    }
    auto state = std::make_unique<Impl>();
    state->path = path;
    state->index.reserve(static_cast<size_t>(record_count));
    u64 running = 0;
    for (u64 i = 0; i < record_count; ++i) {
        const u8* const entry = raw_index.data() + i * IndexEntryBytes;
        IndexEntry out;
        const u32 record_kind = Get<u32>(entry, 0);
        const u32 phase = Get<u32>(entry, 4);
        out.offset = Get<u64>(entry, 8);
        out.length = Get<u32>(entry, 16);
        out.meta.first_use_ordinal = Get<u32>(entry, 20);
        out.meta.first_use_ns = Get<u64>(entry, 24);
        out.meta.translate_cost_us = Get<u32>(entry, 32);
        out.meta.build_cost_us = Get<u32>(entry, 36);
        out.checksum = Get<u64>(entry, 40);
        if (!KnownRecordKind(record_kind)) {
            return reject(RejectReason::BadIndex, 0, record_kind);
        }
        if (phase > static_cast<u32>(FirstUse::Run)) {
            return reject(RejectReason::BadIndex, 0, phase);
        }
        if (out.offset != running) {
            return reject(RejectReason::BadIndex, running, out.offset);
        }
        if (out.length == 0 || out.length > MaxRecordBytes || out.length > payload_bytes - running) {
            return reject(RejectReason::BadIndex, MaxRecordBytes, out.length);
        }
        out.meta.kind = static_cast<RecordKind>(record_kind);
        out.meta.first_use_phase = static_cast<FirstUse>(phase);
        running += out.length;
        state->index.push_back(out);
    }
    if (running != payload_bytes) {
        return reject(RejectReason::BadIndex, payload_bytes, running);
    }
    const u64 index_checksum = Checksum(raw_index);
    const u64 stored_index_checksum = Get<u64>(header, Field::IndexChecksum);
    if (index_checksum != stored_index_checksum) {
        return reject(RejectReason::BadChecksum, index_checksum, stored_index_checksum);
    }

    result = {};
    result.accepted = true;
    result.kind = kind;
    result.records = record_count;
    result.payload_bytes = payload_bytes;
    return std::unique_ptr<EnvelopeReader>(new EnvelopeReader(std::move(state)));
}

std::span<const IndexEntry> EnvelopeReader::Index() const noexcept {
    return impl->index;
}

RejectReason EnvelopeReader::ReadRecord(size_t index, std::vector<u8>& payload) const {
    payload.clear();
    if (index >= impl->index.size()) {
        return RejectReason::BadIndex;
    }
    const IndexEntry& entry = impl->index[index];
    // The file is opened for every read and closed again: an open handle keeps Windows from
    // replacing the file when the next generation is committed, and it makes concurrent reads
    // from the workers independent.
    const Common::FS::IOFile file{impl->path, Common::FS::FileAccessMode::Read};
    if (!file.IsOpen() || !file.Seek(static_cast<s64>(HeaderBytes + entry.offset))) {
        return RejectReason::IoError;
    }
    payload.resize(entry.length);
    if (file.ReadSpan(std::span<u8>(payload)) != payload.size()) {
        payload.clear();
        return RejectReason::IoError;
    }
    if (Checksum(payload) != entry.checksum) {
        payload.clear();
        return RejectReason::BadChecksum;
    }
    return RejectReason::None;
}

struct EnvelopeWriter::Impl {
    fs::path path;
    fs::path temporary;
    CacheIdentity identity;
    u64 max_bytes{};
    Common::FS::IOFile file;
    std::vector<IndexEntry> index;
    std::set<std::array<u64, 3>> seen; ///< kind, length, checksum
    u64 payload_bytes{};
    bool failed{};
    bool seeded{};
    bool finished{};

    [[nodiscard]] u64 Bytes() const noexcept {
        return HeaderBytes + payload_bytes + index.size() * IndexEntryBytes;
    }

    /// Drops the uncommitted generation; the previous file is not touched.
    void Abandon() {
        file.Close();
        std::error_code error;
        fs::remove(temporary, error);
        finished = true;
    }
};

EnvelopeWriter::EnvelopeWriter(std::unique_ptr<Impl> state) noexcept : impl{std::move(state)} {}

EnvelopeWriter::~EnvelopeWriter() {
    if (impl && !impl->finished) {
        impl->Abandon();
    }
}

std::unique_ptr<EnvelopeWriter> EnvelopeWriter::Create(const fs::path& path,
                                                       const CacheIdentity& identity,
                                                       u64 max_bytes) {
    RemoveStaleTemp(path);
    auto state = std::make_unique<Impl>();
    state->path = path;
    state->temporary = TemporaryPath(path);
    state->identity = identity;
    state->max_bytes = std::min(max_bytes, MaxBytes(identity.kind));
    state->file.Open(state->temporary, Common::FS::FileAccessMode::Write);
    if (!state->file.IsOpen()) {
        return nullptr;
    }
    // The header is written last, once the counts are known; its room is reserved now.
    constexpr HeaderBytesArray placeholder{};
    if (state->file.WriteSpan(std::span<const u8>(placeholder)) != placeholder.size()) {
        state->Abandon();
        return nullptr;
    }
    return std::unique_ptr<EnvelopeWriter>(new EnvelopeWriter(std::move(state)));
}

size_t EnvelopeWriter::Seed(const EnvelopeReader& reader) {
    if (impl->seeded || impl->failed || !impl->index.empty()) {
        return 0;
    }
    impl->seeded = true;
    size_t carried = 0;
    std::vector<u8> buffer;
    const auto entries = reader.Index();
    for (size_t i = 0; i < entries.size(); ++i) {
        // A record that no longer reads back (or whose checksum is wrong) is dropped, not copied.
        if (reader.ReadRecord(i, buffer) != RejectReason::None) {
            continue;
        }
        if (Append(entries[i].meta, buffer) == AppendResult::Ok) {
            ++carried;
        }
    }
    return carried;
}

AppendResult EnvelopeWriter::Append(const RecordMeta& meta, std::span<const u8> payload) {
    if (impl->failed || impl->finished) {
        return AppendResult::IoError;
    }
    if (payload.size() > MaxRecordBytes) {
        return AppendResult::TooLarge;
    }
    if (payload.empty()) {
        return AppendResult::IoError; // Not a record: the reader rejects an empty one
    }
    const u64 checksum = Checksum(payload);
    const std::array<u64, 3> identity{static_cast<u64>(meta.kind), payload.size(), checksum};
    if (impl->seen.contains(identity)) {
        return AppendResult::Duplicate;
    }
    if ((impl->index.size() + 1) * IndexEntryBytes > MaxIndexBytes ||
        impl->Bytes() + payload.size() + IndexEntryBytes > impl->max_bytes) {
        return AppendResult::BudgetFull;
    }
    if (impl->file.WriteSpan(payload) != payload.size()) {
        impl->failed = true;
        return AppendResult::IoError;
    }
    IndexEntry entry;
    entry.meta = meta;
    entry.offset = impl->payload_bytes;
    entry.length = static_cast<u32>(payload.size());
    entry.checksum = checksum;
    impl->index.push_back(entry);
    impl->seen.insert(identity);
    impl->payload_bytes += payload.size();
    return AppendResult::Ok;
}

bool EnvelopeWriter::Commit() {
    if (impl->finished) {
        return false;
    }
    if (impl->failed) {
        impl->Abandon();
        return false;
    }
    std::vector<u8> raw_index(impl->index.size() * IndexEntryBytes);
    for (size_t i = 0; i < impl->index.size(); ++i) {
        const IndexEntry& entry = impl->index[i];
        u8* const out = raw_index.data() + i * IndexEntryBytes;
        Put<u32>(out, 0, static_cast<u32>(entry.meta.kind));
        Put<u32>(out, 4, static_cast<u32>(entry.meta.first_use_phase));
        Put<u64>(out, 8, entry.offset);
        Put<u32>(out, 16, entry.length);
        Put<u32>(out, 20, entry.meta.first_use_ordinal);
        Put<u64>(out, 24, entry.meta.first_use_ns);
        Put<u32>(out, 32, entry.meta.translate_cost_us);
        Put<u32>(out, 36, entry.meta.build_cost_us);
        Put<u64>(out, 40, entry.checksum);
    }
    const HeaderBytesArray header = EncodeHeader(impl->identity, impl->index.size(),
                                                 impl->payload_bytes, raw_index.size(),
                                                 Checksum(raw_index));
    bool ok = impl->Bytes() <= impl->max_bytes &&
              impl->file.WriteSpan(std::span<const u8>(raw_index)) == raw_index.size() &&
              impl->file.Seek(0) &&
              impl->file.WriteSpan(std::span<const u8>(header)) == header.size() &&
              impl->file.Commit();
    impl->file.Close();
    if (ok) {
        // Replaces the old generation in one step; a failure leaves it where it is.
        std::error_code error;
        fs::rename(impl->temporary, impl->path, error);
        ok = !error;
        if (error) {
            LOG_WARNING(Common_Filesystem, "Cannot replace cache file: {}", error.message());
        }
    }
    if (!ok) {
        impl->Abandon();
        return false;
    }
    impl->finished = true;
    return true;
}

size_t EnvelopeWriter::Records() const noexcept {
    return impl->index.size();
}

u64 EnvelopeWriter::Bytes() const noexcept {
    return impl->Bytes();
}

} // namespace VideoCommon::CacheEnvelope
