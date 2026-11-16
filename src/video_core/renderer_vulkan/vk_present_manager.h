// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <boost/container/deque.hpp>

#include "common/common_types.h"
#include "common/polyfill_thread.h"
#include "video_core/nx2_spikes.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Core::Frontend {
class EmuWindow;
} // namespace Core::Frontend

namespace Vulkan {

class Device;
class Scheduler;
class Swapchain;

struct Frame {
    u32 width;
    u32 height;
    u32 index;
    vk::Image image;
    vk::ImageView image_view;
    vk::ImageView storage_view;
    vk::Framebuffer framebuffer;
    vk::CommandBuffer cmdbuf;
    vk::Semaphore render_ready;
    vk::Fence present_done;
    bool storage_capable{};
    bool is_generated{};
    u32 generation_index{};
    /// The guest frame this host frame shows (the renderer's count of composited guest frames),
    /// 0 when frame attribution is off. Generated frames carry the id of their source frame.
    u64 guest_frame_id{};
    /// The worker run that produced this frame (its work id), 0 when attribution is off.
    VideoCommon::Nx2Spikes::WorkId producer_work{};
    u32 generation_count{};
};

class PresentManager {
public:
    PresentManager(const vk::Instance& instance,
                   Core::Frontend::EmuWindow& render_window,
                   const Device& device,
                   MemoryAllocator& memory_allocator,
                   Scheduler& scheduler,
                   Swapchain& swapchain,
                   vk::SurfaceKHR& surface);
    ~PresentManager();

    /// Returns the last used presentation frame
    Frame* GetRenderFrame();

    /// Returns a free presentation frame if its present fence is already signaled, or nullptr
    Frame* TryGetRenderFrame();

    /// Pushes a frame for presentation
    void Present(Frame* frame);

    /// Recreates the present frame to match the provided parameters
    void RecreateFrame(Frame* frame, u32 width, u32 height, VkFormat image_view_format,
                       VkRenderPass rd, bool storage);

    [[nodiscard]] bool NeedsStorage(const Frame* frame, bool required) const;

    /// Waits for the present thread to finish presenting all queued frames.
    void WaitPresent();

    /// How many additional frames can be queued without stalling the render thread
    [[nodiscard]] size_t MaxExtraFrames() const;

    [[nodiscard]] std::size_t SwapchainImageCount() const { return swapchain_image_count; }
    [[nodiscard]] VkFormat SwapchainImageFormat() const { return swapchain_image_format; }

private:
    void PresentThread(std::stop_token token);

    void CopyToSwapchain(Frame* frame);

    void CopyToSwapchainImpl(Frame* frame);

    /// Returns false when the surface has no area and the swapchain could not be recreated.
    bool RecreateSwapchain(Frame* frame);

    void DiscardFrame(Frame* frame);

    void SetImageCount();

private:
    const vk::Instance& instance;
    Core::Frontend::EmuWindow& render_window;
    const Device& device;
    MemoryAllocator& memory_allocator;
    Scheduler& scheduler;
    Swapchain& swapchain;
    vk::SurfaceKHR& surface;
    vk::CommandPool cmdpool;
    std::vector<Frame> frames;
    boost::container::deque<Frame*> present_queue;
    boost::container::deque<Frame*> free_queue;
    std::condition_variable_any frame_cv;
    std::condition_variable free_cv;
    std::mutex swapchain_mutex;
    std::mutex queue_mutex;
    std::mutex free_mutex;
    std::jthread present_thread;
    bool blit_supported;
    bool storage_supported;
    bool use_present_thread;
    bool presenting_frame{};
    /// Scheduler worker only: the open span of the worker's run up to the next frame enqueue, and
    /// its work id (the producer the presentation thread's wait links to).
    VideoCommon::Nx2Spikes::SpanToken worker_span{};
    VideoCommon::Nx2Spikes::WorkId worker_work{};
    bool wake_present_hold{};
    std::atomic<std::size_t> image_count{};
    std::atomic<std::size_t> swapchain_image_count{};
    std::atomic<VkFormat> swapchain_image_format{};
};

} // namespace Vulkan
