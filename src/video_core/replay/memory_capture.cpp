// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_core/replay/memory_capture.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace VideoCore::Replay {

namespace {

constexpr u64 PageMask = TrackerPageBytes - 1;
// Bytes of a MEMORY_BLOB payload before its ranges: alloc_id, generation, flags,
// allocation_size, range_count.
constexpr u64 BlobFixedBytes = 8 + 4 + 4 + 8 + 4;
constexpr u64 RangeEntryBytes = 32;

[[nodiscard]] constexpr u64 AlignDown(u64 value) noexcept {
    return value & ~PageMask;
}

[[nodiscard]] constexpr u64 AlignUp(u64 value) noexcept {
    return (value + PageMask) & ~PageMask;
}

// Fills `out` with the ranges of [begin, end) that lie on pages for which `refs` is nonzero.
// Offsets are relative to the window.
void RunsOfMappedPages(const std::vector<u32>& refs, u64 begin, u64 end,
                       std::vector<ByteRange>& out) {
    u64 run_begin = 0;
    bool in_run = false;
    const u64 first_page = begin / TrackerPageBytes;
    const u64 last_page = (end + PageMask) / TrackerPageBytes;
    for (u64 page = first_page; page < last_page; ++page) {
        const bool mapped = refs[static_cast<size_t>(page)] != 0;
        if (mapped && !in_run) {
            run_begin = std::max(begin, page * TrackerPageBytes);
            in_run = true;
        } else if (!mapped && in_run) {
            out.push_back({run_begin, std::min(end, page * TrackerPageBytes)});
            in_run = false;
        }
    }
    if (in_run) {
        out.push_back({run_begin, end});
    }
}

// Adds a blob chunk to `out`. With a sink set, the chunks already held go to it first and in
// order, so a producer never holds more than one chunk (the MemoryOutput contract).
void AppendBlob(MemoryOutput& out, BlobChunk&& chunk) {
    if (out.sink) {
        for (BlobChunk& done : out.blobs) {
            out.sink(std::move(done));
        }
        out.blobs.clear();
    }
    out.blobs.push_back(std::move(chunk));
}

} // namespace

// ---- IntervalSet ------------------------------------------------------------------------------

void IntervalSet::Add(u64 begin, u64 end) {
    if (begin >= end) {
        return;
    }
    auto first = std::lower_bound(ranges.begin(), ranges.end(), begin,
                                  [](const ByteRange& range, u64 value) { return range.end < value; });
    auto last = first;
    while (last != ranges.end() && last->begin <= end) {
        begin = std::min(begin, last->begin);
        end = std::max(end, last->end);
        ++last;
    }
    first = ranges.erase(first, last);
    ranges.insert(first, ByteRange{begin, end});
}

void IntervalSet::Remove(u64 begin, u64 end) {
    if (begin >= end) {
        return;
    }
    auto first = std::lower_bound(ranges.begin(), ranges.end(), begin,
                                  [](const ByteRange& range, u64 value) { return range.end <= value; });
    auto last = first;
    std::vector<ByteRange> kept;
    while (last != ranges.end() && last->begin < end) {
        if (last->begin < begin) {
            kept.push_back({last->begin, begin});
        }
        if (last->end > end) {
            kept.push_back({end, last->end});
        }
        ++last;
    }
    first = ranges.erase(first, last);
    ranges.insert(first, kept.begin(), kept.end());
}

bool IntervalSet::Contains(u64 begin, u64 end) const {
    if (begin >= end) {
        return true;
    }
    const auto it = std::lower_bound(ranges.begin(), ranges.end(), begin,
                                     [](const ByteRange& range, u64 value) { return range.end <= value; });
    return it != ranges.end() && it->begin <= begin && it->end >= end;
}

bool IntervalSet::Intersects(u64 begin, u64 end) const {
    if (begin >= end) {
        return false;
    }
    const auto it = std::lower_bound(ranges.begin(), ranges.end(), begin,
                                     [](const ByteRange& range, u64 value) { return range.end <= value; });
    return it != ranges.end() && it->begin < end;
}

std::vector<ByteRange> IntervalSet::Intersection(u64 begin, u64 end) const {
    std::vector<ByteRange> result;
    if (begin >= end) {
        return result;
    }
    auto it = std::lower_bound(ranges.begin(), ranges.end(), begin,
                               [](const ByteRange& range, u64 value) { return range.end <= value; });
    for (; it != ranges.end() && it->begin < end; ++it) {
        result.push_back({std::max(begin, it->begin), std::min(end, it->end)});
    }
    return result;
}

std::vector<ByteRange> IntervalSet::Gaps(u64 begin, u64 end) const {
    std::vector<ByteRange> result;
    if (begin >= end) {
        return result;
    }
    u64 cursor = begin;
    auto it = std::lower_bound(ranges.begin(), ranges.end(), begin,
                               [](const ByteRange& range, u64 value) { return range.end <= value; });
    for (; it != ranges.end() && it->begin < end && cursor < end; ++it) {
        if (it->begin > cursor) {
            result.push_back({cursor, it->begin});
        }
        cursor = std::max(cursor, it->end);
    }
    if (cursor < end) {
        result.push_back({cursor, end});
    }
    return result;
}

u64 IntervalSet::TotalBytes() const noexcept {
    u64 total = 0;
    for (const ByteRange& range : ranges) {
        total += range.Size();
    }
    return total;
}

// ---- digest -----------------------------------------------------------------------------------

PageDigest DigestBytes(std::span<const u8> bytes) noexcept {
    constexpr u64 k1 = 0x9E3779B97F4A7C15ULL;
    constexpr u64 k2 = 0xC2B2AE3D27D4EB4FULL;
    u64 a = 0x243F6A8885A308D3ULL ^ bytes.size();
    u64 b = 0x13198A2E03707344ULL + bytes.size();
    const u8* cursor = bytes.data();
    size_t left = bytes.size();
    const auto mix = [&](u64 word) {
        a = (a ^ word) * k1;
        a ^= a >> 32;
        b = (b + word) * k2;
        b = (b << 29) | (b >> 35);
        b ^= word >> 7;
    };
    while (left >= 8) {
        u64 word = 0;
        std::memcpy(&word, cursor, 8);
        mix(word);
        cursor += 8;
        left -= 8;
    }
    if (left != 0) {
        u64 word = 0;
        std::memcpy(&word, cursor, left);
        mix(word);
    }
    a ^= a >> 29;
    a *= k2;
    a ^= a >> 32;
    b ^= b >> 31;
    b *= k1;
    b ^= b >> 29;
    return {a, b};
}

bool IsAllZero(std::span<const u8> bytes) noexcept {
    const u8* cursor = bytes.data();
    size_t left = bytes.size();
    while (left >= 8) {
        u64 word = 0;
        std::memcpy(&word, cursor, 8);
        if (word != 0) {
            return false;
        }
        cursor += 8;
        left -= 8;
    }
    for (; left != 0; --left, ++cursor) {
        if (*cursor != 0) {
            return false;
        }
    }
    return true;
}

// ---- MemoryTracker ------------------------------------------------------------------------------

MemoryTracker::MemoryTracker(MemorySource& source_, const TrackerConfig& config_)
    : source{source_}, config{config_} {
    // A window is a whole number of pages and a power of two; anything else is a caller bug that
    // would corrupt the offsets, so it is forced to the default instead of trusted.
    if (config.window_bytes < TrackerPageBytes || (config.window_bytes & (config.window_bytes - 1)) != 0) {
        config.window_bytes = TrackerWindowBytes;
    }
}

MemoryTracker::~MemoryTracker() = default;

MemoryTracker::Window* MemoryTracker::FindWindow(u64 device) noexcept {
    const auto it = windows.find(device / config.window_bytes);
    return it == windows.end() ? nullptr : &it->second;
}

const MemoryTracker::Window* MemoryTracker::FindWindow(u64 device) const noexcept {
    const auto it = windows.find(device / config.window_bytes);
    return it == windows.end() ? nullptr : &it->second;
}

u32 MemoryTracker::GenerationAt(u64 device) const noexcept {
    const Window* window = FindWindow(device);
    return window != nullptr ? window->generation : 0;
}

u64 MemoryTracker::AllocationIdAt(u64 device) const noexcept {
    const Window* window = FindWindow(device);
    return window != nullptr ? window->alloc_id : 0;
}

MemoryTracker::Window& MemoryTracker::AcquireWindow(u64 window_index) {
    const size_t page_count = static_cast<size_t>(config.window_bytes / TrackerPageBytes);
    auto [it, inserted] = windows.try_emplace(window_index);
    Window& window = it->second;
    if (inserted) {
        window.alloc_id = next_alloc_id++;
        window.generation = 1;
        window.base = window_index * config.window_bytes;
        window.page_refs.assign(page_count, 0);
        window.pages.assign(page_count, PageInfo{});
    } else if (window.mapped_pages == 0) {
        // The backing was unmapped and is reused: same allocation id, next generation, nothing of
        // the earlier content is known.
        ++window.generation;
        window.declared = false;
        window.captured.Clear();
        window.derived.Clear();
        window.dirty.Clear();
        window.pages.assign(page_count, PageInfo{});
    }
    return window;
}

MemoryTracker::Window& MemoryTracker::ReferencePages(u64 device, u64 end, MemoryOutput& out) {
    Window& window = AcquireWindow(device / config.window_bytes);
    const u64 begin_offset = AlignDown(device - window.base);
    const u64 end_offset = AlignUp(end - window.base);
    for (u64 page = begin_offset / TrackerPageBytes; page < end_offset / TrackerPageBytes; ++page) {
        if (window.page_refs[static_cast<size_t>(page)]++ == 0) {
            ++window.mapped_pages;
        }
    }
    // Bytes of the newly mapped pages that the replayer does not hold yet.
    std::vector<ByteRange> need;
    for (const ByteRange& gap : window.captured.Gaps(begin_offset, end_offset)) {
        for (const ByteRange& piece : window.derived.Gaps(gap.begin, gap.end)) {
            need.push_back(piece);
        }
    }
    if (!window.declared) {
        // Declare the allocation and which of its bytes nothing will ever provide: those are the
        // bytes outside every mapped page. A read of one is unknown provenance.
        IntervalSet known = window.captured;
        for (const ByteRange& range : need) {
            known.Add(range.begin, range.end);
        }
        for (const ByteRange& range : window.derived.Ranges()) {
            known.Add(range.begin, range.end);
        }
        const std::vector<ByteRange> undefined = known.Gaps(0, config.window_bytes);
        if (!undefined.empty() || need.empty()) {
            BlobChunk chunk;
            chunk.type = ChunkType::MemoryBlob;
            chunk.ordinal = next_ordinal++;
            chunk.blob.alloc_id = window.alloc_id;
            chunk.blob.generation = window.generation;
            chunk.blob.flags = phase == TrackerPhase::Initial ? BlobInitial : BlobDelta;
            chunk.blob.allocation_size = config.window_bytes;
            for (const ByteRange& range : undefined) {
                chunk.blob.ranges.push_back(MemoryRange{.offset = range.begin,
                                                        .length = range.Size(),
                                                        .kind = RangeKind::Undefined,
                                                        .writer = WriterKind::Cpu,
                                                        .ref_sequence = 0,
                                                        .data_offset = 0});
            }
            if (BlobFixedBytes + RangeEntryBytes * chunk.blob.ranges.size() > config.max_chunk_bytes) {
                stats.limit_chunk = true;
                chunk.blob.ranges.clear();
            }
            AppendBlob(out, std::move(chunk));
        }
        window.declared = true;
    }
    CaptureRanges(window, need, ChunkType::MemoryBlob, WriterKind::Cpu, out);
    return window;
}

void MemoryTracker::ReleasePages(u64 device, u64 end) {
    for (u64 cursor = device; cursor < end;) {
        const u64 window_index = cursor / config.window_bytes;
        const u64 window_end = std::min(end, (window_index + 1) * config.window_bytes);
        const auto it = windows.find(window_index);
        if (it != windows.end()) {
            Window& window = it->second;
            const u64 begin_offset = AlignDown(cursor - window.base);
            const u64 end_offset = AlignUp(window_end - window.base);
            for (u64 page = begin_offset / TrackerPageBytes; page < end_offset / TrackerPageBytes; ++page) {
                u32& refs = window.page_refs[static_cast<size_t>(page)];
                if (refs == 0) {
                    continue;
                }
                if (--refs == 0) {
                    --window.mapped_pages;
                    const u64 page_begin = page * TrackerPageBytes;
                    window.captured.Remove(page_begin, page_begin + TrackerPageBytes);
                    window.derived.Remove(page_begin, page_begin + TrackerPageBytes);
                    window.dirty.Remove(page_begin, page_begin + TrackerPageBytes);
                    window.pages[static_cast<size_t>(page)] = PageInfo{};
                }
            }
            if (window.dirty.Empty()) {
                dirty_windows.erase(window_index);
            }
        }
        cursor = window_end;
    }
}

template <typename Visit>
bool MemoryTracker::CarveRange(AddressSpace& space, u64 begin, u64 end, Visit&& visit) {
    bool any = false;
    auto it = space.upper_bound(begin);
    if (it != space.begin()) {
        --it;
    }
    while (it != space.end() && it->first < end) {
        const u64 start = it->first;
        const MapRecord record = it->second;
        if (record.end <= begin) {
            ++it;
            continue;
        }
        any = true;
        const u64 overlap_begin = std::max(start, begin);
        const u64 overlap_end = std::min(record.end, end);
        visit(overlap_begin, overlap_end, record.device + (overlap_begin - start), record.sparse);
        it = space.erase(it);
        if (start < begin) {
            space.emplace(start, MapRecord{begin, record.device, record.sparse});
        }
        if (record.end > end) {
            space.emplace(end, MapRecord{record.end, record.device + (end - start), record.sparse});
            break;
        }
    }
    return any;
}

void MemoryTracker::Map(u32 address_space, u64 gpu_va, u64 size, u64 device, u32 flags,
                        u32 pte_kind, MemoryOutput& out) {
    if (size == 0) {
        return;
    }
    // Both addresses are page aligned in the engine; an unaligned range is widened to whole pages
    // so reference counts stay balanced.
    const u64 delta = device & PageMask;
    device -= delta;
    gpu_va -= delta;
    size = AlignUp(size + delta);

    std::vector<std::pair<u64, u64>> released;  // device ranges of replaced mappings
    AddressSpace& space = spaces[address_space];
    if (CarveRange(space, gpu_va, gpu_va + size, [&](u64 begin, u64 end, u64 old_device, bool sparse) {
            if (!sparse) {
                released.emplace_back(old_device, old_device + (end - begin));
            }
        })) {
        flags |= MapRemap;
    }
    space.emplace(gpu_va, MapRecord{gpu_va + size, device, false});

    // New references first: a remap onto the same device memory must not forget its content.
    for (u64 cursor = device; cursor < device + size;) {
        const u64 window_index = cursor / config.window_bytes;
        const u64 window_end = std::min(device + size, (window_index + 1) * config.window_bytes);
        const Window& window = ReferencePages(cursor, window_end, out);
        out.maps.push_back(MapEntry{.address_space_id = address_space,
                                    .flags = flags,
                                    .gpu_va = gpu_va + (cursor - device),
                                    .size = window_end - cursor,
                                    .alloc_id = window.alloc_id,
                                    .alloc_offset = cursor - window.base,
                                    .generation = window.generation,
                                    .pte_kind = pte_kind});
        cursor = window_end;
    }
    for (const auto& [begin, end] : released) {
        ReleasePages(begin, end);
    }
}

void MemoryTracker::MapSparse(u32 address_space, u64 gpu_va, u64 size, u32 flags,
                              MemoryOutput& out) {
    if (size == 0) {
        return;
    }
    const u64 begin = AlignDown(gpu_va);
    const u64 end = AlignUp(gpu_va + size);
    std::vector<std::pair<u64, u64>> released;
    AddressSpace& space = spaces[address_space];
    if (CarveRange(space, begin, end, [&](u64 range_begin, u64 range_end, u64 old_device, bool sparse) {
            if (!sparse) {
                released.emplace_back(old_device, old_device + (range_end - range_begin));
            }
        })) {
        flags |= MapRemap;
    }
    space.emplace(begin, MapRecord{end, 0, true});
    out.maps.push_back(MapEntry{.address_space_id = address_space,
                                .flags = flags | MapFlag::MapSparse,
                                .gpu_va = begin,
                                .size = end - begin,
                                .alloc_id = 0,
                                .alloc_offset = 0,
                                .generation = 0,
                                .pte_kind = 0});
    for (const auto& [range_begin, range_end] : released) {
        ReleasePages(range_begin, range_end);
    }
}

void MemoryTracker::Unmap(u32 address_space, u64 gpu_va, u64 size, MemoryOutput& out) {
    if (size == 0) {
        return;
    }
    const u64 begin = AlignDown(gpu_va);
    const u64 end = AlignUp(gpu_va + size);
    const auto space_it = spaces.find(address_space);
    if (space_it == spaces.end()) {
        return;
    }
    std::vector<std::pair<u64, u64>> released;
    const bool any = CarveRange(space_it->second, begin, end,
                                [&](u64 range_begin, u64 range_end, u64 old_device, bool sparse) {
                                    if (!sparse) {
                                        released.emplace_back(old_device,
                                                              old_device + (range_end - range_begin));
                                    }
                                });
    if (!any) {
        return;
    }
    out.unmaps.push_back(UnmapEntry{.address_space_id = address_space,
                                    .flags = 0,
                                    .gpu_va = begin,
                                    .size = end - begin});
    for (const auto& [range_begin, range_end] : released) {
        ReleasePages(range_begin, range_end);
    }
}

void MemoryTracker::NoteCpuWrite(u64 device, u64 size) {
    if (size == 0) {
        return;
    }
    const u64 end = device + size;
    std::vector<ByteRange> runs;
    for (u64 cursor = device; cursor < end;) {
        const u64 window_index = cursor / config.window_bytes;
        const u64 window_end = std::min(end, (window_index + 1) * config.window_bytes);
        const auto it = windows.find(window_index);
        if (it != windows.end()) {
            Window& window = it->second;
            runs.clear();
            RunsOfMappedPages(window.page_refs, cursor - window.base, window_end - window.base, runs);
            for (const ByteRange& run : runs) {
                window.dirty.Add(run.begin, run.end);
                window.derived.Remove(run.begin, run.end);
            }
            if (!window.dirty.Empty()) {
                dirty_windows.insert(window_index);
            }
        }
        cursor = window_end;
    }
}

void MemoryTracker::NoteGpuWrite(u64 device, u64 size) {
    if (size == 0) {
        return;
    }
    const u64 end = device + size;
    std::vector<ByteRange> runs;
    for (u64 cursor = device; cursor < end;) {
        const u64 window_index = cursor / config.window_bytes;
        const u64 window_end = std::min(end, (window_index + 1) * config.window_bytes);
        const auto it = windows.find(window_index);
        if (it != windows.end()) {
            Window& window = it->second;
            runs.clear();
            RunsOfMappedPages(window.page_refs, cursor - window.base, window_end - window.base, runs);
            for (const ByteRange& run : runs) {
                window.derived.Add(run.begin, run.end);
                window.dirty.Remove(run.begin, run.end);
                // The page content is no longer what the digest says.
                for (u64 page = run.begin / TrackerPageBytes; page < AlignUp(run.end) / TrackerPageBytes;
                     ++page) {
                    window.pages[static_cast<size_t>(page)].current_valid = false;
                }
            }
            if (window.dirty.Empty()) {
                dirty_windows.erase(window_index);
            }
        }
        cursor = window_end;
    }
}

void MemoryTracker::NoteExternalWrite(u64 device, u64 size, MemoryOutput& out) {
    if (size == 0) {
        return;
    }
    const u64 end = device + size;
    std::vector<ByteRange> runs;
    for (u64 cursor = device; cursor < end;) {
        const u64 window_index = cursor / config.window_bytes;
        const u64 window_end = std::min(end, (window_index + 1) * config.window_bytes);
        const auto it = windows.find(window_index);
        if (it != windows.end()) {
            Window& window = it->second;
            runs.clear();
            RunsOfMappedPages(window.page_refs, cursor - window.base, window_end - window.base, runs);
            for (const ByteRange& run : runs) {
                window.dirty.Remove(run.begin, run.end);
            }
            if (window.dirty.Empty()) {
                dirty_windows.erase(window_index);
            }
            CaptureRanges(window, runs, ChunkType::ExternalWrite, WriterKind::ExternalDevice, out);
        }
        cursor = window_end;
    }
}

bool MemoryTracker::IsResolved(u64 device, u64 size) const {
    if (size == 0) {
        return true;
    }
    const u64 end = device + size;
    for (u64 cursor = device; cursor < end;) {
        const u64 window_index = cursor / config.window_bytes;
        const u64 window_end = std::min(end, (window_index + 1) * config.window_bytes);
        const auto it = windows.find(window_index);
        if (it == windows.end()) {
            return false;
        }
        const Window& window = it->second;
        for (const ByteRange& gap : window.captured.Gaps(cursor - window.base, window_end - window.base)) {
            if (!window.derived.Contains(gap.begin, gap.end)) {
                return false;
            }
        }
        cursor = window_end;
    }
    return true;
}

bool MemoryTracker::Locate(u64 device, u64 size, Location& out) const {
    const Window* window = FindWindow(device);
    if (window == nullptr || window->alloc_id == 0) {
        return false;
    }
    const u64 offset = device - window->base;
    if (size > config.window_bytes - offset) {
        return false;
    }
    out = Location{window->alloc_id, window->generation, offset};
    return true;
}

u32 MemoryTracker::NoteRead(u64 device, u64 size) {
    if (size == 0) {
        return 0;
    }
    const u64 end = device + size;
    u32 added = 0;
    const auto note_unknown = [&](u64 begin, u64 stop) {
        for (const ByteRange& fresh : unresolved_seen.Gaps(begin, stop)) {
            unresolved_seen.Add(fresh.begin, fresh.end);
            ++added;
        }
    };
    for (u64 cursor = device; cursor < end;) {
        const u64 window_index = cursor / config.window_bytes;
        const u64 window_end = std::min(end, (window_index + 1) * config.window_bytes);
        const auto it = windows.find(window_index);
        if (it == windows.end()) {
            note_unknown(cursor, window_end);
        } else {
            const Window& window = it->second;
            for (const ByteRange& gap : window.captured.Gaps(cursor - window.base, window_end - window.base)) {
                for (const ByteRange& unknown : window.derived.Gaps(gap.begin, gap.end)) {
                    note_unknown(window.base + unknown.begin, window.base + unknown.end);
                }
            }
        }
        cursor = window_end;
    }
    stats.unresolved_resources += added;
    return added;
}

void MemoryTracker::Flush(MemoryOutput& out) {
    const std::set<u64> pending = std::move(dirty_windows);
    dirty_windows.clear();
    for (const u64 window_index : pending) {
        const auto it = windows.find(window_index);
        if (it == windows.end()) {
            continue;
        }
        Window& window = it->second;
        const std::vector<ByteRange> ranges = window.dirty.Ranges();
        window.dirty.Clear();
        CaptureRanges(window, ranges, ChunkType::MemoryBlob, WriterKind::Cpu, out);
    }
}

void MemoryTracker::CaptureRanges(Window& window, std::span<const ByteRange> ranges, ChunkType type,
                                  WriterKind writer, MemoryOutput& out) {
    if (ranges.empty()) {
        return;
    }
    const bool initial = phase == TrackerPhase::Initial;
    const u32 blob_flags = initial ? BlobInitial : BlobDelta;
    const u64 chunk_limit = config.max_chunk_bytes;
    std::vector<u8> buffer;
    // Index in out.blobs of the chunk being filled; none when the next piece opens a chunk.
    bool have_chunk = false;
    u64 chunk_bytes = 0;
    const auto open_chunk = [&] {
        BlobChunk chunk;
        chunk.type = type;
        chunk.ordinal = next_ordinal++;
        chunk.blob.alloc_id = window.alloc_id;
        chunk.blob.generation = window.generation;
        chunk.blob.flags = blob_flags;
        chunk.blob.allocation_size = config.window_bytes;
        AppendBlob(out, std::move(chunk));
        have_chunk = true;
        chunk_bytes = BlobFixedBytes;
    };
    bool stop = false;

    for (size_t range_index = 0; range_index < ranges.size(); ++range_index) {
        const ByteRange range = ranges[range_index];
        if (range.begin >= range.end) {
            continue;
        }
        if (stop) {
            // Nothing more can be captured: the bytes are no longer known to the replayer.
            window.captured.Remove(range.begin, range.end);
            continue;
        }
        buffer.resize(static_cast<size_t>(range.Size()));
        if (!source.ReadDevice(window.base + range.begin, buffer)) {
            ++stats.read_failures;
            window.captured.Remove(range.begin, range.end);
            continue;
        }
        u64 position = range.begin;
        while (position < range.end) {
            const u64 page_begin = AlignDown(position);
            const u64 page_end = page_begin + TrackerPageBytes;
            const u64 piece_end = std::min(range.end, page_end);
            const bool whole_page = position == page_begin && piece_end == page_end;
            const std::span<const u8> bytes{buffer.data() + (position - range.begin),
                                            static_cast<size_t>(piece_end - position)};
            const size_t page_index = static_cast<size_t>(page_begin / TrackerPageBytes);
            PageInfo& page = window.pages[page_index];

            RangeKind kind = RangeKind::Data;
            bool skip = false;
            PageDigest digest;
            bool zero = false;
            if (whole_page) {
                digest = DigestBytes(bytes);
                zero = IsAllZero(bytes);
                const bool replay_holds = window.captured.Contains(page_begin, page_end);
                if (replay_holds && page.current_valid && page.current == digest) {
                    skip = true;
                } else if (zero) {
                    kind = RangeKind::Zero;
                }
            } else if (IsAllZero(bytes)) {
                kind = RangeKind::Zero;
            }

            if (skip) {
                stats.dedup_bytes += bytes.size();
                position = piece_end;
                continue;
            }

            if (initial) {
                if (stats.initial_backing_bytes + bytes.size() > config.max_initial_backing_bytes) {
                    stats.limit_initial_backing = true;
                    stop = true;
                }
            } else if (kind == RangeKind::Data &&
                       frame_payload_bytes + bytes.size() > config.max_new_payload_bytes_per_frame) {
                stats.limit_new_payload = true;
                stop = true;
            }
            if (stop) {
                window.captured.Remove(position, range.end);
                break;
            }

            const u64 data_cost = kind == RangeKind::Data ? bytes.size() : 0;
            // A new chunk when the current one has no room for this piece.
            if (have_chunk) {
                const MemoryBlob& current = out.blobs.back().blob;
                const bool extends = !current.ranges.empty() &&
                                     current.ranges.back().kind == kind &&
                                     current.ranges.back().offset + current.ranges.back().length == position;
                const u64 needed = data_cost + (extends ? 0 : RangeEntryBytes);
                if (chunk_bytes + needed > chunk_limit || current.ranges.size() >= MaxRangesPerBlob) {
                    have_chunk = false;
                }
            }
            if (!have_chunk) {
                open_chunk();
                if (chunk_bytes + data_cost + RangeEntryBytes > chunk_limit) {
                    // One piece alone does not fit a chunk: nothing can be written.
                    stats.limit_chunk = true;
                    stop = true;
                    window.captured.Remove(position, range.end);
                    break;
                }
            }
            MemoryBlob& blob = out.blobs.back().blob;
            if (!blob.ranges.empty() && blob.ranges.back().kind == kind &&
                blob.ranges.back().offset + blob.ranges.back().length == position) {
                blob.ranges.back().length += bytes.size();
            } else {
                blob.ranges.push_back(MemoryRange{.offset = position,
                                                  .length = bytes.size(),
                                                  .kind = kind,
                                                  .writer = writer,
                                                  .data_offset = blob.data.size()});
                chunk_bytes += RangeEntryBytes;
            }
            if (kind == RangeKind::Data) {
                blob.data.insert(blob.data.end(), bytes.begin(), bytes.end());
                chunk_bytes += bytes.size();
            }

            if (whole_page) {
                page.current = digest;
                page.current_valid = true;
            } else {
                page.current_valid = false;
            }

            if (initial) {
                stats.initial_backing_bytes += bytes.size();
            } else if (kind == RangeKind::Data) {
                frame_payload_bytes += bytes.size();
                stats.delta_bytes += bytes.size();
                if (type == ChunkType::ExternalWrite) {
                    stats.external_bytes += bytes.size();
                }
            }
            window.captured.Add(position, piece_end);
            window.derived.Remove(position, piece_end);
            position = piece_end;
        }
    }
}

void ApplyTrackerStats(const TrackerStats& stats, CompleteRecord& record) noexcept {
    record.initial_backing_bytes += stats.initial_backing_bytes;
    record.delta_bytes += stats.delta_bytes;
    record.dedup_bytes += stats.dedup_bytes;
    const u32 unresolved = stats.unresolved_resources + stats.read_failures;
    if (unresolved != 0) {
        record.unresolved_resources += unresolved;
        record.incomplete_reasons |= IncompleteUnresolvedResource | IncompleteUnknownProvenance;
    }
    const u32 limits = u32{stats.limit_initial_backing} + u32{stats.limit_new_payload} +
                       u32{stats.limit_chunk};
    if (limits != 0) {
        record.overflow_events += limits;
        record.incomplete_reasons |= IncompleteLimitExceeded;
    }
}

} // namespace VideoCore::Replay
