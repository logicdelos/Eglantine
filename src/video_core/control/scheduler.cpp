// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <memory>
#include <vector>

#include "common/assert.h"
#include "core/core.h"
#include "video_core/control/channel_state.h"
#include "video_core/control/scheduler.h"
#include "video_core/gpu.h"
#include "video_core/gpu_split.h"
#include "video_core/replay/capture.h"
#include "video_core/replay/run_state.h"

namespace Tegra::Control {

void Scheduler::BindChannel(GPU& gpu, const std::shared_ptr<ChannelState>& channel) {
    // With the GPU thread split the rasterizer's channel switches with the methods around it, on
    // the back thread (deferred outside the lock: the back takes it to bind).
    if (auto& split = VideoCommon::GpuSplit::Get(); split.Active()) {
        split.Defer(
            [this, gpu_ptr = &gpu, state = channel.get()] {
                std::scoped_lock lk{scheduling_guard};
                gpu_ptr->BindChannel(*state);
            },
            VideoCommon::GpuSplit::Effect::None);
        return;
    }
    std::scoped_lock lk{scheduling_guard};
    gpu.BindChannel(*channel);
}

void Scheduler::PublishPendingState() {
    any_pending.store(!pending_channels.empty(), std::memory_order_release);
    any_cpu_pending.store(std::ranges::any_of(pending_channels, [](const auto& state) {
        return state->payload->dma_pusher.HasCpuDependentAcquire();
    }), std::memory_order_release);
}

void Scheduler::Push(GPU& gpu, s32 channel, CommandList&& entries) {
    std::shared_ptr<ChannelState> channel_state;
    {
        std::unique_lock lk(scheduling_guard);
        ASSERT(channel > 0 && static_cast<size_t>(channel) <= channels.size());
        channel_state = channels[channel - 1];
    }
    BindChannel(gpu, channel_state);
    // Process commands outside the lock to reduce contention.
    // Multiple channels can prepare their commands in parallel.
    VideoCore::Replay::NoteSubmit(channel, entries);
    channel_state->payload->dma_pusher.Push(std::move(entries));
    channel_state->payload->dma_pusher.DispatchCalls();
    if (channel_state->payload->dma_pusher.HasPendingAcquire() &&
        std::find(pending_channels.begin(), pending_channels.end(), channel_state) ==
            pending_channels.end()) {
        pending_channels.push_back(channel_state);
        PublishPendingState();
    }
    if (!pending_channels.empty()) {
        ResumePending(gpu);
    }
}

bool Scheduler::ResumePending(GPU& gpu) {
    bool any_moved = false;
    // Running one channel can release the semaphore another waits for: retry until none moves.
    for (bool progress = true; progress;) {
        progress = false;
        for (const auto& state : pending_channels) {
            auto& dma_pusher = state->payload->dma_pusher;
            // Powered off (shutting down), DispatchCalls returns at once and the acquire stays
            // pending: resuming it again counted as progress forever and the GPU thread never
            // reached its stop request (closing the game hung).
            if (!VideoCore::Replay::GpuWorkRunning(dma_pusher.system.IsPoweredOn())) {
                PublishPendingState();
                return any_moved;
            }
            if (!dma_pusher.HasPendingAcquire() || !dma_pusher.CanResumeAcquire()) {
                continue;
            }
            BindChannel(gpu, state);
            dma_pusher.DispatchCalls();
            if (auto& split = VideoCommon::GpuSplit::Get(); split.Active()) {
                split.Publish();
            }
            progress = true;
            any_moved = true;
        }
    }
    if (any_moved) {
        if (auto& split = VideoCommon::GpuSplit::Get(); split.Active()) {
            split.Publish();
        }
    }
    std::erase_if(pending_channels, [](const auto& state) {
        return !state->payload->dma_pusher.HasPendingAcquire();
    });
    PublishPendingState();
    return any_moved;
}

std::shared_ptr<ChannelState> Scheduler::AllocateChannel() {
    std::scoped_lock lock{scheduling_guard};
    return channels.emplace_back(std::make_shared<ChannelState>(static_cast<s32>(channels.size() + 1)));
}

std::vector<std::shared_ptr<ChannelState>> Scheduler::SnapshotChannels() {
    std::scoped_lock lock{scheduling_guard};
    return channels;
}

} // namespace Tegra::Control
