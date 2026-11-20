// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Frozen contract of the 0.6 capture file: container, header, chunk framing, bounds and the
// bounds-checked byte codec every payload codec is built from. Slice A owns this header; the
// capture writer and reader (slice B) and the replay session (slice C) consume it unchanged.
// Payload layouts live in events.h, library_call.h and checkpoint.h; the written description is
// plans/contracts_0.6.md, section "Capture file format".
//
// What a capture is
// -----------------
// A capture is a local-only, private file holding the graphics work of a bounded run of
// consecutive accepted presents (the selected frames), plus everything needed to replay it
// without the game: the GPU command words as consumed, the guest memory those words depend on
// (initial blobs plus deltas), the GPU state at the start, and, optionally, the graphics-library
// call stream of the same frames paired with the GPU events (shared allocation ids, frame ids and
// submit/present links), and the live end picture of every selected frame. It is never published
// and never part of a release; make_release.py rejects the magic and the extension.
//
// All integers are little-endian and fixed width. There are no pointers, no host paths, no
// timestamps and no host object ids anywhere in the file. A reader treats every length, count
// and offset as hostile: it checks each against the bounds below and against the bytes that
// remain BEFORE it allocates or reads anything it describes.
//
// File layout
// -----------
//   FileHeader            256 bytes, offset 0
//   chunk 0 .. chunk N-1  each is a 64-byte ChunkHeader followed by stored_length payload bytes
//   INDEX chunk           written after the last frame, listed in no index itself
//   COMPLETE chunk        the last chunk of a finalized file
//
// The writer streams to "<name>.partial", appending chunks in sequence order. Finalizing writes
// the INDEX and COMPLETE chunks, rewrites the header (flags, counts, index_offset, file_length,
// checksum), flushes, and renames the file without replacing an existing file. A capture that
// overflowed a bound, lost an event, failed a write or was interrupted stays ".partial" with the
// Complete flag clear and is never renamed; a reader that is handed one reports NotComplete and
// refuses to replay it.
//
// FileHeader (256 bytes)
//   off  len  field
//     0    8  magic                "EGLCAP\0\0"
//     8    2  format_major         2; a reader rejects any other major (BadVersion)
//    10    2  format_minor         0; additive, optional-only changes bump the minor
//    12    4  header_length        256
//    16    8  required_features    RequiredFeature bits; unknown bit => UnsupportedFeature
//    24    8  optional_features    OptionalFeature bits; unknown bits are ignored
//    32    4  flags                HeaderFlag bits; bits not defined here must be 0
//    36    4  checksum_algorithm   1 = FNV-1a 64 (the only one in v1)
//    40    8  index_offset         file offset of the INDEX chunk header; 0 until finalized
//    48    8  chunk_count          chunks including INDEX and COMPLETE; 0 until finalized
//    56    8  file_length          total bytes; 0 until finalized
//    64    8  first_frame          id of the first selected (compared) frame, >= 1
//    72    4  requested_frames     frames the operator asked for, 1..MaxSelectedFrames
//    76    4  finalized_frames     selected frames fully captured, <= requested_frames
//    80    4  preroll_frames       frames recorded before first_frame that replay executes but
//                                  does not compare; 0 when the checkpoint is at first_frame
//    84    4  reserved             0
//    88    8  build_id             hash of the capturing build; provenance only
//    96    8  settings_hash        hash of the pinned settings of the SESSION chunk
//   104    8  title_id             provenance only; 0 when unknown; never used to pick behaviour
//   112    4  title_version        UnavailableVersion when not obtainable
//   116    4  title_version_status VersionStatus
//   120    8  content_stamp        content identity stamp of the title; 0 when unavailable
//   128    8  module_set_hash      hash of the guest module identities; 0 without a library section
//   136    8  abi_hash             library ABI identity (library_call.h); 0 without a library section
//   144    4  gpu_architecture     GpuArchitecture
//   148    4  compiler             CompilerId
//   152    8  session_id           random nonzero id tying reports and sidecars to this capture
//   160   40  revision             source revision text, NUL padded, printable ASCII
//   200   40  reserved             0
//   240    8  header_checksum      FNV-1a 64 of bytes 0..239
//   248    8  reserved             0
//
// ChunkHeader (64 bytes), directly before every payload
//   off  len  field
//     0    4  type                 ChunkType
//     4    2  version              payload layout version of this type; v1 writes 1
//     6    2  flags                ChunkFlag bits; bits not defined here must be 0
//     8    8  sequence             0, 1, 2 ... strictly +1 in file order
//    16    8  frame_id             owning frame; 0 only for SESSION, INDEX and COMPLETE. The
//                                  checkpoint chunks and the initial MEMORY_BLOB and MAP chunks
//                                  carry the first frame stored in the file
//                                  (first_frame - preroll_frames); non-decreasing otherwise
//    24    8  stored_length        payload bytes that follow, <= MaxChunkBytes
//    32    8  uncompressed_length  equals stored_length in v1 (compression is reserved)
//    40    8  payload_checksum     FNV-1a 64 of the stored payload bytes
//    48    8  reserved             0
//    56    8  header_checksum      FNV-1a 64 of bytes 0..55
//
// Ordering grammar (the reader validates it; a violation is BadOrdering)
//   SESSION                                     sequence 0, frame 0
//   [LIBRARY_SCHEMA]                            optional section, before any library record
//   GPU_CHECKPOINT                              frame = first stored frame (see frame_id)
//   [LIBRARY_CHECKPOINT]
//   MEMORY_BLOB*                                initial backing, frame = first stored frame
//   MAP*                                        initial address-space mappings
//   then for every frame, ascending and gapless from (first_frame - preroll_frames) to
//   (first_frame + finalized_frames - 1):
//     { MAP | UNMAP | EXTERNAL_WRITE | MEMORY_BLOB(delta) | FENCE | GPU_EVENT | LIBRARY_EVENT }*
//     PRESENT
//     { END_PICTURE }*
//     FRAME_END
//   INDEX
//   COMPLETE
//
// Frame ownership: every chunk that precedes PRESENT n belongs to frame n; the PRESENT, its
// END_PICTUREs and FRAME_END carry frame n too. Frame ids are the logical accepted-present ids of
// run_state.h; pre-roll frames keep their true ids (< first_frame).
//
// Chunks are applied by a replayer strictly in sequence order. Unknown chunk types with the
// Optional flag are skipped (their bytes still count towards bounds and the chain digest);
// an unknown chunk type without it is UnsupportedFeature.
//
// BOUNDS (hard defaults; a capture or a reader may only be stricter)
//   See the constants below. The writer checks the budget before it stages each payload and
//   stops the capture as incomplete (never truncates silently) when a bound would be exceeded.
//
// REJECTION (every case maps to ReadError; no ReadError is ever ignored)
//   Truncated file or payload, wrong magic, wrong major, wrong header length, header checksum,
//   payload checksum, chunk header checksum, non-consecutive sequence, frame id going backwards
//   or skipping a frame, a length or count over a bound or over the remaining bytes, a nonzero
//   reserved field, an unknown enum value in a required chunk, a payload that does not consume
//   exactly its stored length, an id (allocation, generation, object, function, submit, fence)
//   that does not resolve to an earlier definition, an INDEX that disagrees with the chunks
//   actually found, a COMPLETE digest that disagrees with the recomputed chain digest, and a
//   header or COMPLETE that says the capture is incomplete (NotComplete).
//
// WRITING
//   Header flags: Complete is set only by the finalizer after INDEX and COMPLETE are on disk.
//   DependencyComplete is set only when COMPLETE.unresolved_resources == 0 and every memory
//   range any command reads or any picture depends on resolves to initial data, a delta or a
//   declared GPU-derived value. LibraryReplayReady is set only when the library section passed
//   the closure check of library_call.h (every function schema_complete, every object and pool
//   reference defined earlier, no unsupported schema, no dropped record).

#pragma once

#include <array>
#include <cstddef>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

#include "common/common_types.h"

namespace VideoCore::Replay {

inline constexpr std::array<u8, 8> Magic{'E', 'G', 'L', 'C', 'A', 'P', 0, 0};
inline constexpr u16 FormatMajor = 2;
inline constexpr u16 FormatMinor = 0;
inline constexpr u32 FileHeaderBytes = 256;
inline constexpr u32 ChunkHeaderBytes = 64;
inline constexpr u32 IndexEntryBytes = 32;
inline constexpr u32 FrameEntryBytes = 40;
inline constexpr u32 ChecksumFnv1a64 = 1;

// File extensions. A capture is only ever written under the first; the second is what an
// unfinished or interrupted capture is left as. Both are rejected by the release tooling.
inline constexpr std::string_view FinalExtension = ".egcap";
inline constexpr std::string_view PartialExtension = ".egcap.partial";

inline constexpr u32 UnavailableVersion = 0xFFFFFFFFu;

// Bounds. Values from the 0.6 plan hard defaults, plus the structural bounds the grammar needs.
inline constexpr u32 MaxSelectedFrames = 120;
inline constexpr u32 MaxTotalFrames = 2048;           // pre-roll + selected
inline constexpr u64 MaxFileBytes = 16ULL << 30;      // 16 GiB
inline constexpr u64 MaxInitialBackingBytes = 8ULL << 30;
inline constexpr u64 MaxCommandBytesPerFrame = 64ULL << 20;
inline constexpr u64 MaxNewPayloadBytesPerFrame = 512ULL << 20;
inline constexpr u64 MaxChunkBytes = 64ULL << 20;
inline constexpr u64 MaxStagingBytes = 512ULL << 20;
inline constexpr u64 MaxChunkCount = 1ULL << 20;
inline constexpr u32 MaxEventsPerChunk = 65536;
inline constexpr u32 MaxRangesPerBlob = 1U << 20;
inline constexpr u32 MaxEntriesPerSubmit = 65536;
inline constexpr u32 MaxWordsPerConsume = 1U << 24;   // 64 MiB of command words
inline constexpr u32 MaxLayersPerPresent = 16;
inline constexpr u32 MaxSemanticOutputs = 4096;
inline constexpr u32 MaxPictureDimension = 8192;
inline constexpr u32 MaxSessionPins = 64;
inline constexpr u32 MaxNameBytes = 96;
inline constexpr u32 MaxChannels = 64;
inline constexpr u32 MaxSubRecords = 4096;
inline constexpr u32 MaxEngineRegisters = 1U << 16;
inline constexpr u32 MaxMacroWords = 1U << 20;
inline constexpr u32 MaxParkedMethods = 16;

enum class ChunkType : u32 {
    Session = 1,
    GpuCheckpoint = 2,
    LibraryCheckpoint = 3,
    MemoryBlob = 4,
    GpuEvent = 5,
    LibraryEvent = 6,
    ExternalWrite = 7,
    Map = 8,
    Unmap = 9,
    Fence = 10,
    Present = 11,
    FrameEnd = 12,
    Index = 13,
    Complete = 14,
    EndPicture = 15,
    LibrarySchema = 16,
};

enum ChunkFlag : u16 {
    ChunkOptional = 1U << 0,    // a reader that does not know the type may skip the chunk
    ChunkCompressed = 1U << 1,  // reserved: v1 writers never set it, v1 readers reject it
};
inline constexpr u16 KnownChunkFlags = ChunkOptional | ChunkCompressed;

// Whether a known chunk type is written with the Optional flag. A GPU-only replayer skips these.
[[nodiscard]] constexpr bool ChunkTypeIsOptional(ChunkType type) noexcept {
    return type == ChunkType::LibraryCheckpoint || type == ChunkType::LibraryEvent ||
           type == ChunkType::LibrarySchema || type == ChunkType::EndPicture;
}

[[nodiscard]] constexpr bool ChunkTypeIsKnown(u32 type) noexcept {
    return type >= static_cast<u32>(ChunkType::Session) &&
           type <= static_cast<u32>(ChunkType::LibrarySchema);
}

// Chunks that carry frame_id 0 because they do not belong to a frame.
[[nodiscard]] constexpr bool ChunkTypeIsFrameless(ChunkType type) noexcept {
    return type == ChunkType::Session || type == ChunkType::Index || type == ChunkType::Complete ||
           type == ChunkType::LibrarySchema;
}

enum RequiredFeature : u64 {
    RequiredBaseV1 = 1ULL << 0,       // always set
    RequiredCompression = 1ULL << 1,  // reserved: no v1 reader supports it
    RequiredExtendedGpuState = 1ULL << 2,  // reserved for a later minor that adds sub-records
};
inline constexpr u64 SupportedRequiredFeatures = RequiredBaseV1;

enum OptionalFeature : u64 {
    OptionalLibrarySection = 1ULL << 0,
    OptionalLivePictures = 1ULL << 1,
    OptionalGuestFramePictures = 1ULL << 2,
    OptionalPostPresentPictures = 1ULL << 3,
};

enum HeaderFlag : u32 {
    HeaderComplete = 1U << 0,
    HeaderLibraryReplayReady = 1U << 1,
    HeaderDependencyComplete = 1U << 2,
    HeaderHasLibrarySection = 1U << 3,
    HeaderHasLivePictures = 1U << 4,
};
inline constexpr u32 KnownHeaderFlags = 0x1F;

enum class VersionStatus : u32 { Unavailable = 0, Known = 1 };

enum class GpuArchitecture : u32 { Unknown = 0, Maxwell = 1, Turing = 2, Ampere = 3 };

enum class CompilerId : u32 { Unknown = 0, ClangMinGw = 1, ClangMsvc = 2, Gcc = 3, Msvc = 4 };

enum class ReadError : u32 {
    None = 0,
    Truncated,
    BadMagic,
    BadVersion,
    BadHeaderLength,
    BadHeaderChecksum,
    UnsupportedFeature,
    NotComplete,
    BadChunkHeader,
    BadChunkChecksum,
    BadOrdering,
    BadLength,
    BadPayload,
    DuplicateFrame,
    DanglingId,
    BadMapping,
    UnsupportedState,
    LimitExceeded,
    BadIndex,
    BadDigest,
};

[[nodiscard]] constexpr std::string_view ReadErrorName(ReadError error) noexcept {
    switch (error) {
    case ReadError::None: return "none";
    case ReadError::Truncated: return "truncated";
    case ReadError::BadMagic: return "bad_magic";
    case ReadError::BadVersion: return "bad_version";
    case ReadError::BadHeaderLength: return "bad_header_length";
    case ReadError::BadHeaderChecksum: return "bad_header_checksum";
    case ReadError::UnsupportedFeature: return "unsupported_feature";
    case ReadError::NotComplete: return "not_complete";
    case ReadError::BadChunkHeader: return "bad_chunk_header";
    case ReadError::BadChunkChecksum: return "bad_chunk_checksum";
    case ReadError::BadOrdering: return "bad_ordering";
    case ReadError::BadLength: return "bad_length";
    case ReadError::BadPayload: return "bad_payload";
    case ReadError::DuplicateFrame: return "duplicate_frame";
    case ReadError::DanglingId: return "dangling_id";
    case ReadError::BadMapping: return "bad_mapping";
    case ReadError::UnsupportedState: return "unsupported_state";
    case ReadError::LimitExceeded: return "limit_exceeded";
    case ReadError::BadIndex: return "bad_index";
    case ReadError::BadDigest: return "bad_digest";
    }
    return "unknown";
}

// ---- checksum -----------------------------------------------------------------------------

inline constexpr u64 Fnv1aOffset = 0xcbf29ce484222325ULL;
inline constexpr u64 Fnv1aPrime = 0x100000001b3ULL;

[[nodiscard]] constexpr u64 Fnv1a64(std::span<const u8> bytes, u64 seed = Fnv1aOffset) noexcept {
    u64 hash = seed;
    for (const u8 byte : bytes) {
        hash = (hash ^ byte) * Fnv1aPrime;
    }
    return hash;
}

// Chain digest of a capture: FNV-1a 64 over the eight little-endian bytes of the header checksum
// of every chunk in sequence order, INDEX and COMPLETE excluded. COMPLETE.chain_digest holds it.
[[nodiscard]] constexpr u64 ChainStep(u64 digest, u64 chunk_header_checksum) noexcept {
    for (u32 i = 0; i < 8; ++i) {
        digest = (digest ^ ((chunk_header_checksum >> (8U * i)) & 0xFFU)) * Fnv1aPrime;
    }
    return digest;
}

// ---- bounds-checked little-endian byte codec ---------------------------------------------------

// Appends to a caller-owned vector. Nothing here can fail; the caller checks its budget first.
class ByteWriter {
public:
    explicit ByteWriter(std::vector<u8>& out_) noexcept : out{out_} {}

    void Put8(u8 value) {
        out.push_back(value);
    }
    void Put16(u16 value) {
        PutLe(value, 2);
    }
    void Put32(u32 value) {
        PutLe(value, 4);
    }
    void Put64(u64 value) {
        PutLe(value, 8);
    }
    void PutBytes(std::span<const u8> bytes) {
        out.insert(out.end(), bytes.begin(), bytes.end());
    }
    void PutZero(size_t count) {
        out.insert(out.end(), count, u8{0});
    }
    // Writes `text` into a fixed-width field, NUL padded; text longer than the field is cut.
    void PutFixedText(std::string_view text, size_t width) {
        const size_t used = text.size() < width ? text.size() : width;
        for (size_t i = 0; i < used; ++i) {
            out.push_back(static_cast<u8>(text[i]));
        }
        PutZero(width - used);
    }
    [[nodiscard]] size_t Size() const noexcept {
        return out.size();
    }
    // Overwrites an already written u32/u64 (length prefixes). Out-of-range patches are ignored.
    void Patch32(size_t offset, u32 value) noexcept {
        PatchLe(offset, value, 4);
    }
    void Patch64(size_t offset, u64 value) noexcept {
        PatchLe(offset, value, 8);
    }

private:
    void PutLe(u64 value, u32 width) {
        for (u32 i = 0; i < width; ++i) {
            out.push_back(static_cast<u8>((value >> (8U * i)) & 0xFFU));
        }
    }
    void PatchLe(size_t offset, u64 value, u32 width) noexcept {
        if (offset > out.size() || out.size() - offset < width) {
            return;
        }
        for (u32 i = 0; i < width; ++i) {
            out[offset + i] = static_cast<u8>((value >> (8U * i)) & 0xFFU);
        }
    }

    std::vector<u8>& out;
};

// Reads from a caller-owned span. The first violation clears Ok() for good; every later read
// returns zero or an empty span. A caller checks Ok() once after a whole structure and returns
// Truncated or BadPayload; it never has to check each read.
class ByteReader {
public:
    explicit constexpr ByteReader(std::span<const u8> data_) noexcept : data{data_} {}

    [[nodiscard]] constexpr bool Ok() const noexcept {
        return ok;
    }
    [[nodiscard]] constexpr size_t Remaining() const noexcept {
        return data.size() - pos;
    }
    [[nodiscard]] constexpr size_t Position() const noexcept {
        return pos;
    }
    constexpr void Fail() noexcept {
        ok = false;
    }

    [[nodiscard]] constexpr u8 Get8() noexcept {
        return static_cast<u8>(GetLe(1));
    }
    [[nodiscard]] constexpr u16 Get16() noexcept {
        return static_cast<u16>(GetLe(2));
    }
    [[nodiscard]] constexpr u32 Get32() noexcept {
        return static_cast<u32>(GetLe(4));
    }
    [[nodiscard]] constexpr u64 Get64() noexcept {
        return GetLe(8);
    }
    [[nodiscard]] constexpr std::span<const u8> GetBytes(size_t count) noexcept {
        if (!Take(count)) {
            return {};
        }
        return data.subspan(pos - count, count);
    }
    constexpr void Skip(size_t count) noexcept {
        static_cast<void>(Take(count));
    }
    // Reads `count` reserved bytes; any nonzero byte fails the reader.
    constexpr void ExpectZero(size_t count) noexcept {
        for (const u8 byte : GetBytes(count)) {
            if (byte != 0) {
                ok = false;
            }
        }
    }
    // Reads a u32 element count. It must not exceed `max_count`, and count * element_bytes must
    // fit in the remaining bytes (element_bytes is the smallest encoded size of one element).
    [[nodiscard]] constexpr u32 GetCount(u64 max_count, u64 element_bytes) noexcept {
        const u32 count = Get32();
        if (!ok) {
            return 0;
        }
        if (count > max_count || (element_bytes != 0 && count > Remaining() / element_bytes)) {
            ok = false;
            return 0;
        }
        return count;
    }
    // Reads a u32 length-prefixed ASCII name of at most MaxNameBytes bytes.
    [[nodiscard]] std::string_view GetName() noexcept {
        const u32 length = Get32();
        if (!ok || length > MaxNameBytes) {
            ok = false;
            return {};
        }
        const std::span<const u8> bytes = GetBytes(length);
        if (!ok) {
            return {};
        }
        return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
    }

private:
    [[nodiscard]] constexpr bool Take(size_t count) noexcept {
        if (!ok || count > data.size() - pos) {
            ok = false;
            return false;
        }
        pos += count;
        return true;
    }
    [[nodiscard]] constexpr u64 GetLe(u32 width) noexcept {
        if (!Take(width)) {
            return 0;
        }
        u64 value = 0;
        for (u32 i = 0; i < width; ++i) {
            value |= u64{data[pos - width + i]} << (8U * i);
        }
        return value;
    }

    std::span<const u8> data;
    size_t pos = 0;
    bool ok = true;
};

inline void PutName(ByteWriter& writer, std::string_view name) {
    const std::string_view cut = name.substr(0, MaxNameBytes);
    writer.Put32(static_cast<u32>(cut.size()));
    writer.PutBytes({reinterpret_cast<const u8*>(cut.data()), cut.size()});
}

// ---- file header ---------------------------------------------------------------------------

struct FileHeader {
    u16 format_major = FormatMajor;
    u16 format_minor = FormatMinor;
    u64 required_features = RequiredBaseV1;
    u64 optional_features = 0;
    u32 flags = 0;
    u64 index_offset = 0;
    u64 chunk_count = 0;
    u64 file_length = 0;
    u64 first_frame = 0;
    u32 requested_frames = 0;
    u32 finalized_frames = 0;
    u32 preroll_frames = 0;
    u64 build_id = 0;
    u64 settings_hash = 0;
    u64 title_id = 0;
    u32 title_version = UnavailableVersion;
    u32 title_version_status = static_cast<u32>(VersionStatus::Unavailable);
    u64 content_stamp = 0;
    u64 module_set_hash = 0;
    u64 abi_hash = 0;
    u32 gpu_architecture = static_cast<u32>(GpuArchitecture::Unknown);
    u32 compiler = static_cast<u32>(CompilerId::Unknown);
    u64 session_id = 0;
    std::array<char, 40> revision{};
};

[[nodiscard]] inline std::vector<u8> EncodeFileHeader(const FileHeader& header) {
    std::vector<u8> bytes;
    bytes.reserve(FileHeaderBytes);
    ByteWriter w{bytes};
    w.PutBytes(Magic);
    w.Put16(header.format_major);
    w.Put16(header.format_minor);
    w.Put32(FileHeaderBytes);
    w.Put64(header.required_features);
    w.Put64(header.optional_features);
    w.Put32(header.flags);
    w.Put32(ChecksumFnv1a64);
    w.Put64(header.index_offset);
    w.Put64(header.chunk_count);
    w.Put64(header.file_length);
    w.Put64(header.first_frame);
    w.Put32(header.requested_frames);
    w.Put32(header.finalized_frames);
    w.Put32(header.preroll_frames);
    w.Put32(0);
    w.Put64(header.build_id);
    w.Put64(header.settings_hash);
    w.Put64(header.title_id);
    w.Put32(header.title_version);
    w.Put32(header.title_version_status);
    w.Put64(header.content_stamp);
    w.Put64(header.module_set_hash);
    w.Put64(header.abi_hash);
    w.Put32(header.gpu_architecture);
    w.Put32(header.compiler);
    w.Put64(header.session_id);
    w.PutFixedText({header.revision.data(), header.revision.size()}, header.revision.size());
    w.PutZero(40);
    w.Put64(Fnv1a64(bytes));
    w.Put64(0);
    return bytes;
}

// Validates and decodes. Order of checks: size, magic, major, header length, checksum, features,
// reserved bytes and flags, then the value bounds. Returns None and fills `out` on success.
[[nodiscard]] inline ReadError DecodeFileHeader(std::span<const u8> bytes, FileHeader& out) {
    if (bytes.size() < FileHeaderBytes) {
        return ReadError::Truncated;
    }
    ByteReader r{bytes.first(FileHeaderBytes)};
    const std::span<const u8> magic = r.GetBytes(Magic.size());
    if (std::memcmp(magic.data(), Magic.data(), Magic.size()) != 0) {
        return ReadError::BadMagic;
    }
    FileHeader h;
    h.format_major = r.Get16();
    h.format_minor = r.Get16();
    if (h.format_major != FormatMajor) {
        return ReadError::BadVersion;
    }
    if (r.Get32() != FileHeaderBytes) {
        return ReadError::BadHeaderLength;
    }
    if (Fnv1a64(bytes.first(240)) != ByteReader{bytes.subspan(240, 8)}.Get64()) {
        return ReadError::BadHeaderChecksum;
    }
    h.required_features = r.Get64();
    h.optional_features = r.Get64();
    h.flags = r.Get32();
    const u32 algorithm = r.Get32();
    h.index_offset = r.Get64();
    h.chunk_count = r.Get64();
    h.file_length = r.Get64();
    h.first_frame = r.Get64();
    h.requested_frames = r.Get32();
    h.finalized_frames = r.Get32();
    h.preroll_frames = r.Get32();
    const u32 reserved_a = r.Get32();
    h.build_id = r.Get64();
    h.settings_hash = r.Get64();
    h.title_id = r.Get64();
    h.title_version = r.Get32();
    h.title_version_status = r.Get32();
    h.content_stamp = r.Get64();
    h.module_set_hash = r.Get64();
    h.abi_hash = r.Get64();
    h.gpu_architecture = r.Get32();
    h.compiler = r.Get32();
    h.session_id = r.Get64();
    const std::span<const u8> revision = r.GetBytes(h.revision.size());
    std::memcpy(h.revision.data(), revision.data(), h.revision.size());
    r.ExpectZero(40);
    r.Skip(8);
    r.ExpectZero(8);
    if (!r.Ok()) {
        return ReadError::BadPayload;
    }
    if (algorithm != ChecksumFnv1a64 || reserved_a != 0 || (h.flags & ~KnownHeaderFlags) != 0) {
        return ReadError::BadPayload;
    }
    if ((h.required_features & ~SupportedRequiredFeatures) != 0 ||
        (h.required_features & RequiredBaseV1) == 0) {
        return ReadError::UnsupportedFeature;
    }
    if (h.first_frame == 0 || h.requested_frames == 0 || h.requested_frames > MaxSelectedFrames ||
        h.finalized_frames > h.requested_frames ||
        u64{h.preroll_frames} + h.requested_frames > MaxTotalFrames ||
        h.chunk_count > MaxChunkCount || h.file_length > MaxFileBytes ||
        h.first_frame < h.preroll_frames) {
        return ReadError::LimitExceeded;
    }
    out = h;
    return ReadError::None;
}

// ---- chunk header --------------------------------------------------------------------------

struct ChunkHeader {
    u32 type = 0;
    u16 version = 1;
    u16 flags = 0;
    u64 sequence = 0;
    u64 frame_id = 0;
    u64 stored_length = 0;
    u64 uncompressed_length = 0;
    u64 payload_checksum = 0;
    u64 header_checksum = 0;  // filled by EncodeChunkHeader and DecodeChunkHeader
};

// Writes the 64 bytes; computes payload_checksum from `payload` and the header checksum, and
// stores both into `header` so the caller can chain them.
[[nodiscard]] inline std::array<u8, ChunkHeaderBytes> EncodeChunkHeader(
    ChunkHeader& header, std::span<const u8> payload) {
    header.stored_length = payload.size();
    header.uncompressed_length = payload.size();
    header.payload_checksum = Fnv1a64(payload);
    std::vector<u8> bytes;
    bytes.reserve(ChunkHeaderBytes);
    ByteWriter w{bytes};
    w.Put32(header.type);
    w.Put16(header.version);
    w.Put16(header.flags);
    w.Put64(header.sequence);
    w.Put64(header.frame_id);
    w.Put64(header.stored_length);
    w.Put64(header.uncompressed_length);
    w.Put64(header.payload_checksum);
    w.Put64(0);
    header.header_checksum = Fnv1a64(bytes);
    w.Put64(header.header_checksum);
    std::array<u8, ChunkHeaderBytes> out{};
    std::memcpy(out.data(), bytes.data(), out.size());
    return out;
}

// Validates the framing of one chunk header: size, header checksum, flags, reserved bytes,
// stored_length <= MaxChunkBytes, uncompressed_length == stored_length. The sequence and frame
// rules need the previous chunk and are checked by the reader.
[[nodiscard]] inline ReadError DecodeChunkHeader(std::span<const u8> bytes, ChunkHeader& out) {
    if (bytes.size() < ChunkHeaderBytes) {
        return ReadError::Truncated;
    }
    ChunkHeader h;
    ByteReader r{bytes.first(ChunkHeaderBytes)};
    h.type = r.Get32();
    h.version = r.Get16();
    h.flags = r.Get16();
    h.sequence = r.Get64();
    h.frame_id = r.Get64();
    h.stored_length = r.Get64();
    h.uncompressed_length = r.Get64();
    h.payload_checksum = r.Get64();
    const u64 reserved = r.Get64();
    h.header_checksum = r.Get64();
    if (Fnv1a64(bytes.first(56)) != h.header_checksum) {
        return ReadError::BadChunkChecksum;
    }
    if (reserved != 0 || (h.flags & ~KnownChunkFlags) != 0 || (h.flags & ChunkCompressed) != 0 ||
        h.version == 0) {
        return ReadError::BadChunkHeader;
    }
    if (h.stored_length > MaxChunkBytes || h.uncompressed_length != h.stored_length) {
        return ReadError::LimitExceeded;
    }
    out = h;
    return ReadError::None;
}

[[nodiscard]] inline ReadError VerifyPayload(const ChunkHeader& header,
                                             std::span<const u8> payload) {
    if (payload.size() != header.stored_length) {
        return ReadError::Truncated;
    }
    return Fnv1a64(payload) == header.payload_checksum ? ReadError::None
                                                       : ReadError::BadChunkChecksum;
}

} // namespace VideoCore::Replay
