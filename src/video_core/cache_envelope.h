// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/common_types.h"

// The cache file envelope, identity and statistics of milestone 0.5 (plan_0.5.md section 3,
// contracts_0.5.md). A frozen contract: slice C implements the reader and writer in
// cache_envelope.cpp and the record codec in shader_environment.cpp, slice D consumes them in the
// pipeline cache, slice F fills the host identity and reads the report. Nothing here changes the
// existing cache files by itself.
//
// ON-DISK GRAMMAR (envelope version 1). All integers are unsigned, fixed width, little endian;
// nothing is padding or a pointer; a reader never trusts a length before checking it.
//
//   File    := Header(160 bytes) Payload Index
//   Header  :=
//     off  size  field
//       0     8  magic "EGL05CCH"
//       8     4  envelope_version (1)
//      12     4  header_length (160)
//      16     4  payload_kind (PayloadKind)
//      20     4  shader_format_version (the environment / translation data model, now 35)
//      24     8  build_id
//      32     4  vendor_id
//      36     4  device_id
//      40     4  driver_id
//      44     4  driver_version
//      48    16  pipeline_cache_uuid
//      64     8  title_id
//      72     4  game_version
//      76     4  reserved, 0
//      80     8  content_stamp (executable and content set, add-ons included)
//      88     8  settings_hash (settings that change generated shaders)
//      96     8  record_count
//     104     8  payload_offset (160)
//     112     8  payload_bytes
//     120     8  index_offset (payload_offset + payload_bytes)
//     128     8  index_bytes (record_count * 48)
//     136     4  checksum_algorithm (1: FNV-1a 64 over the bytes)
//     140     4  reserved, 0
//     144     8  header_checksum (over bytes 0..143)
//     152     8  index_checksum (over the index bytes)
//   Payload := the records back to back, in index order, no gaps
//   Record  := one payload of at most MaxRecordBytes. Pipeline kinds: u32 env_count (1..8);
//              env_count * { u32 env_length, env bytes (GenericEnvironment::Serialize layout) };
//              u32 key_length; key bytes (explicit fixed width fields chosen by the cache, never a
//              raw struct dump). DriverBlob: the opaque Vulkan pipeline cache data, one record.
//   Index   := record_count entries of 48 bytes:
//       0  4  record_kind (RecordKind)
//       4  4  first_use_phase (FirstUse)
//       8  8  offset (from the start of the payload)
//      16  4  length
//      20  4  first_use_ordinal
//      24  8  first_use_ns (since the first host present, 0 for boot)
//      32  4  translate_cost_us
//      36  4  build_cost_us
//      40  8  checksum (FNV-1a 64 of the record; two records of one kind with the same length
//             and checksum are one record)
//
// BOUNDS (a violation is the reason in brackets, nothing is allocated for the payload first):
//   * file at least 160 bytes [truncated]; file size == payload_offset + payload_bytes +
//     index_bytes exactly [truncated]
//   * total size at most MaxBytes(kind) [too_large]; index_bytes at most MaxIndexBytes,
//     == record_count * 48, record_count at most MaxIndexBytes / 48 [bad_index]
//   * every record: offset + length <= payload_bytes (checked without overflow), length at most
//     MaxRecordBytes, offsets contiguous in index order, kind a known RecordKind [bad_index]
//   * a record read back must match its checksum [bad_checksum]; its bytes must decode with every
//     nested count and length checked against the bytes that remain, and env_count >= 1
//     [bad_record]
//
// OPEN ORDER (the first failure is the answer). Reading is header first and small: only the
// header and then the index (at most 8 MiB) are read synchronously; records are read one by one,
// later, from worker threads.
//   magic (an old unenveloped file starts with "yuzucach" or "yuzuvkch": legacy_format; anything
//   else: bad_magic) -> size -> envelope_version -> header_length -> header_checksum ->
//   payload_kind -> shader_format -> build_id -> device (vendor+device id) -> driver (id+version)
//   -> pipeline_uuid -> title -> game_version -> content -> settings -> bounds -> index_checksum.
//   The version and the length are checked before the checksum so that a later format can be
//   rejected cleanly by this one.
//
// REJECTION. A rejected optional cache is a cache miss, never a boot error. The reader returns
// the reason, the expected and the found value (the field's value; device: vendor << 32 | device,
// driver: driver_id << 32 | driver_version, uuid: its first 8 bytes), allocates nothing for the
// payload, and the caller logs CacheRejectLine once, reports it, skips preparation, continues
// loading the game and creates a fresh generation. A rejected file is left alone, except
// legacy_format: it is renamed once to <name>.legacy.bak (BackupLegacy; a bak already there is
// replaced) so that it is not retried and the old executable can still use it. A rejection does
// not start the archive rebuild of the warm file. A bad record inside an accepted file skips
// that record only (a prepare failure) and keeps the rest.
//
// WRITING. One owner writes a generation to <name>.tmp next to the file, seeded once from the
// accepted old file (Seed), then Append of unique new records, then Commit: write the index and
// the header, flush the file to disk, then replace the old file atomically (a rename that
// replaces on Windows). Any failure (disk full, flush, rename) deletes the .tmp, keeps the
// previous file untouched and makes Commit return false. A .tmp found at start is stale: it is
// deleted (RemoveStaleTemp), never loaded. No dual writing of old and new formats.
namespace VideoCommon::CacheEnvelope {

inline constexpr std::array<char, 8> Magic{'E', 'G', 'L', '0', '5', 'C', 'C', 'H'};
inline constexpr u32 EnvelopeVersion = 1;
inline constexpr u32 HeaderBytes = 160;
inline constexpr u32 IndexEntryBytes = 48;
inline constexpr u32 ChecksumFnv1a64 = 1;

inline constexpr u64 MaxRecordBytes = u64{16} << 20;
inline constexpr u64 MaxIndexBytes = u64{8} << 20;
inline constexpr u64 MaxTransferableBytes = u64{512} << 20; ///< Main plus mesh, per title, incl. index
inline constexpr u64 MaxDriverBlobBytes = u64{256} << 20;
inline constexpr u64 MaxWarmBytes = u64{64} << 20;
inline constexpr u32 MaxEnvsPerRecord = 8;

enum class PayloadKind : u32 {
    Transferable = 1,     ///< vulkan.mythos.bin: exact graphics and compute records
    TransferableMesh = 2, ///< vulkan_mesh.mythos.bin
    DriverBlob = 3,       ///< vulkan_pipelines[.unoptimized].mythos.bin: opaque driver data
    WarmCompute = 4,      ///< warm-compute.bin: guessed compute entries
};

inline constexpr std::array<std::string_view, 5> PayloadKindNames{
    "invalid", "transferable", "transferable_mesh", "driver_blob", "warm_compute"};
[[nodiscard]] constexpr std::string_view PayloadKindName(PayloadKind kind) noexcept {
    const auto index = static_cast<size_t>(kind);
    return index < PayloadKindNames.size() ? PayloadKindNames[index] : PayloadKindNames[0];
}

/// The size limit of one file of the kind (the transferable limit covers main plus mesh; the
/// writer of each is given its share by the pipeline cache).
[[nodiscard]] constexpr u64 MaxBytes(PayloadKind kind) noexcept {
    switch (kind) {
    case PayloadKind::Transferable:
    case PayloadKind::TransferableMesh:
        return MaxTransferableBytes;
    case PayloadKind::DriverBlob:
        return MaxDriverBlobBytes;
    case PayloadKind::WarmCompute:
        return MaxWarmBytes;
    }
    return 0;
}

enum class RecordKind : u32 { Graphics = 1, Compute = 2, Opaque = 3 };
enum class FirstUse : u32 { Boot = 0, Run = 1 };

/// What identifies a cache. A file is accepted only when every field equals the running
/// emulator's (the conservative first release: no relaxed device compatibility).
struct HostIdentity {
    u64 build_id{};       ///< Hash of the emulator build and source revision (frontend)
    u64 title_id{};
    u32 game_version{};   ///< Numeric title version
    u64 content_stamp{};  ///< Executable and content set fingerprint, enabled add-ons included
    u64 settings_hash{};  ///< Settings that change generated shaders
};

struct DeviceIdentity {
    u32 vendor_id{};
    u32 device_id{};
    u32 driver_id{};
    u32 driver_version{};
    std::array<u8, 16> pipeline_cache_uuid{};
};

struct CacheIdentity {
    PayloadKind kind{PayloadKind::Transferable};
    u32 envelope_version{EnvelopeVersion};
    u32 shader_format_version{}; ///< The pipeline cache's data model version (CACHE_VERSION)
    HostIdentity host;
    DeviceIdentity device;
};

enum class RejectReason : u8 {
    None, ///< Accepted
    Missing,
    LegacyFormat,
    BadMagic,
    Truncated,
    FormatVersion,
    HeaderLength,
    PayloadKind,
    ShaderFormat,
    BuildId,
    Device,
    Driver,
    PipelineUuid,
    Title,
    GameVersion,
    Content,
    Settings,
    TooLarge,
    BadIndex,
    BadChecksum,
    BadRecord,
    IoError,
    Count,
};

/// The tokens logs and the report use for each reason (the log and schema contract).
inline constexpr std::array<std::string_view, static_cast<size_t>(RejectReason::Count)>
    RejectReasonNames{"none",         "missing",       "legacy_format", "bad_magic",
                      "truncated",    "format_version", "header_length", "payload_kind",
                      "shader_format", "build_id",      "device",        "driver",
                      "pipeline_uuid", "title",         "game_version",  "content",
                      "settings",     "too_large",     "bad_index",     "bad_checksum",
                      "bad_record",   "io_error"};
[[nodiscard]] constexpr std::string_view RejectReasonName(RejectReason reason) noexcept {
    return RejectReasonNames[static_cast<size_t>(reason)];
}

struct CacheLoadResult {
    bool accepted{};
    RejectReason reason{RejectReason::None};
    PayloadKind kind{PayloadKind::Transferable};
    u64 expected{}; ///< For a mismatch, what this emulator expects (see REJECTION above)
    u64 found{};    ///< and what the file holds
    u64 records{};  ///< Accepted: records in the index
    u64 payload_bytes{};
    bool legacy_backed_up{};
};

/// "Nx2Cache reject kind=transferable reason=format_version expected=1 found=999 action=ignore"
/// (action=backup_legacy when legacy_backed_up). Empty for an accepted result.
[[nodiscard]] std::string CacheRejectLine(const CacheLoadResult& result);

enum class WriteStatus : u8 { NotAttempted, Ok, Failed };

/// What the cache did over a run (the cache block of the report), as a snapshot.
struct CacheStats {
    u64 recorded{};         ///< Unique records appended to the new generation
    u64 prepared{};         ///< Records rebuilt into ready objects before use
    u64 adopted{};          ///< Ready objects the game then used
    u64 demand_promoted{};  ///< Draws that found their record queued and promoted it
    u64 demand_wait_ns{};   ///< Time a draw waited on a record being built
    u64 prepare_failures{};
    u64 records_skipped{};  ///< Oversized or not serializable, compiled normally
    u64 queue_high_water{};
    u64 memory_high_water_bytes{};
    u64 budget_pauses{};
    u64 evicted{};          ///< Ready objects released unused: held back builds were stalled
    u64 records{};          ///< Records of the accepted files the run tried to prepare
    u64 demand_building{};  ///< Draws that found their record being built (they waited)
    u64 demand_evicted{};   ///< Draws that found their record released (built again by the draw)
    u64 demand_unknown{};   ///< Draws whose key matched no record: new, or not decoded yet
    u64 held_back{};        ///< Times builds were held at the ready cap with nothing to release
    u64 adopted_lead_ns{};  ///< Sum over adopted objects of (first use - ready): how early
    u64 late_records{};     ///< Records first used while building, queued or released
    u64 late_wait_ns{};     ///< Sum over records first used while building of (ready - first use)
    u64 late_wait_max_ns{}; ///< The longest of those
    u64 checkpoints{};      ///< Generations committed before shutdown
    u64 bytes_committed{};  ///< Bytes of the transferable files committed (checkpoints and final)
    WriteStatus write_status{WriteStatus::NotAttempted};
};

struct CacheFileReport {
    PayloadKind kind{PayloadKind::Transferable};
    CacheLoadResult load;
};

struct CacheReport {
    CacheIdentity identity;
    std::vector<CacheFileReport> files;
    CacheStats stats;
};

/// Preparation policy of the pipeline cache (the plan's initial values, safety limits and not
/// measurements). Demand-required compilation is never denied by any of them.
struct PrepareOptions {
    u32 workers{2};                       ///< Preparation threads (1 with a broken parallel driver)
    u64 memory_budget_bytes{u64{256} << 20}; ///< Index, queued buffers, staging, bookkeeping
    u32 ready_cap{128};                   ///< Prepared but unused live pipelines
    u64 min_available_ram_bytes{u64{2} << 30}; ///< Optional admission pauses below this
    /// Draws that adopt no ready object for this long while ready objects hold the ready cap and
    /// records wait to be built (the draws match no record, or take records still queued) show
    /// that the objects at the cap are not the ones this run needs: unused ready objects are then
    /// released, oldest first, so the rest of the index is still prepared. Draws that find
    /// nothing to adopt are the only clock; a quiet period does not count. A bound, not a
    /// measurement.
    u64 stalled_after_ns{u64{10} * 1'000'000'000};
};

/// One record to append: the index fields the writer does not compute itself.
struct RecordMeta {
    RecordKind kind{RecordKind::Graphics};
    FirstUse first_use_phase{FirstUse::Run};
    u32 first_use_ordinal{};
    u64 first_use_ns{};
    /// Zero in files the pipeline cache writes: translation is timed inside build_cost_us and
    /// not on its own, and the report says "not_separately_measured", never 0.
    u32 translate_cost_us{};
    u32 build_cost_us{};
};

struct IndexEntry {
    RecordMeta meta;
    u64 offset{};
    u32 length{};
    u64 checksum{};
};

/// FNV-1a 64 of `bytes` (the envelope's checksum).
[[nodiscard]] u64 Checksum(std::span<const u8> bytes);

/// The first field of `found` that differs from `expected`, in OPEN ORDER (from shader_format
/// on), as a rejected result; accepted when they are equal. `found.kind` must equal the
/// expected kind or the reason is payload_kind.
[[nodiscard]] CacheLoadResult CheckIdentity(const CacheIdentity& expected,
                                            const CacheIdentity& found);

/// Deletes `<path>.tmp` when it exists (a generation that never committed).
void RemoveStaleTemp(const std::filesystem::path& path);

/// Renames a legacy file to `<path>.legacy.bak`, replacing an older backup. False when it cannot.
bool BackupLegacy(const std::filesystem::path& path);

/// A validated, open envelope file. Only the header and the index are in memory.
class EnvelopeReader {
public:
    /// Opens and validates `path` against `expected` (see OPEN ORDER). Returns null on any
    /// rejection (`result` has the reason and the legacy backup is already made) and also when
    /// the file does not exist (reason missing). Reads at most HeaderBytes + MaxIndexBytes.
    [[nodiscard]] static std::unique_ptr<EnvelopeReader> Open(const std::filesystem::path& path,
                                                              const CacheIdentity& expected,
                                                              CacheLoadResult& result);
    ~EnvelopeReader();
    EnvelopeReader(const EnvelopeReader&) = delete;
    EnvelopeReader& operator=(const EnvelopeReader&) = delete;

    [[nodiscard]] std::span<const IndexEntry> Index() const noexcept;

    /// Reads record `index` into `payload` and verifies its checksum. Safe to call from several
    /// threads at once. bad_checksum, bad_index (out of range), io_error or None.
    [[nodiscard]] RejectReason ReadRecord(size_t index, std::vector<u8>& payload) const;

private:
    struct Impl;
    explicit EnvelopeReader(std::unique_ptr<Impl> state) noexcept;
    std::unique_ptr<Impl> impl;
};

enum class AppendResult : u8 {
    Ok,
    Duplicate,  ///< Same kind, length and checksum as a record already in the generation
    TooLarge,   ///< Over MaxRecordBytes: skipped, never truncated
    BudgetFull, ///< The file would pass max_bytes
    IoError,
};

/// The generation being written. One owner thread; not shared.
class EnvelopeWriter {
public:
    /// Starts `<path>.tmp` (a stale one is replaced) for a file that must stay within
    /// `max_bytes`. Null when the temporary file cannot be created.
    [[nodiscard]] static std::unique_ptr<EnvelopeWriter> Create(const std::filesystem::path& path,
                                                                const CacheIdentity& identity,
                                                                u64 max_bytes);
    /// Abandons an uncommitted generation (deletes the .tmp).
    ~EnvelopeWriter();
    EnvelopeWriter(const EnvelopeWriter&) = delete;
    EnvelopeWriter& operator=(const EnvelopeWriter&) = delete;

    /// Copies the records of an accepted file, in index order, once, before any Append.
    /// Returns how many were carried over.
    size_t Seed(const EnvelopeReader& reader);

    [[nodiscard]] AppendResult Append(const RecordMeta& meta, std::span<const u8> payload);

    /// Writes index and header, flushes, replaces the file atomically. False (previous file kept,
    /// write_status failed) on any failure. The writer is finished afterwards either way.
    [[nodiscard]] bool Commit();

    [[nodiscard]] size_t Records() const noexcept;
    [[nodiscard]] u64 Bytes() const noexcept; ///< The file size so far, header and index included

private:
    struct Impl;
    explicit EnvelopeWriter(std::unique_ptr<Impl> state) noexcept;
    std::unique_ptr<Impl> impl;
};

// Identity inputs shared between the frontend (which owns the title, the build and the
// settings) and the pipeline cache (which owns the device and the format version). The frontend
// sets the host identity before the cache loads; the cache publishes what it did for the report.
// Safe from any thread.
namespace Detail {
struct Registry {
    std::mutex mutex;
    HostIdentity host;
    CacheReport report;
};
inline Registry& Shared() {
    static Registry registry;
    return registry;
}
} // namespace Detail

inline void SetHostIdentity(const HostIdentity& host) {
    auto& registry = Detail::Shared();
    const std::scoped_lock lock{registry.mutex};
    registry.host = host;
}

[[nodiscard]] inline HostIdentity GetHostIdentity() {
    auto& registry = Detail::Shared();
    const std::scoped_lock lock{registry.mutex};
    return registry.host;
}

/// Replaces the cache block of the report (the cache calls it when files were loaded, again with
/// the final statistics at shutdown once its producers have stopped).
inline void PublishCacheReport(CacheReport report) {
    auto& registry = Detail::Shared();
    const std::scoped_lock lock{registry.mutex};
    registry.report = std::move(report);
}

[[nodiscard]] inline CacheReport ReadCacheReport() {
    auto& registry = Detail::Shared();
    const std::scoped_lock lock{registry.mutex};
    return registry.report;
}

} // namespace VideoCommon::CacheEnvelope
