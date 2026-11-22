// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Payload codecs of the 0.6 capture file and the file-level writer and reader. Layouts are the
// comments of events.h, checkpoint.h, library_call.h and format.h; nothing here depends on a host
// address, a path inside a payload or the byte order of the host.

#include <algorithm>
#include <array>
#include <cerrno>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "video_core/replay/checkpoint.h"
#include "video_core/replay/events.h"
#include "video_core/replay/format_io.h"
#include "video_core/replay/library_call.h"
#include "video_core/replay/memory_restore.h"
#include "video_core/replay/present_extent.h"

// Platform headers last, so their macros cannot reach the replay headers above.
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace VideoCore::Replay {
namespace {

// Early return of a failed ReadError; used only inside this file.
#define REPLAY_TRY(expression)                                                                     \
    do {                                                                                           \
        if (const ReadError replay_try_error = (expression); replay_try_error != ReadError::None) { \
            return replay_try_error;                                                               \
        }                                                                                          \
    } while (false)

constexpr u32 KnownMapFlags = MapBigPages | MapSparse | MapReserved | MapRemap;
constexpr u32 KnownBlobFlags = BlobInitial | BlobDelta | BlobGpuDerived;
constexpr u32 KnownChannelFlags = ChannelNativeTiling | ChannelTuringMacros;
constexpr u32 KnownConsumeFlags =
    ConsumeFromSnapshot | ConsumeRefreshed | ConsumeDeferred | ConsumeEntryEnd;
constexpr u32 KnownFrameFlags = FrameComplete | FramePreroll | FrameHasLivePicture;
constexpr u32 KnownSubmitEntryFlags = 0x7;
constexpr u32 KnownIncompleteReasons = (IncompleteMissingFence << 1) - 1;
constexpr u32 MemoryRangeBytes = 32;
constexpr u32 MapEntryBytes = 48;
constexpr u32 UnmapEntryBytes = 24;
constexpr u32 FenceEntryBytes = 40;
constexpr u32 PresentHeadBytes = 48;
constexpr u32 PresentLayerBytes = 72;
constexpr u32 FrameEndHeadBytes = 60;
constexpr u32 SemanticOutputBytes = 32;
constexpr u32 EndPictureHeadBytes = 48;
constexpr u32 CompleteBytes = 136;
constexpr u32 SessionPinBytes = 16;

[[nodiscard]] bool InRange(u32 value, u32 low, u32 high) noexcept {
    return value >= low && value <= high;
}

[[nodiscard]] ReadError PayloadSize(std::span<const u8> payload) noexcept {
    return payload.size() > MaxChunkBytes ? ReadError::LimitExceeded : ReadError::None;
}

// A reader that ran dry is a short payload; a payload with bytes left over is malformed.
[[nodiscard]] ReadError Finish(const ByteReader& reader) noexcept {
    if (!reader.Ok()) {
        return ReadError::Truncated;
    }
    return reader.Remaining() == 0 ? ReadError::None : ReadError::BadPayload;
}

// Reads a u32 element count: over its bound is LimitExceeded, not fitting the remaining bytes is
// Truncated. Nothing has been allocated yet when this returns.
[[nodiscard]] ReadError ReadCount(ByteReader& reader, u64 max_count, u64 element_bytes,
                                  u32& count) noexcept {
    count = reader.Get32();
    if (!reader.Ok()) {
        count = 0;
        return ReadError::Truncated;
    }
    if (count > max_count) {
        count = 0;
        return ReadError::LimitExceeded;
    }
    if (element_bytes != 0 && count > reader.Remaining() / element_bytes) {
        count = 0;
        return ReadError::Truncated;
    }
    return ReadError::None;
}

[[nodiscard]] bool AddOverflows(u64 a, u64 b) noexcept {
    return a > std::numeric_limits<u64>::max() - b;
}

// ---- SESSION ------------------------------------------------------------------------------------

}  // namespace

void Encode(const SessionRecord& value, std::vector<u8>& out) {
    ByteWriter w{out};
    w.Put32(static_cast<u32>(value.pins.size()));
    for (const SessionPin& pin : value.pins) {
        w.Put32(pin.key);
        w.Put32(0);
        w.Put64(pin.value);
    }
    w.Put32(value.device.vendor_id);
    w.Put32(value.device.device_id);
    w.Put32(value.device.driver_id);
    w.Put32(value.device.driver_version);
    w.PutBytes(value.device.pipeline_cache_uuid);
    w.Put64(value.limits.max_file_bytes);
    w.Put64(value.limits.max_initial_backing_bytes);
    w.Put64(value.limits.max_command_bytes_per_frame);
    w.Put64(value.limits.max_new_payload_bytes_per_frame);
    w.Put64(value.limits.max_chunk_bytes);
    w.Put64(value.limits.max_staging_bytes);
    w.Put64(value.limits.reserved0);
    w.Put64(value.limits.reserved1);
}

ReadError Decode(std::span<const u8> payload, SessionRecord& out) {
    REPLAY_TRY(PayloadSize(payload));
    ByteReader r{payload};
    SessionRecord value;
    u32 pin_count = 0;
    REPLAY_TRY(ReadCount(r, MaxSessionPins, SessionPinBytes, pin_count));
    value.pins.reserve(pin_count);
    std::set<u32> keys;
    for (u32 i = 0; i < pin_count; ++i) {
        SessionPin pin;
        pin.key = r.Get32();
        const u32 reserved = r.Get32();
        pin.value = r.Get64();
        if (!r.Ok()) {
            return ReadError::Truncated;
        }
        // A key of zero does not exist and a repeated key would make the pin ambiguous.
        if (reserved != 0 || pin.key == 0 || !keys.insert(pin.key).second) {
            return ReadError::BadPayload;
        }
        value.pins.push_back(pin);
    }
    value.device.vendor_id = r.Get32();
    value.device.device_id = r.Get32();
    value.device.driver_id = r.Get32();
    value.device.driver_version = r.Get32();
    const std::span<const u8> uuid = r.GetBytes(value.device.pipeline_cache_uuid.size());
    if (!r.Ok()) {
        return ReadError::Truncated;
    }
    std::memcpy(value.device.pipeline_cache_uuid.data(), uuid.data(), uuid.size());
    SessionLimits& limits = value.limits;
    limits.max_file_bytes = r.Get64();
    limits.max_initial_backing_bytes = r.Get64();
    limits.max_command_bytes_per_frame = r.Get64();
    limits.max_new_payload_bytes_per_frame = r.Get64();
    limits.max_chunk_bytes = r.Get64();
    limits.max_staging_bytes = r.Get64();
    limits.reserved0 = r.Get64();
    limits.reserved1 = r.Get64();
    REPLAY_TRY(Finish(r));
    if (limits.reserved0 != 0 || limits.reserved1 != 0) {
        return ReadError::BadPayload;
    }
    // A capture may be stricter than the hard defaults, never looser.
    if (limits.max_file_bytes > MaxFileBytes ||
        limits.max_initial_backing_bytes > MaxInitialBackingBytes ||
        limits.max_command_bytes_per_frame > MaxCommandBytesPerFrame ||
        limits.max_new_payload_bytes_per_frame > MaxNewPayloadBytesPerFrame ||
        limits.max_chunk_bytes > MaxChunkBytes || limits.max_staging_bytes > MaxStagingBytes) {
        return ReadError::LimitExceeded;
    }
    out = std::move(value);
    return ReadError::None;
}

// ---- MEMORY_BLOB / EXTERNAL_WRITE ---------------------------------------------------------------

void Encode(const MemoryBlob& value, std::vector<u8>& out) {
    ByteWriter w{out};
    w.Put64(value.alloc_id);
    w.Put32(value.generation);
    w.Put32(value.flags);
    w.Put64(value.allocation_size);
    w.Put32(static_cast<u32>(value.ranges.size()));
    for (const MemoryRange& range : value.ranges) {
        w.Put64(range.offset);
        w.Put64(range.length);
        w.Put32(static_cast<u32>(range.kind));
        w.Put32(static_cast<u32>(range.writer));
        w.Put32(range.ref_sequence);
        w.Put32(0);
    }
    for (const MemoryRange& range : value.ranges) {
        if (range.kind != RangeKind::Data) {
            continue;
        }
        // A Data range that points outside `data` is written short on purpose: the payload then
        // no longer matches its declared ranges and the decoder refuses it, instead of the
        // encoder inventing bytes or reading outside the vector.
        if (range.data_offset >= value.data.size()) {
            continue;
        }
        const u64 available = value.data.size() - range.data_offset;
        const u64 count = std::min<u64>(range.length, available);
        w.PutBytes(std::span<const u8>{value.data}.subspan(range.data_offset, count));
    }
}

ReadError Decode(std::span<const u8> payload, MemoryBlob& out) {
    REPLAY_TRY(PayloadSize(payload));
    ByteReader r{payload};
    MemoryBlob value;
    value.alloc_id = r.Get64();
    value.generation = r.Get32();
    value.flags = r.Get32();
    value.allocation_size = r.Get64();
    u32 range_count = 0;
    REPLAY_TRY(ReadCount(r, MaxRangesPerBlob, MemoryRangeBytes, range_count));
    if (value.alloc_id == 0 || (value.flags & ~KnownBlobFlags) != 0) {
        return ReadError::BadPayload;
    }
    value.ranges.reserve(range_count);
    u64 previous_end = 0;
    u64 data_total = 0;
    for (u32 i = 0; i < range_count; ++i) {
        MemoryRange range;
        range.offset = r.Get64();
        range.length = r.Get64();
        const u32 kind = r.Get32();
        const u32 writer = r.Get32();
        range.ref_sequence = r.Get32();
        const u32 reserved = r.Get32();
        if (!r.Ok()) {
            return ReadError::Truncated;
        }
        if (reserved != 0 || !InRange(kind, 1, 4) || !InRange(writer, 1, 3) || range.length == 0) {
            return ReadError::BadPayload;
        }
        range.kind = static_cast<RangeKind>(kind);
        range.writer = static_cast<WriterKind>(writer);
        // Sorted, non-overlapping, inside the allocation.
        if (range.offset < previous_end || AddOverflows(range.offset, range.length) ||
            range.offset + range.length > value.allocation_size) {
            return ReadError::BadPayload;
        }
        previous_end = range.offset + range.length;
        if ((range.kind == RangeKind::Ref) != (range.ref_sequence != 0)) {
            return ReadError::BadPayload;
        }
        // GPU-derived bytes are recomputed by replay and are never stored.
        if (range.kind == RangeKind::Data && range.writer == WriterKind::GpuDerived) {
            return ReadError::BadPayload;
        }
        if ((value.flags & BlobGpuDerived) != 0 && range.kind != RangeKind::Undefined &&
            range.kind != RangeKind::Ref) {
            return ReadError::BadPayload;
        }
        if (range.kind == RangeKind::Data) {
            range.data_offset = data_total;
            if (AddOverflows(data_total, range.length)) {
                return ReadError::LimitExceeded;
            }
            data_total += range.length;
        }
        value.ranges.push_back(range);
    }
    if (data_total > r.Remaining()) {
        return ReadError::Truncated;
    }
    const std::span<const u8> data = r.GetBytes(static_cast<size_t>(data_total));
    REPLAY_TRY(Finish(r));
    value.data.assign(data.begin(), data.end());
    out = std::move(value);
    return ReadError::None;
}

// ---- MAP / UNMAP / FENCE ------------------------------------------------------------------------

void Encode(std::span<const MapEntry> value, std::vector<u8>& out) {
    ByteWriter w{out};
    w.Put32(static_cast<u32>(value.size()));
    for (const MapEntry& entry : value) {
        w.Put32(entry.address_space_id);
        w.Put32(entry.flags);
        w.Put64(entry.gpu_va);
        w.Put64(entry.size);
        w.Put64(entry.alloc_id);
        w.Put64(entry.alloc_offset);
        w.Put32(entry.generation);
        w.Put32(entry.pte_kind);
    }
}

ReadError Decode(std::span<const u8> payload, std::vector<MapEntry>& out) {
    REPLAY_TRY(PayloadSize(payload));
    ByteReader r{payload};
    u32 count = 0;
    REPLAY_TRY(ReadCount(r, MaxEventsPerChunk, MapEntryBytes, count));
    std::vector<MapEntry> entries;
    entries.reserve(count);
    for (u32 i = 0; i < count; ++i) {
        MapEntry entry;
        entry.address_space_id = r.Get32();
        entry.flags = r.Get32();
        entry.gpu_va = r.Get64();
        entry.size = r.Get64();
        entry.alloc_id = r.Get64();
        entry.alloc_offset = r.Get64();
        entry.generation = r.Get32();
        entry.pte_kind = r.Get32();
        if (!r.Ok()) {
            return ReadError::Truncated;
        }
        if ((entry.flags & ~KnownMapFlags) != 0 || entry.size == 0 ||
            AddOverflows(entry.gpu_va, entry.size) || AddOverflows(entry.alloc_offset, entry.size)) {
            return ReadError::BadPayload;
        }
        // Only a sparse or reserved mapping has no backing allocation.
        if (entry.alloc_id == 0 && (entry.flags & (MapSparse | MapReserved)) == 0) {
            return ReadError::BadPayload;
        }
        entries.push_back(entry);
    }
    REPLAY_TRY(Finish(r));
    out = std::move(entries);
    return ReadError::None;
}

void Encode(std::span<const UnmapEntry> value, std::vector<u8>& out) {
    ByteWriter w{out};
    w.Put32(static_cast<u32>(value.size()));
    for (const UnmapEntry& entry : value) {
        w.Put32(entry.address_space_id);
        w.Put32(entry.flags);
        w.Put64(entry.gpu_va);
        w.Put64(entry.size);
    }
}

ReadError Decode(std::span<const u8> payload, std::vector<UnmapEntry>& out) {
    REPLAY_TRY(PayloadSize(payload));
    ByteReader r{payload};
    u32 count = 0;
    REPLAY_TRY(ReadCount(r, MaxEventsPerChunk, UnmapEntryBytes, count));
    std::vector<UnmapEntry> entries;
    entries.reserve(count);
    for (u32 i = 0; i < count; ++i) {
        UnmapEntry entry;
        entry.address_space_id = r.Get32();
        entry.flags = r.Get32();
        entry.gpu_va = r.Get64();
        entry.size = r.Get64();
        if (!r.Ok()) {
            return ReadError::Truncated;
        }
        if ((entry.flags & ~KnownMapFlags) != 0 || entry.size == 0 ||
            AddOverflows(entry.gpu_va, entry.size)) {
            return ReadError::BadPayload;
        }
        entries.push_back(entry);
    }
    REPLAY_TRY(Finish(r));
    out = std::move(entries);
    return ReadError::None;
}

void Encode(std::span<const FenceEntry> value, std::vector<u8>& out) {
    ByteWriter w{out};
    w.Put32(static_cast<u32>(value.size()));
    for (const FenceEntry& entry : value) {
        w.Put32(static_cast<u32>(entry.kind));
        w.Put32(entry.channel_id);
        w.Put64(entry.id);
        w.Put64(entry.value);
        w.Put64(entry.aux);
        w.Put64(entry.submit_id);
    }
}

ReadError Decode(std::span<const u8> payload, std::vector<FenceEntry>& out) {
    REPLAY_TRY(PayloadSize(payload));
    ByteReader r{payload};
    u32 count = 0;
    REPLAY_TRY(ReadCount(r, MaxEventsPerChunk, FenceEntryBytes, count));
    std::vector<FenceEntry> entries;
    entries.reserve(count);
    for (u32 i = 0; i < count; ++i) {
        FenceEntry entry;
        const u32 kind = r.Get32();
        entry.channel_id = r.Get32();
        entry.id = r.Get64();
        entry.value = r.Get64();
        entry.aux = r.Get64();
        entry.submit_id = r.Get64();
        if (!r.Ok()) {
            return ReadError::Truncated;
        }
        if (!InRange(kind, 1, 4)) {
            return ReadError::BadPayload;
        }
        entry.kind = static_cast<FenceKind>(kind);
        entries.push_back(entry);
    }
    REPLAY_TRY(Finish(r));
    out = std::move(entries);
    return ReadError::None;
}

// ---- GPU_EVENT ----------------------------------------------------------------------------------

void Encode(std::span<const GpuEvent> value, std::vector<u8>& out) {
    ByteWriter w{out};
    w.Put32(static_cast<u32>(value.size()));
    for (const GpuEvent& event : value) {
        w.Put32(static_cast<u32>(event.kind));
        const size_t length_at = w.Size();
        w.Put32(0);
        const size_t body_start = w.Size();
        switch (event.kind) {
        case GpuEventKind::ChannelCreate:
            w.Put32(event.channel_create.channel_id);
            w.Put32(event.channel_create.address_space_id);
            w.Put32(event.channel_create.flags);
            w.Put32(0);
            break;
        case GpuEventKind::ChannelRelease:
            w.Put32(event.channel_release.channel_id);
            w.Put32(0);
            break;
        case GpuEventKind::Submit:
            w.Put32(event.submit.channel_id);
            w.Put32(event.submit.flags);
            w.Put64(event.submit.submit_id);
            w.Put32(event.submit.syncpoint_raise_value);
            w.Put32(static_cast<u32>(event.submit.entries.size()));
            for (const SubmitEntry& entry : event.submit.entries) {
                w.Put64(entry.gpu_va);
                w.Put32(entry.word_count);
                w.Put32(entry.header_flags);
            }
            break;
        case GpuEventKind::Consume:
            w.Put32(event.consume.channel_id);
            w.Put32(event.consume.entry_index);
            w.Put64(event.consume.submit_id);
            w.Put64(event.consume.consume_id);
            w.Put64(event.consume.gpu_va);
            w.Put32(event.consume.word_offset);
            w.Put32(static_cast<u32>(event.consume.words.size()));
            w.Put32(event.consume.flags);
            w.Put32(event.consume.epoch);
            for (const u32 word : event.consume.words) {
                w.Put32(word);
            }
            break;
        }
        w.Patch32(length_at, static_cast<u32>(w.Size() - body_start));
    }
}

namespace {

[[nodiscard]] ReadError DecodeGpuEventBody(GpuEventKind kind, std::span<const u8> body,
                                           GpuEvent& event) {
    ByteReader r{body};
    event.kind = kind;
    switch (kind) {
    case GpuEventKind::ChannelCreate: {
        event.channel_create.channel_id = r.Get32();
        event.channel_create.address_space_id = r.Get32();
        event.channel_create.flags = r.Get32();
        const u32 reserved = r.Get32();
        REPLAY_TRY(Finish(r));
        if (reserved != 0 || (event.channel_create.flags & ~KnownChannelFlags) != 0) {
            return ReadError::BadPayload;
        }
        return ReadError::None;
    }
    case GpuEventKind::ChannelRelease: {
        event.channel_release.channel_id = r.Get32();
        const u32 reserved = r.Get32();
        REPLAY_TRY(Finish(r));
        return reserved == 0 ? ReadError::None : ReadError::BadPayload;
    }
    case GpuEventKind::Submit: {
        SubmitEvent& submit = event.submit;
        submit.channel_id = r.Get32();
        submit.flags = r.Get32();
        submit.submit_id = r.Get64();
        submit.syncpoint_raise_value = r.Get32();
        u32 entry_count = 0;
        REPLAY_TRY(ReadCount(r, MaxEntriesPerSubmit, 16, entry_count));
        if ((submit.flags & ~static_cast<u32>(SubmitGenerated)) != 0 || submit.submit_id == 0) {
            return ReadError::BadPayload;
        }
        submit.entries.reserve(entry_count);
        for (u32 i = 0; i < entry_count; ++i) {
            SubmitEntry entry;
            entry.gpu_va = r.Get64();
            entry.word_count = r.Get32();
            entry.header_flags = r.Get32();
            if ((entry.header_flags & ~KnownSubmitEntryFlags) != 0) {
                return ReadError::BadPayload;
            }
            submit.entries.push_back(entry);
        }
        return Finish(r);
    }
    case GpuEventKind::Consume: {
        ConsumeEvent& consume = event.consume;
        consume.channel_id = r.Get32();
        consume.entry_index = r.Get32();
        consume.submit_id = r.Get64();
        consume.consume_id = r.Get64();
        consume.gpu_va = r.Get64();
        consume.word_offset = r.Get32();
        u32 word_count = 0;
        REPLAY_TRY(ReadCount(r, MaxWordsPerConsume, 0, word_count));
        consume.flags = r.Get32();
        consume.epoch = r.Get32();
        if (!r.Ok()) {
            return ReadError::Truncated;
        }
        if (word_count > r.Remaining() / 4) {
            return ReadError::Truncated;
        }
        if ((consume.flags & ~KnownConsumeFlags) != 0 || consume.consume_id == 0) {
            return ReadError::BadPayload;
        }
        consume.words.reserve(word_count);
        for (u32 i = 0; i < word_count; ++i) {
            consume.words.push_back(r.Get32());
        }
        return Finish(r);
    }
    }
    return ReadError::BadPayload;
}

}  // namespace

ReadError Decode(std::span<const u8> payload, std::vector<GpuEvent>& out) {
    REPLAY_TRY(PayloadSize(payload));
    ByteReader r{payload};
    u32 count = 0;
    REPLAY_TRY(ReadCount(r, MaxEventsPerChunk, 8, count));
    std::vector<GpuEvent> events;
    events.reserve(count);
    for (u32 i = 0; i < count; ++i) {
        const u32 kind = r.Get32();
        const u32 body_length = r.Get32();
        if (!r.Ok()) {
            return ReadError::Truncated;
        }
        if (!InRange(kind, 1, 4)) {
            return ReadError::BadPayload;
        }
        if (body_length > r.Remaining()) {
            return ReadError::Truncated;
        }
        const std::span<const u8> body = r.GetBytes(body_length);
        GpuEvent event;
        REPLAY_TRY(DecodeGpuEventBody(static_cast<GpuEventKind>(kind), body, event));
        events.push_back(std::move(event));
    }
    REPLAY_TRY(Finish(r));
    out = std::move(events);
    return ReadError::None;
}

// ---- PRESENT / FRAME_END / END_PICTURE ----------------------------------------------------------

void Encode(const PresentRecord& value, std::vector<u8>& out) {
    ByteWriter w{out};
    w.Put32(static_cast<u32>(value.path));
    w.Put32(value.flags);
    w.Put64(value.present_id);
    w.Put64(value.output_id);
    w.Put64(value.fence_id);
    w.Put64(value.fence_value);
    w.Put32(static_cast<u32>(value.layers.size()));
    w.Put32(0);
    for (const PresentLayer& layer : value.layers) {
        w.Put64(layer.alloc_id);
        w.Put64(layer.alloc_offset);
        w.Put32(layer.generation);
        w.Put32(layer.framebuffer_offset);
        w.Put32(layer.width);
        w.Put32(layer.height);
        w.Put32(layer.stride);
        w.Put32(layer.pixel_format);
        w.Put32(layer.transform_flags);
        w.Put32(layer.blending);
        w.Put32(layer.layer_stack_mask);
        w.Put32(layer.flags);
        w.Put32(static_cast<u32>(layer.crop_left));
        w.Put32(static_cast<u32>(layer.crop_top));
        w.Put32(static_cast<u32>(layer.crop_right));
        w.Put32(static_cast<u32>(layer.crop_bottom));
    }
}

ReadError Decode(std::span<const u8> payload, PresentRecord& out) {
    REPLAY_TRY(PayloadSize(payload));
    ByteReader r{payload};
    PresentRecord value;
    const u32 path = r.Get32();
    value.flags = r.Get32();
    value.present_id = r.Get64();
    value.output_id = r.Get64();
    value.fence_id = r.Get64();
    value.fence_value = r.Get64();
    u32 layer_count = 0;
    REPLAY_TRY(ReadCount(r, MaxLayersPerPresent, PresentLayerBytes, layer_count));
    const u32 reserved = r.Get32();
    if (!r.Ok()) {
        return ReadError::Truncated;
    }
    if (!InRange(path, 1, 2) || reserved != 0 || value.present_id == 0) {
        return ReadError::BadPayload;
    }
    value.path = static_cast<PresentPath>(path);
    value.layers.reserve(layer_count);
    for (u32 i = 0; i < layer_count; ++i) {
        PresentLayer layer;
        layer.alloc_id = r.Get64();
        layer.alloc_offset = r.Get64();
        layer.generation = r.Get32();
        layer.framebuffer_offset = r.Get32();
        layer.width = r.Get32();
        layer.height = r.Get32();
        layer.stride = r.Get32();
        layer.pixel_format = r.Get32();
        layer.transform_flags = r.Get32();
        layer.blending = r.Get32();
        layer.layer_stack_mask = r.Get32();
        layer.flags = r.Get32();
        layer.crop_left = static_cast<s32>(r.Get32());
        layer.crop_top = static_cast<s32>(r.Get32());
        layer.crop_right = static_cast<s32>(r.Get32());
        layer.crop_bottom = static_cast<s32>(r.Get32());
        if (!r.Ok()) {
            return ReadError::Truncated;
        }
        if (layer.width > MaxPictureDimension || layer.height > MaxPictureDimension) {
            return ReadError::LimitExceeded;
        }
        if ((layer.flags & ~static_cast<u32>(LayerNativeTiling)) != 0) {
            return ReadError::BadPayload;
        }
        value.layers.push_back(layer);
    }
    REPLAY_TRY(Finish(r));
    out = std::move(value);
    return ReadError::None;
}

void Encode(const FrameEndRecord& value, std::vector<u8>& out) {
    ByteWriter w{out};
    w.Put64(value.frame_id);
    w.Put32(value.flags);
    w.Put32(static_cast<u32>(value.outputs.size()));
    w.Put64(value.gpu_words);
    w.Put64(value.first_sequence);
    w.Put64(value.last_sequence);
    w.Put64(value.output_id);
    w.Put64(value.call_count);
    w.Put32(value.effect_count);
    for (const SemanticOutput& output : value.outputs) {
        w.Put32(static_cast<u32>(output.kind));
        w.Put32(output.space_tag);
        w.Put64(output.id);
        w.Put64(output.value_lo);
        w.Put64(output.value_hi);
    }
}

ReadError Decode(std::span<const u8> payload, FrameEndRecord& out) {
    REPLAY_TRY(PayloadSize(payload));
    ByteReader r{payload};
    FrameEndRecord value;
    value.frame_id = r.Get64();
    value.flags = r.Get32();
    u32 output_count = 0;
    REPLAY_TRY(ReadCount(r, MaxSemanticOutputs, SemanticOutputBytes, output_count));
    value.gpu_words = r.Get64();
    value.first_sequence = r.Get64();
    value.last_sequence = r.Get64();
    value.output_id = r.Get64();
    value.call_count = r.Get64();
    value.effect_count = r.Get32();
    if (!r.Ok()) {
        return ReadError::Truncated;
    }
    if (value.frame_id == 0 || (value.flags & ~KnownFrameFlags) != 0 ||
        value.last_sequence < value.first_sequence) {
        return ReadError::BadPayload;
    }
    value.outputs.reserve(output_count);
    for (u32 i = 0; i < output_count; ++i) {
        SemanticOutput output;
        const u32 kind = r.Get32();
        output.space_tag = r.Get32();
        output.id = r.Get64();
        output.value_lo = r.Get64();
        output.value_hi = r.Get64();
        if (!r.Ok()) {
            return ReadError::Truncated;
        }
        const bool memory_kind = kind == static_cast<u32>(SemanticKind::SemaphoreValue) ||
                                 kind == static_cast<u32>(SemanticKind::QueryResult);
        if (!InRange(kind, 1, 5) || (output.space_tag != 0 && !memory_kind)) {
            return ReadError::BadPayload;
        }
        output.kind = static_cast<SemanticKind>(kind);
        value.outputs.push_back(output);
    }
    REPLAY_TRY(Finish(r));
    out = std::move(value);
    return ReadError::None;
}

void Encode(const EndPicture& value, std::vector<u8>& out) {
    ByteWriter w{out};
    w.Put64(value.frame_id);
    w.Put32(static_cast<u32>(value.kind));
    w.Put32(static_cast<u32>(value.format));
    w.Put32(value.width);
    w.Put32(value.height);
    w.Put32(value.stride_bytes);
    w.Put32(static_cast<u32>(value.color));
    w.Put64(value.pixel_checksum);
    w.Put64(0);
    w.PutBytes(value.pixels);
}

ReadError Decode(std::span<const u8> payload, EndPicture& out) {
    REPLAY_TRY(PayloadSize(payload));
    ByteReader r{payload};
    EndPicture value;
    value.frame_id = r.Get64();
    const u32 kind = r.Get32();
    const u32 format = r.Get32();
    value.width = r.Get32();
    value.height = r.Get32();
    value.stride_bytes = r.Get32();
    const u32 color = r.Get32();
    value.pixel_checksum = r.Get64();
    const u64 reserved = r.Get64();
    if (!r.Ok()) {
        return ReadError::Truncated;
    }
    if (value.frame_id == 0 || !InRange(kind, 1, 2) || format != 1 || color != 1 || reserved != 0 ||
        value.width == 0 || value.height == 0) {
        return ReadError::BadPayload;
    }
    if (value.width > MaxPictureDimension || value.height > MaxPictureDimension ||
        value.stride_bytes > MaxPictureDimension * 4U) {
        return ReadError::LimitExceeded;
    }
    if (value.stride_bytes < value.width * 4U) {
        return ReadError::BadPayload;
    }
    const u64 pixel_bytes = u64{value.stride_bytes} * value.height;
    if (pixel_bytes > r.Remaining()) {
        return ReadError::Truncated;
    }
    const std::span<const u8> pixels = r.GetBytes(static_cast<size_t>(pixel_bytes));
    REPLAY_TRY(Finish(r));
    if (Fnv1a64(pixels) != value.pixel_checksum) {
        return ReadError::BadChunkChecksum;
    }
    value.kind = static_cast<PictureKind>(kind);
    value.format = static_cast<PictureFormat>(format);
    value.color = static_cast<ColorEncoding>(color);
    value.pixels.assign(pixels.begin(), pixels.end());
    out = std::move(value);
    return ReadError::None;
}

// ---- INDEX / COMPLETE ---------------------------------------------------------------------------

void Encode(const IndexRecord& value, std::vector<u8>& out) {
    ByteWriter w{out};
    w.Put64(value.entries.size());
    w.Put64(value.frames.size());
    for (const IndexEntry& entry : value.entries) {
        w.Put32(entry.type);
        w.Put16(entry.version);
        w.Put16(entry.flags);
        w.Put64(entry.file_offset);
        w.Put64(entry.frame_id);
        w.Put64(entry.stored_length);
    }
    for (const FrameIndexEntry& frame : value.frames) {
        w.Put64(frame.frame_id);
        w.Put64(frame.first_sequence);
        w.Put64(frame.last_sequence);
        w.Put32(frame.flags);
        w.Put32(0);
        w.Put64(frame.output_id);
    }
}

ReadError Decode(std::span<const u8> payload, IndexRecord& out) {
    REPLAY_TRY(PayloadSize(payload));
    ByteReader r{payload};
    const u64 entry_count = r.Get64();
    const u64 frame_count = r.Get64();
    if (!r.Ok()) {
        return ReadError::Truncated;
    }
    if (entry_count > MaxChunkCount || frame_count > MaxTotalFrames) {
        return ReadError::LimitExceeded;
    }
    if (entry_count > r.Remaining() / IndexEntryBytes ||
        frame_count > (r.Remaining() - entry_count * IndexEntryBytes) / FrameEntryBytes) {
        return ReadError::Truncated;
    }
    IndexRecord value;
    value.entries.reserve(static_cast<size_t>(entry_count));
    u64 previous_offset = 0;
    for (u64 i = 0; i < entry_count; ++i) {
        IndexEntry entry;
        entry.type = r.Get32();
        entry.version = r.Get16();
        entry.flags = r.Get16();
        entry.file_offset = r.Get64();
        entry.frame_id = r.Get64();
        entry.stored_length = r.Get64();
        if (entry.stored_length > MaxChunkBytes) {
            return ReadError::LimitExceeded;
        }
        // File offsets are strictly increasing and never inside the file header.
        if ((entry.flags & ~KnownChunkFlags) != 0 || entry.file_offset < FileHeaderBytes ||
            (i != 0 && entry.file_offset <= previous_offset)) {
            return ReadError::BadPayload;
        }
        previous_offset = entry.file_offset;
        value.entries.push_back(entry);
    }
    value.frames.reserve(static_cast<size_t>(frame_count));
    for (u64 i = 0; i < frame_count; ++i) {
        FrameIndexEntry frame;
        frame.frame_id = r.Get64();
        frame.first_sequence = r.Get64();
        frame.last_sequence = r.Get64();
        frame.flags = r.Get32();
        const u32 reserved = r.Get32();
        frame.output_id = r.Get64();
        if (reserved != 0 || (frame.flags & ~KnownFrameFlags) != 0 ||
            frame.last_sequence < frame.first_sequence) {
            return ReadError::BadPayload;
        }
        if (i != 0) {
            const FrameIndexEntry& before = value.frames.back();
            // Frames are gapless and in ascending sequence.
            if (frame.frame_id != before.frame_id + 1 ||
                frame.first_sequence <= before.last_sequence) {
                return ReadError::BadPayload;
            }
        }
        value.frames.push_back(frame);
    }
    REPLAY_TRY(Finish(r));
    out = std::move(value);
    return ReadError::None;
}

void Encode(const CompleteRecord& value, std::vector<u8>& out) {
    ByteWriter w{out};
    w.Put32(static_cast<u32>(value.status));
    w.Put32(value.incomplete_reasons);
    w.Put64(value.chain_digest);
    w.Put64(value.total_frames);
    w.Put64(value.total_chunks);
    w.Put64(value.total_bytes);
    w.Put64(value.gpu_words);
    w.Put64(value.library_calls);
    w.Put64(value.initial_backing_bytes);
    w.Put64(value.delta_bytes);
    w.Put64(value.dedup_bytes);
    w.Put32(value.dependency_complete);
    w.Put32(value.library_replay_ready);
    w.Put32(value.unsupported_schemas);
    w.Put32(value.unresolved_resources);
    w.Put32(value.overflow_events);
    w.Put32(value.dropped_events);
    w.PutZero(32);
}

ReadError Decode(std::span<const u8> payload, CompleteRecord& out) {
    REPLAY_TRY(PayloadSize(payload));
    if (payload.size() < CompleteBytes) {
        return ReadError::Truncated;
    }
    ByteReader r{payload};
    CompleteRecord value;
    const u32 status = r.Get32();
    value.incomplete_reasons = r.Get32();
    value.chain_digest = r.Get64();
    value.total_frames = r.Get64();
    value.total_chunks = r.Get64();
    value.total_bytes = r.Get64();
    value.gpu_words = r.Get64();
    value.library_calls = r.Get64();
    value.initial_backing_bytes = r.Get64();
    value.delta_bytes = r.Get64();
    value.dedup_bytes = r.Get64();
    value.dependency_complete = r.Get32();
    value.library_replay_ready = r.Get32();
    value.unsupported_schemas = r.Get32();
    value.unresolved_resources = r.Get32();
    value.overflow_events = r.Get32();
    value.dropped_events = r.Get32();
    const std::span<const u8> reserved = r.GetBytes(32);
    REPLAY_TRY(Finish(r));
    if (std::any_of(reserved.begin(), reserved.end(), [](u8 byte) { return byte != 0; })) {
        return ReadError::BadPayload;
    }
    if (!InRange(status, 0, 1) || (value.incomplete_reasons & ~KnownIncompleteReasons) != 0 ||
        value.dependency_complete > 1 || value.library_replay_ready > 1) {
        return ReadError::BadPayload;
    }
    value.status = static_cast<CompleteStatus>(status);
    out = value;
    return ReadError::None;
}

// ---- GPU_CHECKPOINT -----------------------------------------------------------------------------

namespace {

constexpr u32 MaxSubchannelBindings = 8;
constexpr u32 KnownDmaFlags = DmaNonIncrementing | DmaLastCall | DmaIncrementOnce | DmaSignalSync;
constexpr u32 CheckpointFenceBytes = 32;
constexpr u32 AddressSpaceBytes = 24;
constexpr u32 ParkedMethodBytes = 24;

[[nodiscard]] bool IsEngineClass(u32 value) noexcept {
    return InRange(value, 1, 6);
}

// Opens a sub-record; the body length is patched when it closes.
struct SubRecordScope {
    SubRecordScope(ByteWriter& writer_, SubRecordKind kind) : writer{writer_} {
        writer.Put32(static_cast<u32>(kind));
        writer.Put32(1);
        length_at = writer.Size();
        writer.Put64(0);
        body_start = writer.Size();
    }
    ~SubRecordScope() {
        writer.Patch64(length_at, writer.Size() - body_start);
    }
    SubRecordScope(const SubRecordScope&) = delete;
    SubRecordScope& operator=(const SubRecordScope&) = delete;

    ByteWriter& writer;
    size_t length_at = 0;
    size_t body_start = 0;
};

// Indices of `items` ordered by `key` (stable), so equal keys keep the producer's order.
template <typename T, typename Key>
[[nodiscard]] std::vector<size_t> OrderedBy(const std::vector<T>& items, Key key) {
    std::vector<size_t> order(items.size());
    std::iota(order.begin(), order.end(), size_t{0});
    std::stable_sort(order.begin(), order.end(),
                     [&](size_t a, size_t b) { return key(items[a]) < key(items[b]); });
    return order;
}

void PutWords(ByteWriter& w, const std::vector<u32>& words) {
    for (const u32 word : words) {
        w.Put32(word);
    }
}

[[nodiscard]] ReadError ReadWords(ByteReader& r, u32 word_count, std::vector<u32>& words) {
    if (word_count > r.Remaining() / 4) {
        return ReadError::Truncated;
    }
    words.reserve(word_count);
    for (u32 i = 0; i < word_count; ++i) {
        words.push_back(r.Get32());
    }
    return ReadError::None;
}

}  // namespace

void Encode(const GpuCheckpoint& value, std::vector<u8>& out) {
    ByteWriter w{out};
    const size_t count_at = w.Size();
    w.Put32(0);
    u32 records = 0;
    {
        ++records;
        SubRecordScope scope{w, SubRecordKind::ChannelTable};
        w.Put32(static_cast<u32>(value.channels.size()));
        for (const size_t i : OrderedBy(value.channels, [](const ChannelEntry& e) {
                 return e.channel_id;
             })) {
            const ChannelEntry& entry = value.channels[i];
            w.Put32(entry.channel_id);
            w.Put32(entry.address_space_id);
            w.Put32(entry.flags);
            w.Put32(0);
        }
    }
    {
        ++records;
        SubRecordScope scope{w, SubRecordKind::AddressSpaces};
        w.Put32(static_cast<u32>(value.address_spaces.size()));
        for (const size_t i : OrderedBy(value.address_spaces, [](const AddressSpaceEntry& e) {
                 return e.address_space_id;
             })) {
            const AddressSpaceEntry& entry = value.address_spaces[i];
            const u32 flags = entry.syncpoint_shim != 0
                                  ? (entry.flags | AddressSpaceHasSyncpointShim)
                                  : (entry.flags & ~AddressSpaceHasSyncpointShim);
            w.Put32(entry.address_space_id);
            w.Put32(entry.address_space_bits);
            w.Put32(entry.big_page_bits);
            w.Put32(flags);
            w.Put64(entry.split_address);
            if (entry.syncpoint_shim != 0) {
                w.Put64(entry.syncpoint_shim);
            }
        }
    }
    for (const size_t i : OrderedBy(value.engines, [](const EngineRegisters& e) {
             return std::pair{e.channel_id, static_cast<u32>(e.engine_class)};
         })) {
        ++records;
        const EngineRegisters& engine = value.engines[i];
        SubRecordScope scope{w, SubRecordKind::EngineRegisters};
        w.Put32(engine.channel_id);
        w.Put32(static_cast<u32>(engine.engine_class));
        w.Put32(static_cast<u32>(engine.registers.size()));
        PutWords(w, engine.registers);
    }
    for (const size_t i : OrderedBy(value.subchannels, [](const SubchannelBinding& e) {
             return e.channel_id;
         })) {
        ++records;
        const SubchannelBinding& binding = value.subchannels[i];
        SubRecordScope scope{w, SubRecordKind::SubchannelBindings};
        w.Put32(binding.channel_id);
        w.Put32(static_cast<u32>(binding.classes.size()));
        for (const EngineClass engine_class : binding.classes) {
            w.Put32(static_cast<u32>(engine_class));
        }
    }
    for (const size_t i : OrderedBy(value.macros, [](const MacroSection& e) {
             return std::pair{e.channel_id, static_cast<u32>(e.kind)};
         })) {
        ++records;
        const MacroSection& macro = value.macros[i];
        SubRecordScope scope{w, SubRecordKind::MacroSection};
        w.Put32(macro.channel_id);
        w.Put32(static_cast<u32>(macro.kind));
        w.Put32(static_cast<u32>(macro.words.size()));
        PutWords(w, macro.words);
    }
    for (const size_t i : OrderedBy(value.shadows, [](const ShadowRam& e) {
             return e.channel_id;
         })) {
        ++records;
        const ShadowRam& shadow = value.shadows[i];
        SubRecordScope scope{w, SubRecordKind::ShadowRam};
        w.Put32(shadow.channel_id);
        w.Put32(shadow.mode);
        w.Put32(static_cast<u32>(shadow.words.size()));
        PutWords(w, shadow.words);
    }
    for (const size_t i : OrderedBy(value.dma, [](const DmaState& e) { return e.channel_id; })) {
        ++records;
        const DmaState& state = value.dma[i];
        SubRecordScope scope{w, SubRecordKind::DmaState};
        w.Put32(state.channel_id);
        w.Put32(state.method);
        w.Put32(state.subchannel);
        w.Put32(state.method_count);
        w.Put32(state.length_pending);
        w.Put32(state.flags);
        w.Put64(state.dma_get);
        w.Put64(state.dma_word_offset);
        w.Put32(static_cast<u32>(state.parked.size()));
        w.Put32(0);
        w.Put64(state.split_method_end);
        for (const ParkedMethod& parked : state.parked) {
            w.Put32(parked.method);
            w.Put32(parked.subchannel);
            w.Put32(parked.method_count);
            w.Put32(parked.flags);
            w.Put64(parked.resume_address);
        }
    }
    {
        ++records;
        SubRecordScope scope{w, SubRecordKind::Fences};
        w.Put32(static_cast<u32>(value.fences.size()));
        for (const CheckpointFence& fence : value.fences) {
            w.Put32(static_cast<u32>(fence.kind));
            w.Put32(fence.channel_id);
            w.Put64(fence.id);
            w.Put64(fence.value);
            w.Put64(fence.aux);
        }
    }
    for (const size_t i : OrderedBy(value.states, [](const EngineStateSection& e) {
             return std::pair{e.channel_id, static_cast<u32>(e.kind)};
         })) {
        ++records;
        const EngineStateSection& state = value.states[i];
        SubRecordScope scope{w, SubRecordKind::EngineState};
        w.Put32(state.channel_id);
        w.Put32(static_cast<u32>(state.kind));
        w.Put32(static_cast<u32>(state.words.size()));
        PutWords(w, state.words);
    }
    w.Patch32(count_at, records);
}

namespace {

// Ordering key of a sub-record inside its kind (channel, then a secondary key); the singleton
// kinds have no key and may appear once.
struct SubKey {
    u32 primary = 0;
    u32 secondary = 0;
    [[nodiscard]] friend bool operator<(const SubKey& a, const SubKey& b) {
        return a.primary != b.primary ? a.primary < b.primary : a.secondary < b.secondary;
    }
};

[[nodiscard]] ReadError DecodeSubRecord(SubRecordKind kind, std::span<const u8> body,
                                        GpuCheckpoint& value, SubKey& key) {
    ByteReader r{body};
    switch (kind) {
    case SubRecordKind::ChannelTable: {
        u32 count = 0;
        REPLAY_TRY(ReadCount(r, MaxChannels, 16, count));
        value.channels.reserve(count);
        for (u32 i = 0; i < count; ++i) {
            ChannelEntry entry;
            entry.channel_id = r.Get32();
            entry.address_space_id = r.Get32();
            entry.flags = r.Get32();
            const u32 reserved = r.Get32();
            if (!r.Ok()) {
                return ReadError::Truncated;
            }
            if (reserved != 0 || (entry.flags & ~KnownChannelFlags) != 0 ||
                (i != 0 && entry.channel_id <= value.channels.back().channel_id)) {
                return ReadError::BadPayload;
            }
            value.channels.push_back(entry);
        }
        return Finish(r);
    }
    case SubRecordKind::AddressSpaces: {
        u32 count = 0;
        REPLAY_TRY(ReadCount(r, MaxChannels, AddressSpaceBytes, count));
        value.address_spaces.reserve(count);
        for (u32 i = 0; i < count; ++i) {
            AddressSpaceEntry entry;
            entry.address_space_id = r.Get32();
            entry.address_space_bits = r.Get32();
            entry.big_page_bits = r.Get32();
            entry.flags = r.Get32();
            entry.split_address = r.Get64();
            if ((entry.flags & AddressSpaceHasSyncpointShim) != 0) {
                entry.syncpoint_shim = r.Get64();
            }
            if (!r.Ok()) {
                return ReadError::Truncated;
            }
            if (entry.address_space_bits == 0 || entry.address_space_bits > 64 ||
                entry.big_page_bits > 64 ||
                (entry.address_space_bits < 64 &&
                 entry.split_address > (u64{1} << entry.address_space_bits)) ||
                ((entry.flags & AddressSpaceHasSyncpointShim) != 0 && entry.address_space_bits < 64 &&
                 entry.syncpoint_shim >= (u64{1} << entry.address_space_bits)) ||
                (i != 0 && entry.address_space_id <= value.address_spaces.back().address_space_id)) {
                return ReadError::BadPayload;
            }
            value.address_spaces.push_back(entry);
        }
        return Finish(r);
    }
    case SubRecordKind::EngineRegisters: {
        EngineRegisters engine;
        engine.channel_id = r.Get32();
        const u32 engine_class = r.Get32();
        u32 reg_count = 0;
        REPLAY_TRY(ReadCount(r, MaxEngineRegisters, 4, reg_count));
        if (!IsEngineClass(engine_class)) {
            return ReadError::BadPayload;
        }
        engine.engine_class = static_cast<EngineClass>(engine_class);
        REPLAY_TRY(ReadWords(r, reg_count, engine.registers));
        key = {engine.channel_id, engine_class};
        value.engines.push_back(std::move(engine));
        return Finish(r);
    }
    case SubRecordKind::SubchannelBindings: {
        SubchannelBinding binding;
        binding.channel_id = r.Get32();
        u32 count = 0;
        REPLAY_TRY(ReadCount(r, MaxSubchannelBindings, 4, count));
        binding.classes.reserve(count);
        for (u32 i = 0; i < count; ++i) {
            const u32 engine_class = r.Get32();
            if (!IsEngineClass(engine_class)) {
                return r.Ok() ? ReadError::BadPayload : ReadError::Truncated;
            }
            binding.classes.push_back(static_cast<EngineClass>(engine_class));
        }
        key = {binding.channel_id, 0};
        value.subchannels.push_back(std::move(binding));
        return Finish(r);
    }
    case SubRecordKind::MacroSection: {
        MacroSection macro;
        macro.channel_id = r.Get32();
        const u32 section = r.Get32();
        u32 word_count = 0;
        REPLAY_TRY(ReadCount(r, MaxMacroWords, 4, word_count));
        if (!InRange(section, 1, 7)) {
            return ReadError::BadPayload;
        }
        macro.kind = static_cast<MacroSectionKind>(section);
        REPLAY_TRY(ReadWords(r, word_count, macro.words));
        key = {macro.channel_id, section};
        value.macros.push_back(std::move(macro));
        return Finish(r);
    }
    case SubRecordKind::ShadowRam: {
        ShadowRam shadow;
        shadow.channel_id = r.Get32();
        shadow.mode = r.Get32();
        u32 word_count = 0;
        REPLAY_TRY(ReadCount(r, MaxMacroWords, 4, word_count));
        REPLAY_TRY(ReadWords(r, word_count, shadow.words));
        key = {shadow.channel_id, 0};
        value.shadows.push_back(std::move(shadow));
        return Finish(r);
    }
    case SubRecordKind::DmaState: {
        DmaState state;
        state.channel_id = r.Get32();
        state.method = r.Get32();
        state.subchannel = r.Get32();
        state.method_count = r.Get32();
        state.length_pending = r.Get32();
        state.flags = r.Get32();
        state.dma_get = r.Get64();
        state.dma_word_offset = r.Get64();
        u32 parked_count = 0;
        REPLAY_TRY(ReadCount(r, MaxParkedMethods, ParkedMethodBytes, parked_count));
        const u32 reserved = r.Get32();
        state.split_method_end = r.Get64();
        if (!r.Ok()) {
            return ReadError::Truncated;
        }
        if (reserved != 0 || (state.flags & ~KnownDmaFlags) != 0) {
            return ReadError::BadPayload;
        }
        state.parked.reserve(parked_count);
        for (u32 i = 0; i < parked_count; ++i) {
            ParkedMethod parked;
            parked.method = r.Get32();
            parked.subchannel = r.Get32();
            parked.method_count = r.Get32();
            parked.flags = r.Get32();
            parked.resume_address = r.Get64();
            state.parked.push_back(parked);
        }
        key = {state.channel_id, 0};
        value.dma.push_back(std::move(state));
        return Finish(r);
    }
    case SubRecordKind::Fences: {
        u32 count = 0;
        REPLAY_TRY(ReadCount(r, MaxEventsPerChunk, CheckpointFenceBytes, count));
        value.fences.reserve(count);
        for (u32 i = 0; i < count; ++i) {
            CheckpointFence fence;
            const u32 fence_kind = r.Get32();
            fence.channel_id = r.Get32();
            fence.id = r.Get64();
            fence.value = r.Get64();
            fence.aux = r.Get64();
            if (!r.Ok()) {
                return ReadError::Truncated;
            }
            if (!InRange(fence_kind, 1, 4)) {
                return ReadError::BadPayload;
            }
            fence.kind = static_cast<FenceKind>(fence_kind);
            value.fences.push_back(fence);
        }
        return Finish(r);
    }
    case SubRecordKind::EngineState: {
        EngineStateSection state;
        state.channel_id = r.Get32();
        const u32 state_kind = r.Get32();
        u32 word_count = 0;
        REPLAY_TRY(ReadCount(r, MaxMacroWords, 4, word_count));
        if (!InRange(state_kind, 1, 3)) {
            return ReadError::BadPayload;
        }
        state.kind = static_cast<EngineStateKind>(state_kind);
        REPLAY_TRY(ReadWords(r, word_count, state.words));
        key = {state.channel_id, state_kind};
        value.states.push_back(std::move(state));
        return Finish(r);
    }
    }
    return ReadError::UnsupportedState;
}

[[nodiscard]] bool IsSingletonKind(SubRecordKind kind) noexcept {
    return kind == SubRecordKind::ChannelTable || kind == SubRecordKind::AddressSpaces ||
           kind == SubRecordKind::Fences;
}

}  // namespace

ReadError Decode(std::span<const u8> payload, GpuCheckpoint& out) {
    REPLAY_TRY(PayloadSize(payload));
    ByteReader r{payload};
    u32 count = 0;
    REPLAY_TRY(ReadCount(r, MaxSubRecords, 16, count));
    GpuCheckpoint value;
    u32 previous_kind = 0;
    SubKey previous_key;
    for (u32 i = 0; i < count; ++i) {
        const u32 kind = r.Get32();
        const u32 version = r.Get32();
        const u64 body_length = r.Get64();
        if (!r.Ok()) {
            return ReadError::Truncated;
        }
        if (body_length > r.Remaining()) {
            return ReadError::Truncated;
        }
        const std::span<const u8> body = r.GetBytes(static_cast<size_t>(body_length));
        // The checkpoint is required: a kind or a layout version this reader does not know is a
        // state it cannot restore, never something to skip.
        if (!InRange(kind, static_cast<u32>(SubRecordKind::ChannelTable),
                     static_cast<u32>(SubRecordKind::EngineState)) ||
            version != 1) {
            return ReadError::UnsupportedState;
        }
        if (kind < previous_kind) {
            return ReadError::BadPayload;
        }
        const SubRecordKind sub_kind = static_cast<SubRecordKind>(kind);
        if (kind == previous_kind && IsSingletonKind(sub_kind)) {
            return ReadError::BadPayload;
        }
        SubKey key;
        REPLAY_TRY(DecodeSubRecord(sub_kind, body, value, key));
        if (kind == previous_kind && key < previous_key) {
            return ReadError::BadPayload;
        }
        previous_kind = kind;
        previous_key = key;
    }
    REPLAY_TRY(Finish(r));
    out = std::move(value);
    return ReadError::None;
}

// ---- LIBRARY_SCHEMA -----------------------------------------------------------------------------

namespace {

constexpr u32 ModuleEntryBytes = 16;
constexpr u32 TypeEntryMinBytes = 8;
constexpr u32 FunctionMinBytes = 36;
constexpr u32 KnownSchemaFlags = SchemaComplete | SchemaReturnsValue | SchemaMayBlock |
                                 SchemaCallback | SchemaCreatesObject | SchemaDestroysObject;
constexpr u32 KnownRecordFlags = RecordFromCheckpoint | RecordBlocking | RecordExternalResult;
constexpr u32 KnownExtentFlags = ExtentExternalProducer | ExtentDataOmitted;
constexpr u32 KnownArgDescFlags = ArgDescSigned | ArgDescOut | ArgDescIn;

void PutArgDesc(ByteWriter& w, const ArgDesc& desc) {
    w.Put8(static_cast<u8>(desc.kind));
    w.Put8(desc.width);
    w.Put16(desc.flags);
    w.Put32(desc.type_tag);
    w.Put32(0);
}

[[nodiscard]] bool WidthIsValid(u8 width) noexcept {
    return width == 0 || width == 1 || width == 2 || width == 4 || width == 8;
}

[[nodiscard]] ReadError GetArgDesc(ByteReader& r, ArgDesc& desc) {
    const u8 kind = r.Get8();
    desc.width = r.Get8();
    desc.flags = r.Get16();
    desc.type_tag = r.Get32();
    const u32 reserved = r.Get32();
    if (!r.Ok()) {
        return ReadError::Truncated;
    }
    if (kind > static_cast<u8>(ArgKind::CallbackRef) || !WidthIsValid(desc.width) ||
        (desc.flags & ~KnownArgDescFlags) != 0 || reserved != 0) {
        return ReadError::BadPayload;
    }
    desc.kind = static_cast<ArgKind>(kind);
    return ReadError::None;
}

// Names are printable ASCII; GetName already bounds the length.
[[nodiscard]] ReadError GetAsciiName(ByteReader& r, std::string& name) {
    const std::string_view text = r.GetName();
    if (!r.Ok()) {
        return ReadError::Truncated;
    }
    for (const char c : text) {
        if (c < 0x20 || c > 0x7E) {
            return ReadError::BadPayload;
        }
    }
    name.assign(text);
    return ReadError::None;
}

}  // namespace

void Encode(const LibrarySchema& value, std::vector<u8>& out) {
    ByteWriter w{out};
    w.Put64(value.abi_hash);
    w.Put32(value.api_word0);
    w.Put32(value.api_word1);
    w.Put32(value.pointer_bits);
    w.Put32(0);
    w.Put32(static_cast<u32>(value.modules.size()));
    for (const ModuleEntry& module : value.modules) {
        w.Put32(module.module_id);
        w.Put32(0);
        w.Put64(module.identity_hash);
    }
    w.Put32(static_cast<u32>(value.types.size()));
    for (const TypeEntry& type : value.types) {
        w.Put32(type.type_tag);
        PutName(w, type.name);
    }
    w.Put32(static_cast<u32>(value.functions.size()));
    for (const FunctionSchema& function : value.functions) {
        w.Put32(function.function_id);
        w.Put32(function.flags);
        w.Put64(function.signature_hash);
        PutName(w, function.name);
        PutArgDesc(w, function.result);
        w.Put32(static_cast<u32>(function.args.size()));
        for (const ArgDesc& arg : function.args) {
            PutArgDesc(w, arg);
        }
    }
}

ReadError Decode(std::span<const u8> payload, LibrarySchema& out) {
    REPLAY_TRY(PayloadSize(payload));
    ByteReader r{payload};
    LibrarySchema value;
    value.abi_hash = r.Get64();
    value.api_word0 = r.Get32();
    value.api_word1 = r.Get32();
    value.pointer_bits = r.Get32();
    const u32 reserved = r.Get32();
    if (!r.Ok()) {
        return ReadError::Truncated;
    }
    if (reserved != 0 || value.pointer_bits != 64) {
        return ReadError::BadPayload;
    }
    u32 module_count = 0;
    REPLAY_TRY(ReadCount(r, MaxModules, ModuleEntryBytes, module_count));
    std::set<u32> module_ids;
    value.modules.reserve(module_count);
    for (u32 i = 0; i < module_count; ++i) {
        ModuleEntry module;
        module.module_id = r.Get32();
        const u32 module_reserved = r.Get32();
        module.identity_hash = r.Get64();
        if (!r.Ok()) {
            return ReadError::Truncated;
        }
        if (module_reserved != 0 || !module_ids.insert(module.module_id).second) {
            return ReadError::BadPayload;
        }
        value.modules.push_back(module);
    }
    u32 type_count = 0;
    REPLAY_TRY(ReadCount(r, MaxTypes, TypeEntryMinBytes, type_count));
    std::set<u32> type_tags;
    value.types.reserve(type_count);
    for (u32 i = 0; i < type_count; ++i) {
        TypeEntry type;
        type.type_tag = r.Get32();
        REPLAY_TRY(GetAsciiName(r, type.name));
        if (!type_tags.insert(type.type_tag).second) {
            return ReadError::BadPayload;
        }
        value.types.push_back(std::move(type));
    }
    u32 function_count = 0;
    REPLAY_TRY(ReadCount(r, MaxFunctions, FunctionMinBytes, function_count));
    value.functions.reserve(function_count);
    for (u32 i = 0; i < function_count; ++i) {
        FunctionSchema function;
        function.function_id = r.Get32();
        function.flags = r.Get32();
        function.signature_hash = r.Get64();
        REPLAY_TRY(GetAsciiName(r, function.name));
        REPLAY_TRY(GetArgDesc(r, function.result));
        u32 arg_count = 0;
        REPLAY_TRY(ReadCount(r, MaxArgs, ArgDescBytes, arg_count));
        // Function ids are dense from 1, so an id is also its index plus one.
        if (function.function_id != i + 1 || (function.flags & ~KnownSchemaFlags) != 0) {
            return ReadError::BadPayload;
        }
        function.args.resize(arg_count);
        for (ArgDesc& arg : function.args) {
            REPLAY_TRY(GetArgDesc(r, arg));
        }
        value.functions.push_back(std::move(function));
    }
    REPLAY_TRY(Finish(r));
    out = std::move(value);
    return ReadError::None;
}

// ---- call records -------------------------------------------------------------------------------

namespace {

void PutArgValue(ByteWriter& w, const ArgValue& value) {
    w.Put8(static_cast<u8>(value.kind));
    w.Put8(value.width);
    w.Put16(value.flags);
    w.Put32(value.aux);
    w.Put64(value.value);
    w.Put64(value.value2);
}

[[nodiscard]] ReadError GetArgValue(ByteReader& r, u32 extent_count, ArgValue& out) {
    ArgValue value;
    const u8 kind = r.Get8();
    value.width = r.Get8();
    value.flags = r.Get16();
    value.aux = r.Get32();
    value.value = r.Get64();
    value.value2 = r.Get64();
    if (!r.Ok()) {
        return ReadError::Truncated;
    }
    if (kind > static_cast<u8>(ArgKind::CallbackRef) || !WidthIsValid(value.width) ||
        (value.flags & ~static_cast<u16>(ArgSigned)) != 0) {
        return ReadError::BadPayload;
    }
    value.kind = static_cast<ArgKind>(kind);
    switch (value.kind) {
    case ArgKind::None:
        // The canonical empty value: nothing else may be set.
        if (value.width != 0 || value.flags != 0 || value.aux != 0 || value.value != 0 ||
            value.value2 != 0) {
            return ReadError::BadPayload;
        }
        break;
    case ArgKind::Scalar:
        if (value.width == 0) {
            return ReadError::BadPayload;
        }
        break;
    case ArgKind::Float:
        if (value.width != 4 && value.width != 8) {
            return ReadError::BadPayload;
        }
        break;
    case ArgKind::ObjectRef:
        // Object ids are dense from 1.
        if (value.value == 0) {
            return ReadError::BadPayload;
        }
        break;
    case ArgKind::ExtentRef:
        if (value.aux >= extent_count) {
            return ReadError::BadPayload;
        }
        break;
    case ArgKind::PointerIdentity:
    case ArgKind::CallbackRef:
        break;
    }
    out = value;
    return ReadError::None;
}

}  // namespace

bool Encode(const CallRecordView& value, std::vector<u8>& out) {
    if (value.args.size() > MaxArgs || value.extents.size() > MaxExtentsPerCall ||
        value.inline_data.size() > MaxRecordBytes) {
        return false;
    }
    const u64 length = u64{CallHeaderBytes} + value.args.size() * ArgValueBytes +
                       value.extents.size() * ExtentBytes + value.inline_data.size();
    if (length > MaxRecordBytes) {
        return false;
    }
    ByteWriter w{out};
    w.Put32(static_cast<u32>(length));
    w.Put16(static_cast<u16>(value.kind));
    w.Put16(value.flags);
    w.Put64(value.call_seq);
    w.Put32(value.function_id);
    w.Put32(value.thread_id);
    w.Put64(value.frame_id);
    w.Put32(static_cast<u32>(value.link_kind));
    w.Put32(static_cast<u32>(value.args.size()));
    w.Put64(value.link_id);
    w.Put32(static_cast<u32>(value.extents.size()));
    w.Put32(static_cast<u32>(value.inline_data.size()));
    PutArgValue(w, value.result);
    for (const ArgValue& arg : value.args) {
        PutArgValue(w, arg);
    }
    for (const Extent& extent : value.extents) {
        w.Put32(static_cast<u32>(extent.role));
        w.Put32(extent.arg_index);
        w.Put64(extent.length);
        w.Put64(extent.alloc_id);
        w.Put64(extent.offset);
        w.Put32(extent.generation);
        w.Put32(extent.inline_offset);
        w.Put32(extent.flags);
        w.Put32(0);
    }
    w.PutBytes(value.inline_data);
    return true;
}

namespace {

[[nodiscard]] ReadError DecodeCallRecord(ByteReader& r, CallRecord& record) {
    const u32 length = r.Get32();
    if (!r.Ok()) {
        return ReadError::Truncated;
    }
    if (length < CallHeaderBytes || length > MaxRecordBytes) {
        return ReadError::LimitExceeded;
    }
    if (length - 4U > r.Remaining()) {
        return ReadError::Truncated;
    }
    ByteReader b{r.GetBytes(length - 4U)};
    const u16 kind = b.Get16();
    record.flags = b.Get16();
    record.call_seq = b.Get64();
    record.function_id = b.Get32();
    record.thread_id = b.Get32();
    record.frame_id = b.Get64();
    const u32 link_kind = b.Get32();
    const u32 arg_count = b.Get32();
    record.link_id = b.Get64();
    const u32 extent_count = b.Get32();
    const u32 inline_bytes = b.Get32();
    if (!b.Ok()) {
        return ReadError::Truncated;
    }
    if (arg_count > MaxArgs || extent_count > MaxExtentsPerCall) {
        return ReadError::LimitExceeded;
    }
    if (!InRange(kind, 1, 4) || (record.flags & ~KnownRecordFlags) != 0 || record.call_seq == 0 ||
        record.function_id == 0 || link_kind > static_cast<u32>(LinkKind::Fence) ||
        (link_kind == 0) != (record.link_id == 0)) {
        return ReadError::BadPayload;
    }
    const u64 expected = u64{CallHeaderBytes} + u64{arg_count} * ArgValueBytes +
                         u64{extent_count} * ExtentBytes + inline_bytes;
    if (expected != length) {
        return ReadError::BadPayload;
    }
    record.kind = static_cast<CallKind>(kind);
    record.link_kind = static_cast<LinkKind>(link_kind);
    REPLAY_TRY(GetArgValue(b, extent_count, record.result));
    record.args.resize(arg_count);
    for (ArgValue& arg : record.args) {
        REPLAY_TRY(GetArgValue(b, extent_count, arg));
    }
    record.extents.resize(extent_count);
    for (Extent& extent : record.extents) {
        const u32 role = b.Get32();
        extent.arg_index = b.Get32();
        extent.length = b.Get64();
        extent.alloc_id = b.Get64();
        extent.offset = b.Get64();
        extent.generation = b.Get32();
        extent.inline_offset = b.Get32();
        extent.flags = b.Get32();
        const u32 extent_reserved = b.Get32();
        if (!b.Ok()) {
            return ReadError::Truncated;
        }
        if (!InRange(role, 1, 3) || (extent.flags & ~KnownExtentFlags) != 0 ||
            extent_reserved != 0) {
            return ReadError::BadPayload;
        }
        extent.role = static_cast<ExtentRole>(role);
        const bool omitted = (extent.flags & ExtentDataOmitted) != 0;
        const bool has_inline = extent.inline_offset != NoInlineData;
        switch (extent.role) {
        case ExtentRole::InBytes:
            // Copied at call time: no allocation, and the data sits in the inline area unless
            // the bound forbade the copy (which the closure check reports).
            if (extent.alloc_id != 0 || has_inline == omitted) {
                return ReadError::BadPayload;
            }
            break;
        case ExtentRole::OutBytes:
            if (has_inline && (extent.flags & ExtentExternalProducer) == 0) {
                return ReadError::BadPayload;
            }
            break;
        case ExtentRole::PoolRange:
            if (extent.alloc_id == 0 || has_inline) {
                return ReadError::BadPayload;
            }
            break;
        }
        if (has_inline) {
            // Compared without adding: inline_offset + length wraps for a hostile length.
            const u64 area = inline_bytes;
            if (u64{extent.inline_offset} > area || extent.length > area - extent.inline_offset) {
                return ReadError::BadPayload;
            }
        }
    }
    const std::span<const u8> inline_data = b.GetBytes(inline_bytes);
    REPLAY_TRY(Finish(b));
    record.inline_data.assign(inline_data.begin(), inline_data.end());
    return ReadError::None;
}

}  // namespace

ReadError Decode(std::span<const u8> payload, std::vector<CallRecord>& out) {
    REPLAY_TRY(PayloadSize(payload));
    ByteReader r{payload};
    u32 count = 0;
    REPLAY_TRY(ReadCount(r, MaxRecordsPerChunk, CallHeaderBytes, count));
    std::vector<CallRecord> records;
    records.reserve(count);
    for (u32 i = 0; i < count; ++i) {
        CallRecord record;
        REPLAY_TRY(DecodeCallRecord(r, record));
        records.push_back(std::move(record));
    }
    REPLAY_TRY(Finish(r));
    out = std::move(records);
    return ReadError::None;
}

// ---- native file layer --------------------------------------------------------------------------

namespace {

[[nodiscard]] std::string SystemMessage(int code) {
    return std::error_code(code, std::system_category()).message();
}

[[nodiscard]] std::string Number(u64 value) {
    return std::to_string(value);
}

// Positional file I/O on a handle this class owns. Nothing is buffered: a capture file is large
// and written once, and the writer decides when to flush.
class NativeFile {
public:
    NativeFile() = default;
    ~NativeFile() {
        Close();
    }
    NativeFile(const NativeFile&) = delete;
    NativeFile& operator=(const NativeFile&) = delete;

    // Creates a file that must not exist (the existence check and the creation are one call).
    [[nodiscard]] bool CreateNew(const std::filesystem::path& path, std::string& error);
    [[nodiscard]] bool OpenRead(const std::filesystem::path& path, std::string& error);
    [[nodiscard]] bool WriteAt(u64 offset, std::span<const u8> bytes);
    // Reads up to out.size() bytes; `count` is less than that only at the end of the file.
    [[nodiscard]] bool ReadAt(u64 offset, std::span<u8> out, size_t& count);
    [[nodiscard]] bool Size(u64& size) const;
    [[nodiscard]] bool Flush();
    void Close() noexcept;
    [[nodiscard]] bool IsOpen() const noexcept;

private:
#ifdef _WIN32
    [[nodiscard]] bool OpenWith(const std::filesystem::path& path, DWORD access, DWORD share,
                                DWORD disposition, std::string& error);
    HANDLE handle = INVALID_HANDLE_VALUE;
#else
    [[nodiscard]] bool OpenWith(const std::filesystem::path& path, int flags, std::string& error);
    int descriptor = -1;
#endif
};

#ifdef _WIN32

bool NativeFile::OpenWith(const std::filesystem::path& path, DWORD access, DWORD share,
                          DWORD disposition, std::string& error) {
    Close();
    handle = CreateFileW(path.c_str(), access, share, nullptr, disposition, FILE_ATTRIBUTE_NORMAL,
                         nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        error = SystemMessage(static_cast<int>(GetLastError()));
        return false;
    }
    return true;
}

bool NativeFile::CreateNew(const std::filesystem::path& path, std::string& error) {
    return OpenWith(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, CREATE_NEW, error);
}

bool NativeFile::OpenRead(const std::filesystem::path& path, std::string& error) {
    return OpenWith(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    OPEN_EXISTING, error);
}

bool NativeFile::WriteAt(u64 offset, std::span<const u8> bytes) {
    size_t done = 0;
    while (done < bytes.size()) {
        const DWORD want = static_cast<DWORD>(std::min<size_t>(bytes.size() - done, 1U << 30));
        const u64 position = offset + done;
        OVERLAPPED where{};
        where.Offset = static_cast<DWORD>(position & 0xFFFFFFFFU);
        where.OffsetHigh = static_cast<DWORD>(position >> 32);
        DWORD wrote = 0;
        if (!WriteFile(handle, bytes.data() + done, want, &wrote, &where) || wrote != want) {
            return false;
        }
        done += wrote;
    }
    return true;
}

bool NativeFile::ReadAt(u64 offset, std::span<u8> out, size_t& count) {
    count = 0;
    while (count < out.size()) {
        const DWORD want = static_cast<DWORD>(std::min<size_t>(out.size() - count, 1U << 30));
        const u64 position = offset + count;
        OVERLAPPED where{};
        where.Offset = static_cast<DWORD>(position & 0xFFFFFFFFU);
        where.OffsetHigh = static_cast<DWORD>(position >> 32);
        DWORD got = 0;
        if (!ReadFile(handle, out.data() + count, want, &got, &where)) {
            if (GetLastError() == ERROR_HANDLE_EOF) {
                break;
            }
            return false;
        }
        if (got == 0) {
            break;
        }
        count += got;
    }
    return true;
}

bool NativeFile::Size(u64& size) const {
    LARGE_INTEGER value{};
    if (!GetFileSizeEx(handle, &value) || value.QuadPart < 0) {
        return false;
    }
    size = static_cast<u64>(value.QuadPart);
    return true;
}

bool NativeFile::Flush() {
    return FlushFileBuffers(handle) != 0;
}

void NativeFile::Close() noexcept {
    if (handle != INVALID_HANDLE_VALUE) {
        CloseHandle(handle);
        handle = INVALID_HANDLE_VALUE;
    }
}

bool NativeFile::IsOpen() const noexcept {
    return handle != INVALID_HANDLE_VALUE;
}

#else

bool NativeFile::OpenWith(const std::filesystem::path& path, int flags, std::string& error) {
    Close();
    descriptor = ::open(path.c_str(), flags | O_CLOEXEC, 0600);
    if (descriptor < 0) {
        error = SystemMessage(errno);
        return false;
    }
    return true;
}

bool NativeFile::CreateNew(const std::filesystem::path& path, std::string& error) {
    return OpenWith(path, O_RDWR | O_CREAT | O_EXCL, error);
}

bool NativeFile::OpenRead(const std::filesystem::path& path, std::string& error) {
    return OpenWith(path, O_RDONLY, error);
}

bool NativeFile::WriteAt(u64 offset, std::span<const u8> bytes) {
    size_t done = 0;
    while (done < bytes.size()) {
        const ssize_t wrote = ::pwrite(descriptor, bytes.data() + done, bytes.size() - done,
                                       static_cast<off_t>(offset + done));
        if (wrote < 0 && errno == EINTR) {
            continue;
        }
        if (wrote <= 0) {
            return false;
        }
        done += static_cast<size_t>(wrote);
    }
    return true;
}

bool NativeFile::ReadAt(u64 offset, std::span<u8> out, size_t& count) {
    count = 0;
    while (count < out.size()) {
        const ssize_t got = ::pread(descriptor, out.data() + count, out.size() - count,
                                    static_cast<off_t>(offset + count));
        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got < 0) {
            return false;
        }
        if (got == 0) {
            break;
        }
        count += static_cast<size_t>(got);
    }
    return true;
}

bool NativeFile::Size(u64& size) const {
    struct stat info {};
    if (::fstat(descriptor, &info) != 0 || info.st_size < 0) {
        return false;
    }
    size = static_cast<u64>(info.st_size);
    return true;
}

bool NativeFile::Flush() {
    return ::fsync(descriptor) == 0;
}

void NativeFile::Close() noexcept {
    if (descriptor >= 0) {
        ::close(descriptor);
        descriptor = -1;
    }
}

bool NativeFile::IsOpen() const noexcept {
    return descriptor >= 0;
}

#endif

// An exclusive lock on a capture name, held by the recorder from the moment it opens until it is
// finalized or abandoned. Windows: a file opened with no sharing, deleted when closed. POSIX: an
// flock on a lock file that is unlinked while still locked (the inode is re-checked after locking,
// so a waiter never locks an unlinked file).
class OutputLock {
public:
    OutputLock() = default;
    OutputLock(const OutputLock&) = delete;
    OutputLock& operator=(const OutputLock&) = delete;
    ~OutputLock() {
        Release();
    }

    [[nodiscard]] bool Acquire(const std::filesystem::path& path, std::string& error);
    void Release() noexcept;

private:
#ifdef _WIN32
    HANDLE handle = INVALID_HANDLE_VALUE;
#else
    int descriptor = -1;
    std::filesystem::path locked_path;
#endif
};

#ifdef _WIN32

bool OutputLock::Acquire(const std::filesystem::path& path, std::string& error) {
    Release();
    handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        error = SystemMessage(static_cast<int>(GetLastError()));
        return false;
    }
    return true;
}

void OutputLock::Release() noexcept {
    if (handle != INVALID_HANDLE_VALUE) {
        CloseHandle(handle);
        handle = INVALID_HANDLE_VALUE;
    }
}

#else

bool OutputLock::Acquire(const std::filesystem::path& path, std::string& error) {
    Release();
    for (int attempt = 0; attempt < 4; ++attempt) {
        const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (fd < 0) {
            error = SystemMessage(errno);
            return false;
        }
        if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
            error = SystemMessage(errno);
            ::close(fd);
            return false;
        }
        struct stat held{};
        struct stat named{};
        if (::fstat(fd, &held) == 0 && ::stat(path.c_str(), &named) == 0 &&
            held.st_ino == named.st_ino && held.st_dev == named.st_dev) {
            descriptor = fd;
            locked_path = path;
            return true;
        }
        ::close(fd);  // the previous holder unlinked the name while we waited: try again
    }
    error = "the lock file keeps changing";
    return false;
}

void OutputLock::Release() noexcept {
    if (descriptor >= 0) {
        ::unlink(locked_path.c_str());
        ::close(descriptor);
        descriptor = -1;
    }
}

#endif

// True when the file starts with a valid capture file header: a file an earlier recorder left.
[[nodiscard]] bool IsRecorderPartial(const std::filesystem::path& path) {
    NativeFile file;
    std::string ignored;
    if (!file.OpenRead(path, ignored)) {
        return false;
    }
    std::array<u8, FileHeaderBytes> bytes{};
    size_t count = 0;
    FileHeader header;
    return file.ReadAt(0, bytes, count) && count == bytes.size() &&
           DecodeFileHeader(bytes, header) == ReadError::None;
}

// Renames `from` to `to` and fails when `to` exists. Windows: MoveFileExW without
// MOVEFILE_REPLACE_EXISTING is one atomic check-and-rename, so no other process can slip a file in
// between. POSIX: link() fails with EEXIST and is atomic, but the rename is then two steps (link,
// unlink of the partial name); a crash between them leaves both names, never a replaced file.
[[nodiscard]] bool RenameNoReplace(const std::filesystem::path& from,
                                   const std::filesystem::path& to, std::string& error) {
#ifdef _WIN32
    if (MoveFileExW(from.c_str(), to.c_str(), 0) == 0) {
        error = SystemMessage(static_cast<int>(GetLastError()));
        return false;
    }
    return true;
#else
    std::error_code link_error;
    std::filesystem::create_hard_link(from, to, link_error);  // fails when `to` exists
    if (link_error) {
        error = link_error.message();
        return false;
    }
    std::error_code remove_error;
    std::filesystem::remove(from, remove_error);
    if (remove_error) {
        error = "linked the final name but could not remove the partial name: " +
                remove_error.message();
        return false;
    }
    return true;
#endif
}

}  // namespace

// ---- CaptureWriter ------------------------------------------------------------------------------

namespace {

constexpr u64 CompleteChunkBytes = u64{ChunkHeaderBytes} + CompleteBytes;
constexpr u16 CurrentChunkVersion = 1;

// Bytes the INDEX chunk will take for the given counts.
[[nodiscard]] constexpr u64 IndexChunkBytes(u64 entries, u64 frames) noexcept {
    return u64{ChunkHeaderBytes} + 16 + entries * IndexEntryBytes + frames * FrameEntryBytes;
}

[[nodiscard]] bool IsFramelessChunk(ChunkType type) noexcept {
    return ChunkTypeIsFrameless(type);
}

}  // namespace

struct CaptureWriter::Impl {
    OutputLock lock;  // declared before the file: released after it closes
    NativeFile file;
    std::filesystem::path final_path;
    std::filesystem::path partial_path;
    FileHeader header;
    bool open = false;
    bool failed = false;
    bool finished = false;
    u64 next_sequence = 0;
    u64 offset = FileHeaderBytes;
    u64 chain = Fnv1aOffset;
    u64 last_frame = 0;
    bool frame_open = false;
    bool schema_written = false;
    std::vector<IndexEntry> entries;
    std::vector<FrameIndexEntry> frames;
    std::vector<u8> scratch;

    [[nodiscard]] bool Fail(std::string& error, std::string message) {
        failed = true;
        error = std::move(message);
        return false;
    }

    // Writes one framed chunk at the end of the file. Limits are checked before a byte is written.
    // INDEX and COMPLETE are neither listed in the index nor part of the chain digest.
    [[nodiscard]] bool WriteChunk(ChunkType type, u64 frame_id, std::span<const u8> payload,
                                  std::string& error) {
        const bool extra = type == ChunkType::Index || type == ChunkType::Complete;
        if (payload.size() > MaxChunkBytes) {
            return Fail(error, "chunk payload of " + Number(payload.size()) +
                                   " bytes exceeds the chunk limit of " + Number(MaxChunkBytes));
        }
        // The file must always have room left for the INDEX and COMPLETE chunks of its own end.
        const u64 chunk_bytes = u64{ChunkHeaderBytes} + payload.size();
        const u64 listed = entries.size() + (extra ? 0 : 1);
        const u64 reserve = extra ? 0
                                  : IndexChunkBytes(listed, frames.size() + 1) + CompleteChunkBytes;
        if (offset > MaxFileBytes || chunk_bytes > MaxFileBytes - offset ||
            reserve > MaxFileBytes - offset - chunk_bytes) {
            return Fail(error, "the capture would exceed the file limit of " +
                                   Number(MaxFileBytes) + " bytes at offset " + Number(offset));
        }
        // Two more chunks (INDEX, COMPLETE) follow every chunk other than those two.
        if (next_sequence + (extra ? 1 : 3) > MaxChunkCount) {
            return Fail(error, "the capture would exceed the chunk limit of " +
                                   Number(MaxChunkCount) + " at sequence " + Number(next_sequence));
        }
        ChunkHeader chunk;
        chunk.type = static_cast<u32>(type);
        chunk.version = CurrentChunkVersion;
        chunk.flags = ChunkTypeIsOptional(type) ? static_cast<u16>(ChunkOptional) : u16{0};
        chunk.sequence = next_sequence;
        chunk.frame_id = frame_id;
        const std::array<u8, ChunkHeaderBytes> head = EncodeChunkHeader(chunk, payload);
        if (!file.WriteAt(offset, head) || !file.WriteAt(offset + ChunkHeaderBytes, payload)) {
            return Fail(error, "write failed at offset " + Number(offset));
        }
        if (!extra) {
            IndexEntry entry;
            entry.type = chunk.type;
            entry.version = chunk.version;
            entry.flags = chunk.flags;
            entry.file_offset = offset;
            entry.frame_id = frame_id;
            entry.stored_length = payload.size();
            entries.push_back(entry);
            chain = ChainStep(chain, chunk.header_checksum);
        }
        offset += chunk_bytes;
        ++next_sequence;
        return true;
    }

    // Frame bookkeeping and ordering rules the writer enforces before it writes a chunk.
    [[nodiscard]] bool CheckFrame(ChunkType type, u64 frame_id, std::string& error) {
        if (IsFramelessChunk(type)) {
            if (frame_id != 0) {
                return Fail(error, "chunk type " + Number(static_cast<u32>(type)) +
                                       " carries no frame but frame id " + Number(frame_id) +
                                       " was given");
            }
            return true;
        }
        if (frame_id == 0) {
            return Fail(error, "chunk type " + Number(static_cast<u32>(type)) +
                                   " needs a frame id, got 0");
        }
        if (!frames.empty() && frame_id < last_frame) {
            return Fail(error, "frame id " + Number(frame_id) + " goes backwards from " +
                                   Number(last_frame));
        }
        return true;
    }

    void Track(ChunkType type, u64 frame_id, const FrameEndRecord* frame_end) {
        if (type == ChunkType::LibrarySchema) {
            schema_written = true;
        }
        const u64 sequence = next_sequence - 1;
        if (frame_id != 0 && (frames.empty() || frames.back().frame_id != frame_id)) {
            FrameIndexEntry frame;
            frame.frame_id = frame_id;
            frame.first_sequence = sequence;
            frame.last_sequence = sequence;
            frames.push_back(frame);
            frame_open = true;
        }
        if (frame_id != 0) {
            last_frame = frame_id;
        }
        if (type == ChunkType::FrameEnd && frame_end != nullptr && !frames.empty()) {
            FrameIndexEntry& frame = frames.back();
            frame.last_sequence = sequence;
            frame.flags = frame_end->flags;
            frame.output_id = frame_end->output_id;
            frame_open = false;
        }
    }
};

CaptureWriter::CaptureWriter() : impl{std::make_unique<Impl>()} {}

CaptureWriter::~CaptureWriter() = default;

bool CaptureWriter::Open(const std::filesystem::path& final_path, const FileHeader& header,
                         std::string& error) {
    Impl& s = *impl;
    if (s.open || s.finished) {
        error = "the writer is already in use";
        return false;
    }
    if (final_path.extension() != std::filesystem::path{FinalExtension}) {
        error = "a capture path must end in " + std::string(FinalExtension);
        return false;
    }
    // The provisional header carries no flags and no counts; Finalize writes the real ones.
    FileHeader provisional = header;
    provisional.flags = 0;
    provisional.index_offset = 0;
    provisional.chunk_count = 0;
    provisional.file_length = 0;
    provisional.finalized_frames = 0;
    provisional.required_features |= RequiredBaseV1;
    const std::vector<u8> bytes = EncodeFileHeader(provisional);
    FileHeader checked;
    if (const ReadError bad = DecodeFileHeader(bytes, checked); bad != ReadError::None) {
        error = "the file header is not valid: " + std::string(ReadErrorName(bad));
        return false;
    }
    std::error_code ignored;
    // One recorder per capture name, from here until the capture is finalized or abandoned. Both
    // the replacement of a stale partial and the final rename happen under this lock.
    std::filesystem::path lock_path = final_path;
    lock_path += ".lock";
    std::string native_error;
    if (!s.lock.Acquire(lock_path, native_error)) {
        error = "cannot take the name lock " + lock_path.string() +
                " (another capture to this name may be running): " + native_error;
        return false;
    }
    if (std::filesystem::exists(final_path, ignored)) {
        s.lock.Release();
        error = "the capture file already exists and is never replaced: " + final_path.string();
        return false;
    }
    s.final_path = final_path;
    s.partial_path = final_path;
    s.partial_path += ".partial";
    // A partial left by an earlier capture is replaced only when it is a regular file that starts
    // with a valid capture header: ours by content, and nobody else holds the name lock. Anything
    // else is refused by the exclusive create below and left untouched.
    if (std::filesystem::is_regular_file(s.partial_path, ignored) &&
        IsRecorderPartial(s.partial_path)) {
        std::filesystem::remove(s.partial_path, ignored);
    }
    if (!s.file.CreateNew(s.partial_path, native_error)) {
        s.lock.Release();
        error = "cannot create " + s.partial_path.string() + ": " + native_error;
        return false;
    }
    if (!s.file.WriteAt(0, bytes)) {
        s.file.Close();
        error = "write failed at offset 0 of " + s.partial_path.string();
        s.failed = true;
        return false;
    }
    s.header = provisional;
    s.open = true;
    return true;
}

bool CaptureWriter::Append(ChunkType type, u64 frame_id, std::span<const u8> payload,
                           std::string& error) {
    Impl& s = *impl;
    if (!s.open || s.finished) {
        error = "the writer is not open";
        return false;
    }
    if (s.failed) {
        error = "the writer has already failed";
        return false;
    }
    if (!ChunkTypeIsKnown(static_cast<u32>(type)) || type == ChunkType::Index ||
        type == ChunkType::Complete) {
        return s.Fail(error, "chunk type " + Number(static_cast<u32>(type)) +
                                 " cannot be appended; INDEX and COMPLETE are written by Finalize");
    }
    if (!s.CheckFrame(type, frame_id, error)) {
        return false;
    }
    FrameEndRecord frame_end;
    if (type == ChunkType::FrameEnd) {
        if (!s.frame_open || s.frames.empty() || s.frames.back().frame_id != frame_id) {
            return s.Fail(error, "FRAME_END for frame " + Number(frame_id) +
                                     " without chunks of that frame");
        }
        if (const ReadError bad = Decode(payload, frame_end); bad != ReadError::None) {
            return s.Fail(error, "FRAME_END payload is not valid: " +
                                     std::string(ReadErrorName(bad)));
        }
        if (frame_end.frame_id != frame_id) {
            return s.Fail(error, "FRAME_END payload names frame " + Number(frame_end.frame_id) +
                                     " inside a chunk of frame " + Number(frame_id));
        }
    } else if (frame_id != 0 && !s.frames.empty() && frame_id != s.last_frame) {
        // A new frame opens: the previous one must have ended, and ids are gapless.
        if (s.frame_open) {
            return s.Fail(error, "frame " + Number(frame_id) + " starts before frame " +
                                     Number(s.last_frame) + " has ended");
        }
        if (frame_id != s.last_frame + 1) {
            return s.Fail(error, "frame id " + Number(frame_id) + " skips from " +
                                     Number(s.last_frame));
        }
    } else if (frame_id != 0 && !s.frames.empty() && !s.frame_open) {
        return s.Fail(error, "chunk for frame " + Number(frame_id) + " after its FRAME_END");
    }
    if (!s.WriteChunk(type, frame_id, payload, error)) {
        return false;
    }
    s.Track(type, frame_id, type == ChunkType::FrameEnd ? &frame_end : nullptr);
    return true;
}

bool CaptureWriter::Finalize(const FileHeader& header, const CompleteRecord& complete,
                             std::string& error) {
    Impl& s = *impl;
    if (!s.open || s.finished) {
        error = "the writer is not open";
        return false;
    }
    if (s.failed) {
        error = "the writer has already failed; the capture stays partial";
        return false;
    }
    if (s.frame_open) {
        return s.Fail(error, "frame " + Number(s.last_frame) + " has no FRAME_END");
    }
    if (s.frames.empty() ||
        (CompleteIsClean(complete) && header.finalized_frames != header.requested_frames) ||
        u64{header.preroll_frames} + header.finalized_frames != s.frames.size() ||
        header.first_frame < header.preroll_frames ||
        s.frames.front().frame_id != header.first_frame - header.preroll_frames) {
        return s.Fail(error, "the header frame counts do not match the " +
                                 Number(s.frames.size()) + " frames written");
    }
    IndexRecord index;
    index.entries = s.entries;
    index.frames = s.frames;
    const u64 index_offset = s.offset;
    s.scratch.clear();
    Encode(index, s.scratch);
    if (!s.WriteChunk(ChunkType::Index, 0, s.scratch, error)) {
        return false;
    }
    // The writer's own counts replace the caller's: only the writer knows them exactly.
    CompleteRecord record = complete;
    record.chain_digest = s.chain;
    record.total_frames = s.frames.size();
    record.total_chunks = s.next_sequence + 1;
    record.total_bytes = s.offset + CompleteChunkBytes;
    s.scratch.clear();
    Encode(record, s.scratch);
    if (!s.WriteChunk(ChunkType::Complete, 0, s.scratch, error)) {
        return false;
    }
    const bool clean = CompleteIsClean(record);
    FileHeader final_header = header;
    final_header.required_features |= RequiredBaseV1;
    // The writer knows exactly what it wrote, so it owns every flag the reader cross-checks.
    constexpr u32 OwnedFlags = HeaderComplete | HeaderDependencyComplete | HeaderLibraryReplayReady |
                               HeaderHasLibrarySection;
    final_header.flags &= ~OwnedFlags;
    final_header.flags |= clean ? static_cast<u32>(HeaderComplete) : 0U;
    final_header.flags |= record.dependency_complete == 1 ? static_cast<u32>(HeaderDependencyComplete) : 0U;
    final_header.flags |= record.library_replay_ready == 1 ? static_cast<u32>(HeaderLibraryReplayReady) : 0U;
    final_header.flags |= s.schema_written ? static_cast<u32>(HeaderHasLibrarySection) : 0U;
    final_header.index_offset = index_offset;
    final_header.chunk_count = s.next_sequence;
    final_header.file_length = s.offset;
    const std::vector<u8> bytes = EncodeFileHeader(final_header);
    FileHeader checked;
    if (const ReadError bad = DecodeFileHeader(bytes, checked); bad != ReadError::None) {
        return s.Fail(error, "the final file header is not valid: " +
                                 std::string(ReadErrorName(bad)));
    }
    if (!s.file.WriteAt(0, bytes) || !s.file.Flush()) {
        return s.Fail(error, "writing the final header or flushing failed");
    }
    s.file.Close();
    s.finished = true;
    s.open = false;
    if (!clean) {
        s.lock.Release();
        error = "the capture is incomplete (incomplete reasons mask " +
                Number(record.incomplete_reasons) + "); left as " + s.partial_path.string();
        return false;
    }
    // The name lock is held through the rename: no other recorder can replace the partial between
    // the close and the rename.
    std::string rename_error;
    const bool renamed = RenameNoReplace(s.partial_path, s.final_path, rename_error);
    s.lock.Release();
    if (!renamed) {
        s.failed = true;
        error = "cannot rename " + s.partial_path.string() + " to " + s.final_path.string() +
                " without replacing an existing file: " + rename_error;
        return false;
    }
    return true;
}

void CaptureWriter::Abandon() noexcept {
    impl->file.Close();
    impl->open = false;
    impl->lock.Release();
}

u64 CaptureWriter::NextSequence() const noexcept {
    return impl->next_sequence;
}

u64 CaptureWriter::BytesWritten() const noexcept {
    return impl->offset;
}

u64 CaptureWriter::ChunkCount() const noexcept {
    return impl->next_sequence;
}

u64 CaptureWriter::ChainDigest() const noexcept {
    return impl->chain;
}

bool CaptureWriter::Failed() const noexcept {
    return impl->failed;
}

// ---- CaptureReader ------------------------------------------------------------------------------

namespace {

// Reads and verifies single chunks of an open capture file.
class ChunkSource {
public:
    [[nodiscard]] ReadError Open(const std::filesystem::path& path, std::string& detail) {
        std::string error;
        if (!file_.OpenRead(path, error)) {
            detail = "cannot open " + path.string() + ": " + error;
            return ReadError::Truncated;
        }
        if (!file_.Size(size_)) {
            detail = "cannot read the size of " + path.string();
            return ReadError::Truncated;
        }
        return ReadError::None;
    }

    [[nodiscard]] u64 Size() const noexcept {
        return size_;
    }

    // Reads exactly out.size() bytes at `offset`; a file that ends earlier is Truncated.
    [[nodiscard]] ReadError ReadRaw(u64 offset, std::span<u8> out, std::string& detail) {
        if (offset > size_ || out.size() > size_ - offset) {
            detail = "file ends at offset " + Number(size_) + ": " + Number(out.size()) +
                     " bytes were needed at offset " + Number(offset);
            return ReadError::Truncated;
        }
        size_t got = 0;
        if (!file_.ReadAt(offset, out, got) || got != out.size()) {
            detail = "read failed at offset " + Number(offset);
            return ReadError::Truncated;
        }
        return ReadError::None;
    }

    // Reads the header at `offset`, checks it and its payload checksum, then the payload. The
    // payload length is checked against the file size before anything is allocated.
    [[nodiscard]] ReadError ReadChunk(u64 offset, ChunkHeader& header, std::vector<u8>& payload,
                                      std::string& detail) {
        std::array<u8, ChunkHeaderBytes> raw;
        REPLAY_TRY(ReadRaw(offset, raw, detail));
        if (const ReadError bad = DecodeChunkHeader(raw, header); bad != ReadError::None) {
            detail = "chunk header at offset " + Number(offset) + ": " +
                     std::string(ReadErrorName(bad));
            return bad;
        }
        const u64 body = offset + ChunkHeaderBytes;
        if (header.stored_length > size_ - body) {
            detail = "file ends at offset " + Number(size_) + ": the payload of the chunk with "
                     "sequence " + Number(header.sequence) + " needs " +
                     Number(header.stored_length) + " bytes at offset " + Number(body);
            return ReadError::Truncated;
        }
        payload.resize(header.stored_length);
        REPLAY_TRY(ReadRaw(body, payload, detail));
        if (const ReadError bad = VerifyPayload(header, payload); bad != ReadError::None) {
            detail = "payload of the chunk with sequence " + Number(header.sequence) +
                     " at offset " + Number(body) + ": " + std::string(ReadErrorName(bad));
            return bad;
        }
        return ReadError::None;
    }

private:
    NativeFile file_;
    u64 size_ = 0;
};

constexpr u64 MaxAllocations = 1ULL << 22;
constexpr u64 MaxCoveredRanges = 1ULL << 23;
constexpr u64 MaxSubmits = 1ULL << 24;
constexpr u64 MaxObjects = 1ULL << 22;
constexpr u64 MaxTrackedCalls = 1ULL << 26;
constexpr u64 MaxPendingPerFrame = 1ULL << 22;

// The whole-file check behind CaptureReader::Validate. One pass over the chunks in order: it
// verifies every chunk, applies the grammar of format.h and the id rules of events.h, compares the
// INDEX and COMPLETE chunks with what it walked, and runs the library closure check.
class Walker {
public:
    Walker(ChunkSource& source, const FileHeader& header) : source_{source}, header_{header} {}

    [[nodiscard]] ReadError Run(std::string& detail, ClosureResult& closure) {
        const ReadError result = Walk();
        detail = detail_;
        if (result == ReadError::None) {
            closure = closure_;
        }
        return result;
    }

private:
    enum class Phase { Start, AfterSession, AfterSchema, AfterCheckpoint, Body, AfterPresent,
                       AfterFrames, AfterIndex };

    struct BlobCover {
        u64 alloc_id = 0;
        u32 generation = 0;
        std::vector<std::pair<u64, u64>> covered;  // merged [start, end) of defined bytes
    };
    struct ObjectSlot {
        bool alive = false;
        u64 generation = 0;
    };
    struct PoolUse {
        u64 alloc_id = 0;
        u32 generation = 0;
        u64 offset = 0;
        u64 length = 0;
    };
    struct LinkUse {
        LinkKind kind = LinkKind::None;
        u64 id = 0;
    };

    [[nodiscard]] ReadError Bad(ReadError error, const std::string& what) {
        detail_ = what + " (chunk sequence " + Number(seq_) + " at offset " + Number(offset_) + ")";
        return error;
    }

    [[nodiscard]] ReadError Decoded(ReadError error, std::string_view what) {
        if (error == ReadError::None) {
            return ReadError::None;
        }
        return Bad(error, std::string(what) + " payload: " + std::string(ReadErrorName(error)));
    }

    [[nodiscard]] ReadError Walk() {
        total_frames_ = u64{header_.preroll_frames} + header_.finalized_frames;
        first_recorded_ = header_.first_frame - header_.preroll_frames;
        current_frame_ = first_recorded_;
        offset_ = FileHeaderBytes;
        std::vector<u8> payload;
        ChunkHeader chunk;
        bool finished = false;
        while (!finished) {
            if (seq_ >= header_.chunk_count) {
                return Bad(ReadError::BadIndex, "the file has more chunks than the header declares");
            }
            REPLAY_TRY(source_.ReadChunk(offset_, chunk, payload, detail_));
            if (chunk.sequence != seq_) {
                return Bad(ReadError::BadOrdering, "sequence " + Number(chunk.sequence) +
                                                       " where " + Number(seq_) + " was expected");
            }
            REPLAY_TRY(Handle(chunk, payload, finished));
            offset_ += ChunkHeaderBytes + chunk.stored_length;
            ++seq_;
        }
        if (offset_ != source_.Size() || seq_ != header_.chunk_count) {
            return Bad(ReadError::BadLength, "COMPLETE does not end the file at offset " +
                                                 Number(source_.Size()));
        }
        return ReadError::None;
    }

    void RecordChunk(const ChunkHeader& chunk) {
        IndexEntry entry;
        entry.type = chunk.type;
        entry.version = chunk.version;
        entry.flags = chunk.flags;
        entry.file_offset = offset_;
        entry.frame_id = chunk.frame_id;
        entry.stored_length = chunk.stored_length;
        entries_.push_back(entry);
        chain_ = ChainStep(chain_, chunk.header_checksum);
        if (chunk.frame_id != 0 && chunk.frame_id == current_frame_ && !frame_started_) {
            frame_started_ = true;
            frame_first_seq_ = seq_;
        }
    }

    [[nodiscard]] ReadError CheckFrame(const ChunkHeader& chunk) {
        if (chunk.frame_id == current_frame_) {
            return ReadError::None;
        }
        if (frames_done_ != 0 && chunk.frame_id == current_frame_ - 1) {
            return Bad(ReadError::DuplicateFrame, "chunk of frame " + Number(chunk.frame_id) +
                                                      " after that frame ended");
        }
        return Bad(ReadError::BadOrdering, "chunk frame id " + Number(chunk.frame_id) +
                                               " where frame " + Number(current_frame_) +
                                               " is being read");
    }

    [[nodiscard]] ReadError CheckCheckpointFrame(const ChunkHeader& chunk) {
        if (chunk.frame_id != first_recorded_) {
            return Bad(ReadError::BadOrdering, "checkpoint frame id " + Number(chunk.frame_id) +
                                                   " is not the first recorded frame " +
                                                   Number(first_recorded_));
        }
        return ReadError::None;
    }

    [[nodiscard]] ReadError Handle(const ChunkHeader& chunk, std::span<const u8> payload,
                                   bool& finished) {
        const bool optional = (chunk.flags & ChunkOptional) != 0;
        if (!ChunkTypeIsKnown(chunk.type) || chunk.version != CurrentChunkVersion) {
            if (!optional) {
                return Bad(ReadError::UnsupportedFeature,
                           "required chunk type " + Number(chunk.type) + " version " +
                               Number(chunk.version) + " is not supported");
            }
            RecordChunk(chunk);  // skipped, but it still counts for the index and the chain
            return ReadError::None;
        }
        const ChunkType type = static_cast<ChunkType>(chunk.type);
        if (optional != ChunkTypeIsOptional(type)) {
            return Bad(ReadError::BadChunkHeader, "the Optional flag does not match the type");
        }
        if (IsFramelessChunk(type) && chunk.frame_id != 0) {
            return Bad(ReadError::BadOrdering, "a chunk that owns no frame carries frame id " +
                                                   Number(chunk.frame_id));
        }
        switch (type) {
        case ChunkType::Index:
            return HandleIndex(chunk, payload);
        case ChunkType::Complete:
            finished = true;
            return HandleComplete(chunk, payload);
        default:
            break;
        }
        REPLAY_TRY(HandleBody(chunk, type, payload));
        RecordChunk(chunk);
        return ReadError::None;
    }

    [[nodiscard]] ReadError HandleBody(const ChunkHeader& chunk, ChunkType type,
                                       std::span<const u8> payload) {
        switch (type) {
        case ChunkType::Session: {
            if (phase_ != Phase::Start) {
                return Bad(ReadError::BadOrdering, "SESSION is not the first chunk");
            }
            SessionRecord session;
            REPLAY_TRY(Decoded(Decode(payload, session), "SESSION"));
            phase_ = Phase::AfterSession;
            return ReadError::None;
        }
        case ChunkType::LibrarySchema: {
            if (phase_ != Phase::AfterSession) {
                return Bad(ReadError::BadOrdering, "LIBRARY_SCHEMA must directly follow SESSION");
            }
            REPLAY_TRY(Decoded(Decode(payload, schema_), "LIBRARY_SCHEMA"));
            schema_seen_ = true;
            phase_ = Phase::AfterSchema;
            return ReadError::None;
        }
        case ChunkType::GpuCheckpoint: {
            if (phase_ != Phase::AfterSession && phase_ != Phase::AfterSchema) {
                return Bad(ReadError::BadOrdering, "GPU_CHECKPOINT is out of place");
            }
            REPLAY_TRY(CheckCheckpointFrame(chunk));
            GpuCheckpoint checkpoint;
            REPLAY_TRY(Decoded(Decode(payload, checkpoint), "GPU_CHECKPOINT"));
            phase_ = Phase::AfterCheckpoint;
            return ReadError::None;
        }
        case ChunkType::LibraryCheckpoint: {
            if (phase_ != Phase::AfterCheckpoint || !schema_seen_) {
                return Bad(ReadError::BadOrdering,
                           "LIBRARY_CHECKPOINT needs a schema and directly follows GPU_CHECKPOINT");
            }
            REPLAY_TRY(CheckCheckpointFrame(chunk));
            std::vector<CallRecord> records;
            REPLAY_TRY(Decoded(Decode(payload, records), "LIBRARY_CHECKPOINT"));
            REPLAY_TRY(AddRecords(records));
            phase_ = Phase::Body;
            return ReadError::None;
        }
        default:
            break;
        }
        if (phase_ == Phase::AfterCheckpoint) {
            phase_ = Phase::Body;
        }
        const bool frame_part = phase_ == Phase::Body;
        switch (type) {
        case ChunkType::EndPicture:
            return HandleEndPicture(chunk, payload);
        case ChunkType::FrameEnd:
            return HandleFrameEnd(chunk, payload);
        default:
            break;
        }
        if (!frame_part) {
            return Bad(ReadError::BadOrdering, "chunk type " + Number(chunk.type) +
                                                   " is out of place at this position");
        }
        REPLAY_TRY(CheckFrame(chunk));
        switch (type) {
        case ChunkType::MemoryBlob:
        case ChunkType::ExternalWrite: {
            MemoryBlob blob;
            REPLAY_TRY(Decoded(Decode(payload, blob), "memory blob"));
            return AddBlob(type, blob);
        }
        case ChunkType::Map: {
            std::vector<MapEntry> maps;
            REPLAY_TRY(Decoded(Decode(payload, maps), "MAP"));
            for (const MapEntry& map : maps) {
                if (map.alloc_id == 0) {
                    continue;
                }
                const auto found = allocs_.find({map.alloc_id, map.generation});
                if (found == allocs_.end()) {
                    return Bad(ReadError::DanglingId, "MAP names allocation " +
                                                          Number(map.alloc_id) + " generation " +
                                                          Number(map.generation) +
                                                          " that no earlier blob defines");
                }
                if (map.size > found->second || map.alloc_offset > found->second - map.size ||
                    !backing_.at(found->first).UnchangedSince(map.alloc_offset, map.size, seq_)) {
                    return Bad(ReadError::BadMapping, "MAP range lacks captured backing in allocation " +
                                                          Number(map.alloc_id));
                }
            }
            return ReadError::None;
        }
        case ChunkType::Unmap: {
            std::vector<UnmapEntry> unmaps;
            return Decoded(Decode(payload, unmaps), "UNMAP");
        }
        case ChunkType::Fence: {
            std::vector<FenceEntry> fences;
            REPLAY_TRY(Decoded(Decode(payload, fences), "FENCE"));
            for (const FenceEntry& fence : fences) {
                if (fence.submit_id >= next_submit_) {
                    return Bad(ReadError::DanglingId, "FENCE names submit " +
                                                          Number(fence.submit_id) +
                                                          " that was not recorded yet");
                }
                if (schema_seen_) {
                    if (fence_ids_.size() >= MaxPendingPerFrame) {
                        return Bad(ReadError::LimitExceeded, "too many fences in one frame");
                    }
                    fence_ids_.insert(fence.id);
                }
            }
            return ReadError::None;
        }
        case ChunkType::GpuEvent: {
            std::vector<GpuEvent> events;
            REPLAY_TRY(Decoded(Decode(payload, events), "GPU_EVENT"));
            return AddGpuEvents(events);
        }
        case ChunkType::LibraryEvent: {
            if (!schema_seen_) {
                return Bad(ReadError::BadOrdering, "LIBRARY_EVENT without a LIBRARY_SCHEMA");
            }
            std::vector<CallRecord> records;
            REPLAY_TRY(Decoded(Decode(payload, records), "LIBRARY_EVENT"));
            return AddRecords(records);
        }
        case ChunkType::Present:
            return HandlePresent(chunk, payload);
        default:
            return Bad(ReadError::BadOrdering, "chunk type " + Number(chunk.type) +
                                                   " is out of place at this position");
        }
    }

    [[nodiscard]] ReadError AddBlob(ChunkType type, const MemoryBlob& blob) {
        const std::pair<u64, u32> key{blob.alloc_id, blob.generation};
        const auto found = allocs_.find(key);
        if (found != allocs_.end()) {
            if (found->second != blob.allocation_size) {
                return Bad(ReadError::BadPayload, "allocation " + Number(blob.alloc_id) +
                                                      " changes its size between blobs");
            }
        } else {
            if (allocs_.size() >= MaxAllocations) {
                return Bad(ReadError::LimitExceeded, "too many allocations");
            }
            if (blob.allocation_size == 0 ||
                blob.allocation_size > MaxInitialBackingBytes - allocation_bytes_) {
                return Bad(ReadError::LimitExceeded, "allocation backings exceed the capture limit");
            }
            allocation_bytes_ += blob.allocation_size;
            allocs_.emplace(key, blob.allocation_size);
        }
        RangeWriteLog& backing = backing_[key];
        const auto old_segments = backing.SegmentCount();
        BlobCover cover;
        cover.alloc_id = blob.alloc_id;
        cover.generation = blob.generation;
        for (const MemoryRange& range : blob.ranges) {
            if (range.kind == RangeKind::Undefined) {
                backing.Note(range.offset, range.length, 0);
                continue;
            }
            if (range.kind == RangeKind::Ref) {
                const auto earlier = blobs_.find(range.ref_sequence);
                if (range.ref_sequence >= seq_ || earlier == blobs_.end() ||
                    earlier->second.alloc_id != blob.alloc_id ||
                    earlier->second.generation != blob.generation) {
                    return Bad(ReadError::DanglingId,
                               "Ref range names sequence " + Number(range.ref_sequence) +
                                   " that is no earlier blob of the same allocation");
                }
                if (!Covered(earlier->second, range.offset, range.length) ||
                    !backing.UnchangedSince(range.offset, range.length, range.ref_sequence)) {
                    return Bad(ReadError::DanglingId,
                               "Ref range at offset " + Number(range.offset) +
                                   " is not defined in the blob it refers to");
                }
            } else {
                backing.Note(range.offset, range.length, seq_);
            }
            if (!cover.covered.empty() && cover.covered.back().second == range.offset) {
                cover.covered.back().second = range.offset + range.length;
            } else {
                cover.covered.emplace_back(range.offset, range.offset + range.length);
            }
        }
        backing_segments_ -= old_segments;
        backing_segments_ += backing.SegmentCount();
        if (backing_segments_ > MaxCoveredRanges) {
            return Bad(ReadError::LimitExceeded, "too many backing segments in the capture");
        }
        covered_ranges_ += cover.covered.size();
        if (covered_ranges_ > MaxCoveredRanges) {
            return Bad(ReadError::LimitExceeded, "too many memory ranges in the capture");
        }
        if (type == ChunkType::MemoryBlob) {
            blobs_.emplace(seq_, std::move(cover));
        }
        return ReadError::None;
    }

    [[nodiscard]] static bool Covered(const BlobCover& cover, u64 offset, u64 length) {
        const auto next = std::upper_bound(
            cover.covered.begin(), cover.covered.end(), offset,
            [](u64 value, const std::pair<u64, u64>& interval) { return value < interval.first; });
        if (next == cover.covered.begin()) {
            return false;
        }
        const std::pair<u64, u64>& interval = *(next - 1);
        return offset >= interval.first && offset <= interval.second &&
               length <= interval.second - offset;
    }

    [[nodiscard]] ReadError AddGpuEvents(const std::vector<GpuEvent>& events) {
        for (const GpuEvent& event : events) {
            if (event.kind == GpuEventKind::Submit) {
                if (event.submit.submit_id != next_submit_) {
                    return Bad(ReadError::BadOrdering,
                               "submit id " + Number(event.submit.submit_id) +
                                   " where the dense id " + Number(next_submit_) + " was expected");
                }
                if (submit_entries_.size() >= MaxSubmits) {
                    return Bad(ReadError::LimitExceeded, "too many submits");
                }
                submit_entries_.push_back(static_cast<u32>(event.submit.entries.size()));
                if (frame_first_submit_ == 0) {
                    frame_first_submit_ = next_submit_;
                }
                frame_last_submit_ = next_submit_;
                ++next_submit_;
            } else if (event.kind == GpuEventKind::Consume) {
                const ConsumeEvent& consume = event.consume;
                if (consume.consume_id != next_consume_) {
                    return Bad(ReadError::BadOrdering,
                               "consume id " + Number(consume.consume_id) +
                                   " where the dense id " + Number(next_consume_) +
                                   " was expected");
                }
                if (consume.submit_id == 0 || consume.submit_id >= next_submit_ ||
                    consume.entry_index >= submit_entries_[consume.submit_id - 1]) {
                    return Bad(ReadError::DanglingId, "consume names submit " +
                                                          Number(consume.submit_id) + " entry " +
                                                          Number(consume.entry_index) +
                                                          " that does not exist");
                }
                ++next_consume_;
            }
        }
        return ReadError::None;
    }

    [[nodiscard]] ReadError HandlePresent(const ChunkHeader& chunk, std::span<const u8> payload) {
        PresentRecord present;
        REPLAY_TRY(Decoded(Decode(payload, present), "PRESENT"));
        if (present.present_id != chunk.frame_id) {
            return Bad(ReadError::BadOrdering, "PRESENT names frame " + Number(present.present_id) +
                                                   " inside a chunk of frame " +
                                                   Number(chunk.frame_id));
        }
        for (const PresentLayer& layer : present.layers) {
            const auto found = allocs_.find({layer.alloc_id, layer.generation});
            if (layer.alloc_id == 0 || found == allocs_.end()) {
                return Bad(ReadError::DanglingId, "PRESENT layer names allocation " +
                                                      Number(layer.alloc_id) + " generation " +
                                                      Number(layer.generation) +
                                                      " that no earlier blob defines");
            }
            const u64 offset = layer.framebuffer_offset;
            const auto length = PresentBackingBytes(layer.width, layer.height, layer.stride,
                                                    layer.pixel_format);
            if (!length) {
                return Bad(ReadError::UnsupportedState, "invalid PRESENT extent or pixel format");
            }
            if (layer.alloc_offset > found->second || offset > found->second - layer.alloc_offset ||
                !backing_.at(found->first).UnchangedSince(layer.alloc_offset + offset, *length, seq_)) {
                return Bad(ReadError::BadMapping, "PRESENT layer lacks captured backing in allocation " +
                                                      Number(layer.alloc_id));
            }
        }
        phase_ = Phase::AfterPresent;
        present_seen_ = true;
        return ReadError::None;
    }

    [[nodiscard]] ReadError HandleEndPicture(const ChunkHeader& chunk,
                                             std::span<const u8> payload) {
        if (phase_ != Phase::AfterPresent) {
            return Bad(ReadError::BadOrdering, "END_PICTURE outside the part after PRESENT");
        }
        REPLAY_TRY(CheckFrame(chunk));
        EndPicture picture;
        REPLAY_TRY(Decoded(Decode(payload, picture), "END_PICTURE"));
        if (picture.frame_id != chunk.frame_id) {
            return Bad(ReadError::BadOrdering, "END_PICTURE names frame " +
                                                   Number(picture.frame_id) +
                                                   " inside a chunk of frame " +
                                                   Number(chunk.frame_id));
        }
        u32& count = picture.kind == PictureKind::Composed ? composed_ : guest_pictures_;
        if (++count > 1) {
            return Bad(ReadError::DuplicateFrame, "frame " + Number(chunk.frame_id) +
                                                      " has two pictures of one kind");
        }
        return ReadError::None;
    }

    [[nodiscard]] ReadError HandleFrameEnd(const ChunkHeader& chunk, std::span<const u8> payload) {
        if (phase_ != Phase::AfterPresent) {
            return Bad(ReadError::BadOrdering, "FRAME_END without a PRESENT of its frame");
        }
        REPLAY_TRY(CheckFrame(chunk));
        FrameEndRecord end;
        REPLAY_TRY(Decoded(Decode(payload, end), "FRAME_END"));
        if (end.frame_id != chunk.frame_id) {
            return Bad(ReadError::BadOrdering, "FRAME_END names frame " + Number(end.frame_id) +
                                                   " inside a chunk of frame " +
                                                   Number(chunk.frame_id));
        }
        if (end.last_sequence < end.first_sequence || end.last_sequence > seq_) {
            return Bad(ReadError::BadOrdering,
                       "FRAME_END sequence range " + Number(end.first_sequence) + ".." +
                           Number(end.last_sequence) + " is not ordered or runs past its own chunk " +
                           Number(seq_));
        }
        const bool preroll = chunk.frame_id < header_.first_frame;
        if (((end.flags & FramePreroll) != 0) != preroll) {
            return Bad(ReadError::BadPayload, "the Preroll flag of frame " +
                                                  Number(chunk.frame_id) + " is wrong");
        }
        if (((end.flags & FrameHasLivePicture) != 0) != (composed_ > 0)) {
            return Bad(ReadError::BadPayload, "the live picture flag of frame " +
                                                  Number(chunk.frame_id) +
                                                  " disagrees with its pictures");
        }
        if ((header_.flags & HeaderHasLivePictures) != 0 && !preroll && composed_ == 0) {
            return Bad(ReadError::BadOrdering, "selected frame " + Number(chunk.frame_id) +
                                                   " has no composed picture");
        }
        FrameIndexEntry frame;
        frame.frame_id = chunk.frame_id;
        frame.first_sequence = frame_first_seq_;
        frame.last_sequence = seq_;
        frame.flags = end.flags;
        frame.output_id = end.output_id;
        frames_.push_back(frame);
        ResolveFrame(chunk.frame_id);
        frame_started_ = false;
        present_seen_ = false;
        composed_ = 0;
        guest_pictures_ = 0;
        ++frames_done_;
        ++current_frame_;
        phase_ = frames_done_ == total_frames_ ? Phase::AfterFrames : Phase::Body;
        return ReadError::None;
    }

    [[nodiscard]] ReadError HandleIndex(const ChunkHeader& chunk, std::span<const u8> payload) {
        if (phase_ != Phase::AfterFrames || seq_ + 2 != header_.chunk_count ||
            chunk.frame_id != 0) {
            return Bad(ReadError::BadOrdering, "INDEX is out of place");
        }
        if (offset_ != header_.index_offset) {
            return Bad(ReadError::BadIndex, "INDEX is at offset " + Number(offset_) +
                                                " but the header says " +
                                                Number(header_.index_offset));
        }
        IndexRecord index;
        REPLAY_TRY(Decoded(Decode(payload, index), "INDEX"));
        if (index.entries.size() != entries_.size() || index.frames.size() != frames_.size()) {
            return Bad(ReadError::BadIndex, "the index lists " + Number(index.entries.size()) +
                                                " chunks and " + Number(index.frames.size()) +
                                                " frames; the file has " +
                                                Number(entries_.size()) + " and " +
                                                Number(frames_.size()));
        }
        for (size_t i = 0; i < entries_.size(); ++i) {
            const IndexEntry& a = index.entries[i];
            const IndexEntry& b = entries_[i];
            if (a.type != b.type || a.version != b.version || a.flags != b.flags ||
                a.file_offset != b.file_offset || a.frame_id != b.frame_id ||
                a.stored_length != b.stored_length) {
                return Bad(ReadError::BadIndex, "index entry " + Number(i) +
                                                    " disagrees with the chunk at offset " +
                                                    Number(b.file_offset));
            }
        }
        for (size_t i = 0; i < frames_.size(); ++i) {
            const FrameIndexEntry& a = index.frames[i];
            const FrameIndexEntry& b = frames_[i];
            if (a.frame_id != b.frame_id || a.first_sequence != b.first_sequence ||
                a.last_sequence != b.last_sequence || a.flags != b.flags ||
                a.output_id != b.output_id) {
                return Bad(ReadError::BadIndex, "frame entry " + Number(i) +
                                                    " disagrees with frame " +
                                                    Number(b.frame_id));
            }
        }
        phase_ = Phase::AfterIndex;
        return ReadError::None;
    }

    [[nodiscard]] ReadError HandleComplete(const ChunkHeader& chunk, std::span<const u8> payload) {
        if (phase_ != Phase::AfterIndex || seq_ + 1 != header_.chunk_count ||
            chunk.frame_id != 0) {
            return Bad(ReadError::BadOrdering, "COMPLETE is out of place");
        }
        CompleteRecord complete;
        REPLAY_TRY(Decoded(Decode(payload, complete), "COMPLETE"));
        if (complete.chain_digest != chain_) {
            return Bad(ReadError::BadDigest, "the chain digest of COMPLETE does not match the chunks");
        }
        if (complete.total_frames != frames_done_ || complete.total_chunks != header_.chunk_count ||
            complete.total_bytes != source_.Size()) {
            return Bad(ReadError::BadIndex, "the totals of COMPLETE disagree with the file");
        }
        if (!CompleteIsClean(complete)) {
            return Bad(ReadError::NotComplete, "COMPLETE reports an incomplete capture");
        }
        FinishClosure();
        const bool header_dependency = (header_.flags & HeaderDependencyComplete) != 0;
        const bool header_ready = (header_.flags & HeaderLibraryReplayReady) != 0;
        const bool header_library = (header_.flags & HeaderHasLibrarySection) != 0;
        if (header_dependency != (complete.dependency_complete == 1) ||
            header_ready != (complete.library_replay_ready == 1) || header_library != schema_seen_) {
            return Bad(ReadError::BadIndex, "the header flags disagree with COMPLETE or the chunks");
        }
        if (header_ready && !closure_.ready) {
            return Bad(ReadError::BadIndex, "the header claims library replay readiness but the "
                                            "library closure check fails");
        }
        return ReadError::None;
    }

    // ---- library closure ----------------------------------------------------------------------

    [[nodiscard]] ReadError AddRecords(const std::vector<CallRecord>& records) {
        for (const CallRecord& record : records) {
            REPLAY_TRY(AddRecord(record));
        }
        return ReadError::None;
    }

    void Resolve(const ArgValue& value) {
        const u64 id = value.value;
        if (id == 0 || id > objects_.size() || !objects_[id - 1].alive ||
            objects_[id - 1].generation != value.value2) {
            ++closure_.dangling_references;
        }
    }

    [[nodiscard]] ReadError Define(const ArgValue& value) {
        const u64 id = value.value;
        if (id == 0) {
            ++closure_.dangling_references;
            return ReadError::None;
        }
        if (id > MaxObjects) {
            return Bad(ReadError::LimitExceeded, "object id " + Number(id) + " is beyond the limit");
        }
        if (id > objects_.size()) {
            objects_.resize(id);
        }
        ObjectSlot& slot = objects_[id - 1];
        if (slot.alive) {
            ++closure_.dangling_references;
        }
        slot.alive = true;
        slot.generation = value.value2;
        return ReadError::None;
    }

    // The description of the n-th value a record lists. A Call lists every argument; a Return
    // lists only the Out arguments, in schema order.
    [[nodiscard]] static const ArgDesc* DescFor(const FunctionSchema& function, bool call_like,
                                                size_t index) {
        if (call_like) {
            return index < function.args.size() ? &function.args[index] : nullptr;
        }
        size_t seen = 0;
        for (const ArgDesc& desc : function.args) {
            if ((desc.flags & ArgDescOut) == 0) {
                continue;
            }
            if (seen++ == index) {
                return &desc;
            }
        }
        return nullptr;
    }

    [[nodiscard]] ReadError AddRecord(const CallRecord& record) {
        const bool call_like =
            record.kind == CallKind::Call || record.kind == CallKind::CallbackEnter;
        const FunctionSchema* function = nullptr;
        if (record.function_id >= 1 && record.function_id <= schema_.functions.size()) {
            function = &schema_.functions[record.function_id - 1];
        }
        if (function == nullptr) {
            ++closure_.dangling_references;
        } else if ((function->flags & SchemaComplete) == 0) {
            unsupported_.insert(record.function_id);
        }
        for (const Extent& extent : record.extents) {
            if ((extent.flags & ExtentDataOmitted) != 0) {
                unsupported_.insert(record.function_id);
            }
            if (extent.role == ExtentRole::PoolRange) {
                if (pending_pools_.size() >= MaxPendingPerFrame) {
                    return Bad(ReadError::LimitExceeded, "too many pool ranges in one frame");
                }
                pending_pools_.push_back({extent.alloc_id, extent.generation, extent.offset,
                                          extent.length});
            }
        }
        if (record.link_kind != LinkKind::None) {
            if (pending_links_.size() >= MaxPendingPerFrame) {
                return Bad(ReadError::LimitExceeded, "too many links in one frame");
            }
            pending_links_.push_back({record.link_kind, record.link_id});
        }
        if (record.call_seq == 0) {
            ++closure_.dangling_references;
        } else {
            if (record.call_seq > MaxTrackedCalls) {
                return Bad(ReadError::LimitExceeded, "call_seq " + Number(record.call_seq) +
                                                         " is beyond the limit");
            }
            if (record.call_seq >= call_state_.size()) {
                call_state_.resize(record.call_seq + 1, 0);
            }
            u8& state = call_state_[record.call_seq];
            if (call_like) {
                if (state != 0) {
                    ++closure_.dangling_references;
                } else {
                    state = 1;
                    ++calls_seen_;
                    max_call_seq_ = std::max(max_call_seq_, record.call_seq);
                }
            } else if (state != 1) {
                ++closure_.dangling_references;
            } else {
                state = 2;
            }
        }
        if (function == nullptr) {
            return ReadError::None;
        }
        const bool creates = (function->flags & SchemaCreatesObject) != 0;
        const bool destroys = record.kind == CallKind::Call &&
                              (function->flags & SchemaDestroysObject) != 0;
        const size_t expected = call_like ? function->args.size()
                                          : static_cast<size_t>(std::count_if(
                                                function->args.begin(), function->args.end(),
                                                [](const ArgDesc& desc) {
                                                    return (desc.flags & ArgDescOut) != 0;
                                                }));
        if (record.args.size() != expected) {
            unsupported_.insert(record.function_id);
        }
        std::vector<u64> to_destroy;
        for (size_t i = 0; i < record.args.size(); ++i) {
            const ArgValue& value = record.args[i];
            const ArgDesc* desc = DescFor(*function, call_like, i);
            if (value.kind == ArgKind::ObjectRef && desc != nullptr) {
                const bool out_only = (desc->flags & ArgDescOut) != 0 &&
                                      (desc->flags & ArgDescIn) == 0;
                if (call_like) {
                    if (!out_only) {
                        Resolve(value);
                        if (destroys) {
                            to_destroy.push_back(value.value);
                        }
                    }
                } else if (creates && out_only) {
                    REPLAY_TRY(Define(value));
                } else {
                    Resolve(value);
                }
            } else if (value.kind == ArgKind::CallbackRef) {
                const bool known = std::any_of(
                    schema_.modules.begin(), schema_.modules.end(),
                    [&value](const ModuleEntry& module) { return module.module_id == value.value2; });
                if (!known) {
                    ++closure_.dangling_references;
                }
            } else if (value.kind == ArgKind::ExtentRef && value.aux >= record.extents.size()) {
                ++closure_.dangling_references;
            }
        }
        if (!call_like && record.result.kind == ArgKind::ObjectRef) {
            if (creates) {
                REPLAY_TRY(Define(record.result));
            } else {
                Resolve(record.result);
            }
        }
        for (const u64 id : to_destroy) {
            if (id != 0 && id <= objects_.size()) {
                objects_[id - 1].alive = false;
            }
        }
        return ReadError::None;
    }

    // Pool ranges and links are resolved when their frame ends, against what that frame holds.
    void ResolveFrame(u64 frame_id) {
        for (const PoolUse& pool : pending_pools_) {
            const auto found = allocs_.find({pool.alloc_id, pool.generation});
            if (found == allocs_.end() || pool.length > found->second ||
                pool.offset > found->second - pool.length) {
                ++closure_.dangling_references;
            }
        }
        for (const LinkUse& link : pending_links_) {
            bool resolved = false;
            switch (link.kind) {
            case LinkKind::Submit:
                resolved = frame_first_submit_ != 0 && link.id >= frame_first_submit_ &&
                           link.id <= frame_last_submit_;
                break;
            case LinkKind::Present:
                resolved = link.id == frame_id;
                break;
            case LinkKind::Fence:
                resolved = fence_ids_.count(link.id) != 0;
                break;
            case LinkKind::None:
                resolved = true;
                break;
            }
            if (!resolved) {
                ++closure_.unresolved_links;
            }
        }
        pending_pools_.clear();
        pending_links_.clear();
        fence_ids_.clear();
        frame_first_submit_ = 0;
        frame_last_submit_ = 0;
    }

    void FinishClosure() {
        closure_.unresolved_calls =
            static_cast<u32>(std::count(call_state_.begin(), call_state_.end(), u8{1}));
        if (calls_seen_ != max_call_seq_) {
            ++closure_.dangling_references;  // call_seq is dense from 1
        }
        closure_.unsupported_schemas = static_cast<u32>(unsupported_.size());
        closure_.ready = schema_seen_ && closure_.unsupported_schemas == 0 &&
                         closure_.unresolved_calls == 0 && closure_.dangling_references == 0 &&
                         closure_.unresolved_links == 0;
    }

    ChunkSource& source_;
    const FileHeader& header_;
    std::string detail_;
    Phase phase_ = Phase::Start;
    u64 seq_ = 0;
    u64 offset_ = FileHeaderBytes;
    u64 chain_ = Fnv1aOffset;
    std::vector<IndexEntry> entries_;
    std::vector<FrameIndexEntry> frames_;

    u64 total_frames_ = 0;
    u64 first_recorded_ = 0;
    u64 current_frame_ = 0;
    u64 frames_done_ = 0;
    u64 frame_first_seq_ = 0;
    bool frame_started_ = false;
    bool present_seen_ = false;
    u32 composed_ = 0;
    u32 guest_pictures_ = 0;

    std::map<std::pair<u64, u32>, u64> allocs_;
    std::map<std::pair<u64, u32>, RangeWriteLog> backing_;
    std::unordered_map<u64, BlobCover> blobs_;
    u64 covered_ranges_ = 0;
    u64 backing_segments_ = 0;
    u64 allocation_bytes_ = 0;
    u64 next_submit_ = 1;
    u64 next_consume_ = 1;
    u64 frame_first_submit_ = 0;
    u64 frame_last_submit_ = 0;
    std::vector<u32> submit_entries_;
    std::set<u64> fence_ids_;

    bool schema_seen_ = false;
    LibrarySchema schema_;
    ClosureResult closure_;
    std::vector<ObjectSlot> objects_;
    std::vector<u8> call_state_;
    u64 calls_seen_ = 0;
    u64 max_call_seq_ = 0;
    std::set<u32> unsupported_;
    std::vector<PoolUse> pending_pools_;
    std::vector<LinkUse> pending_links_;
};

}  // namespace

struct CaptureReader::Impl {
    ChunkSource source;
    FileHeader header;
    IndexRecord index;
    CompleteRecord complete;
    ClosureResult closure;
    bool opened = false;
    bool validated = false;
    u64 next_offset = FileHeaderBytes;
    u64 next_sequence = 0;
    bool ended = false;
};

CaptureReader::CaptureReader() : impl{std::make_unique<Impl>()} {}

CaptureReader::~CaptureReader() = default;

ReadError CaptureReader::Open(const std::filesystem::path& path, std::string& detail) {
    impl = std::make_unique<Impl>();
    Impl& s = *impl;
    REPLAY_TRY(s.source.Open(path, detail));
    const u64 size = s.source.Size();
    if (size < FileHeaderBytes) {
        detail = "file ends at offset " + Number(size) + ": the file header needs " +
                 Number(FileHeaderBytes) + " bytes";
        return ReadError::Truncated;
    }
    std::array<u8, FileHeaderBytes> raw;
    REPLAY_TRY(s.source.ReadRaw(0, raw, detail));
    if (const ReadError bad = DecodeFileHeader(raw, s.header); bad != ReadError::None) {
        detail = "file header: " + std::string(ReadErrorName(bad));
        return bad;
    }
    const FileHeader& header = s.header;
    if ((header.flags & HeaderComplete) == 0) {
        detail = "the header does not carry the Complete flag: the capture was never finished";
        return ReadError::NotComplete;
    }
    if (header.file_length > size) {
        detail = "file ends at offset " + Number(size) + " but the header says it has " +
                 Number(header.file_length) + " bytes";
        return ReadError::Truncated;
    }
    if (header.file_length < size) {
        detail = "file has " + Number(size) + " bytes but the header says " +
                 Number(header.file_length) + "; the extra bytes start at offset " +
                 Number(header.file_length);
        return ReadError::BadLength;
    }
    constexpr u64 MinIndexChunkBytes = IndexChunkBytes(0, 0);
    if (header.chunk_count < 6 || header.index_offset < FileHeaderBytes ||
        header.index_offset > header.file_length ||
        header.file_length - header.index_offset < MinIndexChunkBytes + CompleteChunkBytes) {
        detail = "the header places INDEX at offset " + Number(header.index_offset) +
                 " with " + Number(header.chunk_count) + " chunks in " +
                 Number(header.file_length) + " bytes";
        return ReadError::BadIndex;
    }
    ChunkHeader chunk;
    std::vector<u8> payload;
    REPLAY_TRY(s.source.ReadChunk(header.index_offset, chunk, payload, detail));
    if (chunk.type != static_cast<u32>(ChunkType::Index) ||
        chunk.sequence != header.chunk_count - 2 || chunk.frame_id != 0) {
        detail = "the chunk at offset " + Number(header.index_offset) + " is not the INDEX";
        return ReadError::BadIndex;
    }
    if (const ReadError bad = Decode(payload, s.index); bad != ReadError::None) {
        detail = "INDEX payload at offset " + Number(header.index_offset) + ": " +
                 std::string(ReadErrorName(bad));
        return bad;
    }
    const u64 complete_offset = header.index_offset + ChunkHeaderBytes + chunk.stored_length;
    REPLAY_TRY(s.source.ReadChunk(complete_offset, chunk, payload, detail));
    if (chunk.type != static_cast<u32>(ChunkType::Complete) ||
        chunk.sequence != header.chunk_count - 1 || chunk.frame_id != 0) {
        detail = "the chunk at offset " + Number(complete_offset) + " is not COMPLETE";
        return ReadError::BadIndex;
    }
    if (complete_offset + ChunkHeaderBytes + chunk.stored_length != header.file_length) {
        detail = "COMPLETE ends at offset " +
                 Number(complete_offset + ChunkHeaderBytes + chunk.stored_length) +
                 " but the file has " + Number(header.file_length) + " bytes";
        return ReadError::BadLength;
    }
    if (const ReadError bad = Decode(payload, s.complete); bad != ReadError::None) {
        detail = "COMPLETE payload at offset " + Number(complete_offset) + ": " +
                 std::string(ReadErrorName(bad));
        return bad;
    }
    if (!CompleteIsClean(s.complete) || header.requested_frames != header.finalized_frames) {
        detail = "COMPLETE reports an incomplete capture (reasons mask " +
                 Number(s.complete.incomplete_reasons) + ")";
        return ReadError::NotComplete;
    }
    s.opened = true;
    return ReadError::None;
}

ReadError CaptureReader::Validate(std::string& detail) {
    Impl& s = *impl;
    if (!s.opened) {
        detail = "Open has not succeeded";
        return ReadError::NotComplete;
    }
    Walker walker{s.source, s.header};
    ClosureResult closure;
    const ReadError result = walker.Run(detail, closure);
    s.validated = result == ReadError::None;
    s.closure = s.validated ? closure : ClosureResult{};
    return result;
}

void CaptureReader::Rewind() {
    impl->next_offset = FileHeaderBytes;
    impl->next_sequence = 0;
    impl->ended = false;
}

ReadError CaptureReader::Next(ChunkView& out, bool& at_end, std::string& detail) {
    Impl& s = *impl;
    at_end = false;
    if (!s.validated) {
        detail = "the entire capture must pass Validate before replay";
        return ReadError::NotComplete;
    }
    while (true) {
        if (s.ended || s.next_offset >= s.header.file_length) {
            at_end = true;
            return ReadError::None;
        }
        ChunkHeader chunk;
        REPLAY_TRY(s.source.ReadChunk(s.next_offset, chunk, out.payload, detail));
        if (chunk.sequence != s.next_sequence) {
            detail = "sequence " + Number(chunk.sequence) + " where " + Number(s.next_sequence) +
                     " was expected at offset " + Number(s.next_offset);
            return ReadError::BadOrdering;
        }
        const bool optional = (chunk.flags & ChunkOptional) != 0;
        const bool understood = ChunkTypeIsKnown(chunk.type) && chunk.version == CurrentChunkVersion;
        if (!understood && !optional) {
            detail = "required chunk type " + Number(chunk.type) + " version " +
                     Number(chunk.version) + " at offset " + Number(s.next_offset) +
                     " is not supported";
            return ReadError::UnsupportedFeature;
        }
        s.next_offset += ChunkHeaderBytes + chunk.stored_length;
        ++s.next_sequence;
        if (!understood) {
            continue;  // an unknown optional chunk is skipped
        }
        out.header = chunk;
        out.type = static_cast<ChunkType>(chunk.type);
        s.ended = out.type == ChunkType::Complete;
        return ReadError::None;
    }
}

const FileHeader& CaptureReader::Header() const noexcept {
    return impl->header;
}

const IndexRecord& CaptureReader::Index() const noexcept {
    return impl->index;
}

const CompleteRecord& CaptureReader::Complete() const noexcept {
    return impl->complete;
}

const ClosureResult& CaptureReader::LibraryClosure() const noexcept {
    return impl->closure;
}

}  // namespace VideoCore::Replay
