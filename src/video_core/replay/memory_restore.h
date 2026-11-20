// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstring>
#include <iterator>
#include <limits>
#include <map>
#include <span>
#include <string>
#include <vector>

#include "video_core/replay/events.h"

namespace VideoCore::Replay {

// ---- written ranges ------------------------------------------------------------------------

// Last writer (chunk sequence) of every byte range of one allocation backing. A Ref range says
// "the same bytes as blob chunk S": that holds only while nothing wrote the range after S, and
// the session refuses the capture instead of guessing when it does not.
class RangeWriteLog {
public:
    void Note(u64 offset, u64 length, u64 sequence) {
        if (length == 0) {
            return;
        }
        const u64 end = offset + length;
        auto it = segments.lower_bound(offset);
        if (it != segments.begin()) {
            const auto previous = std::prev(it);
            if (previous->second.end > offset) {
                it = previous;
            }
        }
        while (it != segments.end() && it->first < end) {
            const u64 start = it->first;
            const Segment segment = it->second;
            it = segments.erase(it);
            if (start < offset) {
                segments.emplace_hint(it, start, Segment{offset, segment.sequence});
            }
            if (segment.end > end) {
                segments.emplace_hint(it, end, Segment{segment.end, segment.sequence});
            }
        }
        if (sequence != 0) {
            segments.emplace(offset, Segment{end, sequence});
        }
    }

    // True when every byte exists and no byte of [offset, offset + length) was written by a chunk after `sequence`.
    [[nodiscard]] bool UnchangedSince(u64 offset, u64 length, u64 sequence) const {
        if (length > std::numeric_limits<u64>::max() - offset) {
            return false;
        }
        const u64 end = offset + length;
        auto it = segments.lower_bound(offset);
        if (it != segments.begin()) {
            const auto previous = std::prev(it);
            if (previous->second.end > offset) {
                it = previous;
            }
        }
        for (; offset < end; ++it) {
            if (it == segments.end() || it->first > offset || it->second.sequence > sequence) {
                return false;
            }
            offset = it->second.end;
        }
        return true;
    }

    [[nodiscard]] size_t SegmentCount() const noexcept {
        return segments.size();
    }

private:
    struct Segment {
        u64 end = 0;
        u64 sequence = 0;
    };
    std::map<u64, Segment> segments;
};

struct WrittenRange {
    u64 offset = 0;
    u64 length = 0;
};

// Applies one MEMORY_BLOB or EXTERNAL_WRITE to the bytes of its allocation's backing. Data and
// Zero ranges are written (and listed in `written`); an Undefined range is left alone; a Ref
// range is checked against the write log and consumes nothing. The reader validated the same
// rules once; they are checked again here because this is the function that writes memory.
[[nodiscard]] inline bool ApplyBlobToBytes(const MemoryBlob& blob, u64 sequence,
                                           std::span<u8> bytes, RangeWriteLog& log,
                                           std::vector<WrittenRange>& written,
                                           std::string& error) {
    if (sequence == 0 || blob.allocation_size != bytes.size()) {
        error = "invalid blob sequence or backing size";
        return false;
    }
    u64 previous_end = 0;
    for (const MemoryRange& range : blob.ranges) {
        if (range.length == 0 || range.offset < previous_end ||
            range.length > bytes.size() || range.offset > bytes.size() - range.length) {
            error = "blob range is empty, unsorted, overlapping or outside the allocation";
            return false;
        }
        previous_end = range.offset + range.length;
        switch (range.kind) {
        case RangeKind::Data:
            if (range.length > blob.data.size() ||
                range.data_offset > blob.data.size() - range.length) {
                error = "blob data range reaches past its data";
                return false;
            }
            break;
        case RangeKind::Zero:
        case RangeKind::Undefined:
            break;
        case RangeKind::Ref:
            if (range.ref_sequence == 0 || range.ref_sequence >= sequence ||
                !log.UnchangedSince(range.offset, range.length, range.ref_sequence)) {
                error = "blob ref range was overwritten after the chunk it refers to";
                return false;
            }
            break;
        default:
            error = "blob range kind is unknown";
            return false;
        }
    }
    // No destination state is changed until every range has been checked.
    for (const MemoryRange& range : blob.ranges) {
        if (range.kind == RangeKind::Ref) {
            continue;
        }
        if (range.kind == RangeKind::Undefined) {
            log.Note(range.offset, range.length, 0);
            continue;
        }
        if (range.kind == RangeKind::Data) {
            std::memcpy(bytes.data() + range.offset, blob.data.data() + range.data_offset,
                        static_cast<size_t>(range.length));
        } else {
            std::memset(bytes.data() + range.offset, 0, static_cast<size_t>(range.length));
        }
        log.Note(range.offset, range.length, sequence);
        written.push_back({range.offset, range.length});
    }
    error.clear();
    return true;
}

} // namespace VideoCore::Replay
