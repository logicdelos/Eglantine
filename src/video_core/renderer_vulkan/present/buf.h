// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <vector>

#include "common/math_util.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class Device;
class Scheduler;

/// BUF, the BasicUpscaler Formula: EASU with its sharpening folded into the one pass.
class BUF {
public:
    explicit BUF(const Device& device, MemoryAllocator& memory_allocator, size_t image_count,
                 VkExtent2D extent);
    VkImageView Draw(const Device& device, Scheduler& scheduler, size_t image_index,
                     VkImage source_image, VkImageView source_image_view,
                     VkExtent2D input_image_extent, const Common::Rectangle<f32>& crop_rect);

private:
    void CreateImages(const Device& device);
    void CreateRenderPass(const Device& device);
    void CreateSampler(const Device& device);
    void CreateShaders(const Device& device);
    void CreateDescriptorPool(const Device& device);
    void CreateDescriptorSetLayout(const Device& device);
    void CreateDescriptorSets(const Device& device);
    void CreatePipelineLayout(const Device& device);
    void CreatePipeline(const Device& device);

    void UploadImages(Scheduler& scheduler);
    void UpdateDescriptorSet(const Device& device, VkImageView image_view, size_t image_index);

    MemoryAllocator& m_memory_allocator;
    const size_t m_image_count;
    const VkExtent2D m_extent;

    vk::DescriptorPool m_descriptor_pool;
    vk::DescriptorSetLayout m_descriptor_set_layout;
    vk::PipelineLayout m_pipeline_layout;
    vk::ShaderModule m_vert_shader;
    vk::ShaderModule m_frag_shader;
    vk::Pipeline m_pipeline;
    vk::RenderPass m_renderpass;
    vk::Sampler m_sampler;

    struct Images {
        vk::DescriptorSets descriptor_sets;
        vk::Image image;
        vk::ImageView image_view;
        vk::Framebuffer framebuffer;
    };
    std::vector<Images> m_dynamic_images;
    bool m_images_ready{};
};

} // namespace Vulkan
