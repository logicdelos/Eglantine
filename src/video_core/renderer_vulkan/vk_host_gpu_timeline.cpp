// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <chrono>

#include "common/logging.h"
#include "video_core/nx2_profile.h"
#include "video_core/renderer_vulkan/vk_host_gpu_timeline.h"
#include "video_core/renderer_vulkan/vk_master_semaphore.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/vulkan_common/vulkan_device.h"

namespace Vulkan {

namespace Host = VideoCommon::Nx2HostGpu;

namespace {

/// Polls of a finished submission's queries that find one unavailable before the frame is given up.
constexpr u32 MaxPolls = 4;

u64 NowNs() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
}

} // Anonymous namespace

HostGpuTimeline::HostGpuTimeline(const Device& device_, Scheduler& scheduler_,
                                 MasterSemaphore& master_semaphore_)
    : device{device_}, scheduler{scheduler_}, master_semaphore{master_semaphore_} {
    valid_bits = device.GetGraphicsTimestampValidBits();
    const float period = device.GetTimestampPeriod();
    if (valid_bits == 0 || !(period > 0.0f)) {
        LOG_WARNING(HW_GPU,
                    "Host GPU time by pass is off: the graphics queue family has no usable "
                    "timestamps (valid bits {}, period {} ns)",
                    valid_bits, period);
        return;
    }
    try {
        query_pool = device.GetLogical().CreateQueryPool({
            .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .queryType = VK_QUERY_TYPE_TIMESTAMP,
            .queryCount = SlotCount * Host::MaxQueriesPerFrame,
            .pipelineStatistics = 0,
        });
    } catch (const vk::Exception& exception) {
        LOG_WARNING(HW_GPU, "Host GPU time by pass is off: no timestamp query pool ({})",
                    exception.what());
        return;
    }
    ns_per_tick = static_cast<double>(period);
    host_reset = device.IsHostQueryResetSupported();
    window_begin_ns = NowNs();
    usable = true;
}

HostGpuTimeline::~HostGpuTimeline() = default;

u32 HostGpuTimeline::BaseOf(const Slot& slot) const noexcept {
    return static_cast<u32>(&slot - slots.data()) * Host::MaxQueriesPerFrame;
}

void HostGpuTimeline::Write(u32 query) {
    // Bottom of pipe: the timestamp is taken when everything before it has finished, so a span
    // is the time from one change of work to the next.
    scheduler.Record([pool = *query_pool, query](vk::CommandBuffer cmdbuf) {
        cmdbuf.WriteTimestamp(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pool, query);
    });
}

void HostGpuTimeline::Reset(u32 base) {
    if (host_reset) {
        // The slot was read back, so the host GPU is done with its queries.
        device.GetLogical().ResetQueryPool(*query_pool, base, Host::MaxQueriesPerFrame);
        return;
    }
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([pool = *query_pool, base](vk::CommandBuffer cmdbuf) {
        cmdbuf.ResetQueryPool(pool, base, Host::MaxQueriesPerFrame);
    });
}

bool HostGpuTimeline::BeginFrame() {
    if (!usable) {
        return false;
    }
    if (recording != nullptr) {
        // The last frame did not reach the present that ends a frame.
        EndFrame();
    }
    ReadBack();
    LogWindow();
    if (frame_index++ % Host::FrameInterval != 0) {
        return false;
    }
    Slot* free_slot = nullptr;
    for (Slot& slot : slots) {
        if (slot.state == State::Free) {
            free_slot = &slot;
            break;
        }
    }
    if (free_slot == nullptr) {
        window.NoteSkipped();
        return false;
    }
    free_slot->log.Clear();
    free_slot->state = State::Recording;
    free_slot->tick = 0;
    free_slot->polls = 0;
    recording = free_slot;
    Reset(BaseOf(*free_slot));
    Note(Host::Label{});
    return true;
}

void HostGpuTimeline::EndFrame() {
    if (recording == nullptr) {
        return;
    }
    Slot& slot = *recording;
    recording = nullptr;
    const u32 index = slot.log.End();
    if (index == Host::FrameLog::NoQuery) {
        slot.state = State::Free;
        window.NoteSkipped();
        return;
    }
    Write(BaseOf(slot) + index);
    // The timestamp is in the submission that carries this tick.
    slot.tick = scheduler.CurrentTick();
    slot.state = State::Pending;
}

void HostGpuTimeline::Note(const Host::Label& label) {
    if (recording == nullptr) {
        return;
    }
    const u32 index = recording->log.Begin(label);
    if (index != Host::FrameLog::NoQuery) {
        Write(BaseOf(*recording) + index);
    }
}

void HostGpuTimeline::ReadBack() {
    bool refreshed = false;
    for (Slot& slot : slots) {
        if (slot.state != State::Pending) {
            continue;
        }
        if (!refreshed) {
            master_semaphore.Refresh();
            refreshed = true;
        }
        if (!master_semaphore.IsFree(slot.tick)) {
            continue;
        }
        const u32 used = slot.log.Used();
        raw.resize(size_t{used} * 2);
        // Without VK_QUERY_RESULT_WAIT_BIT: the call never waits; the submission is done, so the
        // availability words are expected to be set.
        const VkResult result = device.GetLogical().GetQueryResults(
            *query_pool, BaseOf(slot), used, raw.size() * sizeof(u64), raw.data(),
            2 * sizeof(u64), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        const bool readable = result == VK_SUCCESS || result == VK_NOT_READY;
        bool available = readable;
        ticks.resize(used);
        for (u32 i = 0; i < used; ++i) {
            ticks[i] = raw[size_t{i} * 2];
            available = available && raw[size_t{i} * 2 + 1] != 0;
        }
        if (!available && readable && ++slot.polls < MaxPolls) {
            continue;
        }
        if (available) {
            window.AddFrame(ticks, slot.log.Labels(), slot.log.Overflow(), ns_per_tick,
                            valid_bits);
        } else {
            window.NoteSkipped();
        }
        slot.state = State::Free;
    }
}

void HostGpuTimeline::LogWindow() {
    const u64 now = NowNs();
    if (now - window_begin_ns < VideoCommon::Nx2Profile::g_window_ns.load(std::memory_order_relaxed)) {
        return;
    }
    window_begin_ns = now;
    if (window.Frames() != 0) {
        LOG_INFO(HW_GPU, "{}", window.Format());
    }
    window.Clear();
}

} // namespace Vulkan
