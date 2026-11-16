// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string_view>

#include "common/common_types.h"

// Byte counters the memory owners keep and the memory line of Nx2Bottleneck reads. Owners update
// them with relaxed atomics where they already allocate or free; nothing locks or runs per draw.
namespace Common::MemoryAccounting {

enum class Owner : u32 {
    GuestRam,       ///< Guest RAM backing committed from Windows
    JitCode,        ///< Committed code space of the JIT instances
    GpuLocalHeap,   ///< Device-local Vulkan heap in use (the driver's figure)
    GpuLocalBudget, ///< Its budget
    GpuHostHeap,    ///< Vulkan heaps that are not device-local, in use
    GpuHostBudget,  ///< Their budget
    Staging,        ///< Vulkan staging buffers: the pool's and the stream ring
    BufferScratch,  ///< Buffer cache CPU scratch buffers
    TextureScratch, ///< Texture cache CPU scratch buffers
    Count,
};

struct Counter {
    std::atomic<u64> bytes{};
    std::atomic<u64> peak{}; ///< Highest `bytes` seen; zero until the owner reported
};

inline std::array<Counter, static_cast<size_t>(Owner::Count)> counters;

inline void RaisePeak(Counter& counter, u64 value) {
    u64 peak = counter.peak.load(std::memory_order_relaxed);
    while (value > peak &&
           !counter.peak.compare_exchange_weak(peak, value, std::memory_order_relaxed)) {
    }
}

inline void Add(Owner owner, u64 bytes) {
    Counter& counter = counters[static_cast<size_t>(owner)];
    RaisePeak(counter, counter.bytes.fetch_add(bytes, std::memory_order_relaxed) + bytes);
}

inline void Sub(Owner owner, u64 bytes) {
    counters[static_cast<size_t>(owner)].bytes.fetch_sub(bytes, std::memory_order_relaxed);
}

/// For owners that sample a figure rather than count changes. Stores only when it changed.
inline void Set(Owner owner, u64 bytes) {
    Counter& counter = counters[static_cast<size_t>(owner)];
    if (counter.bytes.load(std::memory_order_relaxed) != bytes) {
        counter.bytes.store(bytes, std::memory_order_relaxed);
    }
    RaisePeak(counter, bytes);
}

[[nodiscard]] inline u64 Bytes(Owner owner) {
    return counters[static_cast<size_t>(owner)].bytes.load(std::memory_order_relaxed);
}

[[nodiscard]] inline u64 Peak(Owner owner) {
    return counters[static_cast<size_t>(owner)].peak.load(std::memory_order_relaxed);
}

/// An address range a big owner holds, named for the region census of Nx2Bottleneck
/// (EDEN_NX2_MEMORY_MAP). Owners register where they create the memory and unregister where they
/// free it; the census reads the slots from its own thread.
struct Range {
    /// Generation: (state & 3) is 0 free, 1 being filled in, 2 live; each Register and Unregister
    /// moves it on (0 -> 1 -> 2 -> 4 -> 5 -> 6 -> 8 ...), so a reader that sees the same live
    /// state before and after reading the fields read one registration.
    std::atomic<u64> state{};
    std::atomic<const char*> name{};
    std::atomic<uintptr_t> base{};
    std::atomic<size_t> size{};
};

inline constexpr size_t MaxRanges = 2048;
inline std::array<Range, MaxRanges> ranges;

[[nodiscard]] inline bool RegistrationEnabled() {
    static const bool enabled = [] {
        const char* const value = std::getenv("EDEN_NX2_MEMORY_MAP");
        return value != nullptr && value[0] != '\0' && std::string_view{value} != "0";
    }();
    return enabled;
}

/// `name` must outlive the range (a literal). A full registry drops the range.
inline void Register(const char* name, const void* base, size_t size) {
    if (!RegistrationEnabled() || base == nullptr || size == 0) {
        return;
    }
    for (Range& range : ranges) {
        u64 state = range.state.load();
        if ((state & 3) != 0 || !range.state.compare_exchange_strong(state, state + 1)) {
            continue;
        }
        range.name.store(name);
        range.base.store(reinterpret_cast<uintptr_t>(base));
        range.size.store(size);
        range.state.store(state + 2);
        return;
    }
}

inline void Unregister(const void* base) {
    if (!RegistrationEnabled() || base == nullptr) {
        return;
    }
    for (Range& range : ranges) {
        u64 state = range.state.load();
        if ((state & 3) == 2 && range.base.load() == reinterpret_cast<uintptr_t>(base)) {
            range.state.compare_exchange_strong(state, state + 2);
        }
    }
}

} // namespace Common::MemoryAccounting
