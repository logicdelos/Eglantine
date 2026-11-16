// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/exception.h"
#include "video_core/cache_envelope.h"
#include "video_core/shader_environment.h"

namespace {

namespace CE = VideoCommon::CacheEnvelope;
namespace fs = std::filesystem;

struct TempDir {
    fs::path path;
    TempDir() {
        static std::atomic<u64> counter{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() /
               ("eglantine-envelope-" + std::to_string(stamp) + "-" + std::to_string(counter++));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code error;
        fs::remove_all(path, error);
    }
};

std::vector<u8> ReadFile(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void WriteFile(const fs::path& file, std::span<const u8> bytes) {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
}

void WriteText(const fs::path& file, std::string_view text) {
    WriteFile(file, {reinterpret_cast<const u8*>(text.data()), text.size()});
}

template <typename T>
void PutAt(std::vector<u8>& bytes, size_t offset, T value) {
    REQUIRE(offset + sizeof(T) <= bytes.size());
    std::memcpy(bytes.data() + offset, &value, sizeof(T));
}

template <typename T>
T GetAt(const std::vector<u8>& bytes, size_t offset) {
    REQUIRE(offset + sizeof(T) <= bytes.size());
    T value;
    std::memcpy(&value, bytes.data() + offset, sizeof(T));
    return value;
}

std::vector<u8> Payload(size_t size, u8 seed) {
    std::vector<u8> bytes(size);
    for (size_t i = 0; i < size; ++i) {
        bytes[i] = static_cast<u8>(seed + i * 31);
    }
    return bytes;
}

CE::CacheIdentity MakeIdentity(CE::PayloadKind kind = CE::PayloadKind::Transferable) {
    CE::CacheIdentity identity;
    identity.kind = kind;
    identity.shader_format_version = 35;
    identity.host.build_id = 0x1111222233334444ULL;
    identity.host.title_id = 0x0100000000001000ULL;
    identity.host.game_version = 7;
    identity.host.content_stamp = 0xabcdef0123456789ULL;
    identity.host.settings_hash = 0x5555;
    identity.device.vendor_id = 0x10de;
    identity.device.device_id = 0x2206;
    identity.device.driver_id = 4;
    identity.device.driver_version = 0x00800123;
    for (u8 i = 0; i < 16; ++i) {
        identity.device.pipeline_cache_uuid[i] = static_cast<u8>(0xa0 + i);
    }
    return identity;
}

struct Rec {
    CE::RecordMeta meta;
    std::vector<u8> bytes;
};

std::vector<Rec> ThreeRecords() {
    std::vector<Rec> records(3);
    records[0].meta = {CE::RecordKind::Graphics, CE::FirstUse::Boot, 0, 0, 120, 3400};
    records[0].bytes = Payload(20, 1);
    records[1].meta = {CE::RecordKind::Compute, CE::FirstUse::Run, 5, 1'500'000'000ULL, 7, 90};
    records[1].bytes = Payload(33, 2);
    records[2].meta = {CE::RecordKind::Opaque, CE::FirstUse::Run, 9, 2'000'000'000ULL, 0, 0};
    records[2].bytes = Payload(7, 3);
    return records;
}

void WriteGeneration(const fs::path& path, const CE::CacheIdentity& identity,
                     const std::vector<Rec>& records) {
    auto writer = CE::EnvelopeWriter::Create(path, identity, CE::MaxBytes(identity.kind));
    REQUIRE(writer);
    for (const Rec& record : records) {
        REQUIRE(writer->Append(record.meta, record.bytes) == CE::AppendResult::Ok);
    }
    REQUIRE(writer->Commit());
}

struct Opened {
    std::unique_ptr<CE::EnvelopeReader> reader;
    CE::CacheLoadResult result;
};

Opened TryOpen(const fs::path& path, const CE::CacheIdentity& identity) {
    Opened opened;
    opened.reader = CE::EnvelopeReader::Open(path, identity, opened.result);
    return opened;
}

/// The grammar of cache_envelope.h written out with no help from the implementation under test,
/// so a test can build any file, valid or not.
struct Raw {
    struct Entry {
        u32 kind{2};
        u32 phase{1};
        u64 offset{};
        u32 length{};
        u32 ordinal{};
        u64 ns{};
        u32 translate{};
        u32 build{};
        u64 checksum{};
    };
    CE::CacheIdentity identity;
    std::vector<u8> payload;
    std::vector<Entry> entries;
    std::optional<u64> record_count;
    std::optional<u64> payload_offset;
    std::optional<u64> payload_bytes;
    std::optional<u64> index_offset;
    std::optional<u64> index_bytes;
    std::optional<u64> index_checksum;
    std::optional<u32> version;
    std::optional<u32> header_length;
    std::optional<u32> algorithm;
};

Raw FromRecords(const CE::CacheIdentity& identity, const std::vector<Rec>& records) {
    Raw raw;
    raw.identity = identity;
    for (const Rec& record : records) {
        Raw::Entry entry;
        entry.kind = static_cast<u32>(record.meta.kind);
        entry.phase = static_cast<u32>(record.meta.first_use_phase);
        entry.offset = raw.payload.size();
        entry.length = static_cast<u32>(record.bytes.size());
        entry.ordinal = record.meta.first_use_ordinal;
        entry.ns = record.meta.first_use_ns;
        entry.translate = record.meta.translate_cost_us;
        entry.build = record.meta.build_cost_us;
        entry.checksum = CE::Checksum(record.bytes);
        raw.entries.push_back(entry);
        raw.payload.insert(raw.payload.end(), record.bytes.begin(), record.bytes.end());
    }
    return raw;
}

std::vector<u8> IndexBytes(const Raw& raw) {
    std::vector<u8> index(raw.entries.size() * 48);
    for (size_t i = 0; i < raw.entries.size(); ++i) {
        const Raw::Entry& e = raw.entries[i];
        const size_t at = i * 48;
        PutAt<u32>(index, at + 0, e.kind);
        PutAt<u32>(index, at + 4, e.phase);
        PutAt<u64>(index, at + 8, e.offset);
        PutAt<u32>(index, at + 16, e.length);
        PutAt<u32>(index, at + 20, e.ordinal);
        PutAt<u64>(index, at + 24, e.ns);
        PutAt<u32>(index, at + 32, e.translate);
        PutAt<u32>(index, at + 36, e.build);
        PutAt<u64>(index, at + 40, e.checksum);
    }
    return index;
}

std::vector<u8> Assemble(const Raw& raw) {
    const std::vector<u8> index = IndexBytes(raw);
    std::vector<u8> header(160);
    std::memcpy(header.data(), "EGL05CCH", 8);
    PutAt<u32>(header, 8, raw.version.value_or(1));
    PutAt<u32>(header, 12, raw.header_length.value_or(160));
    PutAt<u32>(header, 16, static_cast<u32>(raw.identity.kind));
    PutAt<u32>(header, 20, raw.identity.shader_format_version);
    PutAt<u64>(header, 24, raw.identity.host.build_id);
    PutAt<u32>(header, 32, raw.identity.device.vendor_id);
    PutAt<u32>(header, 36, raw.identity.device.device_id);
    PutAt<u32>(header, 40, raw.identity.device.driver_id);
    PutAt<u32>(header, 44, raw.identity.device.driver_version);
    std::memcpy(header.data() + 48, raw.identity.device.pipeline_cache_uuid.data(), 16);
    PutAt<u64>(header, 64, raw.identity.host.title_id);
    PutAt<u32>(header, 72, raw.identity.host.game_version);
    PutAt<u64>(header, 80, raw.identity.host.content_stamp);
    PutAt<u64>(header, 88, raw.identity.host.settings_hash);
    const u64 payload_bytes = raw.payload_bytes.value_or(raw.payload.size());
    PutAt<u64>(header, 96, raw.record_count.value_or(raw.entries.size()));
    PutAt<u64>(header, 104, raw.payload_offset.value_or(160));
    PutAt<u64>(header, 112, payload_bytes);
    PutAt<u64>(header, 120, raw.index_offset.value_or(160 + payload_bytes));
    PutAt<u64>(header, 128, raw.index_bytes.value_or(index.size()));
    PutAt<u32>(header, 136, raw.algorithm.value_or(1));
    PutAt<u64>(header, 144, CE::Checksum(std::span<const u8>(header.data(), 144)));
    PutAt<u64>(header, 152, raw.index_checksum.value_or(CE::Checksum(index)));
    std::vector<u8> file = header;
    file.insert(file.end(), raw.payload.begin(), raw.payload.end());
    file.insert(file.end(), index.begin(), index.end());
    return file;
}

/// Opens `bytes` as a file for `identity` and returns what the reader said.
CE::CacheLoadResult OpenBytes(const fs::path& path, const std::vector<u8>& bytes,
                              const CE::CacheIdentity& identity) {
    WriteFile(path, bytes);
    return TryOpen(path, identity).result;
}

u64 UuidValue(const CE::CacheIdentity& identity) {
    u64 value;
    std::memcpy(&value, identity.device.pipeline_cache_uuid.data(), sizeof(value));
    return value;
}

} // Anonymous namespace

TEST_CASE("CacheEnvelope: the checksum is FNV-1a 64", "[video_core]") {
    REQUIRE(CE::Checksum({}) == 0xcbf29ce484222325ULL);
    const std::array<u8, 1> a{'a'};
    REQUIRE(CE::Checksum(a) == 0xaf63dc4c8601ec8cULL);
    const std::array<u8, 6> foobar{'f', 'o', 'o', 'b', 'a', 'r'};
    REQUIRE(CE::Checksum(foobar) == 0x85944171f73967e8ULL);
}

TEST_CASE("CacheEnvelope: a written file has the documented grammar and reads back exactly",
          "[video_core]") {
    TempDir dir;
    const auto path = dir.path / "cache.bin";
    const auto identity = MakeIdentity();
    const auto records = ThreeRecords();
    {
        auto writer = CE::EnvelopeWriter::Create(path, identity, CE::MaxTransferableBytes);
        REQUIRE(writer);
        REQUIRE(writer->Bytes() == 160);
        for (const Rec& record : records) {
            REQUIRE(writer->Append(record.meta, record.bytes) == CE::AppendResult::Ok);
        }
        REQUIRE(writer->Records() == 3);
        REQUIRE(writer->Bytes() == 160 + 60 + 3 * 48);
        // Nothing is in place before the commit.
        REQUIRE_FALSE(fs::exists(path));
        REQUIRE(fs::exists(fs::path(path).concat(".tmp")));
        REQUIRE(writer->Commit());
    }
    REQUIRE_FALSE(fs::exists(fs::path(path).concat(".tmp")));

    // The bytes are the grammar, built independently of the writer.
    REQUIRE(ReadFile(path) == Assemble(FromRecords(identity, records)));
    REQUIRE(fs::file_size(path) == 160 + 60 + 3 * 48);

    auto opened = TryOpen(path, identity);
    REQUIRE(opened.reader);
    REQUIRE(opened.result.accepted);
    REQUIRE(opened.result.reason == CE::RejectReason::None);
    REQUIRE(opened.result.kind == CE::PayloadKind::Transferable);
    REQUIRE(opened.result.records == 3);
    REQUIRE(opened.result.payload_bytes == 60);
    REQUIRE(CE::CacheRejectLine(opened.result).empty());
    const auto index = opened.reader->Index();
    REQUIRE(index.size() == 3);
    u64 offset = 0;
    for (size_t i = 0; i < 3; ++i) {
        REQUIRE(index[i].meta.kind == records[i].meta.kind);
        REQUIRE(index[i].meta.first_use_phase == records[i].meta.first_use_phase);
        REQUIRE(index[i].meta.first_use_ordinal == records[i].meta.first_use_ordinal);
        REQUIRE(index[i].meta.first_use_ns == records[i].meta.first_use_ns);
        REQUIRE(index[i].meta.translate_cost_us == records[i].meta.translate_cost_us);
        REQUIRE(index[i].meta.build_cost_us == records[i].meta.build_cost_us);
        REQUIRE(index[i].offset == offset);
        REQUIRE(index[i].length == records[i].bytes.size());
        REQUIRE(index[i].checksum == CE::Checksum(records[i].bytes));
        offset += index[i].length;
        std::vector<u8> bytes{1, 2, 3};
        REQUIRE(opened.reader->ReadRecord(i, bytes) == CE::RejectReason::None);
        REQUIRE(bytes == records[i].bytes);
    }
    std::vector<u8> bytes{9};
    REQUIRE(opened.reader->ReadRecord(3, bytes) == CE::RejectReason::BadIndex);
    REQUIRE(bytes.empty());
}

TEST_CASE("CacheEnvelope: an empty generation is a valid file", "[video_core]") {
    TempDir dir;
    const auto path = dir.path / "cache.bin";
    const auto identity = MakeIdentity(CE::PayloadKind::WarmCompute);
    WriteGeneration(path, identity, {});
    REQUIRE(fs::file_size(path) == 160);
    auto opened = TryOpen(path, identity);
    REQUIRE(opened.reader);
    REQUIRE(opened.result.records == 0);
    REQUIRE(opened.reader->Index().empty());
}

TEST_CASE("CacheEnvelope: every identity field that differs rejects the file with its reason",
          "[video_core]") {
    struct Mismatch {
        const char* name;
        void (*mutate)(CE::CacheIdentity&);
        CE::RejectReason reason;
        u64 (*value)(const CE::CacheIdentity&);
    };
    const std::vector<Mismatch> table{
        {"envelope version", [](CE::CacheIdentity& i) { i.envelope_version = 2; },
         CE::RejectReason::FormatVersion, [](const CE::CacheIdentity& i) { return u64{i.envelope_version}; }},
        {"payload kind", [](CE::CacheIdentity& i) { i.kind = CE::PayloadKind::TransferableMesh; },
         CE::RejectReason::PayloadKind,
         [](const CE::CacheIdentity& i) { return static_cast<u64>(i.kind); }},
        {"shader format", [](CE::CacheIdentity& i) { i.shader_format_version += 1; },
         CE::RejectReason::ShaderFormat,
         [](const CE::CacheIdentity& i) { return u64{i.shader_format_version}; }},
        {"build", [](CE::CacheIdentity& i) { i.host.build_id ^= 1; }, CE::RejectReason::BuildId,
         [](const CE::CacheIdentity& i) { return i.host.build_id; }},
        {"vendor", [](CE::CacheIdentity& i) { i.device.vendor_id += 1; }, CE::RejectReason::Device,
         [](const CE::CacheIdentity& i) {
             return u64{i.device.vendor_id} << 32 | i.device.device_id;
         }},
        {"device", [](CE::CacheIdentity& i) { i.device.device_id += 1; }, CE::RejectReason::Device,
         [](const CE::CacheIdentity& i) {
             return u64{i.device.vendor_id} << 32 | i.device.device_id;
         }},
        {"driver id", [](CE::CacheIdentity& i) { i.device.driver_id += 1; },
         CE::RejectReason::Driver,
         [](const CE::CacheIdentity& i) {
             return u64{i.device.driver_id} << 32 | i.device.driver_version;
         }},
        {"driver version", [](CE::CacheIdentity& i) { i.device.driver_version += 1; },
         CE::RejectReason::Driver,
         [](const CE::CacheIdentity& i) {
             return u64{i.device.driver_id} << 32 | i.device.driver_version;
         }},
        {"uuid, first byte", [](CE::CacheIdentity& i) { i.device.pipeline_cache_uuid[0] ^= 1; },
         CE::RejectReason::PipelineUuid, [](const CE::CacheIdentity& i) { return UuidValue(i); }},
        {"uuid, last byte", [](CE::CacheIdentity& i) { i.device.pipeline_cache_uuid[15] ^= 1; },
         CE::RejectReason::PipelineUuid, [](const CE::CacheIdentity& i) { return UuidValue(i); }},
        {"title", [](CE::CacheIdentity& i) { i.host.title_id += 1; }, CE::RejectReason::Title,
         [](const CE::CacheIdentity& i) { return i.host.title_id; }},
        {"game version", [](CE::CacheIdentity& i) { i.host.game_version += 1; },
         CE::RejectReason::GameVersion,
         [](const CE::CacheIdentity& i) { return u64{i.host.game_version}; }},
        {"content", [](CE::CacheIdentity& i) { i.host.content_stamp += 1; },
         CE::RejectReason::Content, [](const CE::CacheIdentity& i) { return i.host.content_stamp; }},
        {"settings", [](CE::CacheIdentity& i) { i.host.settings_hash += 1; },
         CE::RejectReason::Settings, [](const CE::CacheIdentity& i) { return i.host.settings_hash; }},
    };
    TempDir dir;
    const auto path = dir.path / "cache.bin";
    const auto written = MakeIdentity();
    WriteGeneration(path, written, ThreeRecords());
    const auto before = ReadFile(path);
    for (const Mismatch& row : table) {
        INFO(row.name);
        auto expected = written;
        row.mutate(expected);
        auto opened = TryOpen(path, expected);
        REQUIRE_FALSE(opened.reader);
        REQUIRE_FALSE(opened.result.accepted);
        REQUIRE(opened.result.reason == row.reason);
        REQUIRE(opened.result.kind == expected.kind);
        REQUIRE_FALSE(opened.result.legacy_backed_up);
        if (row.reason != CE::RejectReason::PipelineUuid) {
            REQUIRE(opened.result.expected == row.value(expected));
            REQUIRE(opened.result.found == row.value(written));
        }
        const auto line = CE::CacheRejectLine(opened.result);
        REQUIRE(line.find("reason=" + std::string(CE::RejectReasonName(row.reason))) !=
                std::string::npos);
        REQUIRE(line.ends_with("action=ignore"));
        // A rejected file is left where it is.
        REQUIRE(ReadFile(path) == before);
    }

    // When several fields differ the first in the open order is the answer.
    auto several = written;
    several.host.settings_hash += 1;
    several.host.title_id += 1;
    several.host.build_id += 1;
    REQUIRE(TryOpen(path, several).result.reason == CE::RejectReason::BuildId);
    several.host.build_id = written.host.build_id;
    REQUIRE(TryOpen(path, several).result.reason == CE::RejectReason::Title);
}

TEST_CASE("CacheEnvelope: the reject line carries reason, both values and action",
          "[video_core]") {
    auto expected = MakeIdentity();
    auto found = expected;
    found.envelope_version = 999;
    const auto result = CE::CheckIdentity(expected, found);
    REQUIRE_FALSE(result.accepted);
    REQUIRE(result.reason == CE::RejectReason::FormatVersion);
    REQUIRE(CE::CacheRejectLine(result) ==
            "Nx2Cache reject kind=transferable reason=format_version expected=1 found=999 "
            "action=ignore");

    // A format the reader does not know is rejected before the checksum is looked at.
    TempDir dir;
    const auto path = dir.path / "cache.bin";
    auto raw = FromRecords(expected, ThreeRecords());
    raw.version = 999;
    const auto bytes = Assemble(raw);
    auto bad = bytes;
    bad[40] ^= 0xff; // The checksum no longer matches either.
    const auto opened = OpenBytes(path, bad, expected);
    REQUIRE(opened.reason == CE::RejectReason::FormatVersion);
    REQUIRE(opened.expected == 1);
    REQUIRE(opened.found == 999);

    REQUIRE(CE::CheckIdentity(expected, expected).accepted);
    CE::CacheLoadResult accepted;
    accepted.accepted = true;
    REQUIRE(CE::CacheRejectLine(accepted).empty());

    CE::CacheLoadResult legacy;
    legacy.reason = CE::RejectReason::LegacyFormat;
    legacy.kind = CE::PayloadKind::DriverBlob;
    legacy.legacy_backed_up = true;
    legacy.expected = 5;
    legacy.found = 6;
    REQUIRE(CE::CacheRejectLine(legacy) ==
            "Nx2Cache reject kind=driver_blob reason=legacy_format expected=5 found=6 "
            "action=backup_legacy");
}

TEST_CASE("CacheEnvelope: a flipped header byte is always a rejection with the documented reason",
          "[video_core]") {
    TempDir dir;
    const auto path = dir.path / "cache.bin";
    const auto identity = MakeIdentity();
    const auto bytes = Assemble(FromRecords(identity, ThreeRecords()));
    for (size_t at = 0; at < 160; ++at) {
        INFO("byte " << at);
        auto flipped = bytes;
        flipped[at] ^= 0x01;
        const auto result = OpenBytes(path, flipped, identity);
        REQUIRE_FALSE(result.accepted);
        if (at < 8) {
            REQUIRE(result.reason == CE::RejectReason::BadMagic);
        } else if (at < 12) {
            REQUIRE(result.reason == CE::RejectReason::FormatVersion);
        } else if (at < 16) {
            REQUIRE(result.reason == CE::RejectReason::HeaderLength);
        } else {
            // Every other field, the reserved ones and the checksums included.
            REQUIRE(result.reason == CE::RejectReason::BadChecksum);
        }
    }
}

TEST_CASE("CacheEnvelope: a file cut at any length is truncated, never half loaded",
          "[video_core]") {
    TempDir dir;
    const auto path = dir.path / "cache.bin";
    const auto identity = MakeIdentity();
    const auto bytes = Assemble(FromRecords(identity, ThreeRecords()));
    REQUIRE(OpenBytes(path, bytes, identity).accepted);
    for (size_t length = 0; length < bytes.size(); ++length) {
        INFO("length " << length);
        const std::vector<u8> cut(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(length));
        const auto result = OpenBytes(path, cut, identity);
        REQUIRE_FALSE(result.accepted);
        REQUIRE(result.reason == CE::RejectReason::Truncated);
    }
    auto longer = bytes;
    longer.push_back(0);
    REQUIRE(OpenBytes(path, longer, identity).reason == CE::RejectReason::Truncated);
}

TEST_CASE("CacheEnvelope: a file with another magic is rejected without a backup", "[video_core]") {
    TempDir dir;
    const auto path = dir.path / "cache.bin";
    const auto identity = MakeIdentity();
    WriteText(path, "yuzu");
    REQUIRE(TryOpen(path, identity).result.reason == CE::RejectReason::BadMagic);
    WriteText(path, "garbage that is long enough to be a header but is not one......");
    const auto result = TryOpen(path, identity).result;
    REQUIRE(result.reason == CE::RejectReason::BadMagic);
    REQUIRE_FALSE(result.legacy_backed_up);
    REQUIRE(fs::exists(path));
    // An empty file and the start of a real magic are short, not foreign.
    WriteText(path, "");
    REQUIRE(TryOpen(path, identity).result.reason == CE::RejectReason::Truncated);
    WriteText(path, "EGL0");
    REQUIRE(TryOpen(path, identity).result.reason == CE::RejectReason::Truncated);
    REQUIRE(TryOpen(dir.path / "nothing.bin", identity).result.reason == CE::RejectReason::Missing);
    REQUIRE_FALSE(fs::exists(dir.path / "nothing.bin"));
}

TEST_CASE("CacheEnvelope: an old file is moved aside once", "[video_core]") {
    TempDir dir;
    const auto path = dir.path / "vulkan.bin";
    const auto backup = fs::path(path).concat(".legacy.bak");
    const auto identity = MakeIdentity();
    for (const char* magic : {"yuzucach", "yuzuvkch"}) {
        INFO(magic);
        // Short as the old warm file's twelve byte header, and long as a cache.
        for (const size_t extra : {size_t{4}, size_t{4000}}) {
            std::string old = magic;
            old.append(extra, 'x');
            WriteText(path, old);
            // An older backup is replaced.
            WriteText(backup, "older backup");
            const auto result = TryOpen(path, identity).result;
            REQUIRE_FALSE(result.accepted);
            REQUIRE(result.reason == CE::RejectReason::LegacyFormat);
            REQUIRE(result.legacy_backed_up);
            REQUIRE(CE::CacheRejectLine(result).ends_with("action=backup_legacy"));
            REQUIRE_FALSE(fs::exists(path));
            REQUIRE(ReadFile(backup) == std::vector<u8>(old.begin(), old.end()));
            // The next open finds nothing to reject and nothing to back up again.
            REQUIRE(TryOpen(path, identity).result.reason == CE::RejectReason::Missing);
        }
    }
    // Exactly the magic and nothing else.
    WriteText(path, "yuzucach");
    REQUIRE(TryOpen(path, identity).result.reason == CE::RejectReason::LegacyFormat);
}

TEST_CASE("CacheEnvelope: the header's sizes are checked before anything is allocated",
          "[video_core]") {
    TempDir dir;
    const auto path = dir.path / "cache.bin";
    const auto identity = MakeIdentity(CE::PayloadKind::WarmCompute);
    const auto records = ThreeRecords();
    REQUIRE(OpenBytes(path, Assemble(FromRecords(identity, records)), identity).accepted);

    const auto with = [&](auto&& change) {
        auto raw = FromRecords(identity, records);
        change(raw);
        return OpenBytes(path, Assemble(raw), identity);
    };
    // A file the kind may never hold: too large, however small the file really is.
    REQUIRE(with([](Raw& r) { r.payload_bytes = CE::MaxWarmBytes; }).reason ==
            CE::RejectReason::TooLarge);
    REQUIRE(with([](Raw& r) { r.payload_bytes = ~u64{}; r.index_bytes = 48; }).reason ==
            CE::RejectReason::TooLarge);
    REQUIRE(with([](Raw& r) { r.payload_bytes = ~u64{}; r.index_bytes = ~u64{}; }).reason ==
            CE::RejectReason::TooLarge);
    REQUIRE(with([](Raw& r) { r.index_bytes = ~u64{} - 100; }).reason ==
            CE::RejectReason::TooLarge);
    // Inside the limit but not the file's size.
    REQUIRE(with([](Raw& r) { r.payload_bytes = 61; }).reason == CE::RejectReason::Truncated);
    REQUIRE(with([](Raw& r) { r.payload_bytes = 59; }).reason == CE::RejectReason::Truncated);
    // Sizes that are the file's size but not an index.
    REQUIRE(with([](Raw& r) { r.record_count = 2; }).reason == CE::RejectReason::BadIndex);
    REQUIRE(with([](Raw& r) { r.record_count = 4; }).reason == CE::RejectReason::BadIndex);
    REQUIRE(with([](Raw& r) { r.record_count = ~u64{}; }).reason == CE::RejectReason::BadIndex);
    REQUIRE(with([](Raw& r) { r.payload_offset = 161; }).reason == CE::RejectReason::BadIndex);
    REQUIRE(with([](Raw& r) { r.index_offset = 221; }).reason == CE::RejectReason::BadIndex);
    // The sum is right (payload 59 + index 145 of a file that has 60 + 144) only for the total.
    REQUIRE(with([](Raw& r) { r.payload_bytes = 59; r.index_bytes = 145; }).reason ==
            CE::RejectReason::BadIndex);
    REQUIRE(with([](Raw& r) { r.version = 2; }).reason == CE::RejectReason::FormatVersion);
    REQUIRE(with([](Raw& r) { r.header_length = 161; }).reason == CE::RejectReason::HeaderLength);
    REQUIRE(with([](Raw& r) { r.algorithm = 2; }).reason == CE::RejectReason::BadChecksum);
}

TEST_CASE("CacheEnvelope: an index with more entries than the limit is refused unread",
          "[video_core]") {
    TempDir dir;
    const auto path = dir.path / "cache.bin";
    const auto identity = MakeIdentity(CE::PayloadKind::WarmCompute);
    constexpr u64 too_many = CE::MaxIndexBytes / 48 + 1;
    Raw raw;
    raw.identity = identity;
    // A file of the size the header states, with zeros where the index would be.
    // The first assembly has no index; the header is then patched to state the oversized one.
    raw.record_count = too_many;
    raw.index_bytes = too_many * 48;
    raw.index_checksum = 0;
    std::vector<u8> bytes = Assemble(raw);
    bytes.resize(160 + too_many * 48);
    REQUIRE(OpenBytes(path, bytes, identity).reason == CE::RejectReason::BadIndex);

    // The same count at the limit would be consistent: the last record that fits.
    Raw at_limit;
    at_limit.identity = identity;
    constexpr u64 fits = CE::MaxIndexBytes / 48;
    at_limit.entries.assign(fits, Raw::Entry{});
    // Entries here are invalid (zero length), so the file is bad but not for its size.
    REQUIRE(OpenBytes(path, Assemble(at_limit), identity).reason == CE::RejectReason::BadIndex);
}

TEST_CASE("CacheEnvelope: every index entry is bounded", "[video_core]") {
    TempDir dir;
    const auto path = dir.path / "cache.bin";
    const auto identity = MakeIdentity();
    const auto records = ThreeRecords();
    const auto with = [&](auto&& change) {
        auto raw = FromRecords(identity, records);
        change(raw);
        return OpenBytes(path, Assemble(raw), identity);
    };
    REQUIRE(with([](Raw&) {}).accepted);
    REQUIRE(with([](Raw& r) { r.entries[1].kind = 0; }).reason == CE::RejectReason::BadIndex);
    REQUIRE(with([](Raw& r) { r.entries[1].kind = 4; }).reason == CE::RejectReason::BadIndex);
    REQUIRE(with([](Raw& r) { r.entries[1].phase = 2; }).reason == CE::RejectReason::BadIndex);
    // A gap, an overlap and an offset that would overflow when added to the length.
    REQUIRE(with([](Raw& r) { r.entries[1].offset += 1; }).reason == CE::RejectReason::BadIndex);
    REQUIRE(with([](Raw& r) { r.entries[1].offset -= 1; }).reason == CE::RejectReason::BadIndex);
    REQUIRE(with([](Raw& r) { r.entries[1].offset = ~u64{} - 3; }).reason ==
            CE::RejectReason::BadIndex);
    REQUIRE(with([](Raw& r) { r.entries[0].offset = 1; }).reason == CE::RejectReason::BadIndex);
    // Lengths: zero, past the payload, and a total that does not reach its end.
    REQUIRE(with([](Raw& r) { r.entries[2].length = 0; }).reason == CE::RejectReason::BadIndex);
    REQUIRE(with([](Raw& r) { r.entries[2].length += 1; }).reason == CE::RejectReason::BadIndex);
    REQUIRE(with([](Raw& r) { r.entries[2].length -= 1; }).reason == CE::RejectReason::BadIndex);
    REQUIRE(with([](Raw& r) { r.entries[2].length = ~u32{}; }).reason ==
            CE::RejectReason::BadIndex);
    // The index checksum is the last check: a wrong one is only a checksum failure when the
    // index is otherwise valid.
    REQUIRE(with([](Raw& r) { r.index_checksum = 1; }).reason == CE::RejectReason::BadChecksum);
    REQUIRE(with([](Raw& r) { r.entries[1].kind = 0; r.index_checksum = 1; }).reason ==
            CE::RejectReason::BadIndex);
}

TEST_CASE("CacheEnvelope: a corrupt record is a failed read of that record only", "[video_core]") {
    TempDir dir;
    const auto path = dir.path / "cache.bin";
    const auto identity = MakeIdentity();
    WriteGeneration(path, identity, ThreeRecords());
    auto bytes = ReadFile(path);
    bytes[160 + 20 + 5] ^= 0x40; // Inside the second record's payload
    WriteFile(path, bytes);
    auto opened = TryOpen(path, identity);
    REQUIRE(opened.reader);
    std::vector<u8> payload;
    REQUIRE(opened.reader->ReadRecord(0, payload) == CE::RejectReason::None);
    REQUIRE(opened.reader->ReadRecord(1, payload) == CE::RejectReason::BadChecksum);
    REQUIRE(payload.empty());
    REQUIRE(opened.reader->ReadRecord(2, payload) == CE::RejectReason::None);

    // The index is covered too: one flipped bit anywhere in it rejects the file.
    const size_t index_start = 160 + 60;
    for (size_t at = index_start; at < bytes.size(); ++at) {
        INFO("byte " << at);
        auto flipped = bytes;
        flipped[at] ^= 0x01;
        WriteFile(path, flipped);
        const auto result = TryOpen(path, identity).result;
        REQUIRE_FALSE(result.accepted);
    }
}

TEST_CASE("CacheEnvelope: a record may be as large as MaxRecordBytes and not larger",
          "[video_core]") {
    TempDir dir;
    const auto path = dir.path / "cache.bin";
    const auto identity = MakeIdentity(CE::PayloadKind::WarmCompute);
    {
        auto writer = CE::EnvelopeWriter::Create(path, identity, CE::MaxWarmBytes);
        REQUIRE(writer);
        std::vector<u8> big(CE::MaxRecordBytes + 1, 0x5a);
        CE::RecordMeta meta;
        meta.kind = CE::RecordKind::Opaque;
        REQUIRE(writer->Append(meta, big) == CE::AppendResult::TooLarge);
        REQUIRE(writer->Records() == 0);
        big.pop_back();
        REQUIRE(writer->Append(meta, big) == CE::AppendResult::Ok);
        REQUIRE(writer->Commit());
    }
    auto opened = TryOpen(path, identity);
    REQUIRE(opened.reader);
    REQUIRE(opened.reader->Index()[0].length == CE::MaxRecordBytes);
    std::vector<u8> payload;
    REQUIRE(opened.reader->ReadRecord(0, payload) == CE::RejectReason::None);
    REQUIRE(payload.size() == CE::MaxRecordBytes);

    // A file that claims one byte more is rejected by the reader as well.
    Raw raw;
    raw.identity = identity;
    raw.payload.assign(CE::MaxRecordBytes + 1, 0x5a);
    Raw::Entry entry;
    entry.kind = 3;
    entry.length = static_cast<u32>(raw.payload.size());
    entry.checksum = CE::Checksum(raw.payload);
    raw.entries.push_back(entry);
    REQUIRE(OpenBytes(path, Assemble(raw), identity).reason == CE::RejectReason::BadIndex);
}

TEST_CASE("CacheEnvelope: the writer refuses duplicates, empty records and records past its budget",
          "[video_core]") {
    TempDir dir;
    const auto path = dir.path / "cache.bin";
    const auto identity = MakeIdentity(CE::PayloadKind::WarmCompute);
    const auto payload = Payload(100, 4);
    CE::RecordMeta compute;
    compute.kind = CE::RecordKind::Compute;
    CE::RecordMeta graphics;
    graphics.kind = CE::RecordKind::Graphics;

    // 160 + 100 + 48 is exactly the budget: the record fits, one byte more does not.
    {
        auto writer = CE::EnvelopeWriter::Create(path, identity, 160 + 100 + 48);
        REQUIRE(writer);
        REQUIRE(writer->Append(compute, payload) == CE::AppendResult::Ok);
        REQUIRE(writer->Bytes() == 160 + 100 + 48);
        REQUIRE(writer->Append(compute, payload) == CE::AppendResult::Duplicate);
        // Another kind with the same bytes is another record, but the budget is full.
        REQUIRE(writer->Append(graphics, payload) == CE::AppendResult::BudgetFull);
        REQUIRE(writer->Append(compute, Payload(1, 9)) == CE::AppendResult::BudgetFull);
        REQUIRE(writer->Append(compute, {}) == CE::AppendResult::IoError);
        REQUIRE(writer->Records() == 1);
        REQUIRE(writer->Commit());
        REQUIRE(fs::file_size(path) == 160 + 100 + 48);
    }
    {
        auto writer = CE::EnvelopeWriter::Create(path, identity, 160 + 100 + 47);
        REQUIRE(writer);
        REQUIRE(writer->Append(compute, payload) == CE::AppendResult::BudgetFull);
        REQUIRE(writer->Commit()); // An empty generation is still a generation
        REQUIRE(fs::file_size(path) == 160);
    }
    {
        auto writer = CE::EnvelopeWriter::Create(path, identity, 160 + 1000);
        REQUIRE(writer);
        REQUIRE(writer->Append(compute, payload) == CE::AppendResult::Ok);
        REQUIRE(writer->Append(graphics, payload) == CE::AppendResult::Ok);
        auto other = payload;
        other[99] ^= 1; // Same length, other checksum
        REQUIRE(writer->Append(compute, other) == CE::AppendResult::Ok);
        REQUIRE(writer->Append(compute, Payload(99, 4)) == CE::AppendResult::Ok);
        REQUIRE(writer->Records() == 4);
        REQUIRE(writer->Commit());
        REQUIRE_FALSE(writer->Commit());
        REQUIRE(writer->Append(compute, Payload(5, 7)) == CE::AppendResult::IoError);
    }
}

TEST_CASE("CacheEnvelope: the limits of the kind and of the index hold", "[video_core]") {
    TempDir dir;
    const auto path = dir.path / "cache.bin";
    const auto identity = MakeIdentity(CE::PayloadKind::WarmCompute);
    CE::RecordMeta meta;
    meta.kind = CE::RecordKind::Compute;
    {
        // A budget above the kind's limit is the kind's limit: three records of the largest size
        // fit in 64 MiB and the fourth does not.
        auto writer = CE::EnvelopeWriter::Create(path, identity, ~u64{});
        REQUIRE(writer);
        std::vector<u8> big(CE::MaxRecordBytes, 0);
        for (u8 i = 0; i < 3; ++i) {
            big[0] = i;
            REQUIRE(writer->Append(meta, big) == CE::AppendResult::Ok);
        }
        big[0] = 3;
        REQUIRE(writer->Append(meta, big) == CE::AppendResult::BudgetFull);
        REQUIRE(writer->Bytes() <= CE::MaxWarmBytes);
    }
    REQUIRE_FALSE(fs::exists(path)); // Never committed
    {
        // The index holds MaxIndexBytes / 48 records and the next is refused.
        constexpr u32 fits = static_cast<u32>(CE::MaxIndexBytes / 48);
        auto writer = CE::EnvelopeWriter::Create(path, identity, CE::MaxWarmBytes);
        REQUIRE(writer);
        for (u32 i = 0; i < fits; ++i) {
            REQUIRE(writer->Append(meta, {reinterpret_cast<const u8*>(&i), sizeof(i)}) ==
                    CE::AppendResult::Ok);
        }
        const u32 next = fits;
        REQUIRE(writer->Append(meta, {reinterpret_cast<const u8*>(&next), sizeof(next)}) ==
                CE::AppendResult::BudgetFull);
        REQUIRE(writer->Commit());
        auto opened = TryOpen(path, identity);
        REQUIRE(opened.reader);
        REQUIRE(opened.result.records == fits);
        std::vector<u8> payload;
        REQUIRE(opened.reader->ReadRecord(fits - 1, payload) == CE::RejectReason::None);
        u32 last;
        std::memcpy(&last, payload.data(), sizeof(last));
        REQUIRE(last == fits - 1);
    }
}

TEST_CASE("CacheEnvelope: a commit replaces the old generation in one step, readers open or not",
          "[video_core]") {
    TempDir dir;
    const auto path = dir.path / "cache.bin";
    const auto tmp = fs::path(path).concat(".tmp");
    const auto identity = MakeIdentity();
    WriteGeneration(path, identity, ThreeRecords());

    // A reader of the old generation stays open while the next one is seeded from it and replaces
    // it: Windows must not refuse the replace because the reader holds the file.
    auto old_reader = TryOpen(path, identity);
    REQUIRE(old_reader.reader);
    auto writer = CE::EnvelopeWriter::Create(path, identity, CE::MaxTransferableBytes);
    REQUIRE(writer);
    REQUIRE(writer->Seed(*old_reader.reader) == 3);
    REQUIRE(writer->Seed(*old_reader.reader) == 0); // Once
    CE::RecordMeta meta;
    meta.kind = CE::RecordKind::Graphics;
    meta.first_use_ordinal = 77;
    REQUIRE(writer->Append(meta, Payload(11, 8)) == CE::AppendResult::Ok);
    REQUIRE(writer->Append(ThreeRecords()[0].meta, ThreeRecords()[0].bytes) ==
            CE::AppendResult::Duplicate);

    // Before the commit the old file is the file.
    REQUIRE(fs::exists(tmp));
    REQUIRE(TryOpen(path, identity).result.records == 3);
    REQUIRE(writer->Commit());
    REQUIRE_FALSE(fs::exists(tmp));

    auto replaced = TryOpen(path, identity);
    REQUIRE(replaced.reader);
    REQUIRE(replaced.result.records == 4);
    std::vector<u8> payload;
    REQUIRE(replaced.reader->ReadRecord(3, payload) == CE::RejectReason::None);
    REQUIRE(payload == Payload(11, 8));
    REQUIRE(replaced.reader->Index()[3].meta.first_use_ordinal == 77);
    // The records carried over keep their order and index fields.
    const auto first = ThreeRecords();
    for (size_t i = 0; i < 3; ++i) {
        REQUIRE(replaced.reader->ReadRecord(i, payload) == CE::RejectReason::None);
        REQUIRE(payload == first[i].bytes);
        REQUIRE(replaced.reader->Index()[i].meta.first_use_ns == first[i].meta.first_use_ns);
        REQUIRE(replaced.reader->Index()[i].meta.kind == first[i].meta.kind);
    }
}

TEST_CASE("CacheEnvelope: seeding drops a record that no longer reads back", "[video_core]") {
    TempDir dir;
    const auto path = dir.path / "cache.bin";
    const auto identity = MakeIdentity();
    WriteGeneration(path, identity, ThreeRecords());
    auto bytes = ReadFile(path);
    bytes[160 + 20 + 5] ^= 0x40; // The second record
    WriteFile(path, bytes);
    auto opened = TryOpen(path, identity);
    REQUIRE(opened.reader);

    const auto next = dir.path / "next.bin";
    auto writer = CE::EnvelopeWriter::Create(next, identity, CE::MaxTransferableBytes);
    REQUIRE(writer);
    REQUIRE(writer->Seed(*opened.reader) == 2);
    REQUIRE(writer->Commit());
    auto reopened = TryOpen(next, identity);
    REQUIRE(reopened.reader);
    REQUIRE(reopened.result.records == 2);
    const auto records = ThreeRecords();
    std::vector<u8> payload;
    REQUIRE(reopened.reader->ReadRecord(0, payload) == CE::RejectReason::None);
    REQUIRE(payload == records[0].bytes);
    REQUIRE(reopened.reader->ReadRecord(1, payload) == CE::RejectReason::None);
    REQUIRE(payload == records[2].bytes);

    // Seeding after an append does nothing.
    auto late = CE::EnvelopeWriter::Create(dir.path / "late.bin", identity, 1 << 20);
    REQUIRE(late->Append(records[0].meta, records[0].bytes) == CE::AppendResult::Ok);
    REQUIRE(late->Seed(*reopened.reader) == 0);
    REQUIRE(late->Records() == 1);
}

TEST_CASE("CacheEnvelope: an uncommitted or stale temporary file is never loaded and is removed",
          "[video_core]") {
    TempDir dir;
    const auto path = dir.path / "cache.bin";
    const auto tmp = fs::path(path).concat(".tmp");
    const auto identity = MakeIdentity();

    // A temporary file left by a killed run: not a cache, deleted at the next start.
    WriteGeneration(dir.path / "other.bin", identity, ThreeRecords());
    fs::copy_file(dir.path / "other.bin", tmp);
    REQUIRE(TryOpen(path, identity).result.reason == CE::RejectReason::Missing);
    CE::RemoveStaleTemp(path);
    REQUIRE_FALSE(fs::exists(tmp));
    CE::RemoveStaleTemp(path); // Nothing there is fine

    WriteText(tmp, "stale");
    {
        auto writer = CE::EnvelopeWriter::Create(path, identity, CE::MaxTransferableBytes);
        REQUIRE(writer);
        REQUIRE(ReadFile(tmp) != std::vector<u8>{'s', 't', 'a', 'l', 'e'}); // The stale file is gone
    }
    REQUIRE_FALSE(fs::exists(tmp)); // Abandoned
    REQUIRE_FALSE(fs::exists(path));

    // An abandoned generation leaves the previous file alone.
    WriteGeneration(path, identity, ThreeRecords());
    const auto before = ReadFile(path);
    {
        auto writer = CE::EnvelopeWriter::Create(path, identity, CE::MaxTransferableBytes);
        REQUIRE(writer);
        REQUIRE(writer->Append(ThreeRecords()[0].meta, Payload(5, 1)) == CE::AppendResult::Ok);
    }
    REQUIRE(ReadFile(path) == before);
    REQUIRE_FALSE(fs::exists(tmp));
}

TEST_CASE("CacheEnvelope: a commit that fails keeps the previous file and leaves no temporary",
          "[video_core]") {
    TempDir dir;
    const auto identity = MakeIdentity();
    const auto records = ThreeRecords();

    SECTION("the temporary file cannot be created") {
        const auto path = dir.path / "cache.bin";
        WriteGeneration(path, identity, records);
        const auto before = ReadFile(path);
        // A directory with content where the temporary file goes: it cannot be removed or opened.
        const auto tmp = fs::path(path).concat(".tmp");
        fs::create_directories(tmp);
        WriteText(tmp / "inside", "x");
        REQUIRE_FALSE(CE::EnvelopeWriter::Create(path, identity, CE::MaxTransferableBytes));
        REQUIRE(ReadFile(path) == before);
        // Nor in a directory that does not exist.
        REQUIRE_FALSE(CE::EnvelopeWriter::Create(dir.path / "missing" / "cache.bin", identity,
                                                 CE::MaxTransferableBytes));
    }
    SECTION("the file cannot be replaced") {
        // The target is a directory: the final rename fails on every platform.
        const auto path = dir.path / "cache.bin";
        fs::create_directories(path);
        WriteText(path / "inside", "x");
        auto writer = CE::EnvelopeWriter::Create(path, identity, CE::MaxTransferableBytes);
        REQUIRE(writer);
        REQUIRE(writer->Append(records[0].meta, records[0].bytes) == CE::AppendResult::Ok);
        REQUIRE_FALSE(writer->Commit());
        REQUIRE_FALSE(fs::exists(fs::path(path).concat(".tmp")));
        REQUIRE(fs::is_directory(path));
        REQUIRE(fs::exists(path / "inside"));
        // The writer is finished.
        REQUIRE(writer->Append(records[1].meta, records[1].bytes) == CE::AppendResult::IoError);
        REQUIRE_FALSE(writer->Commit());
    }
#ifdef _WIN32
    SECTION("the old file is held open by another process") {
        // A handle without delete sharing keeps Windows from replacing the file.
        const auto path = dir.path / "cache.bin";
        WriteGeneration(path, identity, records);
        const auto before = ReadFile(path);
        {
            std::ifstream holder(path, std::ios::binary);
            REQUIRE(holder.is_open());
            auto writer = CE::EnvelopeWriter::Create(path, identity, CE::MaxTransferableBytes);
            REQUIRE(writer);
            REQUIRE(writer->Append(records[0].meta, Payload(9, 9)) == CE::AppendResult::Ok);
            REQUIRE_FALSE(writer->Commit());
            REQUIRE_FALSE(fs::exists(fs::path(path).concat(".tmp")));
        }
        REQUIRE(ReadFile(path) == before);
        auto opened = TryOpen(path, identity);
        REQUIRE(opened.reader);
        REQUIRE(opened.result.records == 3);
    }
#endif
}

TEST_CASE("CacheEnvelope: records are read from several threads at once", "[video_core]") {
    TempDir dir;
    const auto path = dir.path / "cache.bin";
    const auto identity = MakeIdentity();
    std::vector<Rec> records;
    for (u8 i = 0; i < 40; ++i) {
        Rec record;
        record.meta.kind = CE::RecordKind::Graphics;
        record.bytes = Payload(100 + i * 13, i);
        records.push_back(std::move(record));
    }
    WriteGeneration(path, identity, records);
    auto opened = TryOpen(path, identity);
    REQUIRE(opened.reader);
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    for (size_t t = 0; t < 4; ++t) {
        threads.emplace_back([&, t] {
            std::vector<u8> payload;
            for (size_t round = 0; round < 50; ++round) {
                for (size_t i = 0; i < records.size(); ++i) {
                    const size_t record = (i + t * 7) % records.size();
                    if (opened.reader->ReadRecord(record, payload) != CE::RejectReason::None ||
                        payload != records[record].bytes) {
                        ++failures;
                    }
                }
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    REQUIRE(failures == 0);
}

namespace {

u64 CbufKey(u32 index, u32 offset) {
    return static_cast<u64>(index) << 32 | offset;
}

/// An environment with everything a pipeline records, set directly so that the codec can be
/// tested without a GPU.
class RecordedEnv final : public VideoCommon::GenericEnvironment {
public:
    explicit RecordedEnv(Shader::Stage stage_, u8 seed = 1, size_t words = 6,
                         bool with_tables = true) {
        stage = stage_;
        start_address = 0x30;
        code.resize(words);
        for (size_t i = 0; i < words; ++i) {
            code[i] = 0x0123456789abcdefULL * (i + seed);
        }
        cached_lowest = 0x30;
        cached_highest = 0x30 + static_cast<u32>((words - 1) * 8);
        local_memory_size = 0x44;
        texture_bound = 1;
        viewport_transform_state = 0;
        if (with_tables) {
            texture_types.emplace(1, Shader::TextureType::Color2D);
            texture_types.emplace(0x1234, Shader::TextureType::Color3D);
            texture_pixel_formats.emplace(1, static_cast<Shader::TexturePixelFormat>(16));
            texture_pixel_formats.emplace(0x1234, static_cast<Shader::TexturePixelFormat>(3));
            cbuf_values.emplace(CbufKey(0, 0x40), 0xdeadbeefU);
            cbuf_values.emplace(CbufKey(2, 0x10), seed);
            cbuf_values.emplace(CbufKey(3, 0x28), 0);
            cbuf_values.emplace(CbufKey(3, 0x28) + 4, 0);
            storage_pointer_types.emplace(CbufKey(1, 0x20), 1);
            storage_pointer_types.emplace(CbufKey(3, 0x28), 0);
            cbuf_replacements.emplace(CbufKey(0, 0x100), Shader::ReplaceConstant::BaseVertex);
        }
        auto* header = reinterpret_cast<u8*>(&sph);
        for (size_t i = 0; i < sizeof(sph); ++i) {
            header[i] = static_cast<u8>(seed * 3 + i);
        }
        workgroup_size = {8, 4, 2};
        shared_memory_size = 512;
        for (u32 i = 0; i < 8; ++i) {
            gp_passthrough_mask[i] = 0x1000 + i;
        }
        mesh_registers = {11, 22, 33};
        auto* mesh = reinterpret_cast<u8*>(&mesh_primitive_header);
        for (size_t i = 0; i < sizeof(mesh_primitive_header); ++i) {
            mesh[i] = static_cast<u8>(0x80 + i);
        }
    }

    void MarkUnbound() {
        has_unbound_instructions = true;
    }

    u32 ReadCbufValue(u32, u32) override {
        return 0;
    }
    Shader::TextureType ReadTextureType(u32) override {
        return Shader::TextureType::Color2D;
    }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override {
        return {};
    }
    bool IsTexturePixelFormatInteger(u32) override {
        return false;
    }
    u32 ReadViewportTransformState() override {
        return viewport_transform_state;
    }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override {
        return std::nullopt;
    }
};

std::vector<u8> KeyBytes(u8 seed, size_t size = 10) {
    return Payload(size, seed);
}

std::vector<u8> Encode(std::span<const RecordedEnv* const> envs, std::span<const u8> key,
                       bool with_mesh_state = false) {
    std::vector<const VideoCommon::GenericEnvironment*> list(envs.begin(), envs.end());
    std::vector<u8> out;
    REQUIRE(VideoCommon::EncodePipelineRecord(key, list, with_mesh_state, out));
    return out;
}

CE::RejectReason Decode(std::span<const u8> payload, bool with_mesh_state = false) {
    VideoCommon::PipelineRecord record;
    return VideoCommon::DecodePipelineRecord(payload, with_mesh_state, record);
}

template <typename T>
bool SamePod(const T& a, const T& b) {
    return std::memcmp(&a, &b, sizeof(T)) == 0;
}

/// What a decoded environment answers equals what the recorded one answers.
void RequireSameEnvironment(RecordedEnv& source, VideoCommon::FileEnvironment& decoded,
                            bool with_mesh_state) {
    REQUIRE(decoded.ShaderStage() == source.ShaderStage());
    REQUIRE(decoded.StartAddress() == source.StartAddress());
    REQUIRE(decoded.LocalMemorySize() == source.LocalMemorySize());
    REQUIRE(decoded.TextureBoundBuffer() == source.TextureBoundBuffer());
    REQUIRE(decoded.ReadViewportTransformState() == source.ReadViewportTransformState());
    for (u32 i = 0; i < source.CachedSizeWords(); ++i) {
        REQUIRE(decoded.ReadInstruction(0x30 + i * 8) == source.ReadInstruction(0x30 + i * 8));
    }
    REQUIRE_THROWS_AS(decoded.ReadInstruction(0x30 + static_cast<u32>(source.CachedSizeWords()) * 8),
                      Shader::LogicError);
    REQUIRE_THROWS_AS(decoded.ReadInstruction(0x28), Shader::LogicError);
    if (source.ShaderStage() == Shader::Stage::Compute) {
        REQUIRE(decoded.WorkgroupSize() == source.WorkgroupSize());
        REQUIRE(decoded.SharedMemorySize() == source.SharedMemorySize());
    } else {
        REQUIRE(SamePod(decoded.SPH(), source.SPH()));
        if (source.ShaderStage() == Shader::Stage::Geometry) {
            REQUIRE(decoded.GpPassthroughMask() == source.GpPassthroughMask());
        }
        if (with_mesh_state && (source.ShaderStage() == Shader::Stage::Task ||
                                source.ShaderStage() == Shader::Stage::Mesh)) {
            REQUIRE(decoded.MeshRegisters() == source.MeshRegisters());
            REQUIRE(SamePod(decoded.MeshPrimitiveHeader(), source.MeshPrimitiveHeader()));
        } else {
            REQUIRE(decoded.MeshRegisters() == std::array<u32, 3>{});
        }
    }
    REQUIRE(decoded.ReadTextureType(1) == Shader::TextureType::Color2D);
    REQUIRE(decoded.ReadTextureType(0x1234) == Shader::TextureType::Color3D);
    REQUIRE(decoded.ReadTexturePixelFormat(1) == static_cast<Shader::TexturePixelFormat>(16));
    REQUIRE(decoded.ReadTexturePixelFormat(0x1234) == static_cast<Shader::TexturePixelFormat>(3));
    REQUIRE(decoded.ReadCbufValue(0, 0x40) == 0xdeadbeefU);
    REQUIRE(decoded.IsCbufStorageBufferPointer(1, 0x20));
    REQUIRE_FALSE(decoded.IsCbufStorageBufferPointer(3, 0x28));
    REQUIRE(decoded.NullStoragePointerSlots() == source.NullStoragePointerSlots());
    REQUIRE_FALSE(decoded.NullStoragePointerSlots().empty());
    REQUIRE(decoded.MappedStoragePointerKeys() == source.MappedStoragePointerKeys());
    REQUIRE(decoded.GetReplaceConstBuffer(0, 0x100) == Shader::ReplaceConstant::BaseVertex);
    REQUIRE(decoded.HasHLEMacroState());
    // Nothing that was not recorded is answered.
    REQUIRE_THROWS_AS(decoded.ReadCbufValue(5, 0), Shader::LogicError);
    REQUIRE_THROWS_AS(decoded.ReadTextureType(77), Shader::LogicError);
}

} // Anonymous namespace

TEST_CASE("PipelineRecord: environments and key survive the codec exactly", "[video_core]") {
    const RecordedEnv vertex(Shader::Stage::VertexB, 1);
    const RecordedEnv fragment(Shader::Stage::Fragment, 2, 9);
    const auto key = KeyBytes(5, 37);
    std::array<const RecordedEnv*, 2> envs{&vertex, &fragment};
    const auto payload = Encode(envs, key);

    VideoCommon::PipelineRecord record;
    REQUIRE(VideoCommon::DecodePipelineRecord(payload, false, record) == CE::RejectReason::None);
    REQUIRE(record.envs.size() == 2);
    REQUIRE(record.key == key);
    REQUIRE_FALSE(record.is_compute);
    auto source_vertex = RecordedEnv(Shader::Stage::VertexB, 1);
    auto source_fragment = RecordedEnv(Shader::Stage::Fragment, 2, 9);
    RequireSameEnvironment(source_vertex, record.envs[0], false);
    RequireSameEnvironment(source_fragment, record.envs[1], false);
    REQUIRE(Decode(payload) == CE::RejectReason::None);
}

TEST_CASE("PipelineRecord: compute, geometry and mesh state each travel with their stage",
          "[video_core]") {
    {
        RecordedEnv source(Shader::Stage::Compute, 3);
        const auto key = KeyBytes(1, 24);
        const std::array<const RecordedEnv*, 1> envs{&source};
        const auto payload = Encode(envs, key);
        VideoCommon::PipelineRecord record;
        REQUIRE(VideoCommon::DecodePipelineRecord(payload, false, record) ==
                CE::RejectReason::None);
        REQUIRE(record.is_compute);
        REQUIRE(record.key == key);
        RequireSameEnvironment(source, record.envs[0], false);
    }
    {
        RecordedEnv source(Shader::Stage::Geometry, 4);
        const std::array<const RecordedEnv*, 1> envs{&source};
        const auto payload = Encode(envs, KeyBytes(2));
        VideoCommon::PipelineRecord record;
        REQUIRE(VideoCommon::DecodePipelineRecord(payload, false, record) ==
                CE::RejectReason::None);
        RequireSameEnvironment(source, record.envs[0], false);
    }
    for (const auto stage : {Shader::Stage::Task, Shader::Stage::Mesh}) {
        RecordedEnv source(stage, 5);
        const std::array<const RecordedEnv*, 1> envs{&source};
        const auto plain = Encode(envs, KeyBytes(3), false);
        const auto mesh = Encode(envs, KeyBytes(3), true);
        REQUIRE(mesh.size() == plain.size() + 3 * 4 + sizeof(source.MeshPrimitiveHeader()));
        VideoCommon::PipelineRecord record;
        REQUIRE(VideoCommon::DecodePipelineRecord(mesh, true, record) == CE::RejectReason::None);
        RequireSameEnvironment(source, record.envs[0], true);
        REQUIRE(VideoCommon::DecodePipelineRecord(plain, false, record) ==
                CE::RejectReason::None);
        // A record written with the mesh state is not one without it, and the other way round.
        REQUIRE(Decode(mesh, false) == CE::RejectReason::BadRecord);
        REQUIRE(Decode(plain, true) == CE::RejectReason::BadRecord);
    }
}

TEST_CASE("PipelineRecord: what cannot be serialized is refused and leaves the output alone",
          "[video_core]") {
    const RecordedEnv good(Shader::Stage::Compute, 1);
    RecordedEnv unbound(Shader::Stage::Compute, 1);
    unbound.MarkUnbound();
    const auto key = KeyBytes(1);
    const std::vector<u8> sentinel{1, 2, 3};

    const auto refused = [&](std::vector<const VideoCommon::GenericEnvironment*> envs) {
        std::vector<u8> out = sentinel;
        const bool ok = VideoCommon::EncodePipelineRecord(key, envs, false, out);
        return !ok && out == sentinel;
    };
    REQUIRE(refused({}));
    REQUIRE(refused({&unbound}));
    REQUIRE(refused({&good, &unbound}));
    REQUIRE(refused({&good, nullptr}));
    REQUIRE(refused(std::vector<const VideoCommon::GenericEnvironment*>(9, &good)));
    std::vector<u8> out;
    REQUIRE(VideoCommon::EncodePipelineRecord(
        key, std::vector<const VideoCommon::GenericEnvironment*>(8, &good), false, out));
    VideoCommon::PipelineRecord record;
    REQUIRE(VideoCommon::DecodePipelineRecord(out, false, record) == CE::RejectReason::None);
    REQUIRE(record.envs.size() == 8);
}

TEST_CASE("PipelineRecord: a payload of exactly MaxRecordBytes is encoded and one byte more is not",
          "[video_core]") {
    // count, length, environment (76 byte header, code, 16 byte compute trailer), key length, key.
    constexpr u64 fixed = 4 + 4 + 76 + 16 + 4;
    const std::vector<u8> key(8, 0x77);
    const u64 code_bytes = CE::MaxRecordBytes - fixed - key.size();
    REQUIRE(code_bytes % 8 == 0);
    const RecordedEnv exact(Shader::Stage::Compute, 1, code_bytes / 8, false);
    std::vector<u8> out;
    std::vector<const VideoCommon::GenericEnvironment*> envs{&exact};
    REQUIRE(VideoCommon::EncodePipelineRecord(key, envs, false, out));
    REQUIRE(out.size() == CE::MaxRecordBytes);
    REQUIRE(Decode(out) == CE::RejectReason::None);

    const RecordedEnv over(Shader::Stage::Compute, 1, code_bytes / 8 + 1, false);
    envs = {&over};
    const std::vector<u8> sentinel{4, 5};
    out = sentinel;
    REQUIRE_FALSE(VideoCommon::EncodePipelineRecord(key, envs, false, out));
    REQUIRE(out == sentinel);
    // The key alone can push a record over.
    envs = {&exact};
    REQUIRE_FALSE(VideoCommon::EncodePipelineRecord(std::vector<u8>(9, 1), envs, false, out));
    REQUIRE(out == sentinel);
}

TEST_CASE("PipelineRecord: a damaged record is bad_record at every cut and every count",
          "[video_core]") {
    const RecordedEnv vertex(Shader::Stage::VertexB, 1);
    const RecordedEnv fragment(Shader::Stage::Fragment, 2);
    const std::array<const RecordedEnv*, 2> envs{&vertex, &fragment};
    const auto payload = Encode(envs, KeyBytes(5, 21));
    REQUIRE(Decode(payload) == CE::RejectReason::None);

    // Cut at every length (the reader never allocates for a count it cannot back with bytes).
    for (size_t length = 0; length < payload.size(); ++length) {
        INFO("length " << length);
        REQUIRE(Decode(std::span<const u8>(payload.data(), length)) == CE::RejectReason::BadRecord);
    }
    auto longer = payload;
    longer.push_back(0);
    REQUIRE(Decode(longer) == CE::RejectReason::BadRecord);

    // Counts and lengths of the record itself.
    for (const u32 count : {0U, 9U, 0xffffffffU, 3U}) {
        INFO("env count " << count);
        auto bad = payload;
        PutAt<u32>(bad, 0, count);
        REQUIRE(Decode(bad) == CE::RejectReason::BadRecord);
    }
    const u32 first_length = GetAt<u32>(payload, 4);
    for (const u32 length : {0U, first_length - 1, first_length + 1, 0xffffffffU,
                             static_cast<u32>(payload.size())}) {
        INFO("env length " << length);
        auto bad = payload;
        PutAt<u32>(bad, 4, length);
        REQUIRE(Decode(bad) == CE::RejectReason::BadRecord);
    }
    const size_t key_length_at = payload.size() - 21 - 4;
    REQUIRE(GetAt<u32>(payload, key_length_at) == 21);
    for (const u32 length : {0U, 20U, 22U, 0xffffffffU}) {
        INFO("key length " << length);
        auto bad = payload;
        PutAt<u32>(bad, key_length_at, length);
        REQUIRE(Decode(bad) == CE::RejectReason::BadRecord);
    }

    // The failed decodes leave the output as it was.
    VideoCommon::PipelineRecord record;
    record.key = {9, 9};
    record.is_compute = true;
    REQUIRE(VideoCommon::DecodePipelineRecord(longer, false, record) == CE::RejectReason::BadRecord);
    REQUIRE(record.key == std::vector<u8>{9, 9});
    REQUIRE(record.is_compute);
    REQUIRE(record.envs.empty());
}

TEST_CASE("PipelineRecord: the counts and values inside an environment are checked", "[video_core]") {
    // Two texture types and pixel formats, four constant values, two storage pointers, one
    // replacement, six words of code.
    RecordedEnv with_tables(Shader::Stage::Compute, 1, 6, true);
    const auto key = KeyBytes(1, 24);
    const std::array<const RecordedEnv*, 1> envs{&with_tables};
    const auto payload = Encode(envs, key);
    REQUIRE(Decode(payload) == CE::RejectReason::None);
    constexpr size_t env = 8; // After the record's count and the environment's length

    const auto patched_u64 = [&](size_t at, u64 value) {
        auto bad = payload;
        PutAt<u64>(bad, env + at, value);
        return Decode(bad);
    };
    const auto patched_u32 = [&](size_t at, u32 value) {
        auto bad = payload;
        PutAt<u32>(bad, env + at, value);
        return Decode(bad);
    };
    // Six counts. Each far past the bytes that remain, the largest value that still multiplies
    // without overflow, and a value whose product with the entry size wraps.
    for (const size_t at : {size_t{0}, size_t{8}, size_t{16}, size_t{24}, size_t{32}, size_t{40}}) {
        INFO("count at " << at);
        for (const u64 value : {u64{1} << 62, ~u64{}, u64{1} << 61, u64{1} << 40, u64{1} << 24,
                                u64{0x2000000000000001ULL}}) {
            INFO("value " << value);
            REQUIRE(patched_u64(at, value) == CE::RejectReason::BadRecord);
        }
    }
    // A count one larger than the bytes behind it.
    REQUIRE(patched_u64(8, 3) == CE::RejectReason::BadRecord);
    REQUIRE(patched_u64(0, GetAt<u64>(payload, env) + 8) == CE::RejectReason::BadRecord);
    REQUIRE(patched_u64(0, GetAt<u64>(payload, env) - 8) == CE::RejectReason::BadRecord);
    // The stage, and the range of addresses the code must cover.
    REQUIRE(patched_u32(72, 9) == CE::RejectReason::BadRecord);
    REQUIRE(patched_u32(72, 0xffffffffU) == CE::RejectReason::BadRecord);
    REQUIRE(patched_u32(64, 0x30 + 6 * 8) == CE::RejectReason::BadRecord);  // read_highest, 1 word past
    REQUIRE(patched_u32(64, 0x30 + 5 * 8) == CE::RejectReason::None);       // the last word
    REQUIRE(patched_u32(64, 0x2f) == CE::RejectReason::BadRecord);          // below read_lowest
    REQUIRE(patched_u32(60, 0x31 + 5 * 8) == CE::RejectReason::BadRecord);  // lowest above highest
    // The table values: 8 bytes per texture type and pixel format entry, 12 per constant value.
    const size_t tables = 76 + 6 * 8;
    // texture types: two entries, each key then type
    REQUIRE(patched_u32(tables + 4, 9) == CE::RejectReason::BadRecord);
    REQUIRE(patched_u32(tables + 4, 8) == CE::RejectReason::None);
    REQUIRE(patched_u32(tables + 4, 0xffffffffU) == CE::RejectReason::BadRecord);
    // pixel formats: after the two texture entries
    REQUIRE(patched_u32(tables + 16 + 4, 0xffffffffU) == CE::RejectReason::BadRecord);
    REQUIRE(patched_u32(tables + 16 + 4, 16) == CE::RejectReason::None);
    // replacement constants: after 2 + 2 entries of 8, 4 of 12 and 2 of 9
    const size_t replacements = tables + 16 + 16 + 4 * 12 + 2 * 9;
    REQUIRE(patched_u32(replacements + 8, 3) == CE::RejectReason::BadRecord);
    REQUIRE(patched_u32(replacements + 8, 2) == CE::RejectReason::None);
}
