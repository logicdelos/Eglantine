// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Frozen: the accepted-present ledger and the replay lifecycle flag. Header-only on purpose, so
// the one-line hooks in files that must not link against the capture code cost one include.
//
// Accepted-present ledger. A logical frame id is the 1-based count of presents the emulator
// accepted for display, on either present path, in the order they were accepted. Compositor
// refreshes of an unchanged frame, generated or interpolated frames and presents rejected by the
// present path are not counted. The capture (slice B) names frames with these ids, the census
// (slice D) closes call-count frames with them, and the frame-timing log (slice E) tags samples
// with them; the three therefore agree about what "frame N" is. This ledger is deliberately
// separate from Nx2Spikes' host_present_id, which counts host present attempts that qualified for
// the 0.5 attribution and may differ.
//
// Replay lifecycle. A replay runs GPU work without a powered-on System: the replay session sets
// the flag, and the DMA pusher loop asks GpuWorkRunning(system.IsPoweredOn()) instead of
// IsPoweredOn() (hook listed in contracts_0.6.md).

#pragma once

#include <atomic>

#include "common/common_types.h"
#include "video_core/replay/events.h"

namespace VideoCore::Replay {

namespace detail {
inline std::atomic<u64> g_accepted_presents{0};
inline std::atomic<bool> g_replay_gpu_running{false};
} // namespace detail

// Called once for every accepted present; returns its logical id (1, 2, 3 ...). Thread-safe.
inline u64 CountAcceptedPresent() noexcept {
    return detail::g_accepted_presents.fetch_add(1, std::memory_order_acq_rel) + 1;
}

// The id of the most recent accepted present, 0 before the first.
[[nodiscard]] inline u64 LastAcceptedPresentId() noexcept {
    return detail::g_accepted_presents.load(std::memory_order_acquire);
}

// A replay restarts the ledger at the first recorded frame so a replayed frame keeps its
// captured id. Called by the replay session only, before any present is accepted.
inline void SetAcceptedPresentBase(u64 last_id_before_first_frame) noexcept {
    detail::g_accepted_presents.store(last_id_before_first_frame, std::memory_order_release);
}

inline void SetReplayGpuRunning(bool running) noexcept {
    detail::g_replay_gpu_running.store(running, std::memory_order_release);
}

// True while a replay feeds the GPU. The pusher then takes the recorded words of an entry as the
// submitted data and never re-reads the pushbuffer from memory (a replay has not filled it).
[[nodiscard]] inline bool ReplayGpuRunning() noexcept {
    return detail::g_replay_gpu_running.load(std::memory_order_acquire);
}

[[nodiscard]] inline bool GpuWorkRunning(bool system_powered_on) noexcept {
    return system_powered_on || ReplayGpuRunning();
}

// The gpu-frame delay of the test hooks is applied once per frame. A run of the game applies it
// after each composite. A replay applies it when the session ends a frame it measures (the wait
// is added to the frame's time and a preroll frame gets none), so its composites do not.
[[nodiscard]] inline bool CompositeAppliesFrameDelay(bool replay_gpu_running) noexcept {
    return !replay_gpu_running;
}

// One observer of accepted presents outside the capture (the census of slice D registers here).
// The function runs on the presenting thread and must be wait-free. nullptr clears it.
using PresentObserver = void (*)(u64 present_id, PresentPath path) noexcept;

namespace detail {
inline std::atomic<PresentObserver> g_present_observer{nullptr};
} // namespace detail

inline void SetPresentObserver(PresentObserver observer) noexcept {
    detail::g_present_observer.store(observer, std::memory_order_release);
}

inline void NotifyPresentObserver(u64 present_id, PresentPath path) noexcept {
    if (const PresentObserver observer =
            detail::g_present_observer.load(std::memory_order_acquire)) {
        observer(present_id, path);
    }
}

} // namespace VideoCore::Replay
