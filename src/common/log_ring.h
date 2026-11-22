// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "common/common_types.h"

// The in-memory log ring (the debug server's log tail, docs/nx2/debug-mcp.md). Kept out of
// logging.h, which nearly every file includes.
namespace Common::Log {

/// A line kept by the in-memory log ring.
struct RingLine {
    u64 sequence;
    std::string text;
};

/// Keeps the last `capacity` formatted lines in memory; 0 turns the ring off and empties it.
void SetRingCapacity(std::size_t capacity);

[[nodiscard]] std::size_t RingCapacity();

/// The number of the newest line kept (0 before the first).
[[nodiscard]] u64 RingNewestSequence();

/// The kept lines numbered above `after`, oldest first.
[[nodiscard]] std::vector<RingLine> ReadRing(u64 after);

/// What the ring holds now. `evicted` counts lines pushed out (or dropped by a smaller capacity)
/// since the process started; `oldest` is the sequence the next line would take when empty.
struct RingStatus {
    std::size_t capacity = 0;
    u64 newest = 0;
    u64 oldest = 0;
    u64 evicted = 0;
};

/// The lines above `after` and the ring state they were read under, taken together.
struct RingRead {
    std::vector<RingLine> lines;
    RingStatus status;
};

[[nodiscard]] RingRead ReadRingWithStatus(u64 after);

/// How many lines a reader whose last line was `after` can no longer get, given the oldest
/// sequence still kept.
[[nodiscard]] constexpr u64 LinesLostSince(u64 after, u64 oldest) {
    return after >= oldest ? 0 : oldest - after - 1;
}

/// The native log file as written by this process.
struct FileStatus {
    bool open = false;
    std::string path;
    u64 bytes_written = 0;
    u64 limit = 0;
    bool limit_reached = false;
};

[[nodiscard]] FileStatus GetFileStatus();

} // namespace Common::Log
