// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Frozen payload layouts of the GPU section of the capture file (format.h is the container).
// Every payload below is little-endian and fixed width; a decoder reads it with ByteReader,
// checks every count against the bound and the remaining bytes before it allocates, requires
// reserved fields to be zero, and fails with BadPayload when the payload is not consumed exactly.
// The decoded structs are what the writer (slice B) fills and the replay session (slice C)
// consumes; the Encode/Decode functions are implemented by slice B in format_io.cpp.
//
// Allocation model (the relocation contract). A capture never refers to a host or guest address
// for data. Backing memory is named by allocation id: a dense nonzero id the writer assigns to a
// contiguous byte range of guest-visible memory that was alive during the capture, plus a
// generation that increases whenever the same backing is unmapped and reused. MAP records bind a
// GPU virtual range to (alloc_id, generation, offset); MEMORY_BLOB and EXTERNAL_WRITE records
// fill allocations. Replay creates its own backing for each allocation and maps it wherever it
// likes, so command words and framebuffer addresses, which are GPU virtual addresses or
// (alloc_id, offset) pairs, stay valid without any guest process.
//
// Provenance. Every byte a command can read has exactly one WriterKind:
//   Cpu            written by guest CPU code: carried as blob or delta data;
//   ExternalDevice written by another device or service (video decode, copy engines of other
//                  consumers): carried as an EXTERNAL_WRITE with its position in the sequence;
//   GpuDerived     produced by the GPU work inside the capture: never stored, never restored
//                  between frames; replay recomputes it.
// A byte of unknown provenance makes the capture incomplete (COMPLETE.unresolved_resources > 0).
//
// MEMORY_BLOB / EXTERNAL_WRITE payload
//   u64 alloc_id, u32 generation, u32 flags (BlobFlag), u64 allocation_size,
//   u32 range_count (<= MaxRangesPerBlob), then range_count entries of 32 bytes:
//     u64 offset, u64 length, u32 kind (RangeKind), u32 writer (WriterKind),
//     u32 ref_sequence, u32 reserved (0)
//   then, in range order, the bytes of every Data range back to back. A Zero range is `length`
//   bytes of zero; an Undefined range is declared never-read (a command that reads it makes the
//   capture incomplete); a Ref range consumes no data bytes: its bytes equal the range at the
//   same `offset` of the earlier MEMORY_BLOB chunk of the same allocation whose sequence is
//   ref_sequence (deduplication is per offset). Ranges are sorted by offset, do not overlap and
//   lie inside allocation_size. A payload is at most MaxChunkBytes; larger blobs are split into
//   several chunks of the same allocation and generation.
//
// MAP / UNMAP payload
//   u32 entry_count (<= MaxEventsPerChunk), then entries
//   MAP   (48 bytes): u32 address_space_id, u32 flags (MapFlag), u64 gpu_va, u64 size,
//                     u64 alloc_id, u64 alloc_offset, u32 generation, u32 pte_kind
//   UNMAP (24 bytes): u32 address_space_id, u32 flags, u64 gpu_va, u64 size
//
// FENCE payload: u32 entry_count, then 40-byte entries:
//   u32 kind (FenceKind), u32 channel_id, u64 id, u64 value, u64 aux, u64 submit_id
//   The replayer applies the value (as a memory/syncpoint write through the same path a real
//   external producer uses) and wakes any channel waiting on it. submit_id names the Submit event
//   whose wait this satisfies, 0 when none.
//
// GPU_EVENT payload: u32 event_count (<= MaxEventsPerChunk), then events, each
//   u32 kind (GpuEventKind), u32 body_length, body_length bytes of body.
//   ChannelCreate : u32 channel_id, u32 address_space_id, u32 flags (ChannelFlag), u32 reserved
//   ChannelRelease: u32 channel_id, u32 reserved
//   Submit        : u32 channel_id, u32 flags (SubmitFlag), u64 submit_id (dense from 1),
//                   u32 syncpoint_raise_value, u32 entry_count (<= MaxEntriesPerSubmit), then
//                   entries of 16 bytes: u64 gpu_va, u32 word_count, u32 header_flags (bit 0
//                   allow_flush, bit 1 is_push_buffer, bit 2 sync of the submitted list header)
//   Consume       : u32 channel_id, u32 entry_index, u64 submit_id, u64 consume_id (dense from 1,
//                   in the order the pusher consumed), u64 gpu_va, u32 word_offset,
//                   u32 word_count (<= MaxWordsPerConsume), u32 flags (ConsumeFlag), u32 epoch,
//                   then word_count u32 words. The words in Consume are authoritative: replay
//                   feeds exactly them, in this order, to the channel's pusher and never re-reads
//                   the pushbuffer memory (the guest may have reused it).
//
// PRESENT payload (one per accepted present, directly before the frame's pictures)
//   u32 path (PresentPath), u32 flags, u64 present_id (== chunk frame_id), u64 output_id,
//   u64 fence_id, u64 fence_value (the producing fence, 0 when none), u32 layer_count
//   (<= MaxLayersPerPresent), u32 reserved, then layer_count layers of 72 bytes:
//     u64 alloc_id, u64 alloc_offset, u32 generation, u32 framebuffer_offset, u32 width,
//     u32 height, u32 stride, u32 pixel_format, u32 transform_flags, u32 blending,
//     u32 layer_stack_mask, u32 flags (LayerFlag), s32 crop_left, s32 crop_top, s32 crop_right,
//     s32 crop_bottom
//
// FRAME_END payload
//   u64 frame_id, u32 flags (FrameFlag), u32 output_count (<= MaxSemanticOutputs),
//   u64 gpu_words (words consumed in the frame), u64 first_sequence, u64 last_sequence,
//   u64 output_id, u64 call_count, u32 effect_count, then output_count entries of 32 bytes:
//     u32 kind (SemanticKind), u32 space_tag (address space id + 1 for SemaphoreValue and
//     QueryResult, 0 for the others and when the writer did not say), u64 id, u64 value_lo,
//     u64 value_hi
//   Semantic outputs are what the guest could observe besides the picture. One definition, for
//   the writer and the comparer; a mismatch is reported separately from the picture comparison
//   and is never hidden:
//     SemaphoreValue    id = GPU virtual address, value_lo = the u32 stored there, value_hi = 0
//     QueryResult       id = GPU virtual address, value_lo = the u64 stored there, value_hi = 0
//     SyncpointValue    id = syncpoint index, value_lo = its host value at the end of the frame.
//                       Only for a syncpoint a FENCE record sets (absolute values); no recorder
//                       lists it since SyncpointDelta, but a replay still compares it when present.
//     SyncpointDelta    id = syncpoint index, value_lo = its host value at the end of the frame
//                       minus its host value when the replay started (wrapping u32). A replay does
//                       not restore syncpoint values at the checkpoint, so only a count is
//                       comparable: the writer lists a syncpoint the recorded commands raise and no
//                       FENCE record sets, and value_lo is the number of those raises since the
//                       file began.
//     ReadbackChecksum  id = allocation id, value_lo = FNV-1a 64 of backing bytes
//                       [0, value_hi) of the newest generation of the allocation after
//                       FlushRegion, value_hi = the number of bytes hashed
//   An output the replayer cannot observe is not listed, and an expected output that is missing
//   from the replay counts as a mismatch. Replay recomputes the outputs after the frame has
//   drained (Quiesce and WaitForIdle).
//
// END_PICTURE payload (optional chunk, one or two per frame)
//   u64 frame_id, u32 picture_kind (PictureKind), u32 pixel_format (PictureFormat), u32 width,
//   u32 height, u32 stride_bytes, u32 color_encoding (ColorEncoding), u64 pixel_checksum
//   (FNV-1a 64 of the pixel bytes), u64 reserved (0), then stride_bytes * height pixel bytes.
//   0.6 requires one Composed picture per selected frame; GuestFrame is optional.
//
// INDEX payload: u64 entry_count, u64 frame_count, entry_count entries of 32 bytes:
//   u32 type, u16 version, u16 flags, u64 file_offset, u64 frame_id, u64 stored_length
//   then frame_count frame entries of 40 bytes:
//   u64 frame_id, u64 first_sequence, u64 last_sequence, u32 flags (FrameFlag), u32 reserved,
//   u64 output_id
//
// COMPLETE payload (fixed): see CompleteRecord.

#pragma once

#include <array>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/common_types.h"
#include "video_core/replay/format.h"

namespace VideoCore::Replay {

// ---- SESSION --------------------------------------------------------------------------------

// Keys of the pinned settings. A key is stable forever; new keys are appended. The pins are the
// settings the replayer must apply before it creates the renderer; a replayer that cannot honour
// a pin refuses the capture (UnsupportedState). FrameWidth and FrameHeight are informational: they
// record the capturing run's frame size, but a replay sizes each picture from the present's own
// layers, as the capture does (Tegra::PictureExtent), and does not use them.
enum class SettingKey : u32 {
    Backend = 1,           // 1 = Vulkan
    FrameWidth = 2,
    FrameHeight = 3,
    Docked = 4,
    GpuAccuracy = 5,
    ResolutionSetup = 6,
    AntiAliasing = 7,
    ScalingFilter = 8,
    AsyncGpu = 9,
    ShaderBackend = 10,
    NativeTiling = 11,
    TuringMacros = 12,
    VSync = 13,            // 0 in every capture
    AstcRecompression = 14,
    OutputLayerMask = 15,
};

struct SessionPin {
    u32 key = 0;
    u64 value = 0;
};

// The value of a pin whose setting the capturing host could not read. A replay refuses a capture
// with such a pin: it cannot honour a setting nobody recorded.
inline constexpr u64 SessionPinUnknown = ~u64{0};

struct DeviceIdentity {
    u32 vendor_id = 0;
    u32 device_id = 0;
    u32 driver_id = 0;
    u32 driver_version = 0;
    std::array<u8, 16> pipeline_cache_uuid{};
};

// SESSION payload: u32 pin_count (<= MaxSessionPins), pin_count * {u32 key, u32 reserved,
// u64 value}; the device identity as u32 vendor, device, driver id, driver version + 16 uuid
// bytes; then the limits the writer ran with as eight u64 in the order of SessionLimits.
struct SessionLimits {
    u64 max_file_bytes = MaxFileBytes;
    u64 max_initial_backing_bytes = MaxInitialBackingBytes;
    u64 max_command_bytes_per_frame = MaxCommandBytesPerFrame;
    u64 max_new_payload_bytes_per_frame = MaxNewPayloadBytesPerFrame;
    u64 max_chunk_bytes = MaxChunkBytes;
    u64 max_staging_bytes = MaxStagingBytes;
    u64 reserved0 = 0;
    u64 reserved1 = 0;
};

struct SessionRecord {
    std::vector<SessionPin> pins;
    DeviceIdentity device;
    SessionLimits limits;
};

// ---- memory ---------------------------------------------------------------------------------

enum class RangeKind : u32 { Data = 1, Zero = 2, Undefined = 3, Ref = 4 };

enum class WriterKind : u32 { Cpu = 1, ExternalDevice = 2, GpuDerived = 3 };

enum BlobFlag : u32 {
    BlobInitial = 1U << 0,      // part of the state at the first recorded frame
    BlobDelta = 1U << 1,        // a change after the first recorded frame
    BlobGpuDerived = 1U << 2,   // the allocation holds GPU output; ranges are all Undefined/Ref
};

struct MemoryRange {
    u64 offset = 0;
    u64 length = 0;
    RangeKind kind = RangeKind::Data;
    WriterKind writer = WriterKind::Cpu;
    u32 ref_sequence = 0;  // Ref only: sequence of the earlier blob chunk holding the same bytes
    u64 data_offset = 0;   // decoded view: offset of the Data bytes inside `data`
};

struct MemoryBlob {
    u64 alloc_id = 0;
    u32 generation = 0;
    u32 flags = 0;
    u64 allocation_size = 0;
    std::vector<MemoryRange> ranges;
    std::vector<u8> data;
};

enum MapFlag : u32 {
    MapBigPages = 1U << 0,
    MapSparse = 1U << 1,
    MapReserved = 1U << 2,
    MapRemap = 1U << 3,  // replaces an earlier mapping of the same GPU range
};

struct MapEntry {
    u32 address_space_id = 0;
    u32 flags = 0;
    u64 gpu_va = 0;
    u64 size = 0;
    u64 alloc_id = 0;
    u64 alloc_offset = 0;
    u32 generation = 0;
    u32 pte_kind = 0;
};

struct UnmapEntry {
    u32 address_space_id = 0;
    u32 flags = 0;
    u64 gpu_va = 0;
    u64 size = 0;
};

enum class FenceKind : u32 {
    SyncpointRaised = 1,  // id = syncpoint index, value = new value
    SemaphoreWrite = 2,   // id = GPU virtual address, value = value an external producer stored
    QueryValue = 3,       // id = GPU virtual address of an externally produced query result
    Timestamp = 4,        // id = GPU virtual address, value = guest-visible timestamp
};

struct FenceEntry {
    FenceKind kind = FenceKind::SyncpointRaised;
    u32 channel_id = 0;
    u64 id = 0;
    u64 value = 0;
    u64 aux = 0;
    u64 submit_id = 0;
};

// ---- GPU events -----------------------------------------------------------------------------

enum class GpuEventKind : u32 {
    ChannelCreate = 1,
    ChannelRelease = 2,
    Submit = 3,
    Consume = 4,
};

enum ChannelFlag : u32 {
    ChannelNativeTiling = 1U << 0,
    ChannelTuringMacros = 1U << 1,
};

enum SubmitFlag : u32 {
    SubmitGenerated = 1U << 0,  // submitted by emulator code on behalf of the guest, not the guest
};

enum ConsumeFlag : u32 {
    ConsumeFromSnapshot = 1U << 0,  // the pusher read from its snapshot of the entry
    ConsumeRefreshed = 1U << 1,     // the words were re-read after a memory change
    ConsumeDeferred = 1U << 2,      // the entry was deferred until its mapping existed
    ConsumeEntryEnd = 1U << 3,      // last span of the entry
};

struct SubmitEntry {
    u64 gpu_va = 0;
    u32 word_count = 0;
    u32 header_flags = 0;
};

struct ChannelCreateEvent {
    u32 channel_id = 0;
    u32 address_space_id = 0;
    u32 flags = 0;
};

struct ChannelReleaseEvent {
    u32 channel_id = 0;
};

struct SubmitEvent {
    u32 channel_id = 0;
    u32 flags = 0;
    u64 submit_id = 0;
    u32 syncpoint_raise_value = 0;
    std::vector<SubmitEntry> entries;
};

struct ConsumeEvent {
    u32 channel_id = 0;
    u32 entry_index = 0;
    u64 submit_id = 0;
    u64 consume_id = 0;
    u64 gpu_va = 0;
    u32 word_offset = 0;
    u32 flags = 0;
    u32 epoch = 0;
    std::vector<u32> words;
};

// ---- present, frame end, pictures ----------------------------------------------------------------

enum class PresentPath : u32 { Nx2 = 1, LegacyBufferQueue = 2 };

enum LayerFlag : u32 { LayerNativeTiling = 1U << 0 };

struct PresentLayer {
    u64 alloc_id = 0;
    u64 alloc_offset = 0;
    u32 generation = 0;
    u32 framebuffer_offset = 0;
    u32 width = 0;
    u32 height = 0;
    u32 stride = 0;
    u32 pixel_format = 0;
    u32 transform_flags = 0;
    u32 blending = 0;
    u32 layer_stack_mask = 0;
    u32 flags = 0;
    s32 crop_left = 0;
    s32 crop_top = 0;
    s32 crop_right = 0;
    s32 crop_bottom = 0;
};

struct PresentRecord {
    PresentPath path = PresentPath::Nx2;
    u32 flags = 0;
    u64 present_id = 0;
    u64 output_id = 0;
    u64 fence_id = 0;
    u64 fence_value = 0;
    std::vector<PresentLayer> layers;
};

enum FrameFlag : u32 {
    FrameComplete = 1U << 0,
    FramePreroll = 1U << 1,  // executed by replay, not compared
    FrameHasLivePicture = 1U << 2,
};

enum class SemanticKind : u32 {
    SemaphoreValue = 1,
    QueryResult = 2,
    SyncpointValue = 3,
    ReadbackChecksum = 4,  // id = allocation id, value_lo = FNV-1a 64, value_hi = length
    // id = syncpoint index, value_lo = the host value minus the value at replay start (wrapping
    // u32). The recorder lists it only for a syncpoint the recorded commands raise and no FENCE
    // sets, so the number is the count of GPU raises since the start of the file.
    SyncpointDelta = 5,
};

// `space_tag` is the second word of the wire entry: for SemaphoreValue and QueryResult the id of
// the address space the GPU address belongs to plus one, 0 for every other kind and for a file
// that does not say (the replay then reads the address only when one space maps it). The same
// address in two spaces is two outputs.
struct SemanticOutput {
    SemanticKind kind = SemanticKind::SemaphoreValue;
    u64 id = 0;
    u64 value_lo = 0;
    u64 value_hi = 0;
    u32 space_tag = 0;
};

[[nodiscard]] constexpr u32 SpaceTag(u32 address_space_id) noexcept {
    return address_space_id + 1U;
}

struct FrameEndRecord {
    u64 frame_id = 0;
    u32 flags = 0;
    u64 gpu_words = 0;
    u64 first_sequence = 0;
    u64 last_sequence = 0;
    u64 output_id = 0;
    u64 call_count = 0;
    u32 effect_count = 0;
    std::vector<SemanticOutput> outputs;
};

enum class PictureKind : u32 {
    GuestFrame = 1,  // the guest's frame before composition
    Composed = 2,    // what the emulator displayed: the picture the regression check compares
};

enum class PictureFormat : u32 { B8G8R8A8Unorm = 1 };

enum class ColorEncoding : u32 { SdrSrgb = 1 };

struct EndPicture {
    u64 frame_id = 0;
    PictureKind kind = PictureKind::Composed;
    PictureFormat format = PictureFormat::B8G8R8A8Unorm;
    u32 width = 0;
    u32 height = 0;
    u32 stride_bytes = 0;
    ColorEncoding color = ColorEncoding::SdrSrgb;
    u64 pixel_checksum = 0;
    std::vector<u8> pixels;
};

// ---- INDEX and COMPLETE -----------------------------------------------------------------------------

struct IndexEntry {
    u32 type = 0;
    u16 version = 0;
    u16 flags = 0;
    u64 file_offset = 0;
    u64 frame_id = 0;
    u64 stored_length = 0;
};

struct FrameIndexEntry {
    u64 frame_id = 0;
    u64 first_sequence = 0;
    u64 last_sequence = 0;
    u32 flags = 0;
    u64 output_id = 0;
};

struct IndexRecord {
    std::vector<IndexEntry> entries;
    std::vector<FrameIndexEntry> frames;
};

enum class CompleteStatus : u32 { Incomplete = 0, Complete = 1 };

// Reasons a capture is incomplete, as bits of CompleteRecord.incomplete_reasons. A reader treats
// every nonzero bit, and every nonzero counter below, as "refuse to replay".
enum IncompleteReason : u32 {
    IncompleteLimitExceeded = 1U << 0,
    IncompleteDroppedEvents = 1U << 1,
    IncompleteUnresolvedResource = 1U << 2,
    IncompleteUnsupportedSchema = 1U << 3,
    IncompleteWriteFailed = 1U << 4,
    IncompleteInterrupted = 1U << 5,
    IncompleteUnknownProvenance = 1U << 6,
    IncompleteClosureFailed = 1U << 7,
    IncompleteCheckpointUnavailable = 1U << 8,
    IncompleteMissingFence = 1U << 9,  // a wait names a producer the capture never saw
};

// COMPLETE payload, fixed order: u32 status, u32 incomplete_reasons, u64 chain_digest,
// u64 total_frames, u64 total_chunks, u64 total_bytes, u64 gpu_words, u64 library_calls,
// u64 initial_backing_bytes, u64 delta_bytes, u64 dedup_bytes, u32 dependency_complete,
// u32 library_replay_ready, u32 unsupported_schemas, u32 unresolved_resources,
// u32 overflow_events, u32 dropped_events, u64 reserved0..reserved3 (0).
struct CompleteRecord {
    CompleteStatus status = CompleteStatus::Incomplete;
    u32 incomplete_reasons = 0;
    u64 chain_digest = 0;
    u64 total_frames = 0;
    u64 total_chunks = 0;
    u64 total_bytes = 0;
    u64 gpu_words = 0;
    u64 library_calls = 0;
    u64 initial_backing_bytes = 0;
    u64 delta_bytes = 0;
    u64 dedup_bytes = 0;
    u32 dependency_complete = 0;
    u32 library_replay_ready = 0;
    u32 unsupported_schemas = 0;
    u32 unresolved_resources = 0;
    u32 overflow_events = 0;
    u32 dropped_events = 0;
};

// A finalized capture is replayable only when this holds; it is what a reader checks last.
[[nodiscard]] constexpr bool CompleteIsClean(const CompleteRecord& record) noexcept {
    return record.status == CompleteStatus::Complete && record.incomplete_reasons == 0 &&
           record.unsupported_schemas == 0 && record.unresolved_resources == 0 &&
           record.overflow_events == 0 && record.dropped_events == 0 &&
           record.dependency_complete == 1;
}

// ---- codec (slice B: format_io.cpp) ---------------------------------------------------------------
// Encode appends one payload; Decode consumes exactly the span or returns BadPayload (or
// LimitExceeded / Truncated for a count over a bound / a short span).

void Encode(const SessionRecord& value, std::vector<u8>& out);
[[nodiscard]] ReadError Decode(std::span<const u8> payload, SessionRecord& out);
void Encode(const MemoryBlob& value, std::vector<u8>& out);
[[nodiscard]] ReadError Decode(std::span<const u8> payload, MemoryBlob& out);
void Encode(std::span<const MapEntry> value, std::vector<u8>& out);
[[nodiscard]] ReadError Decode(std::span<const u8> payload, std::vector<MapEntry>& out);
void Encode(std::span<const UnmapEntry> value, std::vector<u8>& out);
[[nodiscard]] ReadError Decode(std::span<const u8> payload, std::vector<UnmapEntry>& out);
void Encode(std::span<const FenceEntry> value, std::vector<u8>& out);
[[nodiscard]] ReadError Decode(std::span<const u8> payload, std::vector<FenceEntry>& out);
void Encode(const PresentRecord& value, std::vector<u8>& out);
[[nodiscard]] ReadError Decode(std::span<const u8> payload, PresentRecord& out);
void Encode(const FrameEndRecord& value, std::vector<u8>& out);
[[nodiscard]] ReadError Decode(std::span<const u8> payload, FrameEndRecord& out);
void Encode(const EndPicture& value, std::vector<u8>& out);
[[nodiscard]] ReadError Decode(std::span<const u8> payload, EndPicture& out);
void Encode(const IndexRecord& value, std::vector<u8>& out);
[[nodiscard]] ReadError Decode(std::span<const u8> payload, IndexRecord& out);
void Encode(const CompleteRecord& value, std::vector<u8>& out);
[[nodiscard]] ReadError Decode(std::span<const u8> payload, CompleteRecord& out);

// One decoded GPU_EVENT chunk: the events in order, tagged by kind.
struct GpuEvent {
    GpuEventKind kind = GpuEventKind::Submit;
    ChannelCreateEvent channel_create;
    ChannelReleaseEvent channel_release;
    SubmitEvent submit;
    ConsumeEvent consume;
};
void Encode(std::span<const GpuEvent> value, std::vector<u8>& out);
[[nodiscard]] ReadError Decode(std::span<const u8> payload, std::vector<GpuEvent>& out);

} // namespace VideoCore::Replay
