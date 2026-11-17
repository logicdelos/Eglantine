// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <variant>
#include <boost/container/static_vector.hpp>

#include "common/assert.h"
#include "common/logging.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_update_descriptor.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

UpdateDescriptorQueue::UpdateDescriptorQueue(const Device& device_, size_t frame_payload_size_,
                                             bool supports_descriptor_buffer_)
    : device{device_}, frame_payload_size{frame_payload_size_},
      supports_descriptor_buffer{supports_descriptor_buffer_},
      payload(std::make_unique_for_overwrite<DescriptorUpdateEntry[]>(frame_payload_size_ * FRAMES_IN_FLIGHT))
{
    payload_start = payload.get();
    payload_cursor = payload.get();
}

UpdateDescriptorQueue::~UpdateDescriptorQueue() = default;

void UpdateDescriptorQueue::TickFrame() {
    ++sequence;
    ++ticks;
    last_tick = std::chrono::steady_clock::now();
    if (++frame_index >= FRAMES_IN_FLIGHT) {
        frame_index = 0;
    }
    payload_start = payload.get() + frame_index * frame_payload_size;
    payload_cursor = payload_start;
}

void UpdateDescriptorQueue::AcquireOverflow(Scheduler& scheduler, size_t reserve) {
    ASSERT_MSG(reserve < frame_payload_size, "Descriptor reservation {} >= frame capacity {}",
               reserve, frame_payload_size);
    const size_t used = static_cast<size_t>(std::distance(payload_start, payload_cursor));
    if (used + reserve >= frame_payload_size) {
        using namespace std::chrono;
        constexpr auto LOG_INTERVAL = seconds{5};
        const auto now = steady_clock::now();
        ++overflows;
        if (overflows == 1 || now - last_log >= LOG_INTERVAL) {
            // A tick count that does not move between two warnings means the frame never ends
            // (nothing calls TickFrame), not that one frame needs more than the payload holds.
            const auto since_tick =
                last_tick == steady_clock::time_point{}
                    ? -1
                    : static_cast<long long>(duration_cast<milliseconds>(now - last_tick).count());
            LOG_WARNING(Render_Vulkan,
                        "Payload overflow (used={}, reserve={}, capacity={}): {} overflows and {} "
                        "frame ticks since the previous warning, {} ticks in total, last tick {} ms "
                        "ago (-1: never)",
                        used, reserve, frame_payload_size, overflows - overflows_logged,
                        ticks - ticks_at_last_log, ticks, since_tick);
            overflows_logged = overflows;
            ticks_at_last_log = ticks;
            last_log = now;
        }
        scheduler.WaitWorker();
        payload_cursor = payload_start;
        ++sequence;
    }
    upload_start = payload_cursor;
    ++sequence;
}

} // namespace Vulkan
