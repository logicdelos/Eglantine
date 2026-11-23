// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <vector>

#include "common/common_types.h"
#include "video_core/nx2_host_gpu.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class Device;
class MasterSemaphore;
class Scheduler;

/// The host GPU time of one frame in FrameInterval by pass, from timestamps the scheduler writes
/// where the work changes (nx2_host_gpu.h has what a label is and how the spans add up). Only
/// the recording thread calls it. The timestamps go through Scheduler::Record, so they are in
/// command order on the worker. A profiled frame has a slot of MaxQueriesPerFrame queries; the
/// slot is read back, without waiting, once the frame's last submission is known to be done, a
/// frame or more later, and is free again after that. A frame that finds no free slot is not
/// profiled. Every report window (Nx2Profile's) the timeline logs the line.
class HostGpuTimeline {
public:
    explicit HostGpuTimeline(const Device& device, Scheduler& scheduler,
                             MasterSemaphore& master_semaphore);
    ~HostGpuTimeline();

    /// Whether the graphics queue has timestamps and the pool was made (one log line otherwise).
    [[nodiscard]] bool Usable() const noexcept {
        return usable;
    }

    /// The start of a frame (the previous one ends here if it did not end already): reads back
    /// the frames that are done, logs the window when it is due, and starts profiling this frame
    /// if it is the one in FrameInterval. Returns whether the frame is profiled.
    [[nodiscard]] bool BeginFrame();

    /// Writes the timestamp that ends the profiled frame, in the submission that ends it.
    void EndFrame();

    /// Writes a timestamp that starts a span with `label`, in a profiled frame.
    void Note(const VideoCommon::Nx2HostGpu::Label& label);

private:
    static constexpr u32 SlotCount = 4;

    enum class State : u8 {
        Free,
        Recording,
        Pending, // ended, waiting for the host GPU to finish its submission
    };

    struct Slot {
        VideoCommon::Nx2HostGpu::FrameLog log;
        State state{State::Free};
        u64 tick{}; ///< The submission the frame's last timestamp is in.
        u32 polls{};
    };

    [[nodiscard]] u32 BaseOf(const Slot& slot) const noexcept;
    void Write(u32 query);
    void Reset(u32 base);
    void ReadBack();
    void LogWindow();

    const Device& device;
    Scheduler& scheduler;
    MasterSemaphore& master_semaphore;
    vk::QueryPool query_pool;
    bool usable{};
    bool host_reset{};
    double ns_per_tick{};
    u32 valid_bits{};
    std::array<Slot, SlotCount> slots;
    Slot* recording{};
    u32 frame_index{};
    VideoCommon::Nx2HostGpu::Window window;
    u64 window_begin_ns{};
    std::vector<u64> raw;
    std::vector<u64> ticks;
};

} // namespace Vulkan
