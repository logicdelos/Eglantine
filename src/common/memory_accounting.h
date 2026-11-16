// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <cstddef>

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

} // namespace Common::MemoryAccounting
