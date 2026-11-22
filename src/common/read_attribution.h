// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>
#include <span>
#include <string>
#include <string_view>

#include "common/common_types.h"

// What the game is loading: every guest read of a storage, a file or a NUP member is counted
// against its source, and once per 10 s window a burst (>= 500 reads or >= 64 MiB of one kind of
// source) is logged with its top 5 sources. The hot path is two relaxed adds into a slot plus two
// into the totals: no lock, no allocation, no clock. The name of a source is resolved once, on
// the first read of an open handle (Source::Note takes it as a callable), never per read.
//
// The three kinds overlap: a storage read of a RomFS is served by reads of NUP members or files,
// so the kinds are reported side by side and never added together.
namespace Common::ReadAttribution {

enum class Kind : u8 {
    Storage,   ///< IStorage::Read (the guest's RomFS / data storage)
    File,      ///< IFile::Read (a file opened through fsp)
    NupMember, ///< a member of a NUP container
};
inline constexpr std::size_t KindCount = 3;

[[nodiscard]] std::string_view KindName(Kind kind) noexcept;

inline constexpr std::size_t MaxSources = 4096; ///< slot 0 is "unregistered", the last is overflow
inline constexpr u64 BurstReads = 500;
inline constexpr u64 BurstBytes = u64{64} << 20;
inline constexpr std::size_t TopSources = 5;
inline constexpr double WindowSeconds = 10.0;

namespace Detail {

struct Slot {
    std::atomic<u64> reads{};
    std::atomic<u64> bytes{};
};

inline std::array<Slot, MaxSources> slots{};
inline std::atomic<u64> total_reads{};
inline std::atomic<u64> total_bytes{};
/// Set on a thread that reads on behalf of a verification job (NupContainer::VerifyIntegrity),
/// whose reads are not the game's.
inline thread_local bool suppressed = false;

} // namespace Detail

/// The id of a source (cold: takes a lock, allocates the first time a name is seen). The same
/// kind and name always give the same id, so a file opened a thousand times is one source. Past
/// MaxSources - 1 distinct names every new one shares the "(other sources)" id.
[[nodiscard]] u32 Register(Kind kind, std::string_view name);

/// Counts one read of `bytes` bytes (hot) against a registered source.
inline void Note(u32 id, u64 bytes) noexcept {
    auto& slot = Detail::slots[id];
    slot.reads.fetch_add(1, std::memory_order_relaxed);
    slot.bytes.fetch_add(bytes, std::memory_order_relaxed);
    Detail::total_reads.fetch_add(1, std::memory_order_relaxed);
    Detail::total_bytes.fetch_add(bytes, std::memory_order_relaxed);
}

/// The reads and bytes counted since the process started, for a job that backs off while the game
/// is reading. Not reset by the window report.
[[nodiscard]] inline u64 TotalReads() noexcept {
    return Detail::total_reads.load(std::memory_order_relaxed);
}
[[nodiscard]] inline u64 TotalBytes() noexcept {
    return Detail::total_bytes.load(std::memory_order_relaxed);
}

/// Member of a storage, file or NUP member object: its id, found on its first counted read.
class Source {
public:
    /// `name` is called at most once per object, on the first read.
    template <typename NameFn>
    void Note(Kind kind, u64 bytes, NameFn&& name) const {
        if (Detail::suppressed) {
            return;
        }
        u32 value = id.load(std::memory_order_relaxed);
        if (value == 0) {
            value = Register(kind, std::forward<NameFn>(name)());
            id.store(value, std::memory_order_relaxed);
        }
        ::Common::ReadAttribution::Note(value, bytes);
    }

private:
    mutable std::atomic<u32> id{};
};

/// Reads made while this is alive on this thread are not counted.
class ScopedSuppress {
public:
    ScopedSuppress() noexcept : previous{Detail::suppressed} {
        Detail::suppressed = true;
    }
    ~ScopedSuppress() {
        Detail::suppressed = previous;
    }
    ScopedSuppress(const ScopedSuppress&) = delete;
    ScopedSuppress& operator=(const ScopedSuppress&) = delete;

private:
    bool previous;
};

/// One source of a window.
struct Row {
    Kind kind{};
    std::string name;
    u64 reads{};
    u64 bytes{};
};

/// The log line of a window, or nothing when no kind reached the burst threshold. Pure.
[[nodiscard]] std::optional<std::string> FormatBurst(double seconds, std::span<const Row> rows);

/// Closes the window that began at the last call (or at process start): logs the burst line if
/// the window was one, and starts the next. Does nothing (and keeps counting) until at least
/// 9 s have passed, so it is safe to call from a 10 s report loop and from nowhere else.
void EmitWindowReport();

/// For tests: the window without the clock and the log.
[[nodiscard]] std::vector<Row> TakeWindowForTests();

} // namespace Common::ReadAttribution
