// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Memory provenance of the capture (memory_capture.h). Every byte below is synthetic: a fake
// device memory with a deterministic pattern, and a small "replayer" that rebuilds allocations
// from the tracker's output exactly as a reader of the capture would (resolving Ref ranges by
// chunk ordinal) and is compared with the fake memory.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/replay/checkpoint.h"
#include "video_core/replay/memory_capture.h"
#include "video_core/replay/memory_restore.h"

namespace R = VideoCore::Replay;

namespace {

constexpr u64 TestWindow = 0x10000;
constexpr u64 Page = R::TrackerPageBytes;
constexpr u8 UnknownFill = 0xEE;

class FakeMemory final : public R::MemorySource {
public:
    explicit FakeMemory(size_t size) : bytes(size) {
        for (size_t i = 0; i < bytes.size(); ++i) {
            bytes[i] = static_cast<u8>(((i / Page) * 29 + i * 7 + 1) & 0xFF);
        }
    }

    bool ReadDevice(u64 device_address, std::span<u8> out) override {
        ++reads;
        if (device_address + out.size() > fail_from || device_address + out.size() > bytes.size()) {
            return false;
        }
        std::memcpy(out.data(), bytes.data() + device_address, out.size());
        return true;
    }

    void Fill(u64 begin, u64 end, u8 value) {
        std::fill(bytes.begin() + static_cast<std::ptrdiff_t>(begin),
                  bytes.begin() + static_cast<std::ptrdiff_t>(end), value);
    }

    void Scramble(u64 begin, u64 end, u8 salt) {
        for (u64 i = begin; i < end; ++i) {
            bytes[static_cast<size_t>(i)] = static_cast<u8>(bytes[static_cast<size_t>(i)] + salt + 1);
        }
    }

    std::vector<u8> Copy(u64 begin, u64 end) const {
        return {bytes.begin() + static_cast<std::ptrdiff_t>(begin),
                bytes.begin() + static_cast<std::ptrdiff_t>(end)};
    }

    void Restore(u64 begin, const std::vector<u8>& saved) {
        std::copy(saved.begin(), saved.end(), bytes.begin() + static_cast<std::ptrdiff_t>(begin));
    }

    std::vector<u8> bytes;
    u64 fail_from = ~u64{0};
    u32 reads = 0;
};

// What a replayer of the capture builds from the chunks, in the order they are appended.
class Rebuilt {
public:
    struct Content {
        std::vector<u8> bytes;
        std::vector<u8> known;
        std::vector<u32> writer;  // ordinal + 1 of the chunk that last wrote the byte, 0 for none
    };

    void Apply(const R::MemoryOutput& out) {
        for (const R::UnmapEntry& entry : out.unmaps) {
            Carve(entry.address_space_id, entry.gpu_va, entry.gpu_va + entry.size);
        }
        for (const R::BlobChunk& chunk : out.blobs) {
            ApplyBlob(chunk);
        }
        for (const R::MapEntry& entry : out.maps) {
            Carve(entry.address_space_id, entry.gpu_va, entry.gpu_va + entry.size);
            maps.push_back(entry);
        }
    }

    // The bytes behind a GPU virtual range; known is false when any byte is not known.
    std::vector<u8> ReadGpu(u32 space, u64 gpu_va, u64 length, bool& known) const {
        std::vector<u8> result;
        known = true;
        for (u64 position = gpu_va; position < gpu_va + length;) {
            const R::MapEntry* hit = nullptr;
            for (const R::MapEntry& map : maps) {
                if (map.address_space_id == space && map.alloc_id != 0 && map.gpu_va <= position &&
                    position < map.gpu_va + map.size) {
                    hit = &map;
                    break;
                }
            }
            if (hit == nullptr) {
                known = false;
                return result;
            }
            const auto content = contents.find({hit->alloc_id, hit->generation});
            if (content == contents.end()) {
                known = false;
                return result;
            }
            const u64 count = std::min(gpu_va + length, hit->gpu_va + hit->size) - position;
            const u64 offset = hit->alloc_offset + (position - hit->gpu_va);
            for (u64 i = 0; i < count; ++i) {
                known = known && content->second.known[static_cast<size_t>(offset + i)] != 0;
                result.push_back(content->second.bytes[static_cast<size_t>(offset + i)]);
            }
            position += count;
        }
        return result;
    }

    const Content* Find(u64 alloc_id, u32 generation) const {
        const auto it = contents.find({alloc_id, generation});
        return it == contents.end() ? nullptr : &it->second;
    }

    std::vector<R::MapEntry> maps;
    u32 refs_resolved = 0;

private:
    // Removes [begin, end) of the address space from every entry, keeping the head and tail
    // pieces: the replayer unmaps and maps by range, not by whole entry.
    void Carve(u32 space, u64 begin, u64 end) {
        std::vector<R::MapEntry> kept;
        for (const R::MapEntry& map : maps) {
            const u64 map_end = map.gpu_va + map.size;
            if (map.address_space_id != space || map_end <= begin || end <= map.gpu_va) {
                kept.push_back(map);
                continue;
            }
            if (map.gpu_va < begin) {
                R::MapEntry head = map;
                head.size = begin - map.gpu_va;
                kept.push_back(head);
            }
            if (end < map_end) {
                R::MapEntry tail = map;
                tail.gpu_va = end;
                tail.size = map_end - end;
                tail.alloc_offset += end - map.gpu_va;
                kept.push_back(tail);
            }
        }
        maps = std::move(kept);
    }

    void ApplyBlob(const R::BlobChunk& chunk) {
        const R::MemoryBlob& blob = chunk.blob;
        Content& content = contents[{blob.alloc_id, blob.generation}];
        if (content.bytes.empty()) {
            content.bytes.assign(static_cast<size_t>(blob.allocation_size), UnknownFill);
            content.known.assign(static_cast<size_t>(blob.allocation_size), 0);
            content.writer.assign(static_cast<size_t>(blob.allocation_size), 0);
        }
        for (const R::MemoryRange& range : blob.ranges) {
            for (u64 i = 0; i < range.length; ++i) {
                const size_t at = static_cast<size_t>(range.offset + i);
                switch (range.kind) {
                case R::RangeKind::Data:
                    content.bytes[at] = blob.data[static_cast<size_t>(range.data_offset + i)];
                    content.known[at] = 1;
                    content.writer[at] = chunk.ordinal + 1;
                    break;
                case R::RangeKind::Zero:
                    content.bytes[at] = 0;
                    content.known[at] = 1;
                    content.writer[at] = chunk.ordinal + 1;
                    break;
                case R::RangeKind::Undefined:
                    content.bytes[at] = UnknownFill;
                    content.known[at] = 0;
                    content.writer[at] = chunk.ordinal + 1;
                    break;
                case R::RangeKind::Ref:
                    // What the replayer accepts: the bytes were not written after the chunk the
                    // Ref names. A Ref to a value that was overwritten since is a refused file.
                    REQUIRE(content.writer[at] != 0);
                    REQUIRE(content.writer[at] <= range.ref_sequence + 1);
                    REQUIRE(content.known[at] != 0);
                    ++refs_resolved;
                    break;
                }
            }
        }
    }

    std::map<std::pair<u64, u32>, Content> contents;
};

R::TrackerConfig SmallConfig() {
    R::TrackerConfig config;
    config.window_bytes = TestWindow;
    return config;
}

// The rebuilt bytes behind a GPU range equal the fake memory at `device`.
void ExpectGpuRange(const Rebuilt& rebuilt, const FakeMemory& memory, u32 space, u64 gpu_va,
                    u64 length, u64 device) {
    bool known = false;
    const std::vector<u8> got = rebuilt.ReadGpu(space, gpu_va, length, known);
    REQUIRE(known);
    REQUIRE(got == memory.Copy(device, device + length));
}

bool AnyRangeOverlaps(const R::MemoryOutput& out, u64 alloc_id, u64 begin, u64 end) {
    for (const R::BlobChunk& chunk : out.blobs) {
        if (chunk.blob.alloc_id != alloc_id) {
            continue;
        }
        for (const R::MemoryRange& range : chunk.blob.ranges) {
            if (range.offset < end && begin < range.offset + range.length) {
                return true;
            }
        }
    }
    return false;
}

u64 DataBytesIn(const R::MemoryOutput& out) {
    u64 total = 0;
    for (const R::BlobChunk& chunk : out.blobs) {
        total += chunk.blob.data.size();
    }
    return total;
}

} // namespace

TEST_CASE("replay memory: interval set keeps sorted merged ranges", "[replay][memory]") {
    R::IntervalSet set;
    set.Add(10, 20);
    set.Add(30, 40);
    set.Add(20, 30);  // bridges both
    REQUIRE(set.Ranges().size() == 1);
    REQUIRE(set.Ranges()[0] == R::ByteRange{10, 40});
    REQUIRE(set.Contains(10, 40));
    REQUIRE(set.Contains(15, 15));
    REQUIRE_FALSE(set.Contains(9, 11));
    REQUIRE(set.Intersects(39, 50));
    REQUIRE_FALSE(set.Intersects(40, 50));

    set.Remove(15, 25);
    REQUIRE(set.Ranges().size() == 2);
    REQUIRE(set.Ranges()[0] == R::ByteRange{10, 15});
    REQUIRE(set.Ranges()[1] == R::ByteRange{25, 40});
    REQUIRE(set.TotalBytes() == 20);

    const std::vector<R::ByteRange> gaps = set.Gaps(0, 50);
    REQUIRE(gaps.size() == 3);
    REQUIRE(gaps[0] == R::ByteRange{0, 10});
    REQUIRE(gaps[1] == R::ByteRange{15, 25});
    REQUIRE(gaps[2] == R::ByteRange{40, 50});
    const std::vector<R::ByteRange> inside = set.Intersection(12, 30);
    REQUIRE(inside.size() == 2);
    REQUIRE(inside[0] == R::ByteRange{12, 15});
    REQUIRE(inside[1] == R::ByteRange{25, 30});

    set.Remove(0, 100);
    REQUIRE(set.Empty());
    set.Add(5, 5);  // empty ranges are ignored
    REQUIRE(set.Empty());
}

TEST_CASE("replay memory: digest separates pages and detects zero", "[replay][memory]") {
    std::vector<u8> a(Page, 0);
    std::vector<u8> b(Page, 0);
    REQUIRE(R::IsAllZero(a));
    REQUIRE(R::DigestBytes(a) == R::DigestBytes(b));
    b[Page - 1] = 1;
    REQUIRE_FALSE(R::IsAllZero(b));
    REQUIRE_FALSE(R::DigestBytes(a) == R::DigestBytes(b));
    a[3] = 1;
    b[Page - 1] = 0;
    b[3] = 1;
    REQUIRE(R::DigestBytes(a) == R::DigestBytes(b));
    REQUIRE_FALSE(R::DigestBytes(std::span<const u8>{a}.first(Page - 1)) == R::DigestBytes(a));
}

TEST_CASE("replay memory: aliases remaps and sparse maps", "[replay][memory]") {
    FakeMemory memory{4 * TestWindow};
    R::MemoryTracker tracker{memory, SmallConfig()};
    Rebuilt rebuilt;
    R::MemoryOutput out;

    // First map: eight pages of window 0. The allocation is declared with its unmapped half
    // Undefined, then the mapped half arrives as data.
    tracker.Map(1, 0x100000, 0x8000, 0x0000, 0, 5, out);
    REQUIRE(out.maps.size() == 1);
    REQUIRE(out.blobs.size() == 2);
    const u64 alloc0 = tracker.AllocationIdAt(0);
    REQUIRE(alloc0 != 0);
    REQUIRE(out.maps[0].alloc_id == alloc0);
    REQUIRE(out.maps[0].generation == 1);
    REQUIRE(out.maps[0].alloc_offset == 0);
    REQUIRE(out.maps[0].pte_kind == 5);
    REQUIRE(out.blobs[0].blob.ranges.size() == 1);
    REQUIRE(out.blobs[0].blob.ranges[0].kind == R::RangeKind::Undefined);
    REQUIRE(out.blobs[0].blob.ranges[0].offset == 0x8000);
    REQUIRE(out.blobs[0].blob.ranges[0].length == 0x8000);
    REQUIRE(out.blobs[1].blob.flags == R::BlobInitial);
    REQUIRE(out.blobs[1].blob.data.size() == 0x8000);
    rebuilt.Apply(out);
    ExpectGpuRange(rebuilt, memory, 1, 0x100000, 0x8000, 0);

    // A GPU range that crosses a window boundary is one MAP entry per allocation.
    out.Clear();
    tracker.Map(1, 0x200000, 0x18000, 0x8000, 0, 0, out);
    REQUIRE(out.maps.size() == 2);
    REQUIRE(out.maps[0].alloc_id == alloc0);
    REQUIRE(out.maps[0].alloc_offset == 0x8000);
    REQUIRE(out.maps[0].size == 0x8000);
    REQUIRE(out.maps[1].alloc_id == tracker.AllocationIdAt(TestWindow));
    REQUIRE(out.maps[1].alloc_offset == 0);
    REQUIRE(out.maps[1].size == 0x10000);
    REQUIRE(out.maps[1].gpu_va == 0x208000);
    rebuilt.Apply(out);
    ExpectGpuRange(rebuilt, memory, 1, 0x200000, 0x18000, 0x8000);
    ExpectGpuRange(rebuilt, memory, 1, 0x100000, 0x8000, 0);

    // An alias in another address space: same allocation, other offset, nothing new to store.
    out.Clear();
    tracker.Map(2, 0x400000, 0x4000, 0x2000, 0, 0, out);
    REQUIRE(out.blobs.empty());
    REQUIRE(out.maps.size() == 1);
    REQUIRE(out.maps[0].address_space_id == 2);
    REQUIRE(out.maps[0].alloc_id == alloc0);
    REQUIRE(out.maps[0].alloc_offset == 0x2000);
    rebuilt.Apply(out);
    ExpectGpuRange(rebuilt, memory, 2, 0x400000, 0x4000, 0x2000);
    REQUIRE(tracker.AllocationCount() == 2);

    // A remap of an existing GPU range onto other memory: MapRemap, the replaced pages that no
    // other mapping holds are forgotten, the ones the alias still holds are not.
    out.Clear();
    tracker.Map(1, 0x100000, 0x4000, 3 * TestWindow, 0, 0, out);
    REQUIRE(out.maps.size() == 1);
    REQUIRE((out.maps[0].flags & R::MapRemap) != 0);
    REQUIRE(out.maps[0].alloc_id == tracker.AllocationIdAt(3 * TestWindow));
    rebuilt.Apply(out);
    ExpectGpuRange(rebuilt, memory, 1, 0x100000, 0x4000, 3 * TestWindow);
    REQUIRE_FALSE(tracker.IsResolved(0x0000, 0x2000));
    REQUIRE(tracker.IsResolved(0x2000, 0x2000));
    REQUIRE(tracker.IsResolved(0x4000, 0x4000));

    // A sparse map owns no memory.
    out.Clear();
    const u64 allocations = tracker.AllocationCount();
    tracker.MapSparse(1, 0x800000, 0x10000, 0, out);
    REQUIRE(out.blobs.empty());
    REQUIRE(out.maps.size() == 1);
    REQUIRE(out.maps[0].alloc_id == 0);
    REQUIRE((out.maps[0].flags & R::MapSparse) != 0);
    REQUIRE(tracker.AllocationCount() == allocations);

    // Unmapping the last mapping of a window and mapping it again is the same allocation with
    // the next generation, and nothing of the old content is trusted.
    tracker.SetPhase(R::TrackerPhase::Delta);
    out.Clear();
    tracker.Unmap(1, 0x104000, 0x4000, out);  // right remainder of the first map
    tracker.Unmap(2, 0x400000, 0x4000, out);  // the alias
    tracker.Unmap(1, 0x200000, 0x8000, out);  // the second map's window 0 half
    REQUIRE(out.unmaps.size() == 3);
    REQUIRE_FALSE(tracker.IsResolved(0x0000, 0x10000));
    out.Clear();
    tracker.Map(1, 0x500000, 0x1000, 0x0000, 0, 0, out);
    REQUIRE(tracker.GenerationAt(0) == 2);
    REQUIRE(tracker.AllocationIdAt(0) == alloc0);
    REQUIRE(out.maps.size() == 1);
    REQUIRE(out.maps[0].generation == 2);
    REQUIRE(out.maps[0].alloc_id == alloc0);
    bool delta_blob = false;
    for (const R::BlobChunk& chunk : out.blobs) {
        REQUIRE(chunk.blob.alloc_id == alloc0);
        REQUIRE(chunk.blob.generation == 2);
        delta_blob = delta_blob || chunk.blob.flags == R::BlobDelta;
    }
    REQUIRE(delta_blob);
    rebuilt.Apply(out);
    ExpectGpuRange(rebuilt, memory, 1, 0x500000, 0x1000, 0);
}

TEST_CASE("replay memory: a byte written by the cpu mid frame is stored exactly", "[replay][memory]") {
    FakeMemory memory{2 * TestWindow};
    R::MemoryTracker tracker{memory, SmallConfig()};
    Rebuilt rebuilt;
    R::MemoryOutput out;
    tracker.Map(1, 0x1000000, 0x8000, 0, 0, 0, out);  // half of window 0
    const u64 alloc = tracker.AllocationIdAt(0);
    rebuilt.Apply(out);

    tracker.SetPhase(R::TrackerPhase::Delta);
    tracker.BeginFrame();

    // 100 bytes in the middle of a page.
    memory.Scramble(0x1234, 0x1234 + 100, 3);
    tracker.NoteCpuWrite(0x1234, 100);
    REQUIRE(tracker.HasPendingWrites());
    out.Clear();
    tracker.Flush(out);
    REQUIRE_FALSE(tracker.HasPendingWrites());
    REQUIRE(out.blobs.size() == 1);
    REQUIRE(out.blobs[0].type == R::ChunkType::MemoryBlob);
    REQUIRE(out.blobs[0].blob.flags == R::BlobDelta);
    REQUIRE(out.blobs[0].blob.ranges.size() == 1);
    REQUIRE(out.blobs[0].blob.ranges[0].offset == 0x1234);
    REQUIRE(out.blobs[0].blob.ranges[0].length == 100);
    REQUIRE(out.blobs[0].blob.ranges[0].kind == R::RangeKind::Data);
    REQUIRE(out.blobs[0].blob.ranges[0].writer == R::WriterKind::Cpu);
    REQUIRE(out.blobs[0].blob.data.size() == 100);
    rebuilt.Apply(out);
    ExpectGpuRange(rebuilt, memory, 1, 0x1000000, 0x8000, 0);

    // Writes outside every mapping are not the capture's business.
    tracker.NoteCpuWrite(0x20000, 64);
    tracker.NoteCpuWrite(0x9000, 64);  // mapped window, unmapped page
    REQUIRE_FALSE(tracker.HasPendingWrites());
    // A write that straddles the end of the mapping is cut at the page boundary.
    memory.Scramble(0x7F00, 0x8000, 5);
    tracker.NoteCpuWrite(0x7F00, 0x400);
    out.Clear();
    tracker.Flush(out);
    REQUIRE(out.blobs.size() == 1);
    REQUIRE(out.blobs[0].blob.ranges.size() == 1);
    REQUIRE(out.blobs[0].blob.ranges[0].offset == 0x7F00);
    REQUIRE(out.blobs[0].blob.ranges[0].length == 0x100);
    rebuilt.Apply(out);
    ExpectGpuRange(rebuilt, memory, 1, 0x1000000, 0x8000, 0);

    // Whole pages: new content is data, a return to the first content is data again (the replayer
    // accepts a Ref only for bytes unchanged since the chunk it names), and an unchanged rewrite
    // stores nothing.
    const std::vector<u8> original = memory.Copy(0x3000, 0x4000);
    memory.Scramble(0x3000, 0x4000, 9);
    tracker.NoteCpuWrite(0x3000, Page);
    out.Clear();
    tracker.Flush(out);
    REQUIRE(out.blobs.size() == 1);
    REQUIRE(out.blobs[0].blob.ranges.size() == 1);
    REQUIRE(out.blobs[0].blob.ranges[0].kind == R::RangeKind::Data);
    REQUIRE(tracker.Stats().dedup_bytes == 0);
    rebuilt.Apply(out);

    memory.Restore(0x3000, original);
    tracker.NoteCpuWrite(0x3000, Page);
    out.Clear();
    tracker.Flush(out);
    REQUIRE(out.blobs.size() == 1);
    REQUIRE(out.blobs[0].blob.ranges.size() == 1);
    REQUIRE(out.blobs[0].blob.ranges[0].kind == R::RangeKind::Data);
    REQUIRE(out.blobs[0].blob.ranges[0].ref_sequence == 0);
    REQUIRE(out.blobs[0].blob.data == original);
    REQUIRE(tracker.Stats().dedup_bytes == 0);
    rebuilt.Apply(out);
    REQUIRE(rebuilt.refs_resolved == 0);
    ExpectGpuRange(rebuilt, memory, 1, 0x1000000, 0x8000, 0);

    tracker.NoteCpuWrite(0x3000, Page);
    out.Clear();
    tracker.Flush(out);
    REQUIRE(out.blobs.empty());
    REQUIRE(tracker.Stats().dedup_bytes == Page);

    // A page the CPU zeroed is a Zero range with no bytes.
    memory.Fill(0x5000, 0x6000, 0);
    tracker.NoteCpuWrite(0x5000, Page);
    out.Clear();
    tracker.Flush(out);
    REQUIRE(out.blobs.size() == 1);
    REQUIRE(out.blobs[0].blob.ranges.size() == 1);
    REQUIRE(out.blobs[0].blob.ranges[0].kind == R::RangeKind::Zero);
    REQUIRE(out.blobs[0].blob.data.empty());
    rebuilt.Apply(out);
    ExpectGpuRange(rebuilt, memory, 1, 0x1000000, 0x8000, 0);

    const R::TrackerStats& stats = tracker.Stats();
    // The scrambled page and its return to the first content are both Data (a Ref is only valid
    // for bytes unchanged since the chunk it names), so two whole pages count besides the
    // 100 byte write and the 0x100 byte tail.
    REQUIRE(stats.delta_bytes == 100 + 0x100 + 2 * Page);
    REQUIRE(stats.unresolved_resources == 0);
    static_cast<void>(alloc);
}

TEST_CASE("replay memory: a gpu derived range is never stored", "[replay][memory]") {
    FakeMemory memory{TestWindow};
    R::MemoryTracker tracker{memory, SmallConfig()};
    Rebuilt rebuilt;
    R::MemoryOutput out;
    tracker.Map(1, 0x10000, TestWindow, 0, 0, 0, out);
    const u64 alloc = tracker.AllocationIdAt(0);
    rebuilt.Apply(out);
    tracker.SetPhase(R::TrackerPhase::Delta);

    // The GPU rewrote a page: the memory changed, the capture stores nothing, the bytes are
    // still resolved (replay recomputes them).
    memory.Scramble(0x2000, 0x3000, 11);
    tracker.NoteGpuWrite(0x2000, Page);
    out.Clear();
    tracker.Flush(out);
    REQUIRE(out.Empty());
    REQUIRE(tracker.IsResolved(0x2000, Page));
    REQUIRE(tracker.NoteRead(0x2000, Page) == 0);

    // A CPU write after the GPU write is stored, and only its bytes.
    memory.Scramble(0x2800, 0x2900, 2);
    tracker.NoteCpuWrite(0x2800, 0x100);
    out.Clear();
    tracker.Flush(out);
    REQUIRE(out.blobs.size() == 1);
    REQUIRE(out.blobs[0].blob.ranges.size() == 1);
    REQUIRE(out.blobs[0].blob.ranges[0].offset == 0x2800);
    REQUIRE(out.blobs[0].blob.ranges[0].length == 0x100);
    REQUIRE_FALSE(AnyRangeOverlaps(out, alloc, 0x2000, 0x2800));
    REQUIRE_FALSE(AnyRangeOverlaps(out, alloc, 0x2900, 0x3000));

    // A GPU write after a CPU write of the same bytes wins: nothing is stored for them.
    memory.Scramble(0x5000, 0x5100, 4);
    tracker.NoteCpuWrite(0x5000, 0x100);
    tracker.NoteGpuWrite(0x5000, 0x100);
    memory.Scramble(0x5000, 0x5100, 4);
    out.Clear();
    tracker.Flush(out);
    REQUIRE(out.Empty());

    // A content digest taken before a GPU write is not trusted afterwards: writing the old
    // bytes back must not be taken for "unchanged".
    const std::vector<u8> before = memory.Copy(0x6000, 0x7000);
    memory.Scramble(0x6000, 0x7000, 6);
    tracker.NoteGpuWrite(0x6000, Page);
    memory.Restore(0x6000, before);
    tracker.NoteCpuWrite(0x6000, Page);
    out.Clear();
    tracker.Flush(out);
    REQUIRE(out.blobs.size() == 1);
    REQUIRE(DataBytesIn(out) == Page);

    // A GPU write to an unmapped page is not tracked (nothing can read it through a mapping).
    tracker.SetPhase(R::TrackerPhase::Initial);
    out.Clear();
    tracker.Map(1, 0x90000, 0x2000, 0x20000, 0, 0, out);
    tracker.NoteGpuWrite(0x24000, 0x1000);
    REQUIRE_FALSE(tracker.IsResolved(0x24000, 0x1000));
}

TEST_CASE("replay memory: external writes are stored where they happen", "[replay][memory]") {
    FakeMemory memory{TestWindow};
    R::MemoryTracker tracker{memory, SmallConfig()};
    Rebuilt rebuilt;
    R::MemoryOutput out;
    tracker.Map(1, 0x10000, TestWindow, 0, 0, 0, out);
    const u64 alloc = tracker.AllocationIdAt(0);
    rebuilt.Apply(out);
    tracker.SetPhase(R::TrackerPhase::Delta);

    memory.Scramble(0x4000, 0x6000, 13);
    out.Clear();
    tracker.NoteExternalWrite(0x4000, 0x2000, out);
    REQUIRE(out.blobs.size() == 1);
    REQUIRE(out.blobs[0].type == R::ChunkType::ExternalWrite);
    REQUIRE(out.blobs[0].blob.alloc_id == alloc);
    REQUIRE(out.blobs[0].blob.ranges.size() == 1);
    REQUIRE(out.blobs[0].blob.ranges[0].writer == R::WriterKind::ExternalDevice);
    REQUIRE(tracker.Stats().external_bytes == 0x2000);
    rebuilt.Apply(out);
    ExpectGpuRange(rebuilt, memory, 1, 0x10000, TestWindow, 0);
}

// A, then B written by an external device, then C from the CPU, then B again from the CPU: the
// last step used to be a Ref to the external-write chunk, which the reader does not accept as a
// Ref source. The tracker emits no Ref at all: the content is stored.
TEST_CASE("replay memory: content that returns to an external write is stored again",
          "[replay][memory]") {
    FakeMemory memory{TestWindow};
    R::MemoryTracker tracker{memory, SmallConfig()};
    R::MemoryOutput out;
    tracker.Map(1, 0x10000, TestWindow, 0, 0, 0, out);
    tracker.SetPhase(R::TrackerPhase::Delta);
    tracker.BeginFrame();

    memory.Scramble(0x4000, 0x5000, 21);  // B
    const std::vector<u8> written_b = memory.Copy(0x4000, 0x5000);
    out.Clear();
    tracker.NoteExternalWrite(0x4000, Page, out);
    REQUIRE(out.blobs.size() == 1);
    REQUIRE(out.blobs[0].type == R::ChunkType::ExternalWrite);

    memory.Scramble(0x4000, 0x5000, 22);  // C
    tracker.NoteCpuWrite(0x4000, Page);
    out.Clear();
    tracker.Flush(out);
    REQUIRE(out.blobs.size() == 1);

    memory.Restore(0x4000, written_b);  // B again
    tracker.NoteCpuWrite(0x4000, Page);
    out.Clear();
    tracker.Flush(out);
    REQUIRE(out.blobs.size() == 1);
    REQUIRE(out.blobs[0].type == R::ChunkType::MemoryBlob);
    REQUIRE(out.blobs[0].blob.ranges.size() == 1);
    REQUIRE(out.blobs[0].blob.ranges[0].kind == R::RangeKind::Data);
    REQUIRE(out.blobs[0].blob.data == written_b);
}

TEST_CASE("replay memory: an undefined byte that is read makes the capture incomplete",
          "[replay][memory]") {
    FakeMemory memory{2 * TestWindow};
    R::MemoryTracker tracker{memory, SmallConfig()};
    R::MemoryOutput out;
    tracker.Map(1, 0x1000, 0x4000, 0, 0, 0, out);  // pages 0 to 3 of window 0

    REQUIRE(tracker.NoteRead(0x1000, 0x1000) == 0);
    REQUIRE(tracker.Stats().unresolved_resources == 0);

    // Page 4 is in the allocation but was never mapped: declared Undefined, so a read is
    // unknown provenance. The same bytes count once.
    REQUIRE(tracker.NoteRead(0x4000, 0x100) == 1);
    REQUIRE(tracker.NoteRead(0x4000, 0x100) == 0);
    REQUIRE(tracker.Stats().unresolved_resources == 1);
    // A read that straddles known and unknown bytes counts the unknown part only.
    REQUIRE(tracker.NoteRead(0x3F00, 0x300) == 1);
    // A window the tracker never saw.
    REQUIRE(tracker.NoteRead(TestWindow + 0x10, 0x10) == 1);
    REQUIRE(tracker.Stats().unresolved_resources == 3);

    R::CompleteRecord record;
    record.status = R::CompleteStatus::Complete;
    record.dependency_complete = 1;
    REQUIRE(R::CompleteIsClean(record));
    R::ApplyTrackerStats(tracker.Stats(), record);
    REQUIRE(record.unresolved_resources == 3);
    REQUIRE((record.incomplete_reasons & R::IncompleteUnknownProvenance) != 0);
    REQUIRE((record.incomplete_reasons & R::IncompleteUnresolvedResource) != 0);
    REQUIRE_FALSE(R::CompleteIsClean(record));

    // A tracker that saw nothing wrong leaves the record clean.
    FakeMemory clean_memory{TestWindow};
    R::MemoryTracker clean{clean_memory, SmallConfig()};
    out.Clear();
    clean.Map(1, 0x1000, TestWindow, 0, 0, 0, out);
    REQUIRE(clean.NoteRead(0x2000, 0x2000) == 0);
    R::CompleteRecord clean_record;
    clean_record.status = R::CompleteStatus::Complete;
    clean_record.dependency_complete = 1;
    R::ApplyTrackerStats(clean.Stats(), clean_record);
    REQUIRE(R::CompleteIsClean(clean_record));
    REQUIRE(clean_record.initial_backing_bytes == TestWindow);
}

TEST_CASE("replay memory: a source that refuses a read leaves the bytes unresolved", "[replay][memory]") {
    FakeMemory memory{TestWindow};
    memory.fail_from = 0x4000;
    R::MemoryTracker tracker{memory, SmallConfig()};
    R::MemoryOutput out;
    tracker.Map(1, 0x1000, 0x8000, 0, 0, 0, out);
    REQUIRE(tracker.Stats().read_failures == 1);
    REQUIRE(tracker.IsResolved(0, 0x8000) == false);
    REQUIRE(tracker.NoteRead(0x6000, 0x10) == 1);
    R::CompleteRecord record;
    R::ApplyTrackerStats(tracker.Stats(), record);
    REQUIRE((record.incomplete_reasons & R::IncompleteUnresolvedResource) != 0);
    REQUIRE(record.unresolved_resources == 2);
}

TEST_CASE("replay memory: limits are recorded, never truncated silently", "[replay][memory]") {
    SECTION("initial backing") {
        FakeMemory memory{TestWindow};
        R::TrackerConfig config = SmallConfig();
        config.max_initial_backing_bytes = 2 * Page;
        R::MemoryTracker tracker{memory, config};
        R::MemoryOutput out;
        tracker.Map(1, 0x1000, 4 * Page, 0, 0, 0, out);
        REQUIRE(tracker.Stats().limit_initial_backing);
        REQUIRE(tracker.Stats().initial_backing_bytes == 2 * Page);
        REQUIRE(tracker.IsResolved(0, 2 * Page));
        REQUIRE_FALSE(tracker.IsResolved(2 * Page, 2 * Page));
        R::CompleteRecord record;
        R::ApplyTrackerStats(tracker.Stats(), record);
        REQUIRE((record.incomplete_reasons & R::IncompleteLimitExceeded) != 0);
        REQUIRE(record.overflow_events == 1);
        REQUIRE_FALSE(R::CompleteIsClean(record));
    }
    SECTION("new payload per frame") {
        FakeMemory memory{TestWindow};
        R::TrackerConfig config = SmallConfig();
        config.max_new_payload_bytes_per_frame = Page;
        R::MemoryTracker tracker{memory, config};
        R::MemoryOutput out;
        tracker.Map(1, 0x1000, 4 * Page, 0, 0, 0, out);
        tracker.SetPhase(R::TrackerPhase::Delta);
        tracker.BeginFrame();
        memory.Scramble(0, 2 * Page, 1);
        tracker.NoteCpuWrite(0, 2 * Page);
        out.Clear();
        tracker.Flush(out);
        REQUIRE(tracker.Stats().limit_new_payload);
        REQUIRE(DataBytesIn(out) == Page);
        // The page that did not fit is no longer trusted: its old bytes are stale.
        REQUIRE(tracker.IsResolved(0, Page));
        REQUIRE_FALSE(tracker.IsResolved(Page, Page));
        // The next frame has a new budget.
        tracker.BeginFrame();
        memory.Scramble(2 * Page, 3 * Page, 1);
        tracker.NoteCpuWrite(2 * Page, Page);
        out.Clear();
        tracker.Flush(out);
        REQUIRE(DataBytesIn(out) == Page);
    }
    SECTION("chunk size") {
        FakeMemory memory{TestWindow};
        R::TrackerConfig config = SmallConfig();
        config.max_chunk_bytes = 3 * Page;
        R::MemoryTracker tracker{memory, config};
        Rebuilt rebuilt;
        R::MemoryOutput out;
        tracker.Map(1, 0x1000, 0x10000, 0, 0, 0, out);
        REQUIRE_FALSE(tracker.Stats().limit_chunk);
        REQUIRE(out.blobs.size() > 4);
        for (const R::BlobChunk& chunk : out.blobs) {
            const u64 encoded = 28 + 32 * chunk.blob.ranges.size() + chunk.blob.data.size();
            REQUIRE(encoded <= config.max_chunk_bytes);
        }
        rebuilt.Apply(out);
        ExpectGpuRange(rebuilt, memory, 1, 0x1000, 0x10000, 0);
    }
    SECTION("a sink takes every chunk when the next one opens") {
        FakeMemory memory{2 * TestWindow};
        R::TrackerConfig config = SmallConfig();
        config.max_chunk_bytes = 3 * Page;
        R::MemoryTracker tracker{memory, config};
        Rebuilt rebuilt;
        R::MemoryOutput out;
        size_t streamed = 0;
        size_t most_held = 0;
        out.sink = [&](R::BlobChunk&& chunk) {
            ++streamed;
            most_held = (std::max)(most_held, out.blobs.size());
            R::MemoryOutput one;
            one.blobs.push_back(std::move(chunk));
            rebuilt.Apply(one);
        };
        tracker.Map(1, 0x1000, 0x18000, 0, 0, 0, out);  // two windows, many chunks
        REQUIRE(streamed > 8);
        REQUIRE(most_held == 1);
        REQUIRE(out.blobs.size() == 1);
        rebuilt.Apply(out);
        ExpectGpuRange(rebuilt, memory, 1, 0x1000, 0x18000, 0);
    }
    SECTION("a chunk too small for one page") {
        FakeMemory memory{TestWindow};
        R::TrackerConfig config = SmallConfig();
        config.max_chunk_bytes = Page;
        R::MemoryTracker tracker{memory, config};
        R::MemoryOutput out;
        tracker.Map(1, 0x1000, Page, 0, 0, 0, out);
        REQUIRE(tracker.Stats().limit_chunk);
        REQUIRE_FALSE(tracker.IsResolved(0, Page));
    }
}

TEST_CASE("replay memory: pages that lose their mapping are captured again", "[replay][memory]") {
    FakeMemory memory{TestWindow};
    R::MemoryTracker tracker{memory, SmallConfig()};
    Rebuilt rebuilt;
    R::MemoryOutput out;
    tracker.Map(1, 0x1000, 4 * Page, 0, 0, 0, out);
    rebuilt.Apply(out);
    tracker.SetPhase(R::TrackerPhase::Delta);
    tracker.BeginFrame();

    // Unmap two pages; the guest changes them while the GPU cannot see them (no notification).
    out.Clear();
    tracker.Unmap(1, 0x1000 + 2 * Page, 2 * Page, out);
    REQUIRE(out.unmaps.size() == 1);
    rebuilt.Apply(out);
    memory.Scramble(2 * Page, 4 * Page, 7);
    tracker.NoteCpuWrite(2 * Page, 2 * Page);  // unmapped: ignored
    REQUIRE_FALSE(tracker.HasPendingWrites());
    REQUIRE_FALSE(tracker.IsResolved(2 * Page, 2 * Page));

    // Mapping them again captures what is there now.
    out.Clear();
    tracker.Map(1, 0x1000 + 2 * Page, 2 * Page, 2 * Page, 0, 0, out);
    REQUIRE(DataBytesIn(out) == 2 * Page);
    rebuilt.Apply(out);
    ExpectGpuRange(rebuilt, memory, 1, 0x1000, 4 * Page, 0);
}

TEST_CASE("replay memory: the checkpoint header is usable from this file", "[replay][memory]") {
    // checkpoint.h and memory_capture.h are included together by the capture; this keeps the two
    // headers compilable side by side.
    R::GpuCheckpoint checkpoint;
    REQUIRE(checkpoint.channels.empty());
    REQUIRE(R::SharedChannel == 0xFFFFFFFFu);
}

// ---- the staging sink ---------------------------------------------------------------------------
//
// These run the real CaptureWriter (format_io.cpp) in a private temporary directory.

namespace {

class TempDirectory {
public:
    TempDirectory() {
        static std::atomic<u32> counter{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() /
               ("eglantine_replay_memory_" + std::to_string(stamp) + "_" +
                std::to_string(counter.fetch_add(1)));
        std::filesystem::create_directories(path);
    }
    ~TempDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;

    std::filesystem::path path;
};

R::SessionLimits SmallLimits() {
    R::SessionLimits limits;
    limits.max_file_bytes = 1ULL << 20;
    limits.max_chunk_bytes = 4096;
    limits.max_staging_bytes = 16384;
    return limits;
}

std::vector<u8> Bytes(size_t count) {
    return std::vector<u8>(count, 0x5A);
}

// The real writer decodes the provisional header it writes, and a header with no first frame or
// no requested frames is refused. Every chunk these cases stage belongs to frame 1.
R::FileHeader StagingHeader() {
    R::FileHeader header;
    header.first_frame = 1;
    header.requested_frames = 1;
    return header;
}

} // namespace

TEST_CASE("replay staging: accepted chunks get consecutive sequences", "[replay][memory][staging]") {
    TempDirectory directory;
    const std::string path = (directory.path / "a.egcap").string();
    R::StagingWriter sink{SmallLimits()};
    R::FileHeader header = StagingHeader();
    std::string error;
    REQUIRE(sink.Open(path, header, error));
    const u64 first = sink.NextSequence();
    REQUIRE(sink.Stage(R::ChunkType::Session, 0, Bytes(64)));
    REQUIRE(sink.NextSequence() == first + 1);
    REQUIRE(sink.Stage(R::ChunkType::GpuCheckpoint, 1, Bytes(64)));
    REQUIRE(sink.NextSequence() == first + 2);
    REQUIRE(sink.Healthy());

    R::CompleteRecord complete;
    REQUIRE_FALSE(sink.Finish(false, header, complete, error));
    REQUIRE(sink.ChunkCount() == 2);
    REQUIRE(std::filesystem::exists(path + ".partial"));
    REQUIRE_FALSE(std::filesystem::exists(path));
}

TEST_CASE("replay staging: a chunk over max_chunk_bytes is dropped and counted",
          "[replay][memory][staging]") {
    TempDirectory directory;
    const std::string path = (directory.path / "b.egcap").string();
    R::StagingWriter sink{SmallLimits()};
    R::FileHeader header = StagingHeader();
    std::string error;
    REQUIRE(sink.Open(path, header, error));
    const u64 first = sink.NextSequence();
    REQUIRE_FALSE(sink.Stage(R::ChunkType::Session, 0, Bytes(4097)));
    REQUIRE(sink.NextSequence() == first);  // a dropped chunk takes no sequence
    REQUIRE_FALSE(sink.Healthy());
    const R::StagingStats stats = sink.Stats();
    REQUIRE(stats.dropped_chunks == 1);
    REQUIRE(stats.overflow_events == 1);
    REQUIRE_FALSE(stats.error.empty());

    R::CompleteRecord complete;
    REQUIRE_FALSE(sink.Finish(true, header, complete, error));
    REQUIRE((complete.incomplete_reasons & R::IncompleteLimitExceeded) != 0);
    REQUIRE((complete.incomplete_reasons & R::IncompleteDroppedEvents) != 0);
    REQUIRE(complete.dropped_events == 1);
    REQUIRE(complete.overflow_events == 1);
    REQUIRE(complete.status == R::CompleteStatus::Incomplete);
    // The capture stays .partial: a limit hit never produces a final file.
    REQUIRE(std::filesystem::exists(path + ".partial"));
    REQUIRE_FALSE(std::filesystem::exists(path));
}

TEST_CASE("replay staging: the file size limit counts the room kept for the index",
          "[replay][memory][staging]") {
    TempDirectory directory;
    const std::string path = (directory.path / "c.egcap").string();
    R::SessionLimits limits = SmallLimits();
    limits.max_file_bytes = 1ULL << 20;  // smaller than the INDEX room of a maximal capture
    // Only the file size may stop the loop: with the default 16 KiB queue and no wait, the fifth
    // 4000 byte chunk is refused as a full queue before the file limit is reached.
    limits.max_staging_bytes = limits.max_file_bytes;
    R::StagingWriter sink{limits};
    R::FileHeader header = StagingHeader();
    std::string error;
    REQUIRE(sink.Open(path, header, error));
    // The reserve for INDEX and COMPLETE (frame entries for MaxTotalFrames frames) is about
    // 80 KiB; chunks are accepted until the rest of the 1 MiB is used, then refused.
    u32 accepted = 0;
    bool refused = false;
    for (u32 i = 0; i < 400 && !refused; ++i) {
        // GPU_EVENT belongs to a frame: the real writer refuses frame id 0 for it (CheckFrame),
        // so every chunk here is in frame 1, which stays open (no FRAME_END is needed).
        if (sink.Stage(R::ChunkType::GpuEvent, 1, Bytes(4000))) {
            ++accepted;
        } else {
            refused = true;
        }
    }
    REQUIRE(refused);
    REQUIRE(accepted > 100);
    REQUIRE(accepted < 260);
    R::CompleteRecord complete;
    REQUIRE_FALSE(sink.Finish(true, header, complete, error));
    REQUIRE((complete.incomplete_reasons & R::IncompleteLimitExceeded) != 0);
}

TEST_CASE("replay staging: a full queue is refused for a hook and waited out for the snapshot",
          "[replay][memory][staging]") {
    TempDirectory directory;
    const std::string path = (directory.path / "d.egcap").string();
    R::SessionLimits limits = SmallLimits();
    limits.max_staging_bytes = 8192;
    R::StagingWriter sink{limits};
    R::FileHeader header = StagingHeader();
    std::string error;
    REQUIRE(sink.Open(path, header, error));
    // The writer thread drains the queue, so a snapshot-style Stage that may wait always fits
    // eventually, however many chunks pass through a two-chunk queue.
    for (u32 i = 0; i < 64; ++i) {
        // MEMORY_BLOB belongs to a frame (only SESSION, INDEX, COMPLETE and LIBRARY_SCHEMA and
        // the two checkpoint chunks may carry frame 0): the real writer would fail the file on
        // frame 0. The initial snapshot of the recorder stages its blobs under the first frame.
        REQUIRE(sink.Stage(R::ChunkType::MemoryBlob, 1, Bytes(4000), true));
    }
    REQUIRE(sink.Healthy());
    R::CompleteRecord complete;
    REQUIRE_FALSE(sink.Finish(false, header, complete, error));
    REQUIRE(sink.ChunkCount() == 64);
    REQUIRE(sink.Stats().peak_queued_bytes <= 8192);
}

namespace {

// SESSION, one memory chunk and the FRAME_END of frame 1, staged for a one frame capture.
void StageOneFrame(R::StagingWriter& sink) {
    REQUIRE(sink.Stage(R::ChunkType::Session, 0, Bytes(64)));
    const u64 first = sink.NextSequence();
    REQUIRE(sink.Stage(R::ChunkType::MemoryBlob, 1, Bytes(64)));
    R::FrameEndRecord end;
    end.frame_id = 1;
    end.flags = R::FrameComplete;
    end.first_sequence = first;
    end.last_sequence = sink.NextSequence();
    end.output_id = 1;
    std::vector<u8> payload;
    R::Encode(end, payload);
    REQUIRE(sink.Stage(R::ChunkType::FrameEnd, 1, std::move(payload)));
}

R::CompleteRecord CleanComplete() {
    R::CompleteRecord complete;
    complete.status = R::CompleteStatus::Complete;
    complete.dependency_complete = 1;
    return complete;
}

} // namespace

TEST_CASE("replay staging: the counts of a finished capture include INDEX and COMPLETE",
          "[replay][memory][staging]") {
    TempDirectory directory;
    const std::string path = (directory.path / "e.egcap").string();
    R::StagingWriter sink{SmallLimits()};
    R::FileHeader header = StagingHeader();
    header.finalized_frames = 1;
    std::string error;
    REQUIRE(sink.Open(path, header, error));
    StageOneFrame(sink);
    R::CompleteRecord complete = CleanComplete();
    const bool finished = sink.Finish(true, header, complete, error);
    INFO(error);
    REQUIRE(finished);
    REQUIRE(std::filesystem::exists(path));

    // Three chunks (a session record, a blob and a frame end) are not a replayable capture, so
    // the reader refuses them (it wants at least six chunks); what this case proves is the
    // header the writer finalizes, which is decoded and checked on its own.
    std::vector<u8> head(R::FileHeaderBytes);
    {
        std::ifstream in(path, std::ios::binary);
        in.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(head.size()));
        REQUIRE(in.gcount() == static_cast<std::streamsize>(head.size()));
    }
    R::FileHeader final_header;
    REQUIRE(R::DecodeFileHeader(head, final_header) == R::ReadError::None);
    REQUIRE((final_header.flags & R::HeaderComplete) != 0);
    // Three staged chunks, then INDEX and COMPLETE.
    REQUIRE(final_header.chunk_count == 5);
    REQUIRE(sink.ChunkCount() == 5);
    REQUIRE(sink.BytesWritten() == std::filesystem::file_size(path));
    REQUIRE(sink.BytesWritten() == final_header.file_length);
}

TEST_CASE("replay staging: a failure while finalizing is a write failure",
          "[replay][memory][staging]") {
    TempDirectory directory;
    const std::string path = (directory.path / "f.egcap").string();
    R::StagingWriter sink{SmallLimits()};
    R::FileHeader header = StagingHeader();
    header.finalized_frames = 1;
    std::string error;
    REQUIRE(sink.Open(path, header, error));
    StageOneFrame(sink);
    // Another file takes the final name while the capture runs: every queued chunk is written,
    // and the rename that ends the capture is refused.
    {
        std::ofstream taken{path, std::ios::binary};
        taken << "taken";
    }
    R::CompleteRecord complete = CleanComplete();
    REQUIRE_FALSE(sink.Finish(true, header, complete, error));
    REQUIRE_FALSE(error.empty());
    REQUIRE((complete.incomplete_reasons & R::IncompleteWriteFailed) != 0);
    REQUIRE(complete.status == R::CompleteStatus::Incomplete);
    REQUIRE(sink.ChunkCount() == 5);  // the tail that was written is counted
    REQUIRE(std::filesystem::exists(path + ".partial"));
}

TEST_CASE("Decoded guest bytes restore atomically and undefined pages remain absent",
          "[video_core][replay][memory]") {
    R::MemoryBlob blob;
    blob.alloc_id = blob.generation = 1;
    blob.allocation_size = 4;
    blob.flags = R::BlobInitial;
    R::MemoryRange data;
    data.kind = R::RangeKind::Data;
    data.length = 1;
    R::MemoryRange zero = data;
    zero.kind = R::RangeKind::Zero;
    zero.offset = 1;
    blob.ranges = {data, zero};
    blob.data = {42};
    std::vector<u8> wire;
    R::Encode(blob, wire);
    R::MemoryBlob decoded;
    REQUIRE(R::Decode(wire, decoded) == R::ReadError::None);
    std::vector<u8> bytes(4, 99);
    R::RangeWriteLog log;
    std::vector<R::WrittenRange> written;
    std::string error;
    REQUIRE(R::ApplyBlobToBytes(decoded, 1, bytes, log, written, error));
    REQUIRE(bytes == std::vector<u8>{42, 0, 99, 99});
    REQUIRE(log.UnchangedSince(0, 2, 1));
    REQUIRE_FALSE(log.UnchangedSince(1, 2, 1));
    decoded.data[0] = 13;
    decoded.ranges[1].kind = R::RangeKind::Ref;
    decoded.ranges[1].ref_sequence = 1;
    decoded.ranges[1].length = 2; // Includes one absent byte after a valid write.
    REQUIRE_FALSE(R::ApplyBlobToBytes(decoded, 2, bytes, log, written, error));
    REQUIRE(bytes == std::vector<u8>{42, 0, 99, 99});
    REQUIRE(written.size() == 2);
    REQUIRE(log.UnchangedSince(0, 2, 1));
}
