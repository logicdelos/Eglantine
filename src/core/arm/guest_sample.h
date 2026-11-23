// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <thread>

#include "common/common_types.h"

namespace Core {

/// Where a core's guest code last stopped and which guest thread it ran: a pair another host
/// thread (the no-progress probe) can read while the core keeps running, because the core
/// publishes it itself, at the points where its CPU is not executing guest code, and nothing
/// else ever touches the CPU's context. One writer (the core's host thread), any readers; a
/// reader never sees the program counter of one stop with the thread id of another (a seqlock).
class GuestSample {
public:
    /// The core's host thread only, with the CPU stopped.
    void Publish(u64 pc, u64 thread_id) noexcept {
        const u64 sequence = m_sequence.load(std::memory_order_relaxed);
        // Odd while the pair is being written; the fence keeps the data stores after this one.
        m_sequence.store(sequence + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        m_pc.store(pc, std::memory_order_relaxed);
        m_thread_id.store(thread_id, std::memory_order_relaxed);
        m_sequence.store(sequence + 2, std::memory_order_release);
    }

    /// Any thread. False when the core has not published yet (or the writer never settled, which
    /// takes a host thread preempted for the length of a few yields in the middle of two stores).
    [[nodiscard]] bool Read(u64& pc, u64& thread_id) const noexcept {
        constexpr int MaxAttempts = 64;
        for (int attempt = 0; attempt < MaxAttempts; ++attempt) {
            const u64 before = m_sequence.load(std::memory_order_acquire);
            if (before == 0) {
                return false;
            }
            if ((before & 1) == 0) {
                const u64 read_pc = m_pc.load(std::memory_order_relaxed);
                const u64 read_thread_id = m_thread_id.load(std::memory_order_relaxed);
                std::atomic_thread_fence(std::memory_order_acquire);
                if (m_sequence.load(std::memory_order_relaxed) == before) {
                    pc = read_pc;
                    thread_id = read_thread_id;
                    return true;
                }
            }
            std::this_thread::yield();
        }
        return false;
    }

private:
    std::atomic<u64> m_sequence{0};
    std::atomic<u64> m_pc{0};
    std::atomic<u64> m_thread_id{0};
};

} // namespace Core
