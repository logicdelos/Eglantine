// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/common_types.h"

// Background integrity check of the running game's .nup: every member is hashed with SHA-256 and
// compared with the hash stored in the container. A corrupt member shows up in the log as a
// Warning that names it, instead of as a crash or a missing texture hours later.
//
// The check never gets in the way of the game:
//  - it runs on its own low-priority thread (background IO priority on Windows) with its own file
//    handle, and a stop request cancels it within one 1 MiB step;
//  - it reads at most MaxBytesPerSecond, and waits while the game itself is reading heavily (the
//    read report's counters, see common/read_attribution.h);
//  - it streams: a few MiB of memory, and nothing goes through the decompress cache;
//  - a result is remembered per file (path, size, modification time) in the cache directory, so
//    an unchanged file is not read again at the next launch.
namespace FileSys::NupIntegrity {

inline constexpr std::size_t MaxCacheEntries = 16;
inline constexpr std::size_t NamesPerWarning = 12;
inline constexpr u64 MaxBytesPerSecond = 32ull * 1024 * 1024;
/// The game counts as reading heavily from this many reads or bytes per second.
inline constexpr double BusyReadsPerSecond = 200.0;
inline constexpr double BusyBytesPerSecond = 8.0 * 1024 * 1024;
inline constexpr std::chrono::milliseconds PollInterval{250};

struct FileStamp {
    std::string path; ///< UTF-8
    u64 size{};
    s64 mtime{}; ///< file_time_type ticks; only compared for equality
    bool operator==(const FileStamp&) const = default;
};

struct BadMember {
    std::string path;
    std::string reason;
};

struct CacheEntry {
    FileStamp stamp;
    s64 checked_at{}; ///< unix seconds
    u64 members{};
    std::vector<BadMember> bad;
};

/// The text of the result cache, newest entry first, at most MaxCacheEntries of them.
[[nodiscard]] std::string SerializeCache(std::span<const CacheEntry> entries);
/// Entries of a cache text; unknown or damaged lines are skipped, a wrong header gives none.
[[nodiscard]] std::vector<CacheEntry> ParseCache(std::string_view text);
[[nodiscard]] const CacheEntry* FindCached(std::span<const CacheEntry> entries,
                                           const FileStamp& stamp);
/// Puts `entry` first, replacing an entry of the same path, and drops the oldest beyond the cap.
void Remember(std::vector<CacheEntry>& entries, CacheEntry entry);

/// The Warning lines for `bad`: member names, NamesPerWarning per line, each line prefixed with
/// the file name and the count. Empty when nothing is bad.
[[nodiscard]] std::vector<std::string> FormatBadMembers(std::string_view file_name,
                                                        std::span<const BadMember> bad);

/// Decides when the check may read. Pure: the caller passes the clock and the game's cumulative
/// read counters, so that it can be tested without waiting.
class ReadPacer {
public:
    using Clock = std::chrono::steady_clock;

    /// Zero when `bytes` may be read now (they are charged to the rate limit); otherwise how long
    /// to wait before asking again.
    [[nodiscard]] std::chrono::nanoseconds Admit(u64 game_reads, u64 game_bytes,
                                                 Clock::time_point now, u64 bytes);

    [[nodiscard]] bool GameIsBusy() const noexcept {
        return busy;
    }

private:
    bool started = false;
    bool busy = false;
    Clock::time_point sampled_at{};
    u64 sampled_reads = 0;
    u64 sampled_bytes = 0;
    Clock::time_point next_read{};
};

/// Starts the check of `file` in the background if it is a NUP, and stops the check of any other
/// file. Starting the same file twice is a no-op. Returns at once.
void Start(const std::filesystem::path& file);

/// Cancels the check and waits for its thread (one 1 MiB step at most). Safe to call any time.
void Stop() noexcept;

} // namespace FileSys::NupIntegrity
